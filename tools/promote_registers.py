#!/usr/bin/env python3
"""Guest registers in C locals for each translated function .

Nakagawa's generated C keeps the guest's state in CpuState: every instruction loads its operands from s->r[] and stores its result
back, and since the code is built without strict aliasing, every guest store to memory makes the compiler load them all again. This
pass gives each function its own copy of the integer registers, the FPU registers, HI, LO and the FPU condition (C locals: the
compiler keeps them in host registers) and keeps CpuState in step only where something else can see it:

  - before a call that hands `s` to code that reads or writes the registers (another translated function, the dispatcher, a
    syscall, the call boundary, a tail drain, the VFPU interpreter): the locals written since the last such point (a may-dirty
    dataflow over the function's statements, labels iterated to a fixpoint) are stored; after it, all are loaded again;
  - at a return and at the function's end: the dirty locals are stored;
  - at a timeslice yield: both, only on its slow path (SR_YIELD_SYNC in recomp.h).

A function that hands `s` to anything this pass doesn't know, or yields from inside a `for`, is left as it was (and counted). The
instruction trace hooks (sr_begin/sr_end, compiled out in a release build) are removed.

    python3 -I promote_registers.py IN.c OUT.c        (or: import promote_registers; promote_text(text))
"""
import re
import sys

# calls that hand `s` over and need the registers in CpuState: (name or prefix, returns a value)
SYNC_VALUE = {"dispatch_call_checked", "sr_aot_return_boundary_begin", "sr_aot_return_boundary_end", "sr_vfpu_interp",
              "sr_syscall"}
SYNC_VOID = {"SR_TAIL_DRAIN", "sr_break"}
# calls that take `s` but never touch the promoted state (VFPU operands, the return capture, fatal reports)
NO_SYNC = {"sr_vread", "sr_vwrite", "SR_AOT_RETURN_CAPTURE", "SR_CODE_POINTER", "sr_tail_record", "sr_begin", "sr_end"}
YIELD = "SR_YIELD"
KEYWORDS = {"if", "for", "while", "switch", "return", "sizeof", "__typeof__", "__builtin_expect"}

TOKEN = re.compile(r'\s+|/\*.*?\*/|//[^\n]*|"(?:\\.|[^"\\\n])*"|\'(?:\\.|[^\'\\\n])*\'|'
                   r'0[xX][0-9a-fA-F]+(?:\.[0-9a-fA-F]*)?(?:[pP][+-]?\d+)?[uUlLfF]*|'
                   r'\d+\.?\d*(?:[eE][+-]?\d+)?[uUlLfF]*|\.\d+(?:[eE][+-]?\d+)?[fF]?|'
                   r'[A-Za-z_]\w*|<<=|>>=|->|\+\+|--|<<|>>|<=|>=|==|!=|&&|\|\||[-+*/%&|^]=|.', re.S)
TRACE = re.compile(r'sr_begin\(s, 0x[0-9a-f]+u, 0x[0-9a-f]+u\); ?|sr_end\(s, [^;]*\); ?')
FUNC = re.compile(r'^void ([fr]_[0-9a-f]+)\(CpuState \*s\) \{\n', re.M)
ASSIGN = {"=", "+=", "-=", "*=", "/=", "%=", "&=", "|=", "^=", "<<=", ">>="}
SCALARS = {"hi": 64, "lo": 65, "fpcond": 66, "fcr31": 67}

STATS = {"functions": 0, "promoted": 0, "kept": 0, "syncs": 0, "yields": 0, "returns": 0}
UNKNOWN = {}
REASONS = {}


def local(reg, view=None):
    if reg < 32:
        return f"_g{reg}"
    if reg < 64:
        return f"_F{reg - 32}.{view or 'i'}"
    return "_g" + [k for k, v in SCALARS.items() if v == reg][0]


def home(reg):
    if reg < 32:
        return f"s->r[{reg}]"
    if reg < 64:
        return f"s->fi[{reg - 32}]"
    return "s->" + [k for k, v in SCALARS.items() if v == reg][0]


def bits(mask):
    out, r = [], 0
    while mask:
        if mask & 1:
            out.append(r)
        mask >>= 1
        r += 1
    return out


def store(mask):
    return "".join(f"{home(r)} = {local(r)}; " for r in bits(mask))


def load(mask):
    return "".join(f"{local(r)} = {home(r)}; " for r in bits(mask))


class Keep(Exception):
    pass


def promote_body(body):
    """The function's body text (after its opening brace, before its closing one) with the state in locals, or None."""
    toks = TOKEN.findall(body)
    sig = [i for i, x in enumerate(toks) if not (x[0].isspace() or x.startswith("/*") or x.startswith("//"))]
    t = [toks[i] for i in sig]
    n = len(t)
    t.append(";")  # a guard past the end
    match = {}
    stack = []
    for k, x in enumerate(t[:n]):
        if x in "([{":
            stack.append(k)
        elif x in ")]}":
            if not stack:
                raise Keep("unbalanced")
            match[stack.pop()] = k
    if stack:
        raise Keep("unbalanced")

    # the state's references: sig index -> (reg, view, length, is_write)
    refs = {}
    used = 0
    for k in range(n - 2):
        if t[k] != "s" or t[k + 1] != "->":
            continue
        f = t[k + 2]
        if f in ("r", "f", "fi") and t[k + 3] == "[" and t[k + 5] == "]" and t[k + 4].rstrip("uU").isdigit():
            idx = int(t[k + 4].rstrip("uU"))
            reg = idx if f == "r" else 32 + idx
            view = None if f == "r" else ("f" if f == "f" else "i")
            length = 6
        elif f in SCALARS:
            reg, view, length = SCALARS[f], None, 3
        elif f == "r" or f == "f" or f == "fi":
            raise Keep("a register by a computed index")
        else:
            continue
        nxt = t[k + length]
        write = nxt in ASSIGN or nxt in ("++", "--") or (k > 0 and t[k - 1] in ("++", "--"))
        if k > 1 and t[k - 1] == "&" and not (t[k - 2][0].isalnum() or t[k - 2][0] == "_" or t[k - 2] in (")", "]")):
            raise Keep("a register's address taken")  # a unary &, not an AND
        refs[k] = (reg, view, length, write)
        used |= 1 << reg

    # the calls that take `s`: sig index of the name -> kind
    calls = {}
    for k in range(n - 1):
        if t[k + 1] != "(" or not (t[k][0].isalpha() or t[k][0] == "_"):
            continue
        if t[k] in KEYWORDS:
            continue
        close = match[k + 1]
        takes_s = False
        j = k + 2
        while j < close:  # its own arguments only (a nested call's are its own)
            if t[j] in "([{":
                j = match[j] + 1
                continue
            if t[j] == "s" and t[j - 1] in ("(", ",") and t[j + 1] in (")", ","):
                takes_s = True
                break
            j += 1
        if not takes_s:
            continue
        name = t[k]
        if name in NO_SYNC:
            continue
        if name == YIELD:
            calls[k] = "yield"
        elif name in SYNC_VALUE:
            calls[k] = "value"
        elif name in SYNC_VOID or re.fullmatch(r"[fr]_[0-9a-f]+", name):
            calls[k] = "void"
        else:
            UNKNOWN[name] = UNKNOWN.get(name, 0) + 1
            raise Keep("hands s to " + name)

    label_in = {}
    sites = {}  # sig index -> dirty mask at a sync call, a yield, a return
    state = {"end": 0}

    def join(a, b):
        if a is None:
            return b
        if b is None:
            return a
        return a | b

    def expr(i, j, d):
        """Tokens [i, j): the writes and syncs in order (an assignment's right side before its target)."""
        conditional = any(t[x] in ("&&", "||", "?") for x in range(i, j))
        depth = 0
        for a in range(i, j):  # a top-level assignment to the state: its right side first
            x = t[a]
            if x in "([{":
                depth += 1
            elif x in ")]}":
                depth -= 1
            elif depth == 0 and x in ASSIGN:
                if i in refs and refs[i][2] == a - i:
                    d = expr(a + 1, j, d)
                    return d if d is None else d | (1 << refs[i][0])
                break
        k = i
        while k < j:
            if k in calls:
                close = match[k + 1]
                d = expr(k + 2, close, d)
                sites[k] = d or 0
                if calls[k] != "yield" and not conditional and d is not None:
                    d = 0
                k = close + 1
                continue
            if k in refs:
                reg, _, length, write = refs[k]
                if write and d is not None:
                    d |= 1 << reg
                k += length
                continue
            k += 1
        return d

    def stmt(i, d):
        x = t[i]
        if x == "{":
            end = match[i]
            i += 1
            while i < end:
                i, d = stmt(i, d)
            return end + 1, d
        if x == "if":
            close = match[i + 1]
            d = expr(i + 2, close, d)
            i2, d1 = stmt(close + 1, d)
            if t[i2] == "else":
                i3, d2 = stmt(i2 + 1, d)
                return i3, join(d1, d2)
            return i2, join(d1, d)
        if x == "for":
            close = match[i + 1]
            i2, _ = stmt(close + 1, d)  # its extent
            if any(k in calls for k in range(i, i2)):
                raise Keep("a call inside a for")
            writes = 0
            for k in range(i, i2):
                if k in refs and refs[k][3]:
                    writes |= 1 << refs[k][0]
            if d is None:
                return i2, None
            d |= writes
            stmt(close + 1, d)  # again, every iteration's writes dirty from the start: a return or goto inside sees them all
            return i2, d
        if x == "goto":
            lab = t[i + 1]
            if d is not None:
                label_in[lab] = label_in.get(lab, 0) | d
            return i + 3, None
        if x == "return":
            if t[i + 1] != ";":
                raise Keep("a return with a value")
            sites[i] = d or 0
            return i + 2, None
        if (x[0].isalpha() or x[0] == "_") and t[i + 1] == ":" and x not in ("case", "default"):
            return i + 2, join(d, label_in.get(x))  # any label (L_..., _sr_cont_..., _sr_fallthrough_return): its gotos join here
        if x in ("while", "do", "switch", "case", "default", "break", "continue"):
            raise Keep("a " + x)
        if x == ";":
            return i + 1, d
        j = i
        depth = 0
        while not (t[j] == ";" and depth == 0):
            if t[j] in "([{":
                depth += 1
            elif t[j] in ")]}":
                depth -= 1
            j += 1
        return j + 1, expr(i, j, d)

    for _ in range(64):
        before = dict(label_in)
        sites.clear()
        i, d = 0, 0
        while i < n:
            i, d = stmt(i, d)
        state["end"] = d
        if label_in == before:
            break
    else:
        raise Keep("no fixpoint")

    # the new text
    out = list(toks)
    for k, (reg, view, length, _) in refs.items():
        out[sig[k]] = local(reg, view)
        for m in range(1, length):
            out[sig[k + m]] = ""
    reload = load(used)
    for k, kind in calls.items():
        close = sig[match[k + 1]]
        wb = store(sites.get(k, 0))
        if kind == "yield":
            out[sig[k]] = "SR_YIELD_SYNC"
            out[close] = f", {wb}, {reload})"
            STATS["yields"] += 1
        elif kind == "value":
            out[sig[k]] = "({ " + wb + "__auto_type _sr_v = " + out[sig[k]]
            out[close] = ")" + "; " + reload + "_sr_v; })"
            STATS["syncs"] += 1
        else:
            out[sig[k]] = "({ " + wb + out[sig[k]]
            out[close] = ")" + "; " + reload + "})"
            STATS["syncs"] += 1
    for k in range(n):
        if t[k] == "return" and k in sites:
            out[sig[k]] = "{ " + store(sites[k]) + "return"
            out[sig[k + 1]] = "; }"
            STATS["returns"] += 1
    decl = []
    gprs = [r for r in bits(used) if r < 32]
    fprs = [r - 32 for r in bits(used) if 32 <= r < 64]
    scal = [local(r) for r in bits(used) if r >= 64]
    if gprs or scal:
        decl.append("uint32_t " + ", ".join([f"_g{r}" for r in gprs] + scal) + ";")
    if fprs:
        decl.append("union { float f; uint32_t i; } " + ", ".join(f"_F{r}" for r in fprs) + ";")
    head = "    " + " ".join(decl) + " " + reload + "\n" if used else ""
    tail = "    " + store(state["end"] or 0) + "\n" if state["end"] else ""
    return head + "".join(out) + tail


def promote_text(text):
    text = TRACE.sub("", text)
    out = []
    pos = 0
    for m in FUNC.finditer(text):
        start = m.end()
        end = text.index("\n}\n", start) + 1  # the body's closing brace at column 0
        out.append(text[pos:start])
        body = text[start:end]
        STATS["functions"] += 1
        try:
            out.append(promote_body(body))
            STATS["promoted"] += 1
        except Keep as why:
            out.append(body)
            STATS["kept"] += 1
            REASONS[str(why)] = REASONS.get(str(why), 0) + 1
        pos = end
    out.append(text[pos:])
    return "".join(out)


def promote_job(text):
    """For a process pool: the promoted text and this process's counts."""
    for k in STATS:
        STATS[k] = 0
    REASONS.clear()
    return promote_text(text), dict(STATS), dict(REASONS)


def summary():
    s = STATS
    unk = ", ".join(f"{k} {v}" for k, v in sorted(REASONS.items(), key=lambda kv: -kv[1])[:8])
    return (f"promote: {s['promoted']} of {s['functions']} functions ({s['kept']} kept as they were{': ' + unk if unk else ''}); "
            f"{s['syncs']} calls, {s['yields']} yields, {s['returns']} returns")


if __name__ == "__main__":
    src, dst = sys.argv[1], sys.argv[2]
    result = promote_text(open(src).read())
    open(dst, "w").write(result)
    print(summary())
