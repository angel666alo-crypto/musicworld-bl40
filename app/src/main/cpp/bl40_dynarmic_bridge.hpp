#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

// Interface reserved for a separate Dynarmic A32 verification path.
// It does not currently execute the PXO4 initializer.
struct BL40DynarmicReport {
    bool compiled = false;
    bool executed = false;
    bool success = false;
    unsigned casesPassed = 0;
    unsigned casesRequested = 0;
    unsigned guestInstructions = 0;
    std::string reason = "not-run";
};
BL40DynarmicReport bl40DynarmicCheck(const std::uint8_t* ro,
                                     std::size_t roBytes);
