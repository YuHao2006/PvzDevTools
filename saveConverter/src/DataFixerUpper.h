#pragma once
#include <cstddef>
#if defined(_WIN32)
#define PVZ_DFU_EXPORT __declspec(dllexport)
#else
#define PVZ_DFU_EXPORT __attribute__((visibility("default")))
#endif
// C ABI: no C++ objects or exceptions cross the library boundary.
// Caller owns both paths; input is never modified. 1 = output written, 0 = unchanged, -1 = failure.
extern "C" PVZ_DFU_EXPORT int pvz_dfu_process_v1(
    const char* input, const char* output, char* error, std::size_t errorCapacity) noexcept;

#if defined(_WIN32)
extern "C" PVZ_DFU_EXPORT int pvz_dfu_cli_v1(int argc, char** argv) noexcept;
#endif
