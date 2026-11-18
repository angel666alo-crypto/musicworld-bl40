#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$(readlink -f "$0")")"
python3 - <<'PYHASH'
import hashlib,json
from pathlib import Path
root=Path('app/src/main/assets')
for name, expected in json.loads(Path('INTEGRIDAD_ASSETS.json').read_text()).items():
    got=hashlib.sha256((root/name).read_bytes()).hexdigest()
    assert got==expected, f'Integridad fallida: {name}'
print('OK: integridad de los tres recursos verificada')
PYHASH
D=$(mktemp -d)
trap 'rm -rf "$D"' EXIT
for t in test_clock test_pcm_queue test_replay test_pacing; do
  g++ -std=c++20 -O2 -Wall -Wextra -pthread "tests/$t.cpp" -o "$D/$t"
  "$D/$t"
done
python3 tests/test_boot.py
if command -v clang++ >/dev/null 2>&1; then
  python3 tests/test_native_backend.py
else
  echo 'Nota: prueba del backend C++ omitida: falta clang++'
fi
if command -v node >/dev/null 2>&1; then
  node tests/probar_wasm_pc.js
else
  echo 'Nota: prueba de ejecución WASM omitida: falta Node.js'
fi
