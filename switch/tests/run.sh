#!/usr/bin/env bash
# Builds and runs the cartridge picker test on the host (Linux/WSL). GBPICKER_SHOW=1 prints every screen.
set -euo pipefail
cd "$(dirname "$0")"
g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/gbpicker_test gbpicker_test.cpp ../gbpicker.cpp
/tmp/gbpicker_test
g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/stick_dpad_test stick_dpad_test.cpp
/tmp/stick_dpad_test

g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/migrate_test migrate_test.cpp
/tmp/migrate_test

# ROM finder/converter used by build-nro.sh (made-up files only).
python3 -I rom_tool_test.py

# Hand-edit patcher for the generated rsp/aspMain.cpp (made-up text only).
python3 -I patch_aspmain_test.py

gcc -O1 -I../../lib/N64ModernRuntime/N64Recomp/include -o /tmp/call_post_test call_post_test.c ../call_post_switch.c
/tmp/call_post_test

# Skin loader + renderer, when a dump from tools/dump_stadium_ui.py is available (STADIUM_DUMP=<dir>).
if [ -n "${STADIUM_DUMP:-}" ]; then
    work=$(mktemp -d)
    python3 -I make_synth_rom.py "$STADIUM_DUMP" "$work"
    g++ -std=c++17 -O1 -Wall -Wextra -o /tmp/skin_test skin_test.cpp ../stadium_skin.cpp
    /tmp/skin_test "$STADIUM_DUMP" "$work"
    g++ -std=c++17 -O1 -Wall -Wextra -I. -o /tmp/gfx_preview gfx_preview.cpp ../gbpicker_gfx.cpp ../stadium_skin.cpp ../gbpicker.cpp
    /tmp/gfx_preview "$STADIUM_DUMP" "$work" && echo "gfx preview: wrote $work/{main,list,message}.ppm"
fi
