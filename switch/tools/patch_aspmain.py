#!/usr/bin/env python3
"""Re-applies the upstream author's hand edits to a freshly generated rsp/aspMain.cpp.

Why this exists
    The Osaka-NX repository does not store rsp/aspMain.cpp, because that file is the game's audio microcode translated to C++
    (see switch/NOTICE.md). It is made from YOUR ROM by switch/tools/gen_rsp.sh, which runs RSPRecomp. The upstream project
    (mstan/PokemonStadiumRecomp, GPL-3.0) kept a copy of that output with three hand edits at the end of the file. This script
    makes the same three edits to the new output, so the behaviour stays the same. It adds nothing from the game: the text it
    inserts is the upstream author's own code and comments, copied here with their explanation.

The edits (all inside the function aspMain_impl)
    1. `#include <set>` and `#include <utility>` after the two librecomp includes (needed by edit 4).
    2. In the jump-dispatch `switch`: `case 0x1000: return RspExitReason::Broke;`. A command table entry of 0 means "end of list";
       on real hardware that would land at IMEM[0] and break, and returning Broke is the same thing here.
    3. A comment on `case 0x1BB4:` (the label itself comes from `extra_indirect_branch_targets` in aspMain.us.1.0.toml, where 0x1BB4
       is listed for the reason given in that comment).
    4. After the switch: an unknown dispatch target no longer prints a register dump and aborts. It logs once per
       (target, source line) and returns Broke, so the audio track that needed it stays silent and the game goes on.

It stops with an error when the generated file does not look as expected (an anchor is missing or found twice), so a change of
RSPRecomp output can never be patched wrongly in silence. Running it twice is harmless.

Usage: python3 -I switch/tools/patch_aspmain.py rsp/aspMain.cpp
"""
import re
import sys

MARKER = "unknown command-dispatch target"

INCLUDES_ANCHOR = '#include "librecomp/rsp_vu_impl.hpp"\n'
INCLUDES_ADD = "#include <set>\n#include <utility>\n"

SWITCH_HEAD = re.compile(r"(    switch \(\(jump_target \| 0x1000\) & 0X1FFF\) \{)[ \t]*\n")
SWITCH_ADD = """
        // Audio command-list dispatch: a halfword of 0x0000 in the
        // command table means "end of list" - the audio task is
        // done. On real RSP, `jr $0` would land at IMEM[0] which
        // breaks. Returning Broke matches that semantic and lets
        // the next queued audio task run cleanly. This case is
        // hit by Stadium's menu-music tasks that end with a 0
        // sentinel in the dispatch table.
        case 0x1000: return RspExitReason::Broke;
"""

CASE_1BB4 = "        case 0x1BB4: goto L_1BB4;\n"
CASE_1BB4_COMMENT = """        // Stadium menu-music dispatch - 0xCBB4 (high bit flag + handler
        // 0xBB4) maps via the OR/AND to 0x1BB4. The static analysis
        // missed L_1BB4 because it's mid-block of L_1BB0; it is listed in
        // extra_indirect_branch_targets so the label and case exist and
        // the dispatch lands.
"""

# From the closing brace of the dispatch switch to the end of aspMain_impl, as RSPRecomp writes it (abort with a register dump).
TAIL = re.compile(
    r'\n    \}\n    fprintf\(stderr, "Unhandled jump target 0x%04X in microcode aspMain.*?'
    r"    return RspExitReason::UnhandledJumpTarget;\n\}", re.S)
TAIL_NEW = """
    }
    // Stadium's audio tasks use multiple ucode_data variants with
    // dispatch tables the static analysis didn't fully see. When
    // execution lands on an unknown command-handler offset, the
    // semantic of "audio command we don't recognize" is the same as
    // hitting end-of-list: stop processing this task cleanly so the
    // next task can run. Returning Broke matches that - runner
    // continues, audio for this specific track is silent, no fatal.
    //
    // We log once per (target, debug_line) so the diagnostic is
    // visible without spamming. Keeping the rate-limited log lets us
    // identify which audio tracks have un-handled commands and feed
    // that back into RSPRecomp's static analysis as additional
    // emit-table entries.
    {
        static std::set<std::pair<uint32_t, int>> warned;
        auto key = std::make_pair(jump_target, debug_line);
        if (warned.insert(key).second) {
            fprintf(stderr,
                "[aspMain] unknown command-dispatch target 0x%X from [%s:%d]; "
                "treating as end-of-list (returning Broke). This audio track "
                "will be silent. r1=%08X r25=%08X r26=%08X r28=%08X r29=%08X\\n",
                (unsigned)jump_target, debug_file, debug_line,
                r1, r25, r26, r28, r29);
            fflush(stderr);
        }
    }
    return RspExitReason::Broke;
}"""


def patch(text):
    """Returns the patched text. Raises ValueError when the input is not what RSPRecomp writes."""
    if MARKER in text:
        return text                                   # already patched
    def once(count, what):
        if count != 1:
            raise ValueError("expected exactly one %s in the generated file, found %d" % (what, count))
    once(text.count(INCLUDES_ANCHOR), "librecomp/rsp_vu_impl.hpp include")
    once(len(SWITCH_HEAD.findall(text)), "jump-dispatch switch")
    once(text.count(CASE_1BB4), "'case 0x1BB4' (is 0x1BB4 listed in extra_indirect_branch_targets?)")
    once(len(TAIL.findall(text)), "unhandled-jump-target block")
    if "case 0x1000:" in text:
        raise ValueError("the generated file already has a 'case 0x1000'; this script would add a duplicate")
    text = text.replace(INCLUDES_ANCHOR, INCLUDES_ANCHOR + INCLUDES_ADD)
    text = SWITCH_HEAD.sub(lambda m: m.group(1) + SWITCH_ADD, text)
    text = text.replace(CASE_1BB4, CASE_1BB4_COMMENT + CASE_1BB4)
    text = TAIL.sub(lambda m: TAIL_NEW, text)
    return text


def main(argv):
    if len(argv) != 2:
        print(__doc__.split("Usage:")[1].strip(), file=sys.stderr)
        return 2
    path = argv[1]
    with open(path, "r", encoding="utf-8", newline="") as f:
        text = f.read()
    try:
        new = patch(text)
    except ValueError as e:
        print("patch_aspmain.py: %s: %s" % (path, e), file=sys.stderr)
        return 1
    if new != text:
        with open(path, "w", encoding="utf-8", newline="") as f:
            f.write(new)
        print("patch_aspmain.py: patched %s" % path)
    else:
        print("patch_aspmain.py: %s was already patched" % path)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
