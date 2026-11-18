#include "bl40_dynarmic_bridge.hpp"
// The framebuffer, touch and audio backend uses the reconstructed guest core.
// This variant does not claim that the original LG firmware is booted.
BL40DynarmicReport bl40DynarmicCheck(const std::uint8_t*,std::size_t) {
  BL40DynarmicReport result;
  result.reason = "CPU de escenas: HLE/wasm2c; Dynarmic no integrado en este motor";
  return result;
}
