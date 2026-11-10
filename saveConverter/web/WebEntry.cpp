#include "DataFixerUpper.h"
#include <array>
#include "LevelSaveConverter.h"
#include <emscripten/emscripten.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <sstream>

namespace {
std::string resultName;
std::string resultError;
std::string resultMetadata;
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_save_metadata()
{
    return resultMetadata.c_str();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_inspect_save(const char* filename, int mode, int repair)
{
    resultName.clear();
    resultError.clear();
    resultMetadata.clear();
    try {
        const std::filesystem::path name(filename);
        if (name.empty() || name != name.filename()) throw std::runtime_error("Invalid input filename");
        const auto input = std::filesystem::path("/input") / name;
        // Inspect validates the complete document, including all pools and tails.
        resultMetadata = PvzDat64::InspectV4File(input, mode);
        if (repair == 2) {
            const auto output = std::filesystem::path("/output") / name;
            const bool changed = PvzDat64::RepairSurvivalTransition(input, output, mode);
            resultMetadata = changed ? "{\"survivalTransitionRepaired\":true}" :
                "{\"survivalTransitionRepaired\":false}";
            if (changed) resultName = name.string();
        } else if (repair) {
            const auto output = std::filesystem::path("/output") / name;
            const auto repairs = PvzDat64::RepairPlantAttacks(input, output);
            std::ostringstream metadata;
            metadata << "{\"mode\":" << mode << ",\"repairs\":[";
            for (std::size_t index = 0; index < repairs.size(); ++index) {
                if (index) metadata << ',';
                metadata << "{\"row\":" << repairs[index].mRow + 1 << ",\"column\":" << repairs[index].mColumn + 1 << '}';
            }
            metadata << "]}";
            resultMetadata = metadata.str();
            if (!repairs.empty()) resultName = name.string();
            else std::filesystem::remove(output);
        }
    } catch (const std::exception& error) {
        resultError = error.what();
        resultMetadata.clear();
    }
    return resultError.c_str();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_conversion_output_name()
{
    return resultName.c_str();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_convert_save(const char* filename, int target, int roundTime, int user)
{
    resultName.clear();
    resultError.clear();
    try {
        if (target != 0 && target != 4 && target != 5)
            throw std::runtime_error("目标格式尚未定义；当前可转换原版 1073、DAT64 v4 与 v5");
        const std::filesystem::path name(filename);
        if (name.empty() || name != name.filename())
            throw std::runtime_error("输入必须是单个存档文件名");
        const auto input = std::filesystem::path("/input") / name;
        std::filesystem::path output;
        if (user) {
            const auto source = PvzDat64::DetectUserFormat(input);
            const bool original = source == PvzDat64::UserFormat::LegacyUserDat;
            if (original == (target == 0))
                throw std::runtime_error("输入已经是所选格式");
            output = "/output" / PvzDat64::DefaultUserOutputPath(name, source);
            PvzDat64::ConvertUserFile(input, output);
        } else {
            const auto source = PvzDat64::DetectFormat(input);
            if (source != PvzDat64::Format::Legacy1073Dat && source != PvzDat64::Format::Dat64V4 && source != PvzDat64::Format::Dat64V5)
                throw std::runtime_error("仅支持原版 1073、DAT64 v4 和 v5；不支持旧的 DAT64 v1～v3");
            const auto destination = target == 0 ? PvzDat64::Format::Legacy1073Dat : target == 5 ? PvzDat64::Format::Dat64V5 : PvzDat64::Format::Dat64V4;
            if (source == destination)
                throw std::runtime_error("输入已经是所选格式");
            auto outputName = source == PvzDat64::Format::Legacy1073Dat || destination == PvzDat64::Format::Legacy1073Dat
                ? PvzDat64::DefaultOutputPath(name, source) : name;
            outputName.replace_extension(target == 0 ? ".dat" : ".dat64");
            output = "/output" / outputName;
            PvzDat64::ConversionOptions options;
            options.mTargetFormat = destination;
            options.mRoundTimeToNearestTick = roundTime != 0;
            PvzDat64::ConvertFile(input, output, options);
        }
        resultName = output.filename().string();
    } catch (const std::exception& error) {
        resultError = error.what();
    }
    return resultError.c_str();
}
extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_model_schema()
{
    static const auto schema=PvzDat64::V4ModelSchema();return schema.c_str();
}
extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_model_convert(int encode)
{
    resultError.clear();
    try {
        if(encode) PvzDat64::EncodeV4Model("/input/document.model","/output/generated.dat64");
        else PvzDat64::DecodeV4Model("/input/document.dat64","/output/document.model");
    }catch(const std::exception& error){resultError=error.what();}
    return resultError.c_str();
}

extern "C" EMSCRIPTEN_KEEPALIVE const char* pvz_dfu_save(const char* filename)
{
    resultName.clear(); resultError.clear(); resultMetadata.clear();
    const std::filesystem::path name(filename);
    if (name.empty() || name != name.filename()) { resultError="Invalid DFU filename"; return resultError.c_str(); }
    const auto input=(std::filesystem::path("/input")/name).string();
    const auto output=(std::filesystem::path("/output")/name).string();
    std::array<char,2048> error{};
    const auto status=pvz_dfu_process_v1(input.c_str(),output.c_str(),error.data(),error.size());
    if(status<0)resultError=error.data();
    else if(status>0)resultName=name.string();
    return resultError.c_str();
}
