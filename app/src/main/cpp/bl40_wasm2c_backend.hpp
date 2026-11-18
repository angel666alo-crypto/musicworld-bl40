#pragma once
#include <cstdint>
#include <cstddef>
#include <string>
#include <initializer_list>
// API nativa del core C generado. No incluye wasm_export.h / WAMR.
bool bl40_native_initialize(std::string& error);
void bl40_native_release();
void bl40_native_enter_guest_thread();
void bl40_native_leave_guest_thread();
bool bl40_native_call(const char* name, std::initializer_list<uint32_t> args,
                      uint32_t& result, std::string& error);
bool bl40_native_validate_address(uint32_t offset, uint32_t size);
uint8_t* bl40_native_pointer(uint32_t offset);
