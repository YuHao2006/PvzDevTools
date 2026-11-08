#include "DataFixerUpper.h"
#include "LevelSaveConverter.h"
#include <cstdio>
#include <exception>
#include <stdexcept>

extern "C" int pvz_dfu_process_v1(const char* input, const char* output,
    char* error, std::size_t errorCapacity) noexcept
{
    if (error && errorCapacity) error[0] = '\0';
    try
    {
        if (!input || !*input || !output || !*output)
            throw std::runtime_error("Missing save upgrade path");
        if (std::filesystem::absolute(input).lexically_normal() == std::filesystem::absolute(output).lexically_normal() ||
            std::filesystem::exists(output))
            throw std::runtime_error("DFU output must be a new file distinct from input");
        const auto format = PvzDat64::DetectFormat(input);
        if (format != PvzDat64::Format::Dat64V4 && format != PvzDat64::Format::Dat64V5)
            throw std::runtime_error("DFU accepts DAT64 v4/v5 only");
        const std::filesystem::path repaired = std::string(output) + ".repair.tmp";
        struct Cleanup {
            std::filesystem::path path;
            ~Cleanup() { std::error_code e; std::filesystem::remove(path, e); }
        } cleanup{repaired};
        const auto fixes = PvzDat64::RepairPlantAttacks(input);
        const auto source = fixes.empty() ? std::filesystem::path(input) : repaired;
        if (!fixes.empty()) PvzDat64::RepairPlantAttacks(input, repaired);
        if (format == PvzDat64::Format::Dat64V4)
        {
            PvzDat64::ConversionOptions options;
            options.mTargetFormat = PvzDat64::Format::Dat64V5;
            PvzDat64::ConvertFile(source, output, options);
        }
        else if (!fixes.empty())
            std::filesystem::rename(repaired, output);
        else return 0;
        return 1;
    }
    catch (const std::exception& failure)
    {
        if (error && errorCapacity) std::snprintf(error, errorCapacity, "%s", failure.what());
    }
    catch (...)
    {
        if (error && errorCapacity) std::snprintf(error, errorCapacity, "Unknown save conversion failure");
    }
    return -1;
}

#if defined(_WIN32)
int PvzDfuCliEntry(int argc, char** argv);
extern "C" int pvz_dfu_cli_v1(int argc, char** argv) noexcept
{
    try { return PvzDfuCliEntry(argc, argv); }
    catch (...) { std::fputs("DFU converter failed unexpectedly\n", stderr); return 1; }
}
#endif
