cmake_minimum_required(VERSION 3.22.1)
project(bl40_game_native_c LANGUAGES C CXX)
set(CMAKE_SUPPRESS_REGENERATION ON)
set(CMAKE_C_STANDARD 11)
set(CMAKE_CXX_STANDARD 20)
set(NATIVE_SRC "${CMAKE_CURRENT_SOURCE_DIR}/generated")
if(NOT EXISTS "${NATIVE_SRC}/bl40_core_native.c")
 message(FATAL_ERROR "Falta codigo wasm2c generado. Ejecuta bash compilar_ubuntu.sh")
endif()
if(NOT EXISTS "${NATIVE_SRC}/wasm-rt-impl.c" OR
   NOT EXISTS "${NATIVE_SRC}/wasm-rt-mem-impl.c")
 message(FATAL_ERROR "Faltan los archivos wasm-rt nativos del mismo WABT usado para generar el core")
endif()
set(TINY_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../../../third_party/TinySoundFont")
if(NOT EXISTS "${TINY_ROOT}/tsf.h" OR NOT EXISTS "${TINY_ROOT}/tml.h")
 message(FATAL_ERROR "Falta TinySoundFont. Ejecuta bash compilar_ubuntu.sh")
endif()
add_library(bl40_native_core STATIC
 "${NATIVE_SRC}/bl40_core_native.c"
 "${NATIVE_SRC}/wasm-rt-impl.c"
 "${NATIVE_SRC}/wasm-rt-mem-impl.c")
target_include_directories(bl40_native_core PUBLIC "${NATIVE_SRC}")
target_compile_options(bl40_native_core PRIVATE -O3 -g0 -DNDEBUG -fno-strict-aliasing -fno-math-errno -fomit-frame-pointer -Wno-unused-variable)
# Planificacion de instrucciones para nucleos en orden (Cortex-A53/A55, gama baja); solo ajusta el
# orden del codigo, no exige extensiones de CPU. Se activa solo si el compilador lo acepta.
include(CheckCCompilerFlag)
check_c_compiler_flag("-mtune=cortex-a53" BL40_HAS_MTUNE_A53)
if(BL40_HAS_MTUNE_A53 AND CMAKE_ANDROID_ARCH_ABI STREQUAL "arm64-v8a")
 target_compile_options(bl40_native_core PRIVATE -mtune=cortex-a53)
endif()
add_library(musicworld SHARED native_engine.cpp aaudio_bridge.cpp bl40_dynarmic_bridge.cpp
 bl40_wasm2c_backend.cpp "${NATIVE_SRC}/bl40_core_dispatch.cpp")
target_include_directories(musicworld PRIVATE "${TINY_ROOT}" "${NATIVE_SRC}")
target_compile_options(musicworld PRIVATE -O3 -Wall -Wextra -Wno-unused-parameter)
target_link_libraries(musicworld PRIVATE bl40_native_core android log aaudio m)
