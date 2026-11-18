#!/usr/bin/env bash
# Mide en el PC el coste de bl40_tick con y sin la optimizacion de tools/optimizar_c.py y comprueba que
# los fotogramas sean IDENTICOS (mismo hash). Requiere haber ejecutado compilar_ubuntu.sh (genera el C).
# Uso: bash tools/medir_nucleo.sh [ticks]   (por defecto 6000)
set -Eeuo pipefail
cd "$(dirname "$(readlink -f "$0")")/.."
GEN=app/src/main/cpp/generated
PACK=app/src/main/assets/bl40_assets.pack
N="${1:-6000}"
[[ -s "$GEN/bl40_core_native.c" ]] || { echo "Falta $GEN: ejecuta bash compilar_ubuntu.sh primero"; exit 1; }
CC="${CC:-clang}"; command -v "$CC" >/dev/null || CC=gcc
D=$(mktemp -d); trap 'rm -rf "$D"' EXIT
FL="-O3 -DNDEBUG -fno-strict-aliasing -fno-math-errno -w -I$GEN"
# C sin optimizar = el generado sin la marca ni los always_inline
python3 - "$GEN/bl40_core_native.c" "$D/base.c" <<'PY'
import sys,re
s=open(sys.argv[1]).read().replace('/* BL40-OPT: accesores integrados */\n','')
s=s.replace('__attribute__((always_inline)) static inline ','')
open(sys.argv[2],'w').write(s)
PY
cp "$GEN/bl40_core_native.c" "$D/opt.c"
python3 tools/optimizar_c.py "$D/opt.c" >/dev/null
for v in base opt; do
  $CC $FL -c "$D/$v.c" -o "$D/$v.o"
done
$CC $FL -c "$GEN/wasm-rt-impl.c" -o "$D/rt1.o"; $CC $FL -c "$GEN/wasm-rt-mem-impl.c" -o "$D/rt2.o"
$CC $FL -c tests/bench_nucleo.c -o "$D/bench.o"
for v in base opt; do
  $CC "$D/$v.o" "$D/rt1.o" "$D/rt2.o" "$D/bench.o" -o "$D/run_$v" -lm
  echo "== $v"; "$D/run_$v" "$PACK" "$N" | tee "$D/out_$v.txt" | tail -2
done
h1=$(grep -o 'hash=.*' "$D/out_base.txt"); h2=$(grep -o 'hash=.*' "$D/out_opt.txt")
[[ "$h1" == "$h2" ]] && echo "OK: fotogramas identicos ($h1)" || { echo "ERROR: los fotogramas difieren: $h1 vs $h2"; exit 2; }
