#!/bin/sh
# Runs the self-test demos headless in kestrel64 and prints one line per ROM.
#   tools/test/run_selftests.sh [build-dir] [demo ...]
# A ROM passes when its debug log has a PASS and no FAIL before the instruction cap
# (K64_TEST_INSN, default 1.5e9, about 16 emulated seconds). Exit code = number of failures.
# Needs the ROMs built (ninja -C <build-dir>) and kestrel64 (KESTREL_EXE, see run_kestrel.sh).
SDK=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$SDK/build}
[ $# -gt 0 ] && shift
DEMOS=${*:-"04_rsp 17_timers 18_threads 19_pack 20_fonts 21_particles 22_music 23_impostor \
26_romfs 27_tpak 28_joybus 29_video 30_texfmt 31_memfill"}
INSN=${K64_TEST_INSN:-1500000000}
OUT=${TMPDIR:-/tmp}/k64_selftest
mkdir -p "$OUT"
fails=0
for d in $DEMOS; do
  rom=$BUILD/demos/$d/$d.z64
  if [ ! -f "$rom" ]; then echo "MISSING $d ($rom)"; fails=$((fails + 1)); continue; fi
  insn=$INSN
  env=""
  case $d in
    27_tpak)   # four synthetic Game Boy carts, one Transfer Pak per port (docs/PAK.md)
      ${PYTHON:-python} "$SDK/demos/27_tpak/mkgbrom.py" "$OUT/gb" > /dev/null
      gb=$OUT/gb; command -v cygpath > /dev/null && gb=$(cygpath -m "$gb")
      env="KESTREL_PADS=1111 KESTREL_PADACC=3333 KESTREL_TPAK_ROM1=$gb/mbc1.gb KESTREL_TPAK_ROM2=$gb/mbc3.gb"
      env="$env KESTREL_TPAK_ROM3=$gb/mbc5.gb KESTREL_TPAK_ROM4=$gb/mbc2.gb" ;;
    29_video)  # plays the whole clip twice before the verdict
      insn=$((INSN * 3)) ;;
  esac
  t0=$(date +%s)
  env $env LOG_LINES=2000 sh "$SDK/tools/run_kestrel.sh" "$rom" "$OUT/$d.png" "$insn" > "$OUT/$d.log" 2>&1
  t=$(( $(date +%s) - t0 ))
  if grep -q FAIL "$OUT/$d.log"; then r=FAIL
  elif grep -q PASS "$OUT/$d.log"; then r=PASS
  else r=NORESULT
  fi
  [ $r = PASS ] || fails=$((fails + 1))
  printf '%-8s %-14s %3ds  %s\n' $r $d $t "$OUT/$d.log"
done
echo "failures: $fails"
exit $fails
