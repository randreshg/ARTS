#include "reloc.h"

#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <set>
#include <vector>
#ifdef FFTW_RELOC_PROTECT
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace {
constexpr uint64_t object_tag = UINT64_C(0xe000000000000000);
constexpr uint64_t symbol_tag = UINT64_C(0xd000000000000000);
constexpr uint64_t tag_mask = UINT64_C(0xf000000000000000);
constexpr uint64_t offset_mask = (UINT64_C(1) << 40) - 1;
constexpr uint64_t max_identity = (UINT64_C(1) << 20) - 1;

struct object {
  void *base = nullptr;
  uint64_t size = 0;
  bool owned = false;
  bool retained = false;
  bool alive = false;
  bool snapshot = false;
  uint64_t borrowed_key = 0;
  std::set<uint64_t> pointer_slots;
};

/* What one invocation binds for itself against a published image: the array
 * it lends the transform, and whatever scratch the transform allocates. */
struct binding {
  void *base = nullptr;
  uint64_t size = 0;
  uint64_t identity = 0;
  bool owned = false;
};

/* The published form.  Entries are keyed by identity, so identity resolution
 * is an index; locators are ordered by image offset, so finding the object
 * that contains an address is a search over the image's own array.  Neither
 * needs a structure built per invocation. */
struct image_header {
  uint64_t magic, size, entries, locators, globals, allocations, frees,
      fingerprint;
};
struct image_entry { uint64_t size, data, slots, slot_count, borrowed_key; };
struct image_locator { uint64_t data, size, identity; };
constexpr uint64_t image_magic = UINT64_C(0x4654465752454c32);

uint64_t align64(uint64_t size) { return (size + 63) & ~UINT64_C(63); }

uint64_t manifest_bytes(uint64_t entries, uint64_t locators, uint64_t globals) {
  return sizeof(image_header) + entries * sizeof(image_entry) +
         locators * sizeof(image_locator) + globals * sizeof(uint64_t);
}

[[noreturn]] void fail(const char *message) {
  std::fprintf(stderr, "FFTW-RELOC: %s\n", message);
  std::abort();
}

/* Where an identity currently lives, and whether it is storage this context
 * imported rather than created. */
struct placement {
  void *base = nullptr;
  uint64_t size = 0;
  bool retained = false;
};
}

struct fftw_reloc_context {
  /* Objects this context owns or borrows, with an address index over them.
   * All three stay empty while an image is attached for execution, so
   * attaching one costs no allocation at all.  Identity 0 is the null
   * reference, so the vector's index 0 is never an object. */
  std::vector<object> objects;
  std::map<uintptr_t, uint64_t> bases;
  std::map<uint64_t, uint64_t> globals;
  /* An attached image, read in place. */
  const char *image = nullptr;
  uint64_t image_size = 0;
  const image_entry *entries = nullptr;
  uint64_t entry_count = 0;
  const image_locator *locators = nullptr;
  uint64_t locator_count = 0;
  const uint64_t *global_ids = nullptr;
  uint64_t global_count = 0;
  uint64_t next_identity = 0;
  binding borrowed;
  std::vector<binding> scratch;
  uint64_t allocations = 0;
  uint64_t frees = 0;
  uint64_t pointer_stores = 0;
  uint64_t pointer_loads = 0;
  uint64_t sort_calls = 0;
  uint64_t sort_callbacks = 0;
  bool readonly = false;

  ~fftw_reloc_context() {
    for (auto &o : objects)
      if (o.alive && o.owned) std::free(o.base);
    for (auto &b : scratch)
      if (b.owned) std::free(b.base);
  }
};

static placement describe(const fftw_reloc_context *c, uint64_t id) {
  placement p;
  if (!id) return p;
  if (c->image) {
    if (id < c->entry_count) {
      const image_entry &e = c->entries[id];
      if (e.data) {
        if (e.data > c->image_size || e.size > c->image_size - e.data)
          fail("retained object outside the image");
        p.base = const_cast<char *>(c->image) + e.data;
        p.size = e.size;
        p.retained = true;
        return p;
      }
    }
    if (c->borrowed.base && c->borrowed.identity == id) {
      p.base = c->borrowed.base;
      p.size = c->borrowed.size;
      return p;
    }
    for (const binding &b : c->scratch)
      if (b.identity == id) {
        p.base = b.base;
        p.size = b.size;
        return p;
      }
    return p;
  }
  if (id < c->objects.size() && c->objects[id].alive) {
    const object &o = c->objects[id];
    p.base = o.base;
    p.size = o.size;
    p.retained = o.snapshot;
  }
  return p;
}

static bool inside(uintptr_t address, const void *base, uint64_t size,
                   bool one_past) {
  const uint64_t offset = address - reinterpret_cast<uintptr_t>(base);
  return offset < size || (one_past && offset == size);
}

static uint64_t locate(const fftw_reloc_context *c, const void *p,
                       bool one_past = false) {
  const auto address = reinterpret_cast<uintptr_t>(p);
  if (c->image) {
    const uint64_t relative = address - reinterpret_cast<uintptr_t>(c->image);
    if (relative < c->image_size) {
      uint64_t low = 0, high = c->locator_count;
      while (low < high) {
        const uint64_t middle = low + (high - low) / 2;
        if (c->locators[middle].data <= relative) low = middle + 1;
        else high = middle;
      }
      if (!low) return 0;
      const image_locator &l = c->locators[low - 1];
      const uint64_t offset = relative - l.data;
      return (offset < l.size || (one_past && offset == l.size)) ? l.identity : 0;
    }
    if (c->borrowed.base &&
        inside(address, c->borrowed.base, c->borrowed.size, one_past))
      return c->borrowed.identity;
    for (const binding &b : c->scratch)
      if (inside(address, b.base, b.size, one_past)) return b.identity;
    return 0;
  }
  auto it = c->bases.upper_bound(address);
  if (it == c->bases.begin()) return 0;
  --it;
  const object &o = c->objects[it->second];
  const uint64_t offset = address - it->first;
  return (offset < o.size || (one_past && offset == o.size)) ? it->second : 0;
}

static uint64_t encode(fftw_reloc_context *c, const void *pointer,
                        bool strict) {
  if (!pointer) return 0;
  uint64_t address = reinterpret_cast<uintptr_t>(pointer);
  if ((address & tag_mask) == object_tag ||
      (address & tag_mask) == symbol_tag) return address;
  uint64_t id = locate(c, pointer, true);
  if (id) {
    uint64_t offset = address - reinterpret_cast<uintptr_t>(describe(c, id).base);
    if (offset > offset_mask) fail("object offset exceeds representation");
    return object_tag | (id << 40) | offset;
  }
  for(uint64_t i=0;i<fftw_reloc_initializer_count;++i) {
    const auto &initial=fftw_reloc_initializers[i];
    uint64_t base=reinterpret_cast<uintptr_t>(initial.address);
    if(address>=base && address-base<initial.size) {
      auto *p=static_cast<char *>(fftw_reloc_global(c,i+1,initial.size,initial.address));
      return encode(c,p+(address-base),strict);
    }
  }
  for (uint64_t i = 0; i < fftw_reloc_symbol_count; ++i) {
    const auto &symbol = fftw_reloc_symbols[i];
    uint64_t base = reinterpret_cast<uintptr_t>(symbol.address);
    if (address == base || (symbol.size && address > base &&
                            address - base < symbol.size)) {
      if (i + 1 > max_identity) fail("symbol identity exceeds representation");
      return symbol_tag | ((i + 1) << 40) | (address - base);
    }
  }
  if (strict) fail("retained pointer has no object or compiled symbol identity");
  return address;
}

extern "C" fftw_reloc_context *fftw_reloc_context_create(void) {
  return new fftw_reloc_context;
}

extern "C" void fftw_reloc_context_delete(fftw_reloc_context *c) {
  delete c;
}

extern "C" void fftw_reloc_bind(fftw_reloc_context *c, uint64_t id,
                                 void *base, uint64_t size) {
  if (!id || id > max_identity || !base || size > offset_mask)
    fail("invalid acquisition binding");
  if (id >= c->objects.size()) c->objects.resize(id + 1);
  object &o = c->objects[id];
  if (o.alive) fail("identity already bound");
  o.base = base;
  o.size = size;
  o.alive = true;
  c->bases.emplace(reinterpret_cast<uintptr_t>(base), id);
}

extern "C" void fftw_reloc_bind_borrowed(fftw_reloc_context *c,uint64_t id,
                                          void *base,uint64_t size,uint64_t key) {
  fftw_reloc_bind(c,id,base,size);
  c->objects[id].borrowed_key=key;
}

extern "C" void *fftw_reloc_alloc(void *context, uint64_t size) {
  auto *c = static_cast<fftw_reloc_context *>(context);
  if (!size) size = 1;
  if (size > offset_mask) fail("invalid acquisition binding");
  void *p = nullptr;
  if (posix_memalign(&p, 64, size)) fail("allocation failed");
  if (c->image) {
    if (c->next_identity > max_identity) fail("symbol identity exceeds representation");
    c->scratch.push_back(binding{p, size, c->next_identity++, true});
  } else {
    uint64_t id = c->objects.empty() ? 1 : c->objects.size();
    fftw_reloc_bind(c, id, p, size);
    c->objects[id].owned = true;
    c->objects[id].retained = !c->readonly;
  }
#ifdef FFTW_RELOC_AUDIT
  ++c->allocations;
#endif
  return p;
}

extern "C" void fftw_reloc_free(void *context, void *pointer) {
  if (!pointer) return;
  auto *c = static_cast<fftw_reloc_context *>(context);
  uint64_t id = locate(c, pointer);
  placement p = describe(c, id);
  if (!id || p.base != pointer) fail("free is not an allocation base");
  if (c->readonly && p.retained) fail("execution cannot free a retained object");
  if (c->image) {
    if (c->borrowed.base == pointer) fail("execution cannot free a lent array");
    for (auto at = c->scratch.begin(); at != c->scratch.end(); ++at)
      if (at->base == pointer) {
        if (at->owned) std::free(pointer);
        c->scratch.erase(at);
#ifdef FFTW_RELOC_AUDIT
        ++c->frees;
#endif
        return;
      }
    fail("free is not an allocation base");
  }
  object &o = c->objects[id];
  c->bases.erase(reinterpret_cast<uintptr_t>(pointer));
  if (o.owned) std::free(pointer);
  o.alive = false;
  o.pointer_slots.clear();
#ifdef FFTW_RELOC_AUDIT
  ++c->frees;
#endif
}

extern "C" uint64_t fftw_reloc_reference(fftw_reloc_context *c, const void *p) {
  return encode(c, p, true);
}

extern "C" void *fftw_reloc_resolve(fftw_reloc_context *c, uint64_t value) {
  uint64_t tag = value & tag_mask;
  uint64_t id = (value & ~tag_mask) >> 40;
  uint64_t offset = value & offset_mask;
  if (tag == object_tag) {
    placement p = describe(c, id);
    if (!p.base) fail("dereference of an unacquired or freed object");
    if (offset > p.size) fail("object reference outside its extent");
    return static_cast<char *>(p.base) + offset;
  }
  if (tag == symbol_tag) {
    if (!id || id > fftw_reloc_symbol_count) fail("invalid compiled symbol identity");
    return const_cast<char *>(static_cast<const char *>(fftw_reloc_symbols[id-1].address)) + offset;
  }
  return reinterpret_cast<void *>(static_cast<uintptr_t>(value));
}

extern "C" void *fftw_reloc_load(void *context, void *pointer) {
  auto *c = static_cast<fftw_reloc_context *>(context);
#ifdef FFTW_RELOC_AUDIT
  ++c->pointer_loads;
#endif
  uint64_t address=reinterpret_cast<uintptr_t>(pointer);
  if(address && (address&tag_mask)!=object_tag && (address&tag_mask)!=symbol_tag) {
    for(uint64_t i=0;i<fftw_reloc_initializer_count;++i) {
      const auto &initial=fftw_reloc_initializers[i];
      uint64_t base=reinterpret_cast<uintptr_t>(initial.address);
      if(address>=base && address-base<initial.size)
        return static_cast<char *>(fftw_reloc_global(c,i+1,initial.size,initial.address))+(address-base);
    }
  }
  return fftw_reloc_resolve(c, reinterpret_cast<uintptr_t>(pointer));
}

extern "C" int64_t fftw_reloc_difference(void *context,void *left,void *right) {
  uint64_t l=reinterpret_cast<uintptr_t>(left),r=reinterpret_cast<uintptr_t>(right);
  if ((l&tag_mask)==object_tag || (r&tag_mask)==object_tag) {
    if ((l&~offset_mask)==(r&~offset_mask))
      return static_cast<int64_t>(l&offset_mask)-static_cast<int64_t>(r&offset_mask);
    auto *c=static_cast<fftw_reloc_context *>(context);
    l=reinterpret_cast<uintptr_t>(fftw_reloc_resolve(c,l));
    r=reinterpret_cast<uintptr_t>(fftw_reloc_resolve(c,r));
  }
  return static_cast<int64_t>(l-r);
}

extern "C" uint64_t fftw_reloc_alignment(void *context,void *pointer,uint64_t mask) {
  auto *c=static_cast<fftw_reloc_context *>(context);
  uint64_t id=locate(c,pointer,true);
  uint64_t address=reinterpret_cast<uintptr_t>(pointer);
  if(id) address-=reinterpret_cast<uintptr_t>(describe(c,id).base);
  return address&mask;
}

extern "C" void *fftw_reloc_store(void *context, void *slot, void *pointer) {
  auto *c = static_cast<fftw_reloc_context *>(context);
  uint64_t id = locate(c, slot);
  if (!id) return pointer;
  placement p = describe(c, id);
  if(c->readonly && p.retained) fail("execution cannot modify a retained pointer");
  uint64_t offset = reinterpret_cast<uintptr_t>(slot) - reinterpret_cast<uintptr_t>(p.base);
  if (offset + sizeof(void *) > p.size) fail("pointer slot exceeds allocation");
  /* The slot index is metadata for validation and publication; a context that
   * publishes nothing does not carry it. */
  if (!c->image) c->objects[id].pointer_slots.insert(offset);
#ifdef FFTW_RELOC_AUDIT
  ++c->pointer_stores;
#endif
  return reinterpret_cast<void *>(static_cast<uintptr_t>(encode(c, pointer, false)));
}

extern "C" void *fftw_reloc_global(void *context, uint64_t index,
                                    uint64_t size, const void *initial) {
  auto *c = static_cast<fftw_reloc_context *>(context);
  if (c->image) {
    if (!index || index >= c->global_count || !c->global_ids[index])
      fail("execution cannot create a persistent global");
    placement p = describe(c, c->global_ids[index]);
    if (!p.base || p.size != size) fail("global schema size mismatch");
    return p.base;
  }
  auto found = c->globals.find(index);
  if (found != c->globals.end()) return c->objects[found->second].base;
  if(c->readonly) fail("execution cannot create a persistent global");
  if(!index || index>fftw_reloc_initializer_count) fail("invalid global identity");
  void *p = fftw_reloc_alloc(c, size);
  std::memcpy(p, initial, size);
  c->globals.emplace(index, locate(c, p));
  const auto &description=fftw_reloc_initializers[index-1];
  if(description.size!=size) fail("global schema size mismatch");
  for(uint64_t i=0;i<description.slot_count;++i) {
    auto *slot=static_cast<char *>(p)+description.slots[i];
    void *value;
    std::memcpy(&value,slot,sizeof(value));
    value=fftw_reloc_store(c,slot,value);
    std::memcpy(slot,&value,sizeof(value));
  }
  return p;
}

static void forget_slots(object &o, uint64_t begin, uint64_t size) {
  auto at = o.pointer_slots.lower_bound(begin >= sizeof(void *)-1 ?
                                        begin-(sizeof(void *)-1) : 0);
  while (at != o.pointer_slots.end() && *at < begin+size)
    at = o.pointer_slots.erase(at);
}

extern "C" void fftw_reloc_copy(void *context, void *destination,
                                 const void *source, uint64_t size, int move) {
  auto *c = static_cast<fftw_reloc_context *>(context);
  uint64_t sid = locate(c, source), did = locate(c, destination);
  if(did && c->readonly && describe(c,did).retained)
    fail("execution cannot copy into retained storage");
  std::vector<uint64_t> slots;
  if (sid && did && !c->image) {
    const object &s = c->objects[sid];
    uint64_t start = reinterpret_cast<uintptr_t>(source) - reinterpret_cast<uintptr_t>(s.base);
    auto at = s.pointer_slots.lower_bound(start);
    while (at != s.pointer_slots.end() && *at + sizeof(void *) <= start+size)
      slots.push_back(*at++ - start);
  }
  if (move) std::memmove(destination, source, size);
  else std::memcpy(destination, source, size);
  if (did && !c->image) {
    object &d = c->objects[did];
    uint64_t start = reinterpret_cast<uintptr_t>(destination) - reinterpret_cast<uintptr_t>(d.base);
    forget_slots(d, start, size);
    for (auto offset : slots) d.pointer_slots.insert(start + offset);
  }
}

extern "C" void fftw_reloc_set(void *context, void *destination,
                                int value, uint64_t size) {
  auto *c = static_cast<fftw_reloc_context *>(context);
  uint64_t id = locate(c, destination);
  if (id) {
    if(c->readonly && describe(c,id).retained)
      fail("execution cannot clear retained storage");
    if (!c->image) {
      object &o = c->objects[id];
      uint64_t start = reinterpret_cast<uintptr_t>(destination) - reinterpret_cast<uintptr_t>(o.base);
      forget_slots(o, start, size);
    }
  }
  std::memset(destination, value, size);
}

struct sort_context { void *context; int (*compare)(void *,const void *,const void *); };
static int compare_adapter(const void *a, const void *b, void *opaque) {
  auto *s = static_cast<sort_context *>(opaque);
#ifdef FFTW_RELOC_AUDIT
  ++static_cast<fftw_reloc_context *>(s->context)->sort_callbacks;
#endif
  return s->compare(s->context, a, b);
}
extern "C" void fftw_reloc_qsort(void *context, void *base, uint64_t count,
                                  uint64_t size, void *compare) {
  sort_context s{context, reinterpret_cast<int (*)(void *,const void *,const void *)>(compare)};
#ifdef FFTW_RELOC_AUDIT
  ++static_cast<fftw_reloc_context *>(context)->sort_calls;
#endif
  qsort_r(base, count, size, compare_adapter, &s);
}

extern "C" size_t fftw_reloc_object_count(const fftw_reloc_context *c) {
  return c->objects.size();
}
extern "C" fftw_reloc_object fftw_reloc_object_at(const fftw_reloc_context *c,
                                                  size_t index) {
  const auto &o = c->objects.at(index);
  return {index, o.alive ? o.size : 0, 64, o.alive ? o.base : nullptr};
}
extern "C" void fftw_reloc_rebase(fftw_reloc_context *c, uint64_t id, void *base) {
  object &o = c->objects.at(id);
  if (!o.alive) fail("rebase of dead allocation");
  c->bases.erase(reinterpret_cast<uintptr_t>(o.base));
  if (o.owned) { std::memset(o.base, 0xa5, o.size); std::free(o.base); }
  o.base = base;
  o.owned = true;
  c->bases.emplace(reinterpret_cast<uintptr_t>(base), id);
}
extern "C" int fftw_reloc_validate(const fftw_reloc_context *c) {
  int invalid = 0;
  for (size_t id = 1; id < c->objects.size(); ++id) {
    const object &o = c->objects[id];
    if (!o.alive) continue;
    for (auto offset : o.pointer_slots) {
      uint64_t value;
      std::memcpy(&value, static_cast<const char *>(o.base)+offset, sizeof value);
      uint64_t tag = value & tag_mask;
      if (value && tag != object_tag && tag != symbol_tag) {
        std::fprintf(stderr,"FFTW-RELOC: raw retained pointer object=%zu offset=%lu value=%lx\n",id,offset,value);
        ++invalid;
      } else if(tag==object_tag) {
        uint64_t target=(value&~tag_mask)>>40;
        if(!target || target>=c->objects.size() || !c->objects[target].alive ||
            (value&offset_mask)>c->objects[target].size+3) {
          std::fprintf(stderr,"FFTW-RELOC: dangling edge object=%zu offset=%lu target=%lu\n",id,offset,target);
          ++invalid;
        }
      }
    }
  }
  return invalid;
}
extern "C" void fftw_reloc_trace(const fftw_reloc_context *c) {
  uint64_t live = 0, bytes = 0, slots = 0;
  if (c->image) {
    for (uint64_t id = 1; id < c->entry_count; ++id) {
      const image_entry &e = c->entries[id];
      if (!e.size && !e.data) continue;
      ++live; bytes += e.size; slots += e.slot_count;
    }
    live += (c->borrowed.base ? 1 : 0) + c->scratch.size();
  } else {
    for (const object &o : c->objects) if (o.alive) {
      ++live; bytes += o.size; slots += o.pointer_slots.size();
    }
  }
  std::printf("FFTW-RELOC allocations=%lu frees=%lu live=%lu bytes=%lu pointer_slots=%lu stores=%lu loads=%lu qsort=%lu callbacks=%lu\n",
              c->allocations,c->frees,live,bytes,slots,c->pointer_stores,c->pointer_loads,c->sort_calls,c->sort_callbacks);
}

extern "C" void fftw_reloc_dump(const fftw_reloc_context *c,const char *path) {
  FILE *out=std::fopen(path,"w");
  if(!out)fail("cannot open closure trace");
  std::fprintf(out,"fingerprint\t%016lx\n",fftw_reloc_fingerprint);
  for(uint64_t id=1;id<c->objects.size();++id) {
    const object &o=c->objects[id];
    if(!o.alive)continue;
    std::fprintf(out,"object\t%lu\t%lu\t%s\t%lu\t%lu\n",id,o.size,
                 o.retained?"retained":"borrowed",o.borrowed_key,
                 reinterpret_cast<uintptr_t>(o.base)%64);
    for(auto offset:o.pointer_slots) {
      uint64_t value;std::memcpy(&value,static_cast<const char *>(o.base)+offset,sizeof value);
      uint64_t target=(value&~tag_mask)>>40;
      const char *name="";
      if((value&tag_mask)==symbol_tag && target && target<=fftw_reloc_symbol_count)
        name=fftw_reloc_symbols[target-1].name;
      std::fprintf(out,"field\t%lu\t%lu\t%016lx\t%s\n",id,offset,value,name);
    }
  }
  std::fclose(out);
}

/* Only a live identity can appear in a retained pointer, so the identity-keyed
 * array need not span the identities planning allocated and released. */
static uint64_t entry_span(const fftw_reloc_context *c) {
  uint64_t last = 0;
  for (uint64_t id = 1; id < c->objects.size(); ++id)
    if (c->objects[id].alive) last = id;
  return last + 1;
}

extern "C" size_t fftw_reloc_snapshot_size(const fftw_reloc_context *c) {
  uint64_t locators = 0, payload = 0;
  for (uint64_t id = 1; id < c->objects.size(); ++id) {
    const object &o = c->objects[id];
    if (!o.alive || !o.retained) continue;
    ++locators;
    payload += align64(o.size) + align64(o.pointer_slots.size()*sizeof(uint64_t));
  }
  return align64(manifest_bytes(entry_span(c), locators,
                                fftw_reloc_initializer_count + 1)) + payload;
}

extern "C" void fftw_reloc_snapshot_write(const fftw_reloc_context *c, void *image) {
  if (fftw_reloc_validate(c)) fail("cannot publish a closure with raw pointers");
  const uint64_t entries = entry_span(c);
  const uint64_t globals = fftw_reloc_initializer_count + 1;
  uint64_t locators = 0;
  for (uint64_t id = 1; id < entries; ++id)
    if (c->objects[id].alive && c->objects[id].retained) ++locators;
  auto *bytes = static_cast<char *>(image);
  auto *h = reinterpret_cast<image_header *>(bytes);
  *h = {image_magic, fftw_reloc_snapshot_size(c), entries, locators, globals,
        c->allocations, c->frees, fftw_reloc_fingerprint};
  auto *entry = reinterpret_cast<image_entry *>(bytes + sizeof(*h));
  auto *locator = reinterpret_cast<image_locator *>(entry + entries);
  auto *global = reinterpret_cast<uint64_t *>(locator + locators);
  std::memset(entry, 0, entries * sizeof(*entry));
  std::memset(global, 0, globals * sizeof(*global));
  for (auto g : c->globals) {
    if (!g.first || g.first >= globals) fail("invalid global identity");
    global[g.first] = g.second;
  }
  uint64_t cursor = align64(manifest_bytes(entries, locators, globals));
  uint64_t index = 0;
  for (uint64_t id = 1; id < entries; ++id) {
    const object &o = c->objects[id];
    if (!o.alive) continue;
    entry[id].size = o.size;
    entry[id].borrowed_key = o.borrowed_key;
    if (!o.retained) continue;
    entry[id].data = cursor;
    std::memcpy(bytes + cursor, o.base, o.size);
    cursor += align64(o.size);
    entry[id].slots = cursor;
    entry[id].slot_count = o.pointer_slots.size();
    auto *slots = reinterpret_cast<uint64_t *>(bytes + cursor);
    for (uint64_t slot : o.pointer_slots) *slots++ = slot;
    cursor += align64(o.pointer_slots.size() * sizeof(uint64_t));
    locator[index++] = {entry[id].data, o.size, id};
  }
  if (index != locators || cursor != h->size)
    fail("snapshot size accounting mismatch");
}

/* Read an image's manifest in place: four pointers into the image and no
 * per-object work.  A retained object's extent is checked where it is used,
 * so opening does not walk the manifest. */
static void attach(fftw_reloc_context *c, const void *image, size_t size) {
  if (size < sizeof(image_header)) fail("truncated snapshot");
  const auto *bytes = static_cast<const char *>(image);
  const auto *h = reinterpret_cast<const image_header *>(bytes);
  if (h->magic != image_magic || h->size != size ||
      h->fingerprint != fftw_reloc_fingerprint) fail("incompatible snapshot");
  if (manifest_bytes(h->entries, h->locators, h->globals) > size)
    fail("truncated snapshot manifest");
  c->image = bytes;
  c->image_size = size;
  c->entries = reinterpret_cast<const image_entry *>(bytes + sizeof(*h));
  c->entry_count = h->entries;
  c->locators = reinterpret_cast<const image_locator *>(c->entries + h->entries);
  c->locator_count = h->locators;
  c->global_ids = reinterpret_cast<const uint64_t *>(c->locators + h->locators);
  c->global_count = h->globals;
  c->next_identity = h->entries;
  c->allocations = h->allocations;
  c->frees = h->frees;
  c->readonly = true;
}

/* Lend the array an invocation acquired under the identity its plan was built
 * on.  The extent is the one the manifest recorded, so a row inside it is
 * addressed as that identity plus a byte offset. */
static void lend(fftw_reloc_context *c, uint64_t id, void *array) {
  if (!id || id >= c->entry_count) fail("no manifest identity for a lent array");
  const image_entry &e = c->entries[id];
  if (e.data || !e.size) fail("the lent identity does not name a borrowed array");
  if (!array) fail("invalid acquisition binding");
  c->borrowed = binding{array, e.size, id, false};
}

static double *lent_row(fftw_reloc_context *c, void *array, uint64_t offset) {
  if (offset % (2 * sizeof(double))) fail("a row must start on a complex element");
  if (offset >= c->borrowed.size) fail("row offset outside the lent array");
  return reinterpret_cast<double *>(static_cast<char *>(array) + offset);
}

extern "C" fftw_reloc_context *fftw_reloc_snapshot_open(void *image,size_t size) {
  if(size<sizeof(image_header)) fail("truncated snapshot");
  auto *bytes=static_cast<char *>(image);
  auto *h=reinterpret_cast<image_header *>(bytes);
  if(h->magic!=image_magic || h->size!=size || h->fingerprint!=fftw_reloc_fingerprint)
    fail("incompatible snapshot");
  if(manifest_bytes(h->entries,h->locators,h->globals)>size)
    fail("truncated snapshot manifest");
  auto *entry=reinterpret_cast<const image_entry *>(bytes+sizeof(*h));
  auto *locator=reinterpret_cast<const image_locator *>(entry+h->entries);
  auto *global=reinterpret_cast<const uint64_t *>(locator+h->locators);
  auto *c=fftw_reloc_context_create();
  if(h->entries>max_identity+1) fail("invalid manifest identity");
  c->objects.resize(h->entries ? h->entries : 1);
  for(uint64_t id=1;id<h->entries;++id) {
    const image_entry &e=entry[id];
    if(!e.size && !e.data) continue;
    if(!e.data) {
      c->objects[id].size=e.size;
      c->objects[id].borrowed_key=e.borrowed_key;
      continue;
    }
    if(e.data>size || e.size>size-e.data || e.slots>size ||
        e.slot_count*sizeof(uint64_t)>size-e.slots)
      fail("truncated retained object");
    fftw_reloc_bind(c,id,bytes+e.data,e.size);
    c->objects[id].retained=true;
    c->objects[id].snapshot=true;
    c->objects[id].borrowed_key=e.borrowed_key;
    auto *slots=reinterpret_cast<const uint64_t *>(bytes+e.slots);
    for(uint64_t j=0;j<e.slot_count;++j)
      c->objects[id].pointer_slots.insert(slots[j]);
  }
  for(uint64_t i=1;i<h->globals;++i)
    if(global[i]) c->globals.emplace(i,global[i]);
  c->allocations=h->allocations;
  c->frees=h->frees;
  return c;
}

extern "C" size_t fftw_reloc_image_reserve(size_t size) {
#ifdef FFTW_RELOC_PROTECT
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  return page + ((size + page - 1) & ~(page - 1));
#else
  return size;
#endif
}

extern "C" void *fftw_reloc_image_at(void *storage) {
#ifdef FFTW_RELOC_PROTECT
  const uintptr_t page = (uintptr_t)sysconf(_SC_PAGESIZE);
  return (void *)(((uintptr_t)storage + page - 1) & ~(page - 1));
#else
  return storage;
#endif
}

extern "C" int fftw_reloc_image_protect(void *image, size_t size, int readonly) {
#ifdef FFTW_RELOC_PROTECT
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  if ((uintptr_t)image & (page - 1)) fail("image is not page aligned");
  return mprotect(image, (size + page - 1) & ~(page - 1),
                  readonly ? PROT_READ : (PROT_READ | PROT_WRITE));
#else
  (void)image; (void)size; (void)readonly;
  return 0;
#endif
}

extern "C" {
void *fftw_reloc_fftw_plan_dft_r2c_1d(void *,int,double *,void *,unsigned);
void *fftw_reloc_fftw_plan_dft_1d(void *,int,void *,void *,int,unsigned);
void fftw_reloc_fftw_execute_dft_r2c(void *,void *,double *,void *);
void fftw_reloc_fftw_execute_dft(void *,void *,void *,void *);
void fftw_reloc_fftw_destroy_plan(void *,void *);
void fftw_reloc_fftw_cleanup(void *);
}

extern "C" uint64_t fftw_reloc_plan_r2c(fftw_reloc_context *c,int length,
                                        double *row,unsigned flags) {
  void *p=fftw_reloc_fftw_plan_dft_r2c_1d(c,length,row,row,flags);
  if(!p) fail("r2c planning failed");
  return fftw_reloc_reference(c,p);
}

extern "C" uint64_t fftw_reloc_plan_c2c(fftw_reloc_context *c,int length,
                                        double *row,int sign,unsigned flags) {
  void *p=fftw_reloc_fftw_plan_dft_1d(c,length,row,row,sign,flags);
  if(!p) fail("c2c planning failed");
  return fftw_reloc_reference(c,p);
}

extern "C" void fftw_reloc_execute_r2c(const void *image,size_t size,
                                       uint64_t plan,void *array,uint64_t offset) {
  fftw_reloc_context c;
  attach(&c,image,size);
  lend(&c,FFTW_RELOC_R2C_ARRAY,array);
  double *row=lent_row(&c,array,offset);
  fftw_reloc_fftw_execute_dft_r2c(&c,fftw_reloc_resolve(&c,plan),row,row);
}

extern "C" void fftw_reloc_execute_c2c(const void *image,size_t size,
                                       uint64_t plan,void *array,uint64_t offset) {
  fftw_reloc_context c;
  attach(&c,image,size);
  lend(&c,FFTW_RELOC_C2C_ARRAY,array);
  double *row=lent_row(&c,array,offset);
  fftw_reloc_fftw_execute_dft(&c,fftw_reloc_resolve(&c,plan),row,row);
}

extern "C" void fftw_reloc_destroy_pair(void *image,size_t size,uint64_t r2c,
                                        uint64_t c2c) {
  auto *c=fftw_reloc_snapshot_open(image,size);
  fftw_reloc_fftw_destroy_plan(c,fftw_reloc_resolve(c,r2c));
  fftw_reloc_fftw_destroy_plan(c,fftw_reloc_resolve(c,c2c));
  fftw_reloc_fftw_cleanup(c);
  fftw_reloc_context_delete(c);
}
