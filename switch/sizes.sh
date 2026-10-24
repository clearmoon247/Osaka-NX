#!/usr/bin/env bash
# Where do the bytes of the .nro go? Run after a build; writes build/size_report.txt (and prints it). Send that file back.
#   switch/sizes.sh            report on build/osaka-nx.elf and the static libraries under build/
# Only loaded sections end up in the .nro (code, read-only data, data); debug info and symbols do not.
# TOOLCHAIN_PREFIX overrides the tool prefix (default: devkitA64's aarch64-none-elf-).
cd "$(dirname "$0")/build" || exit 1
PREFIX="${TOOLCHAIN_PREFIX-${DEVKITPRO:-/opt/devkitpro}/devkitA64/bin/aarch64-none-elf-}"
SIZE="${PREFIX}size"; NM="${PREFIX}nm"; OBJDUMP="${PREFIX}objdump"
command -v "$SIZE" >/dev/null || { echo "size tool not found: $SIZE (set DEVKITPRO or TOOLCHAIN_PREFIX)" >&2; exit 1; }
ELF="${ELF:-osaka-nx.elf}"
OUT=size_report.txt

{
echo "== files"
ls -la osaka-nx.nro "$ELF" 2>/dev/null
echo
echo "== loaded sections of $ELF (MB), largest first"
"$SIZE" -A "$ELF" | awk 'NR>2 && $1 != "Total" && $2 > 0 { printf "  %-24s %10d bytes  %8.2f MB\n", $1, $2, $2/1048576 }' | sort -k2 -n -r | head -25
echo
echo "== unwind/exception data (can be dropped for C code that never throws)"
"$SIZE" -A "$ELF" | awk '$1 ~ /^\.(eh_frame|eh_frame_hdr|gcc_except_table|ARM\.exidx|ARM\.extab)$/ { printf "  %-24s %10d bytes  %8.2f MB\n", $1, $2, $2/1048576 }'
echo
echo "== static libraries: code+data (MB), largest first"
for a in $(find . -name '*.a' -size +100k 2>/dev/null); do
    "$SIZE" -t "$a" 2>/dev/null | awk -v n="$a" '$1 ~ /^[0-9]+$/ { last = $4 } END { printf "  %10.2f MB  %s\n", last/1048576, n }'
done | sort -n -r | head -15
echo
echo "== the 15 biggest object files in the recompiled-functions library"
A=$(find . -name 'libRecompiledFuncs.a' | head -1)
[ -n "$A" ] && "$SIZE" "$A" 2>/dev/null | awk 'NR>1 && $1 ~ /^[0-9]+$/ { printf "  %10.2f MB text, %6.2f MB data  %s\n", $1/1048576, $2/1048576, $6 }' | sort -n -r | head -15
echo
echo "== symbol counts and sizes"
"$NM" -S --size-sort --radix=d "$ELF" 2>/dev/null | awk 'NF >= 4 { n++; total += $2 } END { printf "  %d sized symbols, %.1f MB in total\n", n, total/1048576 }'
echo "  largest 12 symbols over 64 KiB:"
"$NM" -S --size-sort --radix=d -C "$ELF" 2>/dev/null | awk 'NF >= 4 && $2 > 65536 { printf "  %8.0f KB  %s\n", $2/1024, $4 }' | tail -12 | sort -n -r
} 2>&1 | tee "$OUT"
echo
echo "Wrote build/$OUT"
