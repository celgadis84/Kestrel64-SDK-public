#!/bin/sh
# Headless run of a ROM in kestrel64 (local dev helper).
#   tools/run_kestrel.sh rom.z64 out.png [maxinsn] [prdp]
# Prints the ISViewer log, writes the last scanned framebuffer as PNG.
ROM=$1; OUT=$2; MAX=${3:-60000000}
EXE=${KESTREL_EXE:-kestrel64}
PY=${PYTHON:-python}
[ "$4" = prdp ] && { EXE=${KESTREL_EXE_PRDP:-$EXE}; export KESTREL_PRDP=1; }
BMP="${OUT%.png}.bmp"; command -v cygpath >/dev/null && { BMP=$(cygpath -m "$BMP"); OUT=$(cygpath -m "$OUT"); }
KESTREL_FBDUMP="$BMP" KESTREL_MAXINSN=$MAX KESTREL_AUDIO=0 timeout 120 "$EXE" "$ROM" --run 2>&1 | tr -d '\0' \
  | grep -v -E '^  jl |^\[(det|threads|exchist|exl|statehash|telemetry|cpu|system)\]' | head -${LOG_LINES:-60}
"$PY" -c "from PIL import Image; Image.open(r'$BMP').save(r'$OUT')" 2>/dev/null && echo "png: $OUT"
