#!/usr/bin/env python3
"""Checks the memory order of a register of publication sites in AArch64 code.

The register (publication_sites.txt) names each site by an anchor, never by a
line: the function that performs it, the kind of access (load, store, rmw, or
a call of another function), the atomic object (the last member or identifier
of the access's first argument; the callee for a call), and the access's
ordinal among the function's accesses of that kind and object, with their
count.  Where the function holds other accesses of that kind and object that
the register does not require the same way, the row also gives a text anchor,
which must occur in that access's context (its statement and the header of
its innermost block) and in no other access's of the group, so a reorder
inside the function cannot retarget the row.

The check guards the sites against ordinary edits written in the project's
style: an order weakened, an access moved, renamed or added, a landing given
another continuation.  A spelling chosen to evade it is out of scope.

What is checked, in the code this tree's cell compiles:
  - Every runtime unit is preprocessed with its own defines, -O2 and the
    triple's default lowering, directives only (conditionals evaluated,
    macros kept), and every function in it is scanned.  A row's function must
    be defined once, its group must hold the stated count, and the anchor must
    pick the stated access.
  - In the runtime's own lines (libs/), these fail: a macro that defines or
    undefines a memory_order_* or atomic_*_explicit name, wraps an atomic
    operation, an atomic builtin or an arts_net_rdzv_expect call, names a
    registered object after "->" or ".", holds an unbalanced brace or pastes
    a member name; a depth-0 brace whose head parses as no function,
    aggregate, initializer or linkage block (a K&R definition's body among
    them); an atomic operation (atomic_*_explicit or atomic_init), a
    registered object's address or an arts_net_rdzv_expect call outside a
    scanned function body; any __atomic_*, __sync_* or __c11_atomic_*
    builtin, called or named; an address of a registered object (&x->obj,
    &x.obj, or &obj for an object the accesses address by name; an offsetof
    naming it counts too) taken anywhere but in the first argument of an
    atomic operation or of a function a "sink" row names, unless an "addr"
    marker names it; arts_net_rdzv_expect named other than in a direct
    call; and an arts_net_rdzv_expect call whose continuation no "ref" row
    names.  Beyond libs/, a runtime unit may compile code only from
    third_party/ (vendored), the build tree (generated) and files outside
    the source tree (system); code from any other project file fails.
  - Completeness: every atomic operation (atomic_init included, as kind
    "init") whose object is registered (an object a required or sink row
    names), and every one whose first argument is not exactly "&<operand>"
    (an access through a pointer variable or a cast, whatever its object),
    in any scanned function (the second kind only in a function defined
    under libs/), belongs to a group (function, kind, object) some row
    names; where the argument is exactly "&<operand>", the operand's object
    must be the access's.  A group that carries no edge of a landed
    install is listed by a marker row (ordinal "-"), which bounds its count
    from above (a build may compile some of its accesses out), so a new
    access there fails too.
  - The order each site states in the source must cover the required half.
  - Every unit is preprocessed again by every given compiler at four
    lowerings, each with its own flags and -O2: the triple alone (outline
    atomics, what an AArch64 build ships), ARMv8.0 LL/SC, ARMv8.1 LSE, and a
    CPU with LSE2 and RCpc.  In every function a row names or that touches a
    registered object, every line a compile keeps must be code in the scan
    and every access the scan found must sit on a line the compile keeps; and
    in no unit may a compile keep a line the scan lacks that names a
    registered object or holds an atomic operation or builtin.  So the
    scanned conditional arms (compiler, feature and optimization macros of
    the four lowerings included) are the compiled ones wherever a registered
    object or an atomic operation is.  And where a compile expands a
    runtime line to an atomic builtin (a fence aside), the line must belong
    to the lines of an atomic operation the scan read, and where it expands
    one to an arts_net_rdzv_expect call, a scanned expectation must start on
    it; the rule is per line, not a count.
  - Each unit that defines a site's function is compiled to AArch64 assembly
    by every compiler at the four lowerings.  Instructions are attributed to
    source positions through .loc: to the site's statement lines, or to the
    access's own text range (line and column) when its statement holds
    another atomic access.  They are collected per emitted function, so each
    inlined copy and clone is judged alone.  Every copy that carries an
    atomic instruction must carry one with the required acquire or release
    half (a libatomic call is judged by the order arguments it is passed);
    every compile must carry at least one such copy.  A site with no atomic
    instruction of its own fails when its own lines carry a plain access of
    its kind, and is otherwise judged by an identical access of the same
    function (same kind, object and source orders), with which the compiler
    merged it.

Named limits, not judged:
  - an operation or a registered name spelled across a backslash-newline,
    which the directives-only text keeps unspliced;
  - a registered word reached under a second name: a macro that renames it
    without member syntax, or another member of a union;
  - atomics reached through code outside libs/ (vendored, generated or
    system): its lines are trusted, a function defined there is judged only
    for the atomic_*_explicit and atomic_init calls it spells on a
    registered name, and its macros' builtins pass on a line that holds a
    scanned atomic operation (mimalloc's, for one);
  - plain (non-atomic) accesses of a registered word's bytes;
  - a marker's claim that its group carries no edge, which is a reviewed
    statement; the check bounds the group's count and nothing more;
  - conditional arms selected by a macro the tree's compile commands and the
    four lowerings leave unchanged: each tree judges only its own cell and
    build type, and FLUSH and CXL trees register no such site;
  - the fabric and a real NIC: the orders judged are the instructions the
    compilers emit, never the ordering a device gives its completions;
  - whether the edge set suffices, and the program order between an install
    and its arm's release.

AMD64 code cannot show any of this: there a release store and an acquire load
are plain moves.
"""
import argparse
import collections
import concurrent.futures
import fnmatch
import functools
import json
import os
import re
import shlex
import subprocess
import sys

NAME = "publication_order_codegen"

LOWERINGS = {
    "def": [],
    "v80": ["-march=armv8-a", "-mno-outline-atomics"],
    "v81": ["-march=armv8.1-a"],
    "nv2": ["-mcpu=neoverse-v2"],
}

ATOMIC_CALL = re.compile(
    r"\batomic_(?:(load|store|exchange|compare_exchange_strong|"
    r"compare_exchange_weak|fetch_add|fetch_sub|fetch_or|fetch_and|"
    r"fetch_xor)_explicit|(init))\s*\(")
ATOMIC_OPS = r"atomic_\w+_explicit|atomic_init|__atomic_\w+|__sync_\w+|__c11_atomic_\w+"
ORDERS = {"memory_order_relaxed": "relaxed", "memory_order_consume": "consume",
          "memory_order_acquire": "acquire", "memory_order_release": "release",
          "memory_order_acq_rel": "acq_rel", "memory_order_seq_cst": "seq_cst"}
HAS_ACQ = {"acquire", "acq_rel", "seq_cst"}
HAS_REL = {"release", "acq_rel", "seq_cst"}
KEYWORDS = {"if", "while", "for", "switch", "return", "sizeof", "do"}
ORDER_DEFINE = re.compile(r"^\s*#\s*(define|undef)\s+(memory_order_\w*|atomic_\w*_explicit)\b")
WRAP_DEFINE = re.compile(r"^\s*#\s*define\b.*\b(atomic_\w+_explicit|atomic_init|__atomic_\w+|"
                         r"__sync_\w+|__c11_atomic_\w+|arts_net_rdzv_expect)\b", re.S)


def fail(msg):
    print(f"FAIL {NAME}: {msg}")


def normalize(s):
    s = re.sub(r"\s+", " ", s).strip()
    s = re.sub(r"([(\[]) ", r"\1", s)
    return re.sub(r" ([)\],;])", r"\1", s)


# ------------------------------------------------------------------ register

class Row:
    def __init__(self, lineno, cells, func, kind, obj, index, count, req, anchor):
        self.lineno, self.cells, self.func, self.kind = lineno, cells, func, kind
        self.obj, self.index, self.count = obj, index, count
        succ, _, failure = req.partition("/")
        self.succ = None if succ == "-" else succ
        self.fail = failure or None
        self.anchor = normalize(anchor) if anchor else None

    @property
    def marker(self):
        return self.index is None and self.kind not in ("call", "ref")

    def label(self):
        i = "-" if self.index is None else self.index
        return f"{self.func}:{self.kind}:{self.obj}#{i}/{self.count}"

    def want(self):
        return (self.succ or "-") + (f"/{self.fail}" if self.fail else "")

    def matches(self, cell, reference):
        for pat in self.cells:
            if pat == "reference":
                if reference:
                    return True
            elif fnmatch.fnmatchcase(cell, pat):
                return True
        return False


def read_register(path):
    rows, bad = [], []
    for n, line in enumerate(open(path), 1):
        try:
            f = shlex.split(line, comments=True)
        except ValueError as e:
            bad.append(f"{path}:{n}: {e}")
            continue
        if not f:
            continue
        m = re.fullmatch(r"(\d+|-)/(\d+)", f[4]) if len(f) in (6, 7) else None
        if not m or f[2] not in ("load", "store", "rmw", "call", "addr", "sink", "init", "ref"):
            bad.append(f"{path}:{n}: malformed row")
            continue
        index = None if m.group(1) == "-" else int(m.group(1))
        count, req = int(m.group(2)), f[5]
        if index is None or f[2] in ("call", "addr", "sink", "init", "ref"):
            ok = {"-"} if index is None else set()
        else:
            ok = {"load": {"acquire"}, "store": {"release"},
                  "rmw": {"acquire", "release", "acq_rel", "acquire/acquire",
                          "release/acquire", "acq_rel/acquire"}}[f[2]]
        if req not in ok:
            bad.append(f"{path}:{n}: a {f[2]} row with ordinal {f[4]} cannot require '{req}'")
            continue
        if index is not None and not 1 <= index <= count:
            bad.append(f"{path}:{n}: ordinal {index} outside 1..{count}")
            continue
        if len(f) == 7 and index is None:
            bad.append(f"{path}:{n}: an anchor names one access; this row names a group")
            continue
        rows.append(Row(n, f[0].split(","), f[1], f[2], f[3], index, count, req,
                        f[6] if len(f) == 7 else None))
    return rows, bad


# ------------------------------------------------------------- source side

def blank(s):
    return re.sub(r"[^\n]", " ", s)


def strip_source(text):
    """Comments, string and character literals and directive lines blanked;
    line structure kept."""
    out, i, n = [], 0, len(text)
    bol = True
    while i < n:
        c = text[i]
        if bol and c in " \t":
            out.append(c)
            i += 1
            continue
        if bol and c == "#":
            j = i
            while True:
                k = text.find("\n", j)
                k = n if k < 0 else k
                if k > i and text[k - 1] == "\\" and k < n:
                    j = k + 1
                    continue
                break
            out.append(blank(text[i:k]))
            i = k
            continue
        bol = False
        if text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(blank(text[i:j]))
            i = j
        elif text.startswith("//", i):
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            if j < n and text[j] == c:
                out.append(c + blank(text[i + 1:j]) + c)
                i = j + 1
            else:
                out.append(c + blank(text[i + 1:j]))
                i = j
        else:
            if c == "\n":
                bol = True
            out.append(c)
            i += 1
    return "".join(out)


LINEMARK = re.compile(r'^#\s*(\d+)\s+"((?:[^"\\]|\\.)*)"')


@functools.lru_cache(maxsize=None)
def real_path(root, name):
    """A preprocessor or assembler file name as the source tree's real path,
    so a symlinked checkout or include directory compares equal."""
    return os.path.realpath(os.path.join(root, name))


def line_table(text, root):
    """(file, line) of every physical line of preprocessor output."""
    table, cur_file, cur_line = [], None, 0
    for raw in text.split("\n"):
        m = LINEMARK.match(raw)
        if m:
            cur_line = int(m.group(1))
            cur_file = real_path(root, m.group(2))
            table.append((None, 0))
            continue
        table.append((cur_file, cur_line))
        cur_line += 1
    return table


FHEAD = re.compile(r"([A-Za-z_]\w*)\s*\(([^;{}()]|\([^;{}()]*\))*\)\s*"
                   r"(__attribute__\s*\(\(.*?\)\)\s*)?\{$", re.S)


def split_args(s):
    args, depth, cur = [], 0, []
    for ch in s:
        if ch in "([{":
            depth += 1
        elif ch in ")]}":
            depth -= 1
        if ch == "," and depth == 0:
            args.append("".join(cur).strip())
            cur = []
        else:
            cur.append(ch)
    args.append("".join(cur).strip())
    return args


def close_paren(text, start):
    depth, j = 1, start
    while j < len(text) and depth:
        if text[j] == "(":
            depth += 1
        elif text[j] == ")":
            depth -= 1
        j += 1
    return j


def object_of(arg):
    arg = re.sub(r"\[[^\[\]]*\]", "", arg).strip()
    m = re.search(r"([A-Za-z_]\w*)\s*\)*\s*$", arg)
    return m.group(1) if m else "?"


def stmt_start(body, p):
    st = max(body.rfind(";", 0, p), body.rfind("{", 0, p), body.rfind("}", 0, p)) + 1
    while st < p and body[st] in " \t\n":
        st += 1
    return st


class Access:
    def __init__(self, kind, obj, succ, failure, span, pos, end, context, arg):
        self.kind, self.obj, self.arg = kind, obj, arg
        self.succ, self.fail = succ, failure
        self.span = span         # (file, first line, last line) of its statement
        self.pos, self.end = pos, end   # (line, column) of its name and its ')'
        self.context = context   # its block's header and its statement, normalized
        self.precise = False     # attributed by its own text range
        self.exact = False       # its first argument is exactly "&<operand>"
        self.operand = None      # that operand's object

    def lines(self):
        if self.precise:
            return range(self.pos[0], self.end[0] + 1)
        return range(self.span[1], self.span[2] + 1)

    def owns(self, line, col):
        if not self.precise:
            return self.span[1] <= line <= self.span[2]
        return self.pos <= (line, col) <= self.end


class Func:
    def __init__(self, name, file, line, body, base, table, offsets, clean, keep_calls):
        self.name, self.file, self.line = name, file, line
        self.accesses = []
        self.calls = collections.Counter()
        self.code_lines = set()

        def at(off):
            k = offsets_index(offsets, base + off)
            return table[k], base + off - offsets[k] + 1

        self.end_line = at(len(body) - 1)[0][1]
        for k in range(offsets_index(offsets, base), offsets_index(offsets, base + len(body) - 1) + 1):
            f, ln = table[k]
            nxt = offsets[k + 1] if k + 1 < len(offsets) else len(clean)
            if f == file and clean[offsets[k]:nxt].strip():
                self.code_lines.add(ln)
        # Each block's opening brace and its matching close.
        opens, stack = [], []
        for i, ch in enumerate(body):
            if ch == "{":
                stack.append(len(opens))
                opens.append([i, len(body)])
            elif ch == "}" and stack:
                opens[stack.pop()][1] = i

        def header(p):
            inner = max((o for o in opens if o[0] < p < o[1]), key=lambda o: o[0])
            if inner[0] == 0:
                return name + "("
            return body[stmt_start(body, inner[0]):inner[0]]

        for m in ATOMIC_CALL.finditer(body):
            j = close_paren(body, m.end())
            args = split_args(body[m.end():j - 1])
            op = m.group(1) or "init"
            ords = [ORDERS.get(a, "?") for a in args if a.startswith("memory_order")]
            kind = {"load": "load", "store": "store", "init": "init"}.get(op, "rmw")
            if op == "init":
                succ, failure = "-", None
            elif op.startswith("compare_exchange"):
                succ, failure = (ords[-2], ords[-1]) if len(ords) >= 2 else ("?", "?")
            else:
                succ, failure = (ords[-1] if ords else "?"), None
            st = stmt_start(body, m.start())
            cst = st
            if body.startswith("while", st):
                # The statement that closes a do-block: its context is the loop.
                k = body.rfind("}", 0, st)
                blk = next((o for o in opens if o[1] == k), None)
                if blk is not None and body[stmt_start(body, blk[0]):blk[0]].strip() == "do":
                    cst = stmt_start(body, blk[0])
            (f0, l0), _ = at(st)
            (fc, lc), cc = at(m.start())
            (f1, l1), c1 = at(j - 1)
            span = (f0, l0, l1) if f0 == f1 else (f0, l0, l0)
            ctx = normalize(header(cst) + " " + body[cst:j])
            acc = Access(kind, object_of(args[0]), succ, failure, span,
                         (lc, cc), (l1, c1), ctx, normalize(args[0]))
            # Exactly "&<operand>": the operand's object is the access's.
            a0 = args[0].strip()
            if a0.startswith("&"):
                o, _, end = operand_object(a0, 1)
                acc.exact = o is not None and not a0[end:].strip()
                acc.operand = o
            self.accesses.append(acc)
        if keep_calls:
            for m in re.finditer(r"(?<![\w.>])([A-Za-z_]\w*)\s*\(", body):
                if m.group(1) not in KEYWORDS and not ATOMIC_CALL.match(body, m.start()):
                    self.calls[m.group(1)] += 1
        for a in self.accesses:
            a.precise = any(b is not a and b.span[0] == a.span[0] and
                            not (b.span[2] < a.span[1] or b.span[1] > a.span[2])
                            for b in self.accesses)

    def group(self, kind, obj):
        return [a for a in self.accesses if a.kind == kind and a.obj == obj]


def offsets_index(offsets, off):
    lo, hi = 0, len(offsets) - 1
    while lo < hi:
        mid = (lo + hi + 1) // 2
        if offsets[mid] <= off:
            lo = mid
        else:
            hi = mid - 1
    return lo


def match_open(s, close):
    """Index of the bracket that opens the one closing at s[close]."""
    pair = {")": "(", "]": "["}[s[close]]
    depth = 0
    for k in range(close, -1, -1):
        if s[k] == s[close]:
            depth += 1
        elif s[k] == pair:
            depth -= 1
            if depth == 0:
                return k
    return -1


ANNOTATION = re.compile(r"(__\w+|[A-Z_][A-Z0-9_]*)$")


def strip_annotation(s):
    """s without a trailing attribute or annotation macro (with or without
    arguments) that follows a parameter list, or None."""
    t = s
    if t.endswith(")"):
        o = match_open(t, len(t) - 1)
        if o < 0:
            return None
        t = t[:o].rstrip()
    m = ANNOTATION.search(t)
    if not m:
        return None
    rest = t[:m.start()].rstrip()
    probe_ = rest
    while probe_ and not probe_.endswith(")"):
        q = ANNOTATION.search(probe_)
        if not q:
            return None
        probe_ = probe_[:q.start()].rstrip()
    return rest if probe_ else None


def head_kind(head):
    """What a depth-0 brace opens: ("func", name), ("other", None) for an
    aggregate, an initializer or a linkage block, or ("bad", None) for a head
    that does not parse as any of them."""
    s = head[:-1].rstrip()
    if not s:
        return "bad", None                    # a K&R definition's body
    if s.endswith(("=", ",")) or re.search(r'\bextern\s+"C"$', s):
        return "other", None
    while True:
        t = strip_annotation(s)
        if t is None:
            break
        s = t
    if s.endswith(")"):
        o = match_open(s, len(s) - 1)
        before = s[:o].rstrip() if o >= 0 else ""
        m = re.search(r"([A-Za-z_]\w*)$", before)
        if m:
            return ("other", None) if m.group(1) in KEYWORDS else ("func", m.group(1))
        if before.endswith(")"):
            o2 = match_open(before, len(before) - 1)
            inner = before[o2 + 1:-1].strip() if o2 >= 0 else ""
            if re.fullmatch(r"[A-Za-z_]\w*", inner):
                return "func", inner          # a parenthesized name
            m = re.match(r"\*+\s*([A-Za-z_]\w*)\s*\(", inner)
            if m:
                return "func", m.group(1)     # a function returning a function pointer
        return "bad", None
    if re.search(r"\b(struct|union|enum)\b", s):
        return "other", None
    return ("bad", None) if "(" in s else ("other", None)


def operand_object(text, i):
    """(The last member or identifier of the operand of an address-of at
    text[i - 1], whether it is a member, where the operand ends); the object
    is None when the operand is not a plain chain of members, subscripts and
    calls."""
    n = len(text)
    j = i
    while j < n and text[j] in " \t\n":
        j += 1
    if j < n and text[j] == "(":
        k = close_paren(text, j + 1)
        inner = text[j + 1:k - 1]
        last, member, e = operand_object(inner, 0)
        if inner[e:].strip():
            last, member = None, False
        j = k
    else:
        m = re.compile(r"[A-Za-z_]\w*").match(text, j)
        if not m:
            return None, False, j
        last, member, j = m.group(0), False, m.end()
    while j < n:
        k = j
        while k < n and text[k] in " \t\n":
            k += 1
        if text.startswith("->", k) or (text.startswith(".", k) and not text[k + 1:k + 2].isdigit()):
            k += 2 if text[k] == "-" else 1
            while k < n and text[k] in " \t\n":
                k += 1
            m = re.compile(r"[A-Za-z_]\w*").match(text, k)
            if not m:
                return None, False, k
            last, member, j = m.group(0), True, m.end()
        elif k < n and text[k] == "[":
            depth = 0
            while k < n:
                depth += {"[": 1, "]": -1}.get(text[k], 0)
                k += 1
                if depth == 0:
                    break
            j = k
        elif k < n and text[k] == "(":
            j = close_paren(text, k + 1)
            last, member = None, False
        else:
            break
    return last, member, j


def unary_amp(text, i):
    """Whether the & at text[i] takes an address (rather than a bitwise and):
    what precedes it is an operator or an opening, or a cast."""
    j = i - 1
    while j >= 0 and text[j] in " \t\n":
        j -= 1
    if j < 0:
        return True
    c = text[j]
    if c == ")":
        o = match_open(text, j)
        k = o - 1
        while k >= 0 and text[k] in " \t\n":
            k -= 1
        # "f(x) & y" is an and; "(type) &x" a cast of an address, and a
        # parenthesized operand of an and is taken as one too: an and of a
        # registered object would be an implicit access, which the build
        # already refuses, so failing closed costs nothing.
        return not (o >= 0 and k >= 0 and (text[k].isalnum() or text[k] in "_)]"))
    if c.isalnum() or c in "_]":
        return bool(re.search(r"\b(return|case|sizeof)$", text[max(0, j - 7):j + 1]))
    return True


def expr_end(text, i):
    """Where the expression starting at text[i] ends: the first top-level
    comma, semicolon, or closing bracket of an enclosing one."""
    depth, j, n = 0, i, len(text)
    while j < n:
        c = text[j]
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                return j
            depth -= 1
        elif c in ",;" and depth == 0:
            return j
        j += 1
    return n


ESCAPE_OK = re.compile(r"\b(atomic_\w+_explicit|atomic_init)\s*\(\s*\(?\s*$")
BUILTIN = re.compile(r"\b(__atomic_\w+|__sync_\w+|__c11_atomic_\w+)\s*\(")


def scan_unit(text, root, src, keep_calls, objects, build=None):
    """Every function defined in one unit's directives-only preprocessor
    output; and, in the runtime's own lines, what would hide an access from
    the scan (problems), the addresses of registered objects taken outside
    an atomic operation's first argument (escapes), and the expectations a
    landing's continuation is registered by (expects)."""
    lib = src + "/libs/"
    table = line_table(text, root)
    problems, escapes, expects = [], [], []
    lines = text.split("\n")
    obj_member = re.compile(r"(->|\.)\s*(" + "|".join(map(re.escape, sorted(objects))) + r")\b") \
        if objects else None
    k = 0
    while k < len(lines):
        d, k0 = lines[k], k
        while d.endswith("\\") and k + 1 < len(lines):
            k += 1
            d = d[:-1] + " " + lines[k]
        k += 1
        if not (table[k0][0] and table[k0][0].startswith(lib)):
            continue
        shown = " ".join(d.split())[:110]
        if re.match(r"\s*#\s*define\b", d):
            if ORDER_DEFINE.match(d) or WRAP_DEFINE.match(d) or (obj_member and obj_member.search(d)):
                problems.append((table[k0], "a macro that names an order, wraps an atomic "
                                 "operation or an expectation, or names a registered object "
                                 f"hides accesses and orders from the scan ({shown})"))
            elif d.count("{") != d.count("}") or re.search(r"(->|\.)\s*\w*\s*##", d):
                problems.append((table[k0], "a macro with an unbalanced brace or a pasted "
                                 f"member hides a definition or an object from the scan ({shown})"))
        elif ORDER_DEFINE.match(d):
            problems.append((table[k0], f"a project line undefines an order or an atomic "
                             f"operation ({shown})"))
    clean = strip_source(text)
    offsets, pos = [], 0
    for ln in clean.split("\n"):
        offsets.append(pos)
        pos += len(ln) + 1
    code_lines, outside = set(), {}
    trusted = (lib, src + "/third_party/") + ((build + "/",) if build else ())
    for k in range(len(offsets)):
        f, ln = table[k]
        nxt = offsets[k + 1] if k + 1 < len(offsets) else len(clean)
        if not f or not clean[offsets[k]:nxt].strip():
            continue
        if f.startswith(lib):
            code_lines.add((f, ln))
        elif f.startswith(src + "/") and not f.startswith(trusted):
            outside.setdefault(f, ln)
    for f, ln in outside.items():
        problems.append(((f, ln), "a runtime unit compiles code from this project file "
                         "outside libs/ and third_party/, whose lines the scan does not "
                         "judge"))

    def where(off):
        return table[offsets_index(offsets, off)]

    found, spans, depth, i = [], [], 0, 0
    head_start = 0
    while i < len(clean):
        ch = clean[i]
        if ch == "{":
            if depth == 0:
                kind, name = head_kind(clean[head_start:i + 1])
                f, ln = where(i)
                j, d = i + 1, 1
                while j < len(clean) and d:
                    d += {"{": 1, "}": -1}.get(clean[j], 0)
                    j += 1
                if kind == "bad" and f and f.startswith(lib):
                    problems.append(((f, ln), "a definition whose head does not parse as a "
                                     "function's is not scanned"))
                if kind == "func" and f:
                    spans.append((i, j, name))
                    body = clean[i:j]
                    if name in keep_calls or ATOMIC_CALL.search(body):
                        found.append(Func(name, f, ln, body, i, table, offsets, clean,
                                          name in keep_calls))
            depth += 1
        elif ch == "}":
            depth -= 1
            if depth == 0:
                head_start = i + 1
        elif ch == ";" and depth == 0:
            head_start = i + 1
        i += 1

    def owner(off):
        return next((nm for a, b, nm in spans if a <= off < b), None)

    ops = re.compile(r"\b(" + ATOMIC_OPS + r")\b(\s*\()?")
    for m in ops.finditer(clean):
        f, ln = where(m.start())
        if not f or not f.startswith(lib):
            continue
        if m.group(1).startswith("__"):
            problems.append(((f, ln), f"{m.group(1)} in {owner(m.start()) or 'file scope'}: "
                             f"the runtime uses only atomic_*_explicit and atomic_init, which "
                             f"the scan reads"))
        elif m.group(2) and owner(m.start()) is None:
            problems.append(((f, ln), f"{m.group(1)} outside a scanned function body"))
    for m in re.finditer(r"\barts_net_rdzv_expect\b(?!\s*\()", clean):
        f, ln = where(m.start())
        if f and f.startswith(lib):
            problems.append(((f, ln), "arts_net_rdzv_expect named other than in a direct "
                             "call, so the scan cannot read the continuation it registers"))
    for m in re.finditer(r"\barts_net_rdzv_expect\s*\(", clean):
        f, ln = where(m.start())
        fn = owner(m.start())
        if not f or not f.startswith(lib):
            continue
        if fn is not None:
            args = split_args(clean[m.end():close_paren(clean, m.end()) - 1])
            expects.append((fn, normalize(args[2]) if len(args) > 2 else "?", f, ln))
        elif not re.search(r"\bvoid\s*$", clean[max(0, m.start() - 40):m.start()]):
            problems.append(((f, ln), "arts_net_rdzv_expect outside a scanned function body"))
    if objects:
        token = "|".join(map(re.escape, sorted(objects)))
        member_re = re.compile(r"(->|\.)\s*(" + token + r")\b")
        bare_re = re.compile(r"\s*\(?\s*(" + token + r")\b")
        for m in re.finditer(r"(?<!&)&(?![&=])", clean):
            f, ln = where(m.start())
            if not f or not f.startswith(lib) or not unary_amp(clean, m.start()):
                continue
            region = clean[m.end():expr_end(clean, m.end())]
            hit = member_re.search(region)
            obj, member = (hit.group(2), True) if hit else (None, False)
            if not hit:
                b = bare_re.match(region)
                obj = b.group(1) if b else None
            if obj is None:
                continue
            before = clean[max(0, m.start() - 120):m.start()]
            if ESCAPE_OK.search(before):
                continue
            fn = owner(m.start())
            if fn is None:
                problems.append(((f, ln), f"the address of '{obj}' is taken outside a scanned "
                                 f"function body"))
                continue
            c = re.search(r"\b([A-Za-z_]\w*)\s*\(\s*\(?\s*$", before)
            escapes.append((fn, obj, member, c.group(1) if c else None, f, ln))
        for m in re.finditer(r"\b(?:__builtin_)?offsetof\s*\(", clean):
            f, ln = where(m.start())
            if not f or not f.startswith(lib):
                continue
            args = split_args(clean[m.end():close_paren(clean, m.end()) - 1])
            obj = object_of(args[-1]) if len(args) == 2 else None
            if obj in objects:
                fn = owner(m.start())
                if fn is None:
                    problems.append(((f, ln), f"an offsetof of '{obj}' outside a scanned "
                                     f"function body"))
                else:
                    escapes.append((fn, obj, True, "offsetof", f, ln))
    return found, problems, escapes, expects, code_lines


def active_lines(text, root, src):
    """The (file, line) pairs a full preprocessor output keeps tokens on, and
    the text it keeps on each of the project's lines."""
    lib = src + "/libs/"
    out, texts, cur_file, cur_line = set(), {}, None, 0
    for raw in text.split("\n"):
        m = LINEMARK.match(raw)
        if m:
            cur_line = int(m.group(1))
            cur_file = real_path(root, m.group(2))
            continue
        if raw.strip() and not raw.lstrip().startswith("#"):
            out.add((cur_file, cur_line))
            if cur_file and cur_file.startswith(lib):
                texts[(cur_file, cur_line)] = texts.get((cur_file, cur_line), "") + raw
        cur_line += 1
    return out, texts


# ------------------------------------------------------------ compile side

def unit_flags(entry):
    args = entry.get("arguments") or shlex.split(entry["command"])
    keep, take = [], 0
    for a in args[1:]:
        if take:
            keep.append(a)
            take -= 1
        elif a in ("-D", "-U", "-I", "-isystem", "-iquote", "-include"):
            keep.append(a)
            take = 1
        elif a.startswith(("-D", "-U", "-I", "-isystem", "-iquote", "-std=")):
            keep.append(a)
    return keep


FILEDIR = re.compile(r'^\s*\.file\s+(\d+)\s+"((?:[^"\\]|\\.)*)"(?:\s+"((?:[^"\\]|\\.)*)")?')
LOC = re.compile(r"^\s*\.loc\s+(\d+)\s+(\d+)(?:\s+(\d+))?")
FLABEL = re.compile(r"^([A-Za-z_][\w.$]*):")
INSN = re.compile(r"^\t([a-z][a-z0-9.]*)(?:\s+(.*))?$")
ATOMIC_MN = re.compile(
    r"^(ldar|ldapr|ldapur|stlr|stlur|ldaxr|ldxr|stlxr|stxr|ldaxp|ldxp|stlxp|stxp|"
    r"casp?|swpp?|ld(?:add|clr|eor|set|smax|smin|umax|umin)|"
    r"st(?:add|clr|eor|set|smax|smin|umax|umin))(?:al|a|l)?[bh]?$")
PLAIN = {"load": re.compile(r"^(ldr|ldur)(b|h|sb|sh|sw)?$|^ldp$"),
         "store": re.compile(r"^(str|stur)(b|h)?$|^stp$")}
LIBATOMIC = re.compile(r"^__atomic_(load|store|exchange|compare_exchange)_(1|2|4|8|16)$")
OUTLINE = re.compile(r"^__aarch64_(cas|swp|ldadd|ldclr|ldeor|ldset)(1|2|4|8|16)_"
                     r"(relax|acq_rel|acq|rel|sync)$")
CNUM = {0: "relaxed", 1: "consume", 2: "acquire", 3: "release", 4: "acq_rel", 5: "seq_cst"}
REG = re.compile(r"^[wx](\d+)$")
# Instructions whose first operand is not a register they write.
NO_WRITE = {"bl", "b", "blr", "br", "ret", "cbz", "cbnz", "tbz", "tbnz", "cmp", "cmn",
            "tst", "ccmp", "ccmn", "str", "strb", "strh", "stur", "sturb", "sturh", "stp",
            "stnp", "stlr", "stlrb", "stlrh", "stlur", "stlurb", "stlurh", "prfm"}
# Instructions that write their first two operands; an atomic instruction
# (whose written register is the second operand for ld<op> and swp) is
# treated as one too.
PAIR_WRITE = {"ldp", "ldnp", "ldpsw", "ldxp", "ldaxp", "casp", "caspa", "caspl", "caspal"}


def parse_asm(asm, root, wanted_lines):
    """{(file, line): [(emitted function, column, token), ...]} for wanted lines."""
    files, loc, func, regs = {}, None, None, {}
    hits = collections.defaultdict(list)
    for ln in asm.split("\n"):
        m = FILEDIR.match(ln)
        if m:
            p = os.path.join(m.group(2), m.group(3)) if m.group(3) else m.group(2)
            files[m.group(1)] = real_path(root, p)
            continue
        m = LOC.match(ln)
        if m:
            loc = (files.get(m.group(1)), int(m.group(2)), int(m.group(3) or 0))
            continue
        m = FLABEL.match(ln)
        if m and not m.group(1).startswith("."):
            func, regs = m.group(1), {}
            continue
        m = INSN.match(ln)
        if not m:
            continue
        mn, ops = m.group(1), (m.group(2) or "").split("//")[0].strip()
        opl = [o.strip() for o in ops.split(",")]
        tok = None
        tgt = opl[0] if opl and mn in ("bl", "b") else ""
        if mn == "bl" or (mn == "b" and not tgt.startswith(".")):
            la = LIBATOMIC.match(tgt)
            if la:
                # The order arguments follow the value: a 16-byte value takes
                # an even register pair (AAPCS64), so its orders start at w4.
                op, wide = la.group(1), la.group(2) == "16"
                if op == "compare_exchange":
                    s, f = (regs.get(4), regs.get(5)) if wide else (regs.get(3), regs.get(4))
                    tok = f"lib:{op}:{CNUM.get(s, '?')}/{CNUM.get(f, '?')}"
                elif op == "load":
                    tok = f"lib:load:{CNUM.get(regs.get(1), '?')}"
                else:
                    tok = f"lib:{op}:{CNUM.get(regs.get(4 if wide else 2), '?')}"
            elif OUTLINE.match(tgt):
                tok = "bl " + tgt
            else:
                tok = "call " + tgt
            regs = {}
        elif mn == "blr":
            regs = {}
        elif mn == "dmb":
            tok = "dmb " + ops
        elif ATOMIC_MN.match(mn):
            tok = mn
        elif PLAIN["load"].match(mn) or PLAIN["store"].match(mn):
            tok = "plain:" + mn
        # Track the constants a libatomic call's order arguments are made
        # from; any other write of a register forgets it.
        if mn not in NO_WRITE and opl and REG.match(opl[0]):
            r = int(REG.match(opl[0]).group(1))
            src_ = opl[1] if mn == "mov" and len(opl) == 2 else ""
            if re.fullmatch(r"#?\d+", src_):
                regs[r] = int(src_.lstrip("#"))
            elif REG.match(src_) and int(REG.match(src_).group(1)) in regs:
                regs[r] = regs[int(REG.match(src_).group(1))]
            else:
                regs.pop(r, None)
            if (mn in PAIR_WRITE or ATOMIC_MN.match(mn)) and len(opl) > 1 and REG.match(opl[1]):
                regs.pop(int(REG.match(opl[1]).group(1)), None)
        if tok and loc and loc[:2] in wanted_lines:
            hits[loc[:2]].append((func, loc[2], tok))
    return hits


LSE = re.compile(r"^(casp|cas|swpp|swp|ld(?:add|clr|eor|set|smax|smin|umax|umin))"
                 r"(al|a|l)?(b|h)?$")


def atomic_token(t):
    return ATOMIC_MN.match(t) is not None or t.startswith(("bl __aarch64_", "lib:"))


def atomic_copy(toks):
    """A copy that carries an atomic access: an atomic instruction, or a pair
    access a barrier orders (the 16-byte forms of LSE2)."""
    return any(atomic_token(t) for t in toks) or (
        ("plain:ldp" in toks or "plain:stp" in toks) and any(t.startswith("dmb") for t in toks))


def plain_token(t, kind):
    return kind in PLAIN and t.startswith("plain:") and PLAIN[kind].match(t[6:]) is not None


def missing(row, toks):
    """The required halves no instruction of one copy carries."""
    need_acq = row.succ in HAS_ACQ
    need_fail = row.fail in HAS_ACQ
    need_rel = row.succ in HAS_REL
    acq = fail_acq = rel = False
    for t in toks:
        if t.startswith("lib:"):
            op, order = t.split(":")[1:]
            s, _, f = order.partition("/")
            if (op == "load") == (row.kind == "load") and (op == "store") == (row.kind == "store"):
                acq |= s in HAS_ACQ
                fail_acq |= (f or s) in HAS_ACQ
                rel |= s in HAS_REL
            continue
        if row.kind == "load":
            acq |= bool(re.match(r"^(ldar|ldapr|ldapur)[bh]?$", t) or t == "ldaxp"
                        or re.match(r"^casp(a|al)$", t)
                        or (t == "plain:ldp" and ("dmb ish" in toks or "dmb ishld" in toks)))
        elif row.kind == "store":
            rel |= bool(re.match(r"^(stlr|stlur)[bh]?$", t) or t == "stlxp"
                        or re.match(r"^casp(l|al)$", t)
                        or (t == "plain:stp" and "dmb ish" in toks))
        else:
            m = LSE.match(t)
            o = OUTLINE.match(t[3:]) if t.startswith("bl ") else None
            if m:
                a, r = "a" in (m.group(2) or ""), "l" in (m.group(2) or "")
            elif o:
                a, r = o.group(3) in ("acq", "acq_rel", "sync"), o.group(3) in ("rel", "acq_rel", "sync")
            else:
                a = bool(re.match(r"^ldaxr[bh]?$", t) or t == "ldaxp")
                r = bool(re.match(r"^stlxr[bh]?$", t) or t == "stlxp")
            # A single instruction's acquire covers the read a failing
            # exchange makes as well.
            acq |= a
            fail_acq |= a
            rel |= r
    out = []
    if need_acq and not acq:
        out.append("acquire")
    if need_fail and not fail_acq and "acquire" not in out:
        out.append("failure-order acquire")
    if need_rel and not rel:
        out.append("release")
    return out


def covers(have, want):
    if want is None:
        return True
    return (want not in HAS_ACQ or have in HAS_ACQ) and (want not in HAS_REL or have in HAS_REL)


def probe(cmd):
    """Exit status 0 when the compiler accepts every lowering."""
    for low in LOWERINGS.values():
        try:
            r = subprocess.run(cmd + low + ["-O2", "-S", "-x", "c", "-", "-o", os.devnull],
                               stdin=subprocess.DEVNULL, capture_output=True)
        except OSError as e:
            print(f"{' '.join(cmd)} cannot run: {e.strerror}")
            return 1
        if r.returncode != 0:
            print(f"{' '.join(cmd)} refuses {' '.join(low) or 'the default lowering'}")
            return 1
    return 0


# --------------------------------------------------------------------- main

def main():
    if len(sys.argv) > 2 and sys.argv[1] == "--probe":
        return probe(sys.argv[2:])
    ap = argparse.ArgumentParser()
    ap.add_argument("--source-dir", required=True)
    ap.add_argument("--build-dir", required=True)
    ap.add_argument("--work-dir", required=True)
    ap.add_argument("--register", required=True)
    ap.add_argument("--cell", required=True, help="VALIDITY.RETENTION.READ_SOURCE")
    ap.add_argument("--reference", action="store_true")
    ap.add_argument("--gcc", required=True)
    ap.add_argument("--clang", default="")
    ap.add_argument("--jobs", type=int, default=4)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--list", default="",
                    help="print the accesses of these comma-separated functions "
                         "as this cell compiles them, and exit")
    a = ap.parse_args()
    src = os.path.realpath(a.source_dir)
    a.work_dir = os.path.abspath(a.work_dir)
    os.makedirs(a.work_dir, exist_ok=True)

    rows, bad = read_register(a.register)
    for b in bad:
        fail(b)
    if bad:
        return 1
    # Rows checked here; rows that also cover an access (a reference row
    # covers its protocol-independent function in every tree).
    live = [r for r in rows if r.matches(a.cell, a.reference)]
    cover = [r for r in rows if r.matches(a.cell, True)]
    if not live and not a.list:
        fail(f"no register row applies to cell {a.cell}")
        return 1
    call_funcs = set(a.list.split(",")) if a.list else {r.func for r in live if r.kind == "call"}
    objects = {r.obj for r in cover if r.kind in ("load", "store", "rmw") and not r.marker} \
        | {r.obj for r in cover if r.kind == "sink"}

    dbpath = os.path.join(a.build_dir, "compile_commands.json")
    if not os.path.exists(dbpath):
        fail(f"no {dbpath}")
        return 1
    units, seen = [], set()
    for e in json.load(open(dbpath)):
        f = os.path.realpath(e["file"])
        out = e.get("output") or ""
        if not out:
            args = e.get("arguments") or shlex.split(e["command"])
            out = args[args.index("-o") + 1] if "-o" in args else ""
        if not f.startswith(os.path.join(src, "libs", "src") + "/") or not f.endswith(".c") \
                or re.search(r"(^|/)(tests|examples|benchmarks)/", out) or f in seen:
            continue
        seen.add(f)
        units.append((f, e["directory"], tuple(unit_flags(e))))
    if not units:
        fail("the compile database names no runtime unit")
        return 1

    def run(cmd, d):
        r = subprocess.run(cmd, cwd=d, capture_output=True)
        return r.returncode, r.stdout.decode("latin-1"), r.stderr.decode("latin-1")

    def preprocess(u):
        f, d, flags = u
        return u, run([a.gcc, "-E", "-fdirectives-only", "-w", "-O2"] + LOWERINGS["def"]
                      + list(flags) + [f], d)

    defs = collections.defaultdict(dict)        # func -> (file, line) -> Func
    unit_of = collections.defaultdict(set)      # func -> units defining it
    unit_funcs, unit_code = {}, {}              # unit -> its functions, its code lines
    problems, escapes, expects = set(), set(), set()
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for u, (rc, out, err) in ex.map(preprocess, units):
            if rc != 0:
                fail(f"preprocessing {u[0]} failed: {err.strip().splitlines()[-1:]}")
                return 1
            found, probs, escs, exps, code = scan_unit(out, u[1], src, call_funcs, objects,
                                                       os.path.realpath(a.build_dir))
            problems.update(probs)
            escapes.update(escs)
            expects.update(exps)
            unit_funcs[u], unit_code[u] = found, code
            for fn in found:
                defs[fn.name].setdefault((fn.file, fn.line), fn)
                unit_of[fn.name].add(u)

    if a.list:
        for name in sorted(call_funcs):
            for (f, l), fn in sorted(defs.get(name, {}).items()):
                print(f"{name}  {os.path.relpath(f, src)}:{l}")
                n = collections.Counter((x.kind, x.obj) for x in fn.accesses)
                k = collections.Counter()
                for x in fn.accesses:
                    k[(x.kind, x.obj)] += 1
                    order = x.succ + (f"/{x.fail}" if x.fail else "")
                    print(f"  {x.kind:5} {x.obj:20} {k[(x.kind, x.obj)]}/{n[(x.kind, x.obj)]}"
                          f"  {order:16} {os.path.relpath(x.span[0], src)}:{x.pos[0]}"
                          f"  {x.context[:90]}")
                for c, m in sorted(fn.calls.items()):
                    print(f"  call  {c:20} {m}")
        return 0

    errors = [f"{os.path.relpath(f, src)}:{ln}: {what}" for (f, ln), what in sorted(problems)]
    # An address of a registered object outside an atomic operation's first
    # argument lets an access reach it under another name.  A member's address
    # counts wherever it is taken; a bare name's only for an object the
    # register's accesses address by name (a file-scope object), since a bare
    # pointer that the accesses use is passed by value.
    allowed = {(r.func, r.obj) for r in cover if r.kind == "addr"}
    sinks = {(r.func, r.obj) for r in cover if r.kind == "sink"}
    named_objs = {x.obj for d in defs.values() for fn in d.values() for x in fn.accesses
                  if x.arg == "&" + x.obj}
    taken = collections.Counter()
    for func, obj, member, callee, f, ln in sorted(escapes, key=str):
        if not member and obj not in named_objs:
            continue
        if (callee, obj) in sinks:
            continue
        taken[(func, obj)] += 1
        if (func, obj) not in allowed:
            errors.append(f"{os.path.relpath(f, src)}:{ln}: {func} takes the address of '{obj}' "
                          f"outside an atomic operation's first argument, so the scan cannot "
                          f"follow its accesses (an \"addr\" marker names one that carries none)")

    # ---- locate every row
    sites = []                                   # (row, Access, Func)
    by_group = collections.defaultdict(list)     # (func, kind, obj) -> rows
    for r in live:
        by_group[(r.func, r.kind, r.obj)].append(r)
    for r in live:
        d = defs.get(r.func, {})
        if r.kind == "sink":
            continue
        if r.kind == "ref":
            n = sum(1 for fn_, cb, _, _ in expects if (fn_, cb) == (r.func, r.obj))
            if n != r.count:
                errors.append(f"{r.label()}: {r.func} registers {r.obj} as a landing's "
                              f"continuation {n} times")
            continue
        if r.kind == "addr":
            if taken[(r.func, r.obj)] > r.count:
                errors.append(f"{r.label()}: {r.func} takes the address of '{r.obj}' "
                              f"{taken[(r.func, r.obj)]} times, the register marks at most "
                              f"{r.count}")
            continue
        if r.marker:
            # A marker bounds its group: a build may compile some of the
            # accesses, or the whole function, out.
            for fn in d.values():
                n = len(fn.group(r.kind, r.obj))
                if n > r.count:
                    errors.append(f"{r.label()}: {r.func} ({os.path.relpath(fn.file, src)}) "
                                  f"has {n} {r.kind} accesses of '{r.obj}', the register "
                                  f"marks at most {r.count}: say what the new one must keep")
            continue
        if len(d) != 1:
            where = ", ".join(f"{os.path.relpath(f, src)}:{l}" for f, l in d) or "no unit"
            errors.append(f"{r.label()} (register line {r.lineno}): {len(d)} definitions "
                          f"of {r.func} in this cell's units ({where})")
            continue
        fn = next(iter(d.values()))
        if r.kind == "call":
            n = fn.calls.get(r.obj, 0)
            if n != r.count:
                errors.append(f"{r.label()}: {r.func} calls {r.obj} {n} times")
            continue
        grp = fn.group(r.kind, r.obj)
        if len(grp) != r.count:
            errors.append(f"{r.label()}: {r.func} has {len(grp)} {r.kind} accesses of "
                          f"'{r.obj}', the register states {r.count}")
            continue
        s = grp[r.index - 1]
        where = f"{os.path.relpath(s.span[0], src)}:{s.pos[0]}"
        if r.anchor:
            hit = [k + 1 for k, x in enumerate(grp) if r.anchor in x.context]
            if hit != [r.index]:
                errors.append(f"{r.label()} at {where}: its anchor \"{r.anchor}\" picks "
                              f"{'no access' if not hit else 'access ' + ', '.join(map(str, hit))} "
                              f"of the group, not access {r.index}")
                continue
        else:
            same = {(x.index, x.succ, x.fail) for x in by_group[(r.func, r.kind, r.obj)]
                    if not x.marker}
            if len(grp) > 1 and ({i for i, _, _ in same} != set(range(1, len(grp) + 1))
                                 or len({(sc, fl) for _, sc, fl in same}) > 1):
                errors.append(f"{r.label()} at {where}: the group holds accesses the register "
                              f"does not require the same way, so the row needs an anchor")
                continue
        if s.precise and any(o is not s and o.span[0] == s.span[0] and
                             not (o.end < s.pos or o.pos > s.end) for o in fn.accesses):
            errors.append(f"{r.label()} at {where}: its text holds another atomic access, "
                          f"so its instructions cannot be told apart")
            continue
        sites.append((r, s, fn))

    # Every expectation names its continuation by a ref row, so a landing
    # re-routed to another continuation fails at its root.
    pinned = {(r.func, r.obj) for r in cover if r.kind == "ref"}
    for fn_, cb, f, ln in sorted(expects):
        if (fn_, cb) not in pinned:
            errors.append(f"{os.path.relpath(f, src)}:{ln}: {fn_} registers {cb} as a landing's "
                          f"continuation and no ref row names it")

    # ---- completeness: every access of a registered object belongs to a named
    # group, and so does every access whose first argument is not exactly
    # "&<operand>" (an access through a pointer variable or a cast).
    named = {(r.func, r.kind, r.obj) for r in cover
             if r.kind not in ("call", "addr", "sink", "ref")}
    uniq = {}
    for name, d in defs.items():
        for key, fn in d.items():
            uniq[(name,) + key] = fn
    lib = src + "/libs/"
    for (name, f, l), fn in sorted(uniq.items()):
        for x in fn.accesses:
            if x.exact and x.operand != x.obj:
                errors.append(f"{name} ({os.path.relpath(f, src)}:{x.pos[0]}): the access's "
                              f"operand names '{x.operand}', its object reads '{x.obj}'")
        for kind, obj in sorted({(x.kind, x.obj) for x in fn.accesses if x.obj in objects or
                                 (not x.exact and f.startswith(lib))}):
            if (name, kind, obj) not in named:
                x = fn.group(kind, obj)
                errors.append(f"{name} ({os.path.relpath(f, src)}:{x[0].pos[0]}) has "
                              f"{len(x)} {kind} access(es) of '{obj}' that no row names: "
                              f"register the order it must keep, or mark the group "
                              f"\"-/{len(x)} -\" if it carries no edge of a landed install")
    if errors:
        for e in errors:
            fail(e)
        return 1

    # ---- the order each site states in the source; failures here do not
    # stop the instruction check below.
    bad = 0
    for r, s, fn in sites:
        if not covers(s.succ, r.succ) or not covers(s.fail, r.fail):
            bad += 1
            got = s.succ + (f"/{s.fail}" if s.fail else "")
            fail(f"{r.label()} at {os.path.relpath(s.span[0], src)}:{s.pos[0]}: "
                 f"the source orders it {got}, the register requires {r.want()}")

    compilers = [("gcc", [a.gcc])]
    if a.clang:
        compilers.append(("clang", [a.clang, "--target=aarch64-linux-gnu"]))
    twins = {id(s): [o for o in fn.accesses if o is not s and
                     (o.kind, o.obj, o.succ, o.fail) == (s.kind, s.obj, s.succ, s.fail)]
             for r, s, fn in sites}
    wanted_lines = set()
    for r, s, fn in sites:
        for acc in [s] + twins[id(s)]:
            wanted_lines.update((acc.span[0], ln) for ln in acc.lines())
    cunits = sorted({u for r, s, fn in sites for u in unit_of[r.func]})
    jobs = [(u, c, low, u in cunits) for u in units for c in compilers for low in LOWERINGS]

    def compile_one(job):
        (f, d, flags), (cname, cmd), low, asm_too = job
        base = cmd + LOWERINGS[low] + ["-O2", "-w"] + list(flags)
        rc, pre, err = run(base + ["-E", f], d)
        if rc != 0:
            return job, None, None, err.strip().splitlines()[-1:]
        hits = {}
        if asm_too:
            out = os.path.join(a.work_dir, cname, low,
                               os.path.relpath(f, src).replace("/", "__") + ".s")
            os.makedirs(os.path.dirname(out), exist_ok=True)
            rc, _, err = run(base + ["-g", "-S", f, "-o", out], d)
            if rc != 0:
                return job, None, None, err.strip().splitlines()[-1:]
            asm = open(out, encoding="latin-1").read()
            if not a.keep:
                os.remove(out)
            hits = parse_asm(asm, d, wanted_lines)
        return job, hits, active_lines(pre, d, src), None

    # ---- each compile preprocesses every unit with its own flags: the arms it
    # keeps must be the scanned ones in every function a row names or that
    # touches a registered object, and nowhere may it keep a line the scan
    # lacks that names a registered object.
    relevant = {r.func for r in cover} | {
        fn.name for d in defs.values() for fn in d.values()
        if any(x.obj in objects for x in fn.accesses)}
    token = re.compile(r"\b(" + "|".join(list(map(re.escape, sorted(objects))) + [ATOMIC_OPS])
                       + r")\b")
    # What a line's macros expand to: an atomic builtin (a fence aside) or an
    # expectation must stand on a line the scan read an atomic operation or an
    # expectation on, so a macro defined outside libs/ cannot carry one past it.
    expanded = re.compile(r"\b(?!(?:__atomic|__c11_atomic)_(?:thread|signal)_fence\b)"
                          r"(__atomic_\w+|__sync_\w+|__c11_atomic_\w+|arts_net_rdzv_expect)\s*\(")
    read_lines = {u: {(x.span[0], ln) for fn in fns for x in fn.accesses for ln in x.lines()}
                  for u, fns in unit_funcs.items()}
    expect_lines = {(f, ln) for _, _, f, ln in expects}
    attributed = collections.defaultdict(list)   # (cname, low, (file, line)) -> hits
    arms = set()
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for job, hits, kept, err in ex.map(compile_one, jobs):
            u, (cname, _), low, _ = job
            if hits is None:
                fail(f"{cname} {low} could not compile {os.path.relpath(u[0], src)}: {err}")
                return 1
            for loc, hs in hits.items():
                attributed[(cname, low, loc)] += [(f"{os.path.basename(u[0])}:{fu}", c, t)
                                                  for fu, c, t in hs]
            act, texts = kept
            for fn in unit_funcs[u]:
                if fn.name not in relevant:
                    continue
                extra = tuple(sorted(ln for (f, ln) in act if f == fn.file and
                                     fn.line <= ln <= fn.end_line and ln not in fn.code_lines))
                # A compile may fold a macro call that spans lines onto its
                # first line, so an access counts as kept when any line of its
                # statement is.
                gone = tuple(sorted({x.pos[0] for x in fn.accesses
                                     if not any((fn.file, ln) in act
                                                for ln in range(x.span[1], x.span[2] + 1))}))
                if extra or gone:
                    arms.add(f"{fn.name} ({os.path.relpath(fn.file, src)}): {cname} {low} "
                             f"compiles another conditional arm than the scanned one (lines "
                             f"kept only by the compile: {list(extra) or 'none'}; accesses it "
                             f"drops: {list(gone) or 'none'})")
            for (f, ln), txt in texts.items():
                if (f, ln) not in unit_code[u] and token.search(txt):
                    arms.add(f"{os.path.relpath(f, src)}:{ln}: {cname} {low} compiles a line "
                             f"the scanned arm does not hold, and it holds "
                             f"'{token.search(txt).group(1)}'")
                for m in expanded.finditer(txt):
                    if m.group(1) == "arts_net_rdzv_expect":
                        if re.search(r"\bvoid\s*$", txt[:m.start()]):
                            continue                  # its declaration or definition
                        seen = expect_lines
                    else:
                        seen = read_lines[u]
                    if (f, ln) not in seen:
                        arms.add(f"{os.path.relpath(f, src)}:{ln}: {cname} {low} expands a "
                                 f"macro on this line to {m.group(1)}, which the scan does "
                                 f"not read there")
    for m in sorted(arms):
        bad += 1
        fail(m)

    def copies_of(cname, low, acc):
        copies = collections.defaultdict(list)
        for ln in acc.lines():
            for func, col, tok in attributed.get((cname, low, (acc.span[0], ln)), []):
                if acc.owns(ln, col):
                    copies[func].append(tok)
        return copies

    report = []
    for r, s, fn in sites:
        where = f"{os.path.relpath(s.span[0], src)}:{s.pos[0]}"
        for cname, _ in compilers:
            for low in LOWERINGS:
                own = copies_of(cname, low, s)
                judged = {k: v for k, v in own.items() if atomic_copy(v)}
                via = ""
                if not judged:
                    plain = sorted(k for k, v in own.items()
                                   if any(plain_token(t, r.kind) for t in v))
                    if plain:
                        bad += 1
                        fail(f"{r.label()} at {where}: {cname} {low} emits only a plain "
                             f"{r.kind} for it in {', '.join(plain)}, which carries no "
                             f"{r.want()}")
                        continue
                    for t in twins[id(s)]:
                        judged.update({k: v for k, v in copies_of(cname, low, t).items()
                                       if atomic_copy(v)})
                    via = " (merged with an identical access)" if judged else ""
                if not judged:
                    bad += 1
                    fail(f"{r.label()} at {where}: {cname} {low} emits no atomic "
                         f"instruction for it, so nothing carries the {r.want()} the "
                         f"register requires")
                    continue
                for func, toks in sorted(judged.items()):
                    toks = [t for t in toks if atomic_token(t) or t.startswith("dmb")
                            or t in ("plain:ldp", "plain:stp")]
                    lack = missing(r, toks)
                    shown = " ".join(dict.fromkeys(toks))
                    report.append(f"{'BAD' if lack else 'ok'}\t{r.label()}\t{where}\t"
                                  f"{cname}.{low}\t{func}{via}\t{shown}")
                    if lack:
                        bad += 1
                        fail(f"{r.label()} at {where}: {cname} {low} in {func}{via} "
                             f"emits {shown}: no {' or '.join(lack)}, the register "
                             f"requires {r.want()}")
    with open(os.path.join(a.work_dir, "sites.tsv"), "w") as fp:
        fp.write("\n".join(report) + "\n")
    if bad:
        return 1
    calls = sum(1 for r in live if r.kind == "call")
    marks = sum(1 for r in live if r.marker)
    print(f"PASS {NAME}: cell {a.cell}: {len(sites)} sites, {calls} calls, {marks} marked "
          f"groups; every access of {len(objects)} objects registered; "
          f"{len(cunits)} units x {len(compilers)} compilers x {len(LOWERINGS)} lowerings")
    return 0


if __name__ == "__main__":
    sys.exit(main())
