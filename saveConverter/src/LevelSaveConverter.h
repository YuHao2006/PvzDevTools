#pragma once
#include <filesystem>
#include <optional>
#include <string>

namespace PvzDat64
{
enum class Format
{
    Legacy1073Dat,
    Dat64V1,
    Dat64V2,
    Dat64V4
};

struct ConversionOptions
{
    // With no explicit target, legacy inputs become current v4; DAT64 inputs become legacy.
    std::optional<Format> mTargetFormat;
    bool mRoundTimeToNearestTick = false;
};

struct ConversionResult
{
    Format mSourceFormat;
    Format mTargetFormat;
    std::filesystem::path mSourcePath;
    std::filesystem::path mTargetPath;
    std::uintmax_t mSourceSize;
    std::uintmax_t mTargetSize;
};

// 玩家明细档：user<ID>.dat（1073 v12 布局，挑战记录/小游戏完成槽序=旧模式
// 值-1 恒等式）与 user<ID>.dat64（PVZUSR64 魔数 + v13 明细流，槽序=GameMode
// v3 直下标）。转换做字段级走读+槽序搬运，绝不字节手术（变长段前缀使后续
// 偏移漂移）。
enum class UserFormat
{
    LegacyUserDat,
    UserDat64
};

struct UserConversionResult
{
    UserFormat mSourceFormat;
    UserFormat mTargetFormat;
    std::filesystem::path mSourcePath;
    std::filesystem::path mTargetPath;
    std::uintmax_t mSourceSize;
    std::uintmax_t mTargetSize;
};

Format DetectFormat(const std::filesystem::path& thePath);
std::filesystem::path DefaultOutputPath(
    const std::filesystem::path& theInput,
    Format theInputFormat);
ConversionResult ConvertFile(
    const std::filesystem::path& theInput,
    const std::filesystem::path& theOutput,
    const ConversionOptions& theOptions = {});
const char* FormatName(Format theFormat);

UserFormat DetectUserFormat(const std::filesystem::path& thePath);
std::filesystem::path DefaultUserOutputPath(
    const std::filesystem::path& theInput,
    UserFormat theInputFormat);
UserConversionResult ConvertUserFile(
    const std::filesystem::path& theInput,
    const std::filesystem::path& theOutput);
const char* UserFormatName(UserFormat theFormat);
}
