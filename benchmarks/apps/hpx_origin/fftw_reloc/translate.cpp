#include <llvm/Bitcode/BitcodeWriter.h>
#include <llvm/IR/Constants.h>
#include <llvm/IR/DebugInfo.h>
#include <llvm/IR/IRBuilder.h>
#include <llvm/IR/InlineAsm.h>
#include <llvm/IR/InstIterator.h>
#include <llvm/IR/IntrinsicInst.h>
#include <llvm/IR/LegacyPassManager.h>
#include <llvm/IR/Module.h>
#include <llvm/IR/Verifier.h>
#include <llvm/IRReader/IRReader.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/SourceMgr.h>
#include <llvm/Support/MemoryBuffer.h>
#include <llvm/Transforms/Utils/Cloning.h>
#include <llvm/Transforms/Utils/ValueMapper.h>

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>

using namespace llvm;

struct Translation {
  Module &m;
  LLVMContext &c;
  /* The configured SIMD alignment in bytes.  Every alignment predicate the
   * library applies to an address masks it with a value below this, so a mask
   * at or under `alignment - 1` identifies one and its address operands are
   * evaluated relative to their logical allocation base.  A wider mask is
   * something other than an alignment test and is left alone. */
  uint64_t alignment_bytes;
  Type *byteptr, *word;
  std::map<Function *, Function *> functions;
  std::map<GlobalVariable *, uint64_t> globals;
  FunctionCallee load, store, allocate, release, global, copy, set, sort, difference, alignment;

  void pointer_offsets(Type *type,uint64_t base,std::vector<uint64_t> &out) {
    const auto &layout=m.getDataLayout();
    if(type->isPointerTy()) out.push_back(base);
    else if(auto *s=dyn_cast<StructType>(type)) {
      if(s->isOpaque()) return;
      const auto *fields=layout.getStructLayout(s);
      for(unsigned i=0;i<s->getNumElements();++i)
        pointer_offsets(s->getElementType(i),base+fields->getElementOffset(i),out);
    } else if(auto *a=dyn_cast<ArrayType>(type)) {
      uint64_t stride=layout.getTypeAllocSize(a->getElementType());
      for(uint64_t i=0;i<a->getNumElements();++i)
        pointer_offsets(a->getElementType(),base+i*stride,out);
    }
  }

  void schema(raw_ostream &out) {
    const auto &layout=m.getDataLayout();
    out<<"layout\t"<<m.getDataLayoutStr()<<"\n";
    for(StructType *s:m.getIdentifiedStructTypes()) {
      if(s->isOpaque())continue;
      out<<"type\t"<<s->getName()<<"\t"<<layout.getTypeAllocSize(s)
         <<"\t"<<layout.getABITypeAlign(s).value();
      std::vector<uint64_t> fields;pointer_offsets(s,0,fields);
      for(auto offset:fields)out<<"\t"<<offset;
      out<<"\n";
    }
    for(Function &f:m) if(!f.isDeclaration()) {
      unsigned loads=0,stores=0,callbacks=0;
      for(Instruction &i:instructions(f)) {
        if(auto *l=dyn_cast<LoadInst>(&i)) loads+=l->getType()->isPointerTy();
        if(auto *s=dyn_cast<StoreInst>(&i)) stores+=s->getValueOperand()->getType()->isPointerTy();
        if(auto *call=dyn_cast<CallInst>(&i)) {
          callbacks+=!call->getCalledFunction()&&!call->isInlineAsm();
          if(isa<MemTransferInst>(call)) {
            out<<"copy\t"<<f.getName()<<"\t";call->print(out);out<<"\n";
          }
        }
        if(isa<PtrToIntInst>(i)||isa<IntToPtrInst>(i)) {
          out<<"address\t"<<f.getName()<<"\t";i.print(out);out<<"\n";
        }
      }
      out<<"function\t"<<f.getName()<<"\t"<<loads<<"\t"<<stores<<"\t"<<callbacks<<"\n";
    }
    for(GlobalVariable &g:m.globals()) if(g.hasInitializer()) {
      out<<"global\t"<<g.getName()<<"\t"<<(g.isConstant()?"constant":"context")<<"\t";
      g.getValueType()->print(out);out<<"\n";
    }
  }

  void initializers() {
    StructType *entry=StructType::get(byteptr,word,PointerType::getUnqual(word),word);
    std::vector<Constant *> values(globals.size());
    for(auto p:globals) {
      std::vector<uint64_t> offsets;pointer_offsets(p.first->getValueType(),0,offsets);
      std::vector<Constant *> fields;
      for(auto n:offsets)fields.push_back(ConstantInt::get(word,n));
      auto *array=ArrayType::get(word,fields.size());
      auto *slots=new GlobalVariable(m,array,true,GlobalValue::PrivateLinkage,
          ConstantArray::get(array,fields),"fftw_reloc_initializer_slots");
      values[p.second-1]=ConstantStruct::get(entry,
          ConstantExpr::getBitCast(p.first,byteptr),
          ConstantInt::get(word,m.getDataLayout().getTypeAllocSize(p.first->getValueType())),
          ConstantExpr::getBitCast(slots,PointerType::getUnqual(word)),
          ConstantInt::get(word,fields.size()));
      p.first->setConstant(true);
    }
    auto *array=ArrayType::get(entry,values.size());
    new GlobalVariable(m,array,true,GlobalValue::ExternalLinkage,
        ConstantArray::get(array,values),"fftw_reloc_initializers");
    new GlobalVariable(m,word,true,GlobalValue::ExternalLinkage,
        ConstantInt::get(word,values.size()),"fftw_reloc_initializer_count");
  }

  bool has_pointer(Type *type) {
    if (type->isPointerTy()) return true;
    if (auto *s=dyn_cast<StructType>(type)) {
      for (Type *field:s->elements()) if (has_pointer(field)) return true;
    } else if (auto *a=dyn_cast<ArrayType>(type)) return has_pointer(a->getElementType());
    else if (auto *v=dyn_cast<VectorType>(type)) return has_pointer(v->getElementType());
    return false;
  }

  Translation(Module &module, uint64_t simd_alignment)
      : m(module), c(m.getContext()), alignment_bytes(simd_alignment) {
    byteptr = Type::getInt8PtrTy(c);
    word = Type::getInt64Ty(c);
    auto *unit = Type::getVoidTy(c);
    load = m.getOrInsertFunction("fftw_reloc_load", byteptr, byteptr, byteptr);
    store = m.getOrInsertFunction("fftw_reloc_store", byteptr, byteptr, byteptr, byteptr);
    difference = m.getOrInsertFunction("fftw_reloc_difference",word,byteptr,byteptr,byteptr);
    alignment = m.getOrInsertFunction("fftw_reloc_alignment",word,byteptr,byteptr,word);
    allocate = m.getOrInsertFunction("fftw_reloc_alloc", byteptr, byteptr, word);
    release = m.getOrInsertFunction("fftw_reloc_free", unit, byteptr, byteptr);
    global = m.getOrInsertFunction("fftw_reloc_global", byteptr, byteptr, word, word, byteptr);
    copy = m.getOrInsertFunction("fftw_reloc_copy", unit, byteptr, byteptr, byteptr, word, Type::getInt32Ty(c));
    set = m.getOrInsertFunction("fftw_reloc_set", unit, byteptr, byteptr, Type::getInt32Ty(c), word);
    sort = m.getOrInsertFunction("fftw_reloc_qsort", unit, byteptr, byteptr, word, word, byteptr);
  }

  FunctionType *context_type(FunctionType *type) {
    std::vector<Type *> arguments{byteptr};
    for (Type *t : type->params()) arguments.push_back(t);
    return FunctionType::get(type->getReturnType(), arguments, type->isVarArg());
  }

  Value *pointer(IRBuilder<> &b, Value *value) {
    return b.CreateBitCast(value, byteptr);
  }

  Value *alignment_expression(IRBuilder<> &b,Value *value,Value *context,
                               bool &changed) {
    if(auto *address=dyn_cast<PtrToIntInst>(value)) {
      changed=true;
      return b.CreateCall(alignment,{context,pointer(b,address->getPointerOperand()),
                                     b.getInt64(alignment_bytes-1)});
    }
    if(auto *op=dyn_cast<BinaryOperator>(value)) {
      unsigned code=op->getOpcode();
      if(code==Instruction::And || code==Instruction::Or ||
         code==Instruction::Xor || code==Instruction::Add || code==Instruction::Sub) {
        Value *a=alignment_expression(b,op->getOperand(0),context,changed);
        Value *d=alignment_expression(b,op->getOperand(1),context,changed);
        if(a!=op->getOperand(0) || d!=op->getOperand(1))
          return b.CreateBinOp(static_cast<Instruction::BinaryOps>(code),a,d);
      }
    }
    return value;
  }

  Value *materialize(Value *value, Instruction *before, Value *context) {
    IRBuilder<> b(before);
    if (auto *g = dyn_cast<GlobalVariable>(value)) {
      auto it = globals.find(g);
      if (it == globals.end()) return g;
      uint64_t size = m.getDataLayout().getTypeAllocSize(g->getValueType());
      Value *p = b.CreateCall(global, {context, b.getInt64(it->second),
                                      b.getInt64(size), pointer(b, g)});
      return b.CreateBitCast(p, g->getType());
    }
    if (auto *expression = dyn_cast<ConstantExpr>(value)) {
      Instruction *instruction = expression->getAsInstruction();
      instruction->insertBefore(before);
      for (unsigned i=0; i<instruction->getNumOperands(); ++i)
        instruction->setOperand(i, materialize(instruction->getOperand(i), instruction, context));
      return instruction;
    }
    return value;
  }

  void clone() {
    std::vector<Function *> original;
    for (Function &f : m) if (!f.isDeclaration()) original.push_back(&f);
    for (Function *f : original) {
      auto *n = Function::Create(context_type(f->getFunctionType()),
                                  f->getLinkage(), f->getName()+".context", m);
      n->setCallingConv(f->getCallingConv());
      functions.emplace(f,n);
    }
    ValueToValueMapTy map;
    for (auto entry : functions)
      map[entry.first] = ConstantExpr::getBitCast(entry.second, entry.first->getType());
    for (auto entry : functions) {
      Function *old = entry.first, *fresh = entry.second;
      ValueToValueMapTy local;
      for (auto mapping : map) local[mapping.first] = mapping.second;
      auto arg = fresh->arg_begin();
      arg->setName("reloc_context");
      ++arg;
      for (Argument &oldarg : old->args()) {
        arg->setName(oldarg.getName());
        local[&oldarg] = &*arg++;
      }
      SmallVector<ReturnInst *,8> returns;
      CloneFunctionInto(fresh, old, local, CloneFunctionChangeType::LocalChangesOnly, returns);
      fresh->setAttributes(AttributeList());
      for (StringRef key : {"target-cpu", "target-features", "tune-cpu"})
        if (old->hasFnAttribute(key)) fresh->addFnAttr(old->getFnAttribute(key));
    }
    for (GlobalVariable &g : m.globals())
      if (g.hasInitializer()) g.setInitializer(MapValue(g.getInitializer(), map));
    for (auto entry : functions) {
      entry.first->replaceAllUsesWith(ConstantExpr::getBitCast(entry.second,entry.first->getType()));
    }
    for (auto entry : functions) entry.first->deleteBody();
  }

  void rewrite(Function &f) {
    Value *context = &*f.arg_begin();
    std::set<LoadInst *> offset_loads;
    std::vector<BinaryOperator *> differences;
    for (Instruction &i:llvm::instructions(f)) {
      auto *sub=dyn_cast<BinaryOperator>(&i);
      if (!sub || sub->getOpcode()!=Instruction::Sub || sub->getType()!=word) continue;
      auto *left=dyn_cast<PtrToIntInst>(sub->getOperand(0));
      auto *right=dyn_cast<PtrToIntInst>(sub->getOperand(1));
      if (!left || !right) continue;
      auto *l=dyn_cast<LoadInst>(left->getPointerOperand());
      auto *r=dyn_cast<LoadInst>(right->getPointerOperand());
      if (l && r && l->hasOneUse() && r->hasOneUse()) differences.push_back(sub);
    }
    for (BinaryOperator *sub:differences) {
      auto *left=cast<PtrToIntInst>(sub->getOperand(0));
      auto *right=cast<PtrToIntInst>(sub->getOperand(1));
      auto *l=cast<LoadInst>(left->getPointerOperand());
      auto *r=cast<LoadInst>(right->getPointerOperand());
      offset_loads.insert(l);offset_loads.insert(r);
      IRBuilder<> b(sub);
      Value *result=b.CreateCall(difference,{context,pointer(b,l),pointer(b,r)});
      sub->replaceAllUsesWith(result);sub->eraseFromParent();
      if(left->use_empty())left->eraseFromParent();
      if(right->use_empty())right->eraseFromParent();
    }
    std::vector<Instruction *> instructions;
    for (Instruction &i : llvm::instructions(f)) instructions.push_back(&i);
    for (Instruction *i : instructions) {
      for (unsigned operand=0; operand<i->getNumOperands(); ++operand) {
        Value *v = i->getOperand(operand);
        if (isa<BasicBlock>(v) || isa<Function>(v)) continue;
        if (isa<PHINode>(i)) {
          auto *phi = cast<PHINode>(i);
          if (operand >= phi->getNumIncomingValues()) continue;
          Instruction *at = phi->getIncomingBlock(operand)->getTerminator();
          i->setOperand(operand, materialize(v, at, context));
        } else {
          i->setOperand(operand, materialize(v, i, context));
        }
      }
      if (auto *l = dyn_cast<LoadInst>(i)) {
        l->setAlignment(Align(std::min<uint64_t>(8,l->getAlign().value())));
        if (!l->getType()->isPointerTy() && has_pointer(l->getType()))
          report_fatal_error("aggregate pointer load requires scalarization");
        if (!l->getType()->isPointerTy()) continue;
        if (offset_loads.count(l)) continue;
        IRBuilder<> b(l->getNextNode());
        Value *argument = pointer(b,l);
        CallInst *call = b.CreateCall(load,{context,argument});
        Value *decoded = b.CreateBitCast(call,l->getType());
        SmallVector<Use *,16> uses;
        for (Use &u : l->uses()) if (u.getUser()!=argument && u.getUser()!=call) uses.push_back(&u);
        for (Use *u : uses) u->set(decoded);
      } else if (auto *s = dyn_cast<StoreInst>(i)) {
        s->setAlignment(Align(std::min<uint64_t>(8,s->getAlign().value())));
        Value *v = s->getValueOperand();
        if (!v->getType()->isPointerTy() && has_pointer(v->getType()))
          report_fatal_error("aggregate pointer store requires scalarization");
        if (!v->getType()->isPointerTy()) continue;
        IRBuilder<> b(s);
        Value *encoded = b.CreateCall(store,{context,pointer(b,s->getPointerOperand()),pointer(b,v)});
        s->setOperand(0,b.CreateBitCast(encoded,v->getType()));
      } else if (auto *call = dyn_cast<CallInst>(i)) {
        IRBuilder<> b(call);
        if (call->isInlineAsm()) {
          auto *assembly=cast<InlineAsm>(call->getCalledOperand());
          std::string text=assembly->getAsmString();
          for (auto pair : {std::make_pair("movapd","movupd"),
                            std::make_pair("movaps","movups"),
                            std::make_pair("movdqa","movdqu")}) {
            size_t at=0;
            while((at=text.find(pair.first,at))!=std::string::npos) {
              text.replace(at,strlen(pair.first),pair.second);at+=strlen(pair.second);
            }
          }
          call->setCalledOperand(InlineAsm::get(assembly->getFunctionType(),text,
              assembly->getConstraintString(),assembly->hasSideEffects(),
              assembly->isAlignStack(),assembly->getDialect(),assembly->canThrow()));
          continue;
        }
        if (auto *memory = dyn_cast<MemTransferInst>(call)) {
          b.CreateCall(copy,{context,pointer(b,memory->getDest()),pointer(b,memory->getSource()),
                             b.CreateZExtOrTrunc(memory->getLength(),word),b.getInt32(isa<MemMoveInst>(memory))});
          call->eraseFromParent();
          continue;
        }
        if (auto *memory = dyn_cast<MemSetInst>(call)) {
          b.CreateCall(set,{context,pointer(b,memory->getDest()),
                            b.CreateZExt(memory->getValue(),Type::getInt32Ty(c)),
                            b.CreateZExtOrTrunc(memory->getLength(),word)});
          call->eraseFromParent();
          continue;
        }
        Value *target = call->getCalledOperand()->stripPointerCasts();
        auto *callee = dyn_cast<Function>(target);
        if (callee && callee->getName()=="qsort") {
          b.CreateCall(sort,{context,call->getArgOperand(0),call->getArgOperand(1),
                             call->getArgOperand(2),pointer(b,call->getArgOperand(3))});
          call->eraseFromParent();
          continue;
        }
        bool internal = !callee || callee->getName().endswith(".context");
        if (!internal) continue;
        FunctionType *type = context_type(call->getFunctionType());
        Value *converted = b.CreateBitCast(call->getCalledOperand(),PointerType::getUnqual(type));
        std::vector<Value *> arguments{context};
        for (Use &arg : call->args()) arguments.push_back(arg.get());
        CallInst *replacement = b.CreateCall(type,converted,arguments);
        replacement->setCallingConv(call->getCallingConv());
        call->replaceAllUsesWith(replacement);
        call->eraseFromParent();
      } else if (auto *bits=dyn_cast<BinaryOperator>(i)) {
        if(bits->getOpcode()!=Instruction::And || bits->getType()!=word) continue;
        auto *mask=dyn_cast<ConstantInt>(bits->getOperand(1));
        if(!mask || mask->getZExtValue()>alignment_bytes-1) continue;
        IRBuilder<> b(bits);
        bool changed=false;
        Value *value=alignment_expression(b,bits->getOperand(0),context,changed);
        if(changed) {
          bits->replaceAllUsesWith(b.CreateAnd(value,mask));bits->eraseFromParent();
        }
      }
    }
    if (f.getName()=="fftw_kernel_malloc.context") {
      f.deleteBody();
      BasicBlock *block=BasicBlock::Create(c,"entry",&f);
      IRBuilder<> b(block);
      auto it=f.arg_begin(); Value *ctx=&*it++; Value *size=&*it;
      b.CreateRet(b.CreateCall(allocate,{ctx,b.CreateZExtOrTrunc(size,word)}));
    } else if (f.getName()=="fftw_kernel_free.context") {
      f.deleteBody();
      BasicBlock *block=BasicBlock::Create(c,"entry",&f);
      IRBuilder<> b(block);
      auto it=f.arg_begin(); Value *ctx=&*it++; Value *p=&*it;
      b.CreateCall(release,{ctx,pointer(b,p)});
      b.CreateRetVoid();
    }
  }

  void symbols() {
    StructType *entry = StructType::get(byteptr,word,byteptr);
    std::vector<GlobalValue *> symbols;
    for (auto p : functions) symbols.push_back(p.second);
    for (GlobalVariable &g : m.globals())
      if (g.hasInitializer() && g.isConstant() &&
           !g.getName().startswith("llvm.")) symbols.push_back(&g);
    std::sort(symbols.begin(),symbols.end(),[](GlobalValue *a,GlobalValue *b) {
      return a->getName()<b->getName();
    });
    std::vector<Constant *> values;
    for (GlobalValue *s : symbols) {
      uint64_t size=0;
      if (auto *g=dyn_cast<GlobalVariable>(s)) size=m.getDataLayout().getTypeAllocSize(g->getValueType());
      Constant *label=ConstantDataArray::getString(c,s->getName(),true);
      auto *name=new GlobalVariable(m,label->getType(),true,GlobalValue::PrivateLinkage,label,"reloc_symbol_name");
      values.push_back(ConstantStruct::get(entry,ConstantExpr::getBitCast(s,byteptr),
                                           ConstantInt::get(word,size),ConstantExpr::getBitCast(name,byteptr)));
    }
    auto *type=ArrayType::get(entry,values.size());
    new GlobalVariable(m,type,true,GlobalValue::ExternalLinkage,
                        ConstantArray::get(type,values),"fftw_reloc_symbols");
    new GlobalVariable(m,word,true,GlobalValue::ExternalLinkage,
                        ConstantInt::get(word,values.size()),"fftw_reloc_symbol_count");
  }

  void run() {
    uint64_t id=1;
    for (GlobalVariable &g : m.globals())
      if (g.hasInitializer() && !g.isConstant() && !g.getName().startswith("llvm."))
        globals.emplace(&g,id++);
    clone();
    for (auto pair : functions) rewrite(*pair.second);
    symbols();
    initializers();
    for (auto pair : functions) {
      std::string name=pair.first->getName().str();
      pair.first->eraseFromParent();
      pair.second->setName("fftw_reloc_"+name);
    }
    for (GlobalVariable &g : m.globals())
      if (g.hasInitializer() && !g.getName().startswith("fftw_reloc_") &&
          !g.getName().startswith("llvm.")) g.setName("fftw_reloc_"+g.getName());
    errs()<<"context functions="<<functions.size()<<" mutable globals="<<globals.size()
          <<" simd alignment="<<alignment_bytes<<"\n";
  }
};

int main(int argc,char **argv) {
  if (argc!=4) {
    errs()<<"usage: fftw-reloc-translate input.bc output.bc simd-alignment-bytes\n";
    return 2;
  }
  /* The alignment comes from the library's own configuration.  The pass can
   * only normalise alignment predicates whose masks it recognises, so a set
   * whose alignment is not a power of two at or above the pointer-difference
   * unit is refused rather than silently left denormalised. */
  const uint64_t simd_alignment=strtoull(argv[3],nullptr,10);
  if (simd_alignment<16 || (simd_alignment&(simd_alignment-1))) {
    errs()<<"fftw-reloc-translate: unsupported SIMD alignment "<<simd_alignment<<"\n";
    return 2;
  }
  LLVMContext context;
  SMDiagnostic diagnostic;
  std::unique_ptr<Module> module=parseIRFile(argv[1],diagnostic,context);
  if (!module) { diagnostic.print(argv[0],errs()); return 1; }
  std::error_code schema_error;
  raw_fd_ostream schema(std::string(argv[2])+".schema",schema_error,sys::fs::OF_None);
  if(schema_error) return 1;
  Translation translation(*module,simd_alignment);
  translation.schema(schema);
  auto input=MemoryBuffer::getFile(argv[1]);
  if(!input)return 1;
  uint64_t fingerprint=UINT64_C(14695981039346656037);
  for(unsigned char byte:(*input)->getBuffer())fingerprint=(fingerprint^byte)*UINT64_C(1099511628211);
  new GlobalVariable(*module,Type::getInt64Ty(context),true,GlobalValue::ExternalLinkage,
      ConstantInt::get(Type::getInt64Ty(context),fingerprint),"fftw_reloc_fingerprint");
  translation.run();
  /* The front end keeps debug metadata so the pass sees the source's typed
   * operations; the result is codegen input whose committed form is assembly,
   * and debug lines there would be most of it. */
  StripDebugInfo(*module);
  if (verifyModule(*module,&errs())) return 1;
  std::error_code error;
  raw_fd_ostream output(argv[2],error,sys::fs::OF_None);
  if (error) { errs()<<error.message()<<"\n"; return 1; }
  WriteBitcodeToFile(*module,output);
}
