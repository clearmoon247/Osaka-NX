#!/usr/bin/env python3
"""Tests switch/tools/patch_aspmain.py on made-up text shaped like RSPRecomp's output (no game data involved).
Run: python3 -I patch_aspmain_test.py"""
import importlib.util, os, subprocess, sys, tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TOOL = os.path.join(HERE, "..", "tools", "patch_aspmain.py")
spec = importlib.util.spec_from_file_location("patch_aspmain", TOOL)
pa = importlib.util.module_from_spec(spec); spec.loader.exec_module(pa)

SAMPLE = '''#include "rsp.h"
#include "librecomp/rsp_vu_impl.hpp"
RspExitReason aspMain(uint8_t* rdram, uint32_t ucode_addr) {
    uint32_t jump_target = 0;
    switch ((jump_target | 0x1000) & 0X1FFF) { 
        case 0x1BB0: goto L_1BB0;
        case 0x1BB4: goto L_1BB4;
    }
    fprintf(stderr, "Unhandled jump target 0x%04X in microcode aspMain\\n", jump_target);
    fprintf(stderr, "r1 = %08X\\n", r1);
    return RspExitReason::UnhandledJumpTarget;
}
'''

fails = 0
def check(cond, what):
    global fails
    if not cond:
        fails += 1; print("FAIL:", what)

out = pa.patch(SAMPLE)
check(out != SAMPLE, "patch changes the sample")
check(out.count("unknown command-dispatch target") == 1, "log line added once")
check(out.count("case 0x1000: return RspExitReason::Broke;") == 1, "case 0x1000 added once")
check("#include <set>" in out and "#include <utility>" in out, "includes added")
check("Unhandled jump target" not in out, "old abort block replaced")
check("UnhandledJumpTarget" not in out, "no UnhandledJumpTarget return left")
check(out.rstrip().endswith("}"), "function still closed")
check(out.index("case 0x1000:") < out.index("case 0x1BB0:"), "case 0x1000 sits at the top of the switch")
check(out.index("menu-music dispatch") < out.index("case 0x1BB4:"), "comment sits before case 0x1BB4")
check(pa.patch(out) == out, "second run changes nothing (idempotent)")

def must_fail(text, what):
    try:
        pa.patch(text); check(False, what + " (no error raised)")
    except ValueError:
        pass
must_fail(SAMPLE.replace("        case 0x1BB4: goto L_1BB4;\n", ""), "missing 0x1BB4 anchor is an error")
must_fail(SAMPLE.replace('#include "librecomp/rsp_vu_impl.hpp"\n', ""), "missing include anchor is an error")
must_fail(SAMPLE.replace("Unhandled jump target", "Something else"), "missing tail is an error")
must_fail(SAMPLE.replace("case 0x1BB0:", "case 0x1000: goto X;\n        case 0x1BB0:"), "existing case 0x1000 is an error")
must_fail(SAMPLE + SAMPLE.split('#include "librecomp/rsp_vu_impl.hpp"\n')[1], "duplicated anchors are an error")

# Command line: patches a file in place, then reports nothing to do; a bad file exits 1.
with tempfile.TemporaryDirectory() as d:
    p = os.path.join(d, "aspMain.cpp")
    open(p, "w").write(SAMPLE)
    r = subprocess.run([sys.executable, "-I", TOOL, p], capture_output=True, text=True)
    check(r.returncode == 0 and "unknown command-dispatch target" in open(p).read(), "CLI patches the file")
    r = subprocess.run([sys.executable, "-I", TOOL, p], capture_output=True, text=True)
    check(r.returncode == 0, "CLI second run succeeds")
    open(p, "w").write("nothing useful\n")
    r = subprocess.run([sys.executable, "-I", TOOL, p], capture_output=True, text=True)
    check(r.returncode == 1, "CLI exits 1 on unexpected input")

print("patch_aspmain_test: %s" % ("FAILED (%d)" % fails if fails else "all passed"))
sys.exit(1 if fails else 0)
