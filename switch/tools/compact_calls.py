#!/usr/bin/env python3
"""Rewrites the call-site bookkeeping in generated/funcs_*.c into calls of shared helpers (switch/call_post_switch.c).

The recompiler emits the same ~20 lines after every jal/jalr (tail-call handling, stack-pointer check): about 1.09 million
copies in this game, roughly 30% of the compiled code. Each is replaced by one call that does exactly the same:

    if (psr_call(rdram, ctx, func_8140C038, 0x8433CC24u)) return;

Call sites in functions that have local labels reachable by a tail call also carry a switch with one case per label (30 or
more cases, at every call site). Those use a per-function table and one shared dispatch at the end of the function.

A block is only rewritten when it matches the recompiler's text exactly; anything else is left alone and counted. Names of
symbols are unchanged, line numbers change. Idempotent. The helpers' behaviour is the original code; tests/call_post_test.c
checks that on random states.

    compact_calls.py generated/funcs_9.c ...          rewrite in place
    compact_calls.py --check generated/*.c            report only, change nothing
    compact_calls.py --style=post generated/*.c       keep the direct call, move only the bookkeeping (less saving)
"""
import os, re, sys

def note(name):
    return 'recomp_cf_note("%s", (uint32_t)recomp_call_sp, recomp_prev_host_return, recomp_call_host_return, ctx);' % name

HEAD = ("    {\n"
        "    gpr recomp_call_sp = ctx->r29;\n"
        "    uint32_t recomp_prev_host_return = ctx->host_return_target;\n"
        "    uint32_t recomp_call_host_return = (uint32_t)ctx->r31;\n"
        "    ctx->host_return_target = recomp_call_host_return;\n"
        "    @CALL@;\n"
        "    if (ctx->tailcall_pending) {\n")
RET_BRANCH = ("        if (ctx->tailcall_target == @RET@) {\n"
              "            " + note("call-return-tailcall") + "\n"
              "            ctx->tailcall_pending = 0;\n"
              "            ctx->tailcall_target = 0;\n"
              "            ctx->tailcall_func = 0;\n"
              "            ctx->host_return_target = recomp_prev_host_return;\n"
              "            if (ctx->r29 != recomp_call_sp) { " + note("call-sp-mismatch") + " return; }\n"
              "            goto @LABEL@;\n"
              "        }\n")
CASE_T = ("            case @C@:\n"
          "                " + note("call-local-tailcall") + "\n"
          "                ctx->tailcall_pending = 0;\n"
          "                ctx->tailcall_target = 0;\n"
          "                ctx->tailcall_func = 0;\n"
          "                ctx->host_return_target = recomp_prev_host_return;\n"
          "                if (ctx->r29 != recomp_call_sp) { " + note("call-sp-mismatch") + " return; }\n"
          "                goto @L@;\n")
SWITCH_T = ("        if (ctx->tailcall_target != 0) {\n"
            "            switch ((uint32_t)ctx->tailcall_target) {\n"
            "@CASES@"
            "            default: break;\n"
            "            }\n"
            "        }\n")
TAIL = ("        if (ctx->tailcall_dispatching >= 64u) {\n"
        "            " + note("call-bubble-dispatch") + "\n"
        "            ctx->host_return_target = recomp_prev_host_return;\n"
        "            return;\n"
        "        }\n"
        "        recomp_handle_tailcalls(rdram, ctx);\n"
        "    }\n"
        "    ctx->host_return_target = recomp_prev_host_return;\n"
        "    if (ctx->r29 != recomp_call_sp) { " + note("call-sp-mismatch") + " return; }\n"
        "}\n")

PH = {"@CALL@": r"(?P<call>[^\n;]+)", "@RET@": r"(?P<ret>0x[0-9A-Fa-f]+u)", "@LABEL@": r"(?P<label>L_[0-9A-Fa-f]+)"}
def rx(template, extra=None):
    ph = dict(PH); ph.update(extra or {})
    out = []
    for p in re.split("(" + "|".join(map(re.escape, ph)) + ")", template):
        out.append(ph[p] if p in ph else re.escape(p))
    return "".join(out)

CASE_RX = rx(CASE_T, {"@C@": r"(?P<c>0x[0-9A-Fa-f]+u)", "@L@": r"(?P<l>L_[0-9A-Fa-f]+)"})
CASE_ONE = re.compile(CASE_RX)
# After the block the recompiler emits "goto after_N;", a dead copy of the delay slot and then "after_N:" + the return label.
# The label the block jumps to is that same label, so "continue after the call" and "goto the label" are the same thing.
FOLLOW = r"(?P<follow>        goto (?P<after>after_\d+);\n(?:(?:    [^\n]*)?\n){0,12}?    (?P=after):\n(?P<next>L_[0-9A-Fa-f]+):\n)?"
RX       = re.compile(rx(HEAD + RET_BRANCH + TAIL) + FOLLOW)
RX_NL    = re.compile(rx(HEAD + TAIL))
RX_LOCAL = re.compile(rx(HEAD + RET_BRANCH + SWITCH_T + TAIL, {"@CASES@": r"(?P<cases>(?:" + CASE_RX.replace("(?P<c>", "(?:").replace("(?P<l>", "(?:") + r")+)"}) + FOLLOW)

CALL_FN = re.compile(r"^(?P<fn>[A-Za-z_][A-Za-z0-9_]*)\(rdram, ctx\)$")
CALL_LK = re.compile(r"^(?P<fn>LOOKUP_FUNC\(.+\))\(rdram, ctx\)$")
def callee(call):
    m = CALL_FN.match(call) or CALL_LK.match(call)
    return m.group("fn") if m else None

def keep_head(call):
    return ("    {\n"
            "    gpr recomp_call_sp = ctx->r29;\n"
            "    uint32_t recomp_prev_host_return = ctx->host_return_target;\n"
            "    ctx->host_return_target = (uint32_t)ctx->r31;\n"
            "    %s;\n") % call

class Rewriter:
    def __init__(self, style):
        self.style = style
        self.count = 0

    def sub(self, m):
        call, ret, label = m.group("call"), m.group("ret"), m.group("label")
        fn = callee(call)
        falls_through = m.group("follow") is not None and m.group("next") == label
        self.count += 1
        if self.style == "wrap" and fn and falls_through:
            return "    if (psr_call(rdram, ctx, %s, %s)) return;\n%s" % (fn, ret, m.group("follow"))
        return keep_head(call) + (
            "    switch (psr_after_call(rdram, ctx, recomp_call_sp, recomp_prev_host_return, %s)) { case 1: return; case 2: goto %s; }\n}\n"
            % (ret, label)) + (m.group("follow") or "")

    def sub_nl(self, m):
        call = m.group("call"); fn = callee(call)
        self.count += 1
        if self.style == "wrap" and fn:
            return "    if (psr_call_nl(rdram, ctx, %s)) return;\n" % fn
        return keep_head(call) + "    if (psr_after_call_nl(rdram, ctx, recomp_call_sp, recomp_prev_host_return)) return;\n}\n"

FUNC_RX = re.compile(r"^RECOMP_FUNC void (\w+)\(uint8_t\* rdram, recomp_context\* ctx\) \{\n", re.M)

def rewrite_local(span, name):
    """One function: replace the local-switch call sites with psr_call_l + a shared dispatch. Returns (new_span, sites)."""
    cases = []
    sites = [0]
    def sub(m):
        cs = [(c.group("c"), c.group("l")) for c in CASE_ONE.finditer(m.group("cases"))]
        call, ret = m.group("call"), m.group("ret")
        fn = callee(call)
        if (not fn or m.group("follow") is None or m.group("next") != m.group("label")
                or any(l != "L_" + c[2:-1] for c, l in cs) or (cases and cases != cs)):
            return m.group(0)
        if not cases:
            cases.extend(cs)
        sites[0] += 1
        return ("    psr_idx = psr_call_l(rdram, ctx, %s, %s, psr_tbl_%s);\n"
                "    if (psr_idx) { if (psr_idx == 1) return; psr_idx -= 2; goto psr_local; }\n%s") % (fn, ret, name, m.group("follow"))
    new = RX_LOCAL.sub(sub, span)
    if not sites[0]:
        return span, 0
    table = "static const uint32_t psr_tbl_%s[] = { %s0 };\n" % (name, "".join(c[:-1] + ", " for c, _ in cases))
    dispatch = "    psr_local:\n    switch (psr_idx) {\n" + "".join(
        "    case %d: goto %s;\n" % (i, l) for i, (c, l) in enumerate(cases[:-1])) + "    default: goto %s;\n    }\n" % cases[-1][1]
    end = new.rfind("\n;}")            # the recompiler ends every function with ";}" on its own line
    if end < 0 or new[end:].strip() != ";}":
        return span, 0
    head_end = FUNC_RX.match(new).end()
    new = table + new[:head_end] + "    uint32_t psr_idx;\n" + new[head_end:end + 1] + dispatch + new[end + 1:]
    return new, sites[0]

DECL = re.compile(r"^void ([A-Za-z_][A-Za-z0-9_]*)\(uint8_t\* rdram, recomp_context\* ctx\);$", re.M)
USED = re.compile(r"psr_call(?:_nl|_l)?\(rdram, ctx, ([A-Za-z_][A-Za-z0-9_]*)[,)]")

def declared_names(cache, directory):
    if directory not in cache:
        try:
            cache[directory] = set(DECL.findall(open(directory + "/funcs.h", encoding="utf-8").read()))
        except OSError:
            cache[directory] = set()
    return cache[directory]

def add_prototypes(text, known):
    """Callees such as osDpSetStatus_recomp are never declared (a plain call to an undeclared function compiles, taking its
    address does not), so declare the ones used as values here."""
    have = set(DECL.findall(text)) | known
    missing = sorted(set(USED.findall(text)) - have)
    if not missing:
        return text
    protos = "".join("void %s(uint8_t* rdram, recomp_context* ctx);\n" % n for n in missing)
    marker = '#include "funcs.h"\n'
    i = text.find(marker)
    return text[:i + len(marker)] + protos + text[i + len(marker):] if i >= 0 else protos + text

def process(text, style, cache, directory):
    rw = Rewriter(style)
    local = 0
    if style == "wrap" and "switch ((uint32_t)ctx->tailcall_target)" in text:
        starts = [m.start() for m in FUNC_RX.finditer(text)]
        if starts:
            starts.append(len(text))
            parts = [text[:starts[0]]]
            for a, b in zip(starts, starts[1:]):
                span = text[a:b]
                name = FUNC_RX.match(span).group(1)
                span, n = rewrite_local(span, name)
                local += n
                parts.append(span)
            text = "".join(parts)
    text = RX.sub(rw.sub, text)
    text = RX_NL.sub(rw.sub_nl, text)
    total = rw.count + local
    if total:
        text = add_prototypes(text, declared_names(cache, directory))
    return text, total

def main(argv):
    check = "--check" in argv
    style = "wrap"
    for a in argv:
        if a.startswith("--style="):
            style = a.split("=", 1)[1]
    if style not in ("wrap", "post"):
        print("unknown style", style); return 2
    cache = {}
    done = left = 0
    for f in [a for a in argv if not a.startswith("--")]:
        s = open(f, encoding="utf-8").read()
        new, n = process(s, style, cache, os.path.dirname(os.path.abspath(f)))
        done += n
        left += new.count("recomp_handle_tailcalls(rdram, ctx);")
        if n and not check:
            open(f, "w", encoding="utf-8").write(new)
    print("compact_calls (%s): %d call sites %s, %d unrecognised blocks left as they were" %
          (style, done, "found" if check else "rewritten", left))
    return 0

if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
