#!/usr/bin/env bash
set -Eeuo pipefail
cd "$(dirname "$(readlink -f "$0")")"

export JAVA_HOME="${JAVA_HOME:-/usr/lib/jvm/java-17-openjdk-amd64}"
export ANDROID_HOME="${ANDROID_HOME:-${ANDROID_SDK_ROOT:-$HOME/Android/Sdk}}"
export ANDROID_SDK_ROOT="$ANDROID_HOME"

fail(){ echo "ERROR: $*" >&2; exit 1; }
for cmd in git cmake ninja python3 g++ javac unzip curl sha256sum; do
  command -v "$cmd" >/dev/null 2>&1 || fail "Falta $cmd. Instala las dependencias con apt.";
done
[[ -d "$ANDROID_HOME/platforms/android-35" ]] || fail "Falta Android SDK platform android-35 en $ANDROID_HOME"
[[ -f "$ANDROID_HOME/ndk/25.2.9519653/source.properties" ]] || fail "Falta Android NDK 25.2.9519653"
[[ -d "$ANDROID_HOME/cmake/3.22.1" ]] || fail "Falta Android SDK CMake 3.22.1"
[[ -x "$JAVA_HOME/bin/java" ]] || fail "No se encuentra Java 17 en JAVA_HOME=$JAVA_HOME"

bash probar.sh
mkdir -p third_party
if [[ ! -f third_party/TinySoundFont/tsf.h || ! -f third_party/TinySoundFont/tml.h ]]; then
  [[ ! -e third_party/TinySoundFont ]] || fail "TinySoundFont existe pero está incompleto"
  git clone --depth 1 https://github.com/schellingb/TinySoundFont.git third_party/TinySoundFont
fi
if [[ ! -f third_party/wabt/wasm2c/wasm-rt-impl.c ]]; then
  [[ ! -e third_party/wabt ]] || fail "WABT existe pero está incompleto"
  git clone --depth 1 --branch 1.0.39 https://github.com/WebAssembly/wabt.git third_party/wabt
fi
WABT_REV="$(git -C third_party/wabt rev-parse HEAD 2>/dev/null || echo unknown)"
if [[ ! -x .host-wabt-build/wasm2c ]]; then
  cmake -S third_party/wabt -B .host-wabt-build -G Ninja -DCMAKE_BUILD_TYPE=Release -DBUILD_TESTS=OFF
  cmake --build .host-wabt-build --target wasm2c -j2
fi

CORE=app/src/main/assets/bl40_core.wasm
GEN=app/src/main/cpp/generated
mkdir -p "$GEN"
STAMP="$(sha256sum "$CORE" | awk '{print $1}')|$WABT_REV"
if [[ ! -s "$GEN/bl40_core_native.c" || ! -s "$GEN/.core-stamp" || "$(cat "$GEN/.core-stamp")" != "$STAMP" ]]; then
  echo "Generando backend nativo C..."
  # --disable-exceptions no existe en todas las versiones de wasm2c: se intenta y, si falla, se omite.
  if ! .host-wabt-build/wasm2c --disable-exceptions --module-name bl40core "$CORE" -o "$GEN/bl40_core_native.c" 2>/dev/null; then
    .host-wabt-build/wasm2c --module-name bl40core "$CORE" -o "$GEN/bl40_core_native.c"
  fi
  python3 tools/generar_puente_wasm2c.py "$CORE" "$GEN/bl40_core_dispatch.cpp" "$GEN/bl40_core_native.h"
  cp third_party/wabt/wasm2c/wasm-rt.h "$GEN/"
  cp third_party/wabt/wasm2c/wasm-rt-impl.h "$GEN/"
  cp third_party/wabt/wasm2c/wasm-rt-impl.c "$GEN/"
  cp third_party/wabt/wasm2c/wasm-rt-mem-impl.c "$GEN/"
  for h in third_party/wabt/wasm2c/*.inc third_party/wabt/wasm2c/*.h; do
    [[ ! -f "$h" ]] || cp -f "$h" "$GEN/"
  done
  echo "$STAMP" > "$GEN/.core-stamp"
fi
# Optimizacion de rendimiento del C generado (idempotente; ver tools/optimizar_c.py)
python3 tools/optimizar_c.py "$GEN/bl40_core_native.c"

GRADLE="${GRADLE_BIN:-}"
if [[ -z "$GRADLE" ]] && command -v gradle >/dev/null 2>&1 && gradle --version 2>/dev/null | grep -q "Gradle 8.11.1"; then
  GRADLE="$(command -v gradle)"
fi
if [[ -z "$GRADLE" ]]; then
  GRADLE="$(find "$HOME/.gradle/wrapper/dists" "$HOME/.local" -type f -path '*/gradle-8.11.1/bin/gradle' -print -quit 2>/dev/null || true)"
fi
if [[ -z "$GRADLE" ]]; then
  GRADLE="$PWD/.local-gradle/gradle-8.11.1/bin/gradle"
  if [[ ! -x "$GRADLE" ]]; then
    mkdir -p .local-gradle
    curl -fL --retry 3 -o .local-gradle/gradle-8.11.1-bin.zip https://services.gradle.org/distributions/gradle-8.11.1-bin.zip
    unzip -oq .local-gradle/gradle-8.11.1-bin.zip -d .local-gradle
  fi
fi
[[ -x "$GRADLE" ]] || fail "No se encontró Gradle 8.11.1"
mkdir -p salida
"$GRADLE" --no-daemon --max-workers=2 --console=plain :app:assembleDebug 2>&1 | tee salida/compilacion.log
APK=app/build/outputs/apk/debug/app-debug.apk
[[ -s "$APK" ]] || fail "Gradle no generó el APK"
cp "$APK" salida/MusicWorld_BL40.apk
printf '\nAPK generado: %s\n' "$PWD/salida/MusicWorld_BL40.apk"
# Copiar a Descargas cuando WSL interop está disponible; no requiere ADB.
if command -v cmd.exe >/dev/null 2>&1 && command -v wslpath >/dev/null 2>&1; then
  PROFILE_WIN="$(cmd.exe /C 'echo %USERPROFILE%' 2>/dev/null | tr -d '\r' | tail -1)"
  if [[ "$PROFILE_WIN" == ?:* ]]; then
    DOWNLOADS="$(wslpath "$PROFILE_WIN")/Downloads"
    if [[ -d "$DOWNLOADS" ]]; then cp -f "$APK" "$DOWNLOADS/MusicWorld_BL40.apk"; echo "Copia Windows: $DOWNLOADS/MusicWorld_BL40.apk"; fi
  fi
fi
