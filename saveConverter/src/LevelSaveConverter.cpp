#include "LevelSaveConverter.h"

// The standalone converter and game probes share the game-source include root.
#include "ConstEnums.h"
#include "Lawn/System/LevelSaveFormat.h"
#include "Lawn/System/Dat64TimeSchema.h"
#include "Lawn/System/V7SaveSchema.h"
#include "FrozenResourceIds.h"
#include "ResourceIdRemap.h"
#include "Sexy.TodLib/generated/ReanimAppearanceSaveCatalog.h"
#include "Sexy.TodLib/generated/ParticleAppearanceSaveCatalog.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cctype>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <optional>
#include <set>
#include <span>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#ifdef _WIN32
#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <Windows.h>
#endif

namespace PvzDat64
{
namespace
{
using Byte = std::uint8_t;
using LevelSaveFormat::RecordKind;
using LevelSaveFormat::RecordLayout;

constexpr bool HasV7State(Format format)
{
    return format == Format::Dat64V4;
}
constexpr bool IsNanosecondFormat(Format format)
{
    return format == Format::Dat64V2 || HasV7State(format);
}
constexpr bool HasParticleVariants(Format format)
{
    return HasV7State(format);
}
constexpr bool HasReanimationVariants(Format format)
{
    return HasV7State(format);
}

constexpr std::string_view LEGACY_PARTICLE_NAMES[] = {
#define LEGACY_PARTICLE_ID(name) #name,
#include "LegacyParticleIds.inc"
#undef LEGACY_PARTICLE_ID
};
constexpr std::int32_t FROZEN_PARTICLE_COUNT = 162;
constexpr auto FROZEN_PARTICLE_NAMES = [] {
    auto names = std::to_array(LEGACY_PARTICLE_NAMES);
    std::sort(names.begin(), names.end());
    return names;
}();
constexpr std::int32_t FrozenParticleId(std::string_view name)
{
    const auto found = std::lower_bound(FROZEN_PARTICLE_NAMES.begin(), FROZEN_PARTICLE_NAMES.end(), name);
    if (found == FROZEN_PARTICLE_NAMES.end() || *found != name)
        throw "unknown frozen particle name";
    return static_cast<std::int32_t>(found - FROZEN_PARTICLE_NAMES.begin());
}
constexpr auto LEGACY_PARTICLE_IDS = [] {
    std::array<std::int32_t, FROZEN_PARTICLE_COUNT> result{};
    for (std::size_t i = 0; i < result.size(); ++i)
        result[i] = FrozenParticleId(LEGACY_PARTICLE_NAMES[i]);
    return result;
}();
static_assert([] {
    std::array<bool, FROZEN_PARTICLE_COUNT> seen{};
    for (const auto id : LEGACY_PARTICLE_IDS)
    {
        if (id < 0 || id >= FROZEN_PARTICLE_COUNT || seen[id]) return false;
        seen[id] = true;
    }
    return true;
}(), "Frozen particle mapping must name every v3 definition exactly once");

constexpr std::uint32_t LEGACY_MAGIC = 0xFEEDDEADU;
constexpr std::uint32_t LEGACY_VERSION = 2U;
// 官方 1073 档的构建日期戳；0xB7EFF6A8 为本工程早期构建（运行时 crc32
// 计算 SAVE_FILE_DATE 的版本）写出的档。日期戳无语义，两个值都收。
constexpr std::uint32_t LEGACY_DATE_OFFICIAL = 0xAC9B80ACU;
constexpr std::uint32_t LEGACY_DATE_PROJECT = 0xB7EFF6A8U;
constexpr std::uint32_t DATA_ARRAY_KEY_MASK = 0xFFFF0000U;
constexpr std::uint32_t DATA_ARRAY_INDEX_MASK = 0x0000FFFFU;
constexpr std::uint32_t LEGACY_FIXED_TAIL_SIZE = 2056U;
constexpr std::uint32_t DAT64_FIXED_TAIL_SIZE = 2292U;

// ---------------------------------------------------------------------------
// 旧工程方言（pre-migration dialect）
//
// 2025-09 ~ 2026-08-25 期间本工程的构建写出的 game*.dat 与 1073 考古规范有
// 两处结构差异（移植初期缺失、2026-09-02 考古补回）：
//
//   Board 块：22092 字节（规范 22108）。差异不是尾部截断，而是 Board 结构里
//   mPrevBoardResult 之后的 6 个 GOTY 统计字段共 16 字节当时还没有移植：
//     canonical [22024,22040) =
//       mPeaShooterUsed(bool) mCatapultPlantsUsed(bool) +2 pad
//       mLevelCoinsCollected(int) mGargantuarsKillsByCornCob(int)
//       mMushroomAndCoffeeBeansOnly(bool) mMushroomsUsed(bool) +2 pad
//   旧档里 [22024,22092)（mTriggeredLawnMowers..mChocolateCollected，含
//   int 版 mGameID@22060）整体前移 16 字节。转换时在 canonical 22024 处插入
//   16 字节全零（1073 构造语义的初值），回写 legacy 时按映射剥掉。
//
//   Zombie 池槽步长：348（规范 360，即记录 344 vs 356）。这批文件里 Zombie
//   池全部为空（mSize=0），死槽字节按原样保留迁移；若出现存活对象则没有
//   字段映射可用，明确报错拒绝。
constexpr std::uint32_t LEGACY_BOARD_STATE_SIZE = LevelSaveFormat::LEGACY_BOARD_STATE_SIZE;
constexpr std::uint32_t LEGACY_BOARD_STATE_SIZE_PRE_MIGRATION = 22092U;
constexpr std::uint32_t BOARD_GOTY_STATS_OFFSET = 22024U;
constexpr std::uint32_t BOARD_GOTY_STATS_SIZE = 16U;
constexpr std::uint32_t LEGACY_ZOMBIE_SLOT_SIZE = 360U;
constexpr std::uint32_t LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION = 348U;

// dat64 棋盘块尾部 [22116,22120) 是原生结构的对齐衬垫，游戏读档整块 memcpy
// 后从不按字段访问。转换器用它回记源 legacy 档的方言：0 = 规范（22108 板 +
// 360 zombie 槽），22092 = 旧工程方言（22092 板 + 348 zombie 槽）。这样
// dat→dat64→dat 能选择正确的 legacy 写法；游戏自己写的 dat64 该处为零，
// 自然落入规范分支。
constexpr std::uint32_t BOARD_DIALECT_MARKER_OFFSET =
    LevelSaveFormat::DAT64_BOARD_STATE_SIZE - 4U;
constexpr std::uint32_t BOARD_DIALECT_MARKER_CANONICAL = 0U;
constexpr std::uint32_t BOARD_DIALECT_MARKER_PRE_MIGRATION =
    LEGACY_BOARD_STATE_SIZE_PRE_MIGRATION;

// dat64 SeedBank [52,56) 是 numPackets 与 8 字节对齐的包数组之间的原生衬
// 垫，同样不被游戏按字段读取。转换器用它回记源 legacy 头的日期戳（无语义
// 的构建指纹），保证回环逐字节一致；为 0（游戏自写的 dat64）时回写官方值。
constexpr std::uint32_t SEED_BANK_DATE_ECHO_OFFSET = 52U;

// Trail 池在 dat64 诞生（2026-10-01 b8a08486）时从游戏里整体退役，但
// legacy 流里它仍然排在 Reanimation 与 Attachment 之间（1073 与本工程旧构建
// 都写它），之后还跟一段每存活 Trail 一个 i32 的定义补充节。考古结论：该池
// 恒空。dat64 流（现行）里这两节都不存在；2026-09-30 的“过渡 dat64”构建
// 仍按 12 池 + 定义补充节落盘（Trail dat64 槽步长 256）。转换器按原始字节
// 保留 legacy 侧的池块以便回环；非空的池无法迁移到 dat64，明确报错。
constexpr std::uint32_t LEGACY_TRAIL_SLOT_SIZE = 236U;
constexpr std::uint32_t DAT64_TRAIL_SLOT_SIZE = 256U;
constexpr std::uint32_t LEGACY_TRAIL_CAPACITY = 1024U;

struct ArrayDescriptor
{
    RecordKind mKind;
    const char* mName;
    std::uint32_t mCapacity;
};

// 与文件流顺序一致：Zombie..Reanimation 之后是退役的 Trail 池（legacy 专
// 有），再是 Attachment。dat64 流共 11 个池。
constexpr std::array<ArrayDescriptor, 11> ARRAY_DESCRIPTORS = {{
    {RecordKind::Zombie, "Zombie", 1024},
    {RecordKind::Plant, "Plant", 1024},
    {RecordKind::Projectile, "Projectile", 1024},
    {RecordKind::Coin, "Coin", 1024},
    {RecordKind::LawnMower, "LawnMower", 32},
    {RecordKind::GridItem, "GridItem", 128},
    {RecordKind::ParticleSystem, "ParticleSystem", 1024},
    {RecordKind::ParticleEmitter, "ParticleEmitter", 1024},
    {RecordKind::Particle, "Particle", 1024},
    {RecordKind::Reanimation, "Reanimation", 1024},
    {RecordKind::Attachment, "Attachment", 1024},
}};

enum class BoardDialect
{
    Canonical,
    PreMigration,
};

// dat64 载荷有两种读法：
//   Current      —— 现行 11 池（b8a08486 之后），游戏当前读写端。
//   Transitional —— 2026-09-30 前后的便携构建写出的“过渡 dat64”：dat64 框架
//                   （头部/棋盘 22120/步长/补充节/固定尾全部与现行一致），
//                   但池段仍是 12 个——Reanimation 之后还带一个退役 Trail 池
//                   （dat64 槽步长 256，恒空），重动画补充节之后还有一段
//                   每存活 Trail 一个 i32 的定义补充节（恒空）。b8a08486 删
//                   Trail 时未升 DAT64_VERSION，现行游戏读端按 11 池走位，
//                   把 Trail 池头误当空 Attachment 池消费后在补充节失步拒读
//                   （log：DAT64_BOARD_REJECT）。转换器接受该方言作为输入。
enum class Dat64Grammar
{
    Current,
    Transitional,
};

[[noreturn]] void Fail(
    std::size_t theOffset,
    const std::string& theField,
    const std::string& theReason)
{
    std::ostringstream aMessage;
    aMessage << "偏移 0x" << std::uppercase << std::hex << theOffset
             << "，字段 " << theField << "：" << theReason;
    throw std::runtime_error(aMessage.str());
}

[[noreturn]] void Fail(const std::string& theReason)
{
    throw std::runtime_error(theReason);
}

std::int32_t ParticleIdToCanonical(std::int32_t id, Format format)
{
    if (id == -1 || HasParticleVariants(format)) return id;
    const auto count = format == Format::Dat64V2 ? std::size(LEGACY_PARTICLE_IDS) : 106U;
    if (id < 0 || static_cast<std::size_t>(id) >= count)
        Fail("旧格式粒子定义 ID 越界：" + std::to_string(id));
    return LEGACY_PARTICLE_IDS[id];
}

std::int32_t ParticleIdFromCanonical(std::int32_t id, Format format)
{
    if (id == -1 || HasParticleVariants(format)) return id;
    const auto found = std::find(std::begin(LEGACY_PARTICLE_IDS), std::end(LEGACY_PARTICLE_IDS), id);
    const auto index = static_cast<std::size_t>(found - std::begin(LEGACY_PARTICLE_IDS));
    const auto count = format == Format::Dat64V2 ? std::size(LEGACY_PARTICLE_IDS) : 106U;
    if (index >= count) Fail("粒子定义无法导出到旧格式：" + std::to_string(id));
    return static_cast<std::int32_t>(index);
}

const auto& ParticleAppearanceFromCanonical(std::int32_t oldEffect)
{
    const auto& entries = ParticleAppearanceSaveCatalog::Mappings;
    const auto found = std::find_if(std::begin(entries), std::end(entries),
        [&](const auto& entry) { return oldEffect >= 0 &&
            entry.oldV3Effect == static_cast<std::uint32_t>(oldEffect); });
    if (found == std::end(entries))
        Fail("Unknown historical particle appearance ID " + std::to_string(oldEffect));
    return *found;
}

std::int32_t ParticleAppearanceToCanonical(std::int32_t baseEffect, std::uint32_t variant)
{
    const auto& entries = ParticleAppearanceSaveCatalog::Mappings;
    const auto found = std::find_if(std::begin(entries), std::end(entries),
        [&](const auto& entry) { return baseEffect >= 0 &&
            entry.baseEffect == static_cast<std::uint32_t>(baseEffect) && entry.variant == variant; });
    if (found == std::end(entries))
        Fail("Unknown particle base/variant pair: base=" + std::to_string(baseEffect) +
            ", variant=" + std::to_string(variant));
    return static_cast<std::int32_t>(found->oldV3Effect);
}

class Reader
{
public:
    explicit Reader(std::span<const Byte> theData)
        : mData(theData), mPosition(0)
    {
    }

    std::size_t Size() const { return mData.size(); }
    std::size_t Remaining() const { return mData.size() - mPosition; }

    std::span<const Byte> mData;
    std::size_t mPosition;
    std::vector<std::uint64_t> mNanoseconds;

    void SetPosition(std::size_t thePosition, const std::string& theField)
    {
        if (thePosition > mData.size())
        {
            Fail(mPosition, theField, "目标位置越过文件末尾");
        }
        mPosition = thePosition;
    }

    std::uint32_t U32(const std::string& theField)
    {
        Require(4, theField);
        const std::uint32_t aValue =
            static_cast<std::uint32_t>(mData[mPosition]) |
            (static_cast<std::uint32_t>(mData[mPosition + 1]) << 8) |
            (static_cast<std::uint32_t>(mData[mPosition + 2]) << 16) |
            (static_cast<std::uint32_t>(mData[mPosition + 3]) << 24);
        mPosition += 4;
        return aValue;
    }

    std::int32_t I32(const std::string& theField)
    {
        return std::bit_cast<std::int32_t>(U32(theField));
    }

    std::uint64_t U64(const std::string& theField)
    {
        const std::uint64_t aLow = U32(theField + ".low");
        const std::uint64_t aHigh = U32(theField + ".high");
        return aLow | (aHigh << 32);
    }

    std::span<const Byte> Bytes(std::size_t theSize, const std::string& theField)
    {
        Require(theSize, theField);
        const auto aResult = mData.subspan(mPosition, theSize);
        mPosition += theSize;
        return aResult;
    }

    std::span<const Byte> SyncBytes(
        std::size_t theExpectedSize,
        const std::string& theField)
    {
        const std::size_t aLengthOffset = mPosition;
        const std::uint32_t anActualSize = U32(theField + ".size");
        if (anActualSize != theExpectedSize)
        {
            std::ostringstream aReason;
            aReason << "SyncBytes 长度为 " << anActualSize
                    << "，应为 " << theExpectedSize;
            Fail(aLengthOffset, theField, aReason.str());
        }
        return Bytes(theExpectedSize, theField);
    }

private:
    void Require(std::size_t theSize, const std::string& theField) const
    {
        if (theSize > mData.size() - mPosition)
        {
            std::ostringstream aReason;
            aReason << "需要 " << theSize << " 字节，只剩 "
                    << (mData.size() - mPosition) << " 字节";
            Fail(mPosition, theField, aReason.str());
        }
    }
};

class Writer
{
public:
    std::size_t Position() const { return mData.size(); }
    std::vector<Byte> mData;
    const std::vector<std::uint64_t>* mNanoseconds = nullptr;
    std::size_t mTimeIndex = 0;

    void U32(std::uint32_t theValue)
    {
        mData.push_back(static_cast<Byte>(theValue));
        mData.push_back(static_cast<Byte>(theValue >> 8));
        mData.push_back(static_cast<Byte>(theValue >> 16));
        mData.push_back(static_cast<Byte>(theValue >> 24));
    }

    void I32(std::int32_t theValue)
    {
        U32(std::bit_cast<std::uint32_t>(theValue));
    }

    void U64(std::uint64_t theValue)
    {
        U32(static_cast<std::uint32_t>(theValue));
        U32(static_cast<std::uint32_t>(theValue >> 32));
    }

    void Bytes(std::span<const Byte> theBytes)
    {
        mData.insert(mData.end(), theBytes.begin(), theBytes.end());
    }

    void SyncBytes(std::span<const Byte> theBytes)
    {
        if (theBytes.size() > std::numeric_limits<std::uint32_t>::max())
        {
            Fail("内部错误：SyncBytes 块大于 4 GiB");
        }
        U32(static_cast<std::uint32_t>(theBytes.size()));
        Bytes(theBytes);
    }
};

std::uint32_t ReadU32At(std::span<const Byte> theData, std::size_t theOffset)
{
    return static_cast<std::uint32_t>(theData[theOffset]) |
        (static_cast<std::uint32_t>(theData[theOffset + 1]) << 8) |
        (static_cast<std::uint32_t>(theData[theOffset + 2]) << 16) |
        (static_cast<std::uint32_t>(theData[theOffset + 3]) << 24);
}

Format Dat64Format(std::span<const Byte> file)
{
    if (file.size() < LevelSaveFormat::DAT64_HEADER_SIZE)
        Fail(0, "dat64.header", "文件短于 48 字节头部");
    switch (ReadU32At(file, 8))
    {
    case LevelSaveFormat::DAT64_V1_VERSION: return Format::Dat64V1;
    case LevelSaveFormat::DAT64_V2_VERSION: return Format::Dat64V2;
    case LevelSaveFormat::DAT64_VERSION:
        if (ReadU32At(file, 40) == LevelSaveFormat::DAT64_MINOR_VERSION &&
            ReadU32At(file, 36) == LevelSaveFormat::DAT64_SCHEMA_ID)
            return Format::Dat64V4;
        Fail(36, "dat64.schema", "不支持的 v4 schema");
    case 3:
        Fail(8, "dat64.version", "DAT64 v3 系列开发格式已停止支持；仅支持原版 DAT、DAT64 v1、v2、v4");
    default: Fail(8, "dat64.version", "不支持的存档版本");
    }
}

std::vector<Byte> ReadTimeRecord(
    Reader& reader, std::span<const Byte> bytes, Dat64TimeSchema::Fields fields,
    Format format)
{
    if (!IsNanosecondFormat(format))
        return {bytes.begin(), bytes.end()};
    return Dat64TimeSchema::Collapse(bytes, fields,
        [&](const Dat64TimeSchema::Field& field, std::uint64_t ns)
        {
            reader.mNanoseconds.push_back(ns);
            // The complete value is retained separately. This temporary v1
            // image exists solely for the established structural validators.
            try { return Dat64TimeSchema::QuantizeNanoseconds(field, ns, true); }
            catch (const std::exception&) { return std::uint32_t(0); }
        });
}

std::vector<Byte> WriteTimeRecord(
    Writer& writer, std::span<const Byte> bytes, Dat64TimeSchema::Fields fields,
    Format format)
{
    if (!IsNanosecondFormat(format)) return {bytes.begin(), bytes.end()};
    return Dat64TimeSchema::Expand(bytes, fields,
        [&](const Dat64TimeSchema::Field&, std::uint32_t)
        {
            if (!writer.mNanoseconds || writer.mTimeIndex >= writer.mNanoseconds->size())
                Fail("内部错误：写出时间字段缺失");
            return (*writer.mNanoseconds)[writer.mTimeIndex++];
        });
}

std::int32_t ReadI32At(std::span<const Byte> theData, std::size_t theOffset)
{
    return std::bit_cast<std::int32_t>(ReadU32At(theData, theOffset));
}

std::uint64_t ReadU64At(std::span<const Byte> theData, std::size_t theOffset)
{
    return static_cast<std::uint64_t>(ReadU32At(theData, theOffset)) |
        (static_cast<std::uint64_t>(ReadU32At(theData, theOffset + 4)) << 32);
}

void WriteU32At(std::span<Byte> theData, std::size_t theOffset, std::uint32_t theValue)
{
    theData[theOffset] = static_cast<Byte>(theValue);
    theData[theOffset + 1] = static_cast<Byte>(theValue >> 8);
    theData[theOffset + 2] = static_cast<Byte>(theValue >> 16);
    theData[theOffset + 3] = static_cast<Byte>(theValue >> 24);
}

void WriteU64At(std::span<Byte> theData, std::size_t theOffset, std::uint64_t theValue)
{
    WriteU32At(theData, theOffset, static_cast<std::uint32_t>(theValue));
    WriteU32At(theData, theOffset + 4, static_cast<std::uint32_t>(theValue >> 32));
}

float ReadFloatAt(std::span<const Byte> theData, std::size_t theOffset)
{
    return std::bit_cast<float>(ReadU32At(theData, theOffset));
}

std::uint32_t Crc32(std::span<const Byte> theData)
{
    std::uint32_t aCrc = 0xFFFFFFFFU;
    for (const Byte aByte : theData)
    {
        aCrc ^= aByte;
        for (int aBit = 0; aBit < 8; ++aBit)
        {
            const std::uint32_t aMask = 0U - (aCrc & 1U);
            aCrc = (aCrc >> 1) ^ (0xEDB88320U & aMask);
        }
    }
    return aCrc ^ 0xFFFFFFFFU;
}

std::vector<Byte> ReadFileFully(const std::filesystem::path& thePath)
{
    std::ifstream aFile(thePath, std::ios::binary | std::ios::ate);
    if (!aFile)
    {
        Fail("无法打开输入文件：" + thePath.string());
    }
    const std::streamoff aSize = aFile.tellg();
    if (aSize < 0 || static_cast<std::uint64_t>(aSize) >
            static_cast<std::uint64_t>(std::numeric_limits<std::size_t>::max()))
    {
        Fail("输入文件大小无效：" + thePath.string());
    }
    std::vector<Byte> aData(static_cast<std::size_t>(aSize));
    aFile.seekg(0);
    if (!aData.empty())
    {
        aFile.read(reinterpret_cast<char*>(aData.data()), aSize);
    }
    if (!aFile)
    {
        Fail("读取输入文件失败：" + thePath.string());
    }
    return aData;
}

void ValidateBool(
    std::span<const Byte> theRecord,
    std::size_t theOffset,
    std::size_t theFileOffset,
    const std::string& theField)
{
    if (theRecord[theOffset] > 1)
    {
        Fail(theFileOffset + theOffset, theField, "bool 不是 0 或 1");
    }
}

void ValidateFinite(
    std::span<const Byte> theRecord,
    std::size_t theOffset,
    std::size_t theFileOffset,
    const std::string& theField)
{
    if (!std::isfinite(ReadFloatAt(theRecord, theOffset)))
    {
        Fail(theFileOffset + theOffset, theField, "浮点值为 NaN 或无穷大");
    }
}

void ValidateI32Range(
    std::span<const Byte> theRecord,
    std::size_t theOffset,
    std::int32_t theMinimum,
    std::int32_t theMaximum,
    std::size_t theFileOffset,
    const std::string& theField)
{
    const std::int32_t aValue = ReadI32At(theRecord, theOffset);
    if (aValue < theMinimum || aValue > theMaximum)
    {
        std::ostringstream aReason;
        aReason << "值 " << aValue << " 超出 [" << theMinimum
                << ", " << theMaximum << "]";
        Fail(theFileOffset + theOffset, theField, aReason.str());
    }
}

bool KindHasGameObjectBase(RecordKind theKind)
{
    switch (theKind)
    {
    case RecordKind::Zombie:
    case RecordKind::Plant:
    case RecordKind::Projectile:
    case RecordKind::Coin:
    case RecordKind::CursorObject:
    case RecordKind::CursorPreview:
    case RecordKind::SeedPacket:
        return true;
    default:
        return false;
    }
}

void ValidateCanonicalRecord(
    RecordKind theKind,
    std::span<const Byte> theRecord,
    std::size_t theFileOffset,
    const std::string& theName)
{
    const RecordLayout& aLayout = LevelSaveFormat::GetRecordLayout(theKind);
    if (theRecord.size() != aLayout.mDat64Size)
    {
        Fail(theFileOffset, theName, "内部记录大小与 dat64 schema 不一致");
    }

    if (KindHasGameObjectBase(theKind))
    {
        ValidateBool(theRecord, 32, theFileOffset, theName + ".mVisible");
    }

    const auto finiteList = [&](std::initializer_list<std::size_t> theOffsets)
    {
        for (const std::size_t anOffset : theOffsets)
        {
            ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".float");
        }
    };
    const auto boolList = [&](std::initializer_list<std::size_t> theOffsets)
    {
        for (const std::size_t anOffset : theOffsets)
        {
            ValidateBool(theRecord, anOffset, theFileOffset, theName + ".bool");
        }
    };

    switch (theKind)
    {
    case RecordKind::Zombie:
        ValidateI32Range(theRecord, 48, 0, NUM_ZOMBIE_TYPES - 1, theFileOffset, theName + ".mZombieType");
        ValidateI32Range(theRecord, 208, 0, 9, theFileOffset, theName + ".mHelmType");
        ValidateI32Range(theRecord, 228, 0, 3, theFileOffset, theName + ".mShieldType");
        finiteList({56, 60, 64, 144, 296, 300, 304});
        boolList({92, 93, 124, 132, 148, 196, 197, 198, 199, 200, 201, 202, 203,
                  248, 272, 348, 360});
        break;
    case RecordKind::Plant:
        ValidateI32Range(theRecord, 48, 0, NUM_SEED_TYPES - 1, theFileOffset, theName + ".mSeedType");
        ValidateI32Range(theRecord, 324, -1, NUM_SEED_TYPES - 1, theFileOffset, theName + ".mImitaterType");
        finiteList({204, 208});
        for (std::size_t item = 0; item < 5; ++item)
        {
            const std::size_t aBase = 212 + item * 20;
            finiteList({aBase, aBase + 4, aBase + 8, aBase + 12});
        }
        boolList({332, 333, 334, 335, 336, 337});
        break;
    case RecordKind::Projectile:
        ValidateI32Range(theRecord, 104, 0, NUM_PROJECTILES - 1, theFileOffset, theName + ".mProjectileType");
        finiteList({60, 64, 68, 72, 76, 80, 84, 88, 116, 120, 140});
        boolList({92, 124});
        break;
    case RecordKind::Coin:
        finiteList({48, 52, 56, 60, 64, 76, 80, 112});
        boolList({68, 92, 208, 209, 210});
        break;
    case RecordKind::LawnMower:
        ValidateI32Range(theRecord, 60, 0, NUM_MOWER_TYPES - 1, theFileOffset, theName + ".mMowerType");
        finiteList({16, 20, 64});
        boolList({56, 57});
        break;
    case RecordKind::GridItem:
        ValidateI32Range(theRecord, 16, 0, 12, theFileOffset, theName + ".mGridItemType");
        finiteList({44, 48, 52, 56});
        for (std::size_t anOffset = 92; anOffset < 236; anOffset += 4)
        {
            ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".mMotionTrailFrames");
        }
        boolList({40, 80});
        break;
    case RecordKind::ParticleSystem:
        ValidateI32Range(theRecord, 0, 0, FROZEN_PARTICLE_COUNT - 1, theFileOffset, theName + ".mEffectType");
        boolList({56, 57, 64});
        break;
    case RecordKind::ParticleEmitter:
        finiteList({48, 52, 56, 72, 76, 104});
        for (std::size_t anOffset = 132; anOffset < 204; anOffset += 4)
        {
            ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".interpolation");
        }
        boolList({80, 100});
        break;
    case RecordKind::Particle:
        for (const std::size_t anOffset : {16U, 20U, 24U, 28U, 32U, 36U, 40U, 48U, 52U})
        {
            ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".float");
        }
        for (std::size_t anOffset = 64; anOffset < 160; anOffset += 4)
        {
            ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".interpolation");
        }
        break;
    case RecordKind::Reanimation:
        ValidateI32Range(theRecord, 0, 0, NUM_REANIMS - 1, theFileOffset, theName + ".mReanimationType");
        finiteList({4, 8, 44, 48, 52, 56, 60, 64, 68, 72, 76, 168});
        boolList({28, 120, 144, 164});
        break;
    case RecordKind::Attachment:
    {
        const std::int32_t aCount = ReadI32At(theRecord, 768);
        if (aCount < 0 || aCount > 16)
        {
            Fail(theFileOffset + 768, theName + ".mNumEffects", "不在 0..16");
        }
        for (int i = 0; i < aCount; ++i)
        {
            const std::size_t aBase = static_cast<std::size_t>(i) * 48;
            ValidateI32Range(theRecord, aBase + 4, 0, 4, theFileOffset, theName + ".mEffectType");
            for (std::size_t anOffset = aBase + 8; anOffset < aBase + 44; anOffset += 4)
            {
                ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".mOffset");
            }
            boolList({aBase + 44, aBase + 45});
        }
        ValidateBool(theRecord, 772, theFileOffset, theName + ".mDead");
        break;
    }
    case RecordKind::CursorObject:
    case RecordKind::CursorPreview:
        break;
    case RecordKind::MessageWidget:
        break;
    case RecordKind::SeedPacket:
        ValidateI32Range(theRecord, 64, -1, 80, theFileOffset, theName + ".mPacketType");
        ValidateI32Range(theRecord, 68, -1, NUM_SEED_TYPES - 1, theFileOffset, theName + ".mImitaterType");
        ValidateFinite(theRecord, 80, theFileOffset, theName + ".mSlotMachiningPosition");
        boolList({84, 85});
        break;
    case RecordKind::Challenge:
        for (std::size_t anOffset = 28; anOffset < 82; ++anOffset)
        {
            ValidateBool(theRecord, anOffset, theFileOffset, theName + ".mBeghouledEated");
        }
        for (std::size_t anOffset = 82; anOffset < 85; ++anOffset)
        {
            ValidateBool(theRecord, anOffset, theFileOffset, theName + ".mBeghouledPurchasedUpgrade");
        }
        break;
    case RecordKind::Music:
        ValidateI32Range(theRecord, 16, -1, 13, theFileOffset, theName + ".mCurMusicTune");
        for (const std::size_t anOffset : {20U, 24U, 28U})
        {
            ValidateI32Range(theRecord, anOffset, -1, 4, theFileOffset, theName + ".MusicFile");
        }
        ValidateI32Range(theRecord, 32, -1, 2, theFileOffset, theName + ".mBurstOverride");
        for (const auto [offset, maximum] :
             {std::pair{36U, 1000.0f}, std::pair{40U, 100.0f}})
        {
            const float value = ReadFloatAt(theRecord, offset);
            if (!std::isfinite(value) || value < 1.0f || value > maximum)
                Fail(theFileOffset + offset, theName + ".tempo", "浮点速度超出有效范围");
        }
        ValidateI32Range(theRecord, 44, 0, 3, theFileOffset, theName + ".mMusicBurstState");
        ValidateI32Range(theRecord, 52, 0, 4, theFileOffset, theName + ".mMusicDrumsState");
        boolList({72, 73});
        break;
    case RecordKind::ReanimatorTrackInstance:
        for (std::size_t anOffset = 8; anOffset < 40; anOffset += 4)
        {
            ValidateFinite(theRecord, anOffset, theFileOffset, theName + ".mBlendTransform");
        }
        finiteList({64, 68, 72});
        boolList({108, 109, 110, 111});
        break;
    }
}

// Generic records copy only schema segments; skipped pointer/padding bytes stay
// zero in both formats. Board dialect and SeedBank date metadata use their own
// conversion paths below.
std::vector<Byte> ToCanonicalRecord(
    std::span<const Byte> theSource,
    Format theSourceFormat,
    RecordKind theKind,
    std::size_t theFileOffset,
    const std::string& theName)
{
    const RecordLayout& aLayout = LevelSaveFormat::GetRecordLayout(theKind);
    const std::size_t anExpectedSize = theSourceFormat == Format::Legacy1073Dat
        ? aLayout.mLegacySize
        : aLayout.mDat64Size;
    if (theSource.size() != anExpectedSize)
    {
        Fail(theFileOffset, theName, "对象长度与 schema 不符");
    }

    std::vector<Byte> aCanonical(aLayout.mDat64Size, 0);
    if (theSourceFormat != Format::Legacy1073Dat)
    {
        // The dat64 source uses canonical offsets; copy only schema segments.
        for (std::size_t i = 0; i < aLayout.mSegmentCount; ++i)
        {
            const auto& aSegment = aLayout.mSegments[i];
            std::memcpy(
                aCanonical.data() + aSegment.mDat64Offset,
                theSource.data() + aSegment.mDat64Offset,
                aSegment.mSize);
        }
    }
    else
    {
        for (std::size_t i = 0; i < aLayout.mSegmentCount; ++i)
        {
            const auto& aSegment = aLayout.mSegments[i];
            std::memcpy(
                aCanonical.data() + aSegment.mDat64Offset,
                theSource.data() + aSegment.mLegacyOffset,
                aSegment.mSize);
        }
        // legacy 非语义区同样不寄存：aCanonical 构造即零，段区外保持零。
    }

    if (theKind == RecordKind::Music)
    {
        // Legacy/v1 stored integer tempos. Early v2 saves also used integers;
        // their valid positive raw values are disjoint from valid float bits.
        for (const auto [offset, maximum] :
             {std::pair{36U, 1000U}, std::pair{40U, 100U}})
        {
            const auto raw = ReadU32At(aCanonical, offset);
            if (!IsNanosecondFormat(theSourceFormat) ||
                (theSourceFormat == Format::Dat64V2 && raw >= 1 && raw <= maximum))
            {
                ValidateI32Range(aCanonical, offset, 1, static_cast<int>(maximum),
                    theFileOffset, theName + ".tempo");
                WriteU32At(aCanonical, offset,
                    std::bit_cast<std::uint32_t>(static_cast<float>(raw)));
            }
        }
    }
    if (theKind == RecordKind::ParticleSystem)
        WriteU32At(aCanonical, 0, static_cast<std::uint32_t>(
            HasParticleVariants(theSourceFormat)
                ? ParticleAppearanceToCanonical(ReadI32At(theSource, 0), ReadU32At(theSource, 4))
                : ParticleIdToCanonical(ReadI32At(aCanonical, 0), theSourceFormat)));
    if (theKind == RecordKind::Reanimation && HasReanimationVariants(theSourceFormat))
        WriteU32At(aCanonical, 12, ReadU32At(theSource, 12));
    ValidateCanonicalRecord(theKind, aCanonical, theFileOffset, theName);
    return aCanonical;
}

std::vector<Byte> FromCanonicalRecord(
    std::span<const Byte> theCanonical,
    Format theTargetFormat,
    RecordKind theKind)
{
    const RecordLayout& aLayout = LevelSaveFormat::GetRecordLayout(theKind);
    std::vector<Byte> aIntegerMusic;
    if (theKind == RecordKind::Music && !HasParticleVariants(theTargetFormat))
    {
        aIntegerMusic.assign(theCanonical.begin(), theCanonical.end());
        for (const auto offset : {36U, 40U})
            WriteU32At(aIntegerMusic, offset,
                static_cast<std::uint32_t>(std::lround(ReadFloatAt(aIntegerMusic, offset))));
        theCanonical = aIntegerMusic;
    }
    std::vector<Byte> aOldParticle;
    if (theKind == RecordKind::ParticleSystem && HasParticleVariants(theTargetFormat))
    {
        aOldParticle.assign(theCanonical.begin(), theCanonical.end());
        const auto& appearance = ParticleAppearanceFromCanonical(ReadI32At(theCanonical, 0));
        WriteU32At(aOldParticle, 0, appearance.baseEffect);
        WriteU32At(aOldParticle, 4, appearance.variant);
        theCanonical = aOldParticle;
    }
    if (theKind == RecordKind::ParticleSystem && !HasParticleVariants(theTargetFormat))
    {
        aOldParticle.assign(theCanonical.begin(), theCanonical.end());
        WriteU32At(aOldParticle, 0, static_cast<std::uint32_t>(
            ParticleIdFromCanonical(ReadI32At(theCanonical, 0), theTargetFormat)));
        theCanonical = aOldParticle;
    }
    if (theTargetFormat != Format::Legacy1073Dat)
    {
        // Canonical records already have zeroed skipped bytes.
        return std::vector<Byte>(theCanonical.begin(), theCanonical.end());
    }

    const std::size_t aTargetSize = aLayout.mLegacySize;
    if (theCanonical.size() != aLayout.mDat64Size)
    {
        Fail("内部错误：canonical 记录尺寸不符");
    }
    std::vector<Byte> aTarget(aTargetSize, 0);
    for (std::size_t i = 0; i < aLayout.mSegmentCount; ++i)
    {
        const auto& aSegment = aLayout.mSegments[i];
        std::memcpy(
            aTarget.data() + aSegment.mLegacyOffset,
            theCanonical.data() + aSegment.mDat64Offset,
            aSegment.mSize);
    }
    // Bytes outside the legacy schema segments remain zero.
    return aTarget;
}

// ---------------------------------------------------------------------------
// Board
// ---------------------------------------------------------------------------

void ValidateCanonicalBoard(
    std::span<const Byte> aBoard, std::size_t theFileOffset)
{
    ValidateBool(aBoard, 0, theFileOffset, "Board.mPaused");
    for (std::size_t i = 0; i < 54; ++i)
    {
        ValidateI32Range(aBoard, 4 + i * 4, 0, 4, theFileOffset, "Board.mGridSquareType");
    }
    ValidateBool(aBoard, 1120, theFileOffset, "Board.mEnableGraveStones");
    ValidateFinite(aBoard, 1132, theFileOffset, "Board.mFogOffset");
    for (std::size_t row = 0; row < 6; ++row)
    {
        const std::size_t aBase = 1264 + row * 16;
        ValidateFinite(aBoard, aBase + 4, theFileOffset, "Board.mRowPickingArray.mWeight");
        ValidateFinite(aBoard, aBase + 8, theFileOffset, "Board.mRowPickingArray.mLastPicked");
        ValidateFinite(aBoard, aBase + 12, theFileOffset, "Board.mRowPickingArray.mSecondLastPicked");
    }
    // 1073 consumes only mNumWaves rows and stops each row at ZOMBIE_INVALID.
    // The unused tail is not a list of zombie enums: even game-written saves
    // contain leftover bytes there. Preserve those bytes without interpreting
    // them, but validate every entry that the game can actually consume.
    constexpr std::size_t aWaveCapacity = 100;
    constexpr std::size_t aZombiesPerWave = 50;
    ValidateI32Range(aBoard, 0x5400, 0, aWaveCapacity, theFileOffset, "Board.mNumWaves");
    const std::size_t aNumWaves = static_cast<std::size_t>(ReadI32At(aBoard, 0x5400));
    for (std::size_t wave = 0; wave < aNumWaves; ++wave)
    {
        for (std::size_t zombie = 0; zombie < aZombiesPerWave; ++zombie)
        {
            const std::size_t anOffset = 1360 + (wave * aZombiesPerWave + zombie) * 4;
            if (ReadI32At(aBoard, anOffset) == -1)
            {
                break;
            }
            ValidateI32Range(aBoard, anOffset, 0, NUM_ZOMBIE_TYPES - 1,
                theFileOffset, "Board.mZombiesInWave");
        }
    }
    for (std::size_t i = 0; i < 100; ++i)
    {
        ValidateBool(aBoard, 21360 + i, theFileOffset, "Board.mZombieAllowed");
    }
    ValidateI32Range(aBoard, 0x53E8, 0, 9, theFileOffset, "Board.mBackground");
    // 1073 initializes exactly 0x42 help flags (0040D840); the next two
    // bytes align mHelpIndex and are not booleans. Keep their bytes intact.
    for (std::size_t i = 0; i < 66; ++i)
    {
        ValidateBool(aBoard, 21572 + i, theFileOffset, "Board.mHelpDisplayed");
    }
    for (const std::size_t anOffset : {
             21644U, 21645U, 21656U, 21672U, 21992U,
             22012U, 22013U, 22014U, 22015U, 22016U, 22017U, 22018U, 22019U,
             22024U, 22025U, 22036U, 22037U})
    {
        ValidateBool(aBoard, anOffset, theFileOffset, "Board.bool");
    }
    ValidateFinite(aBoard, 22068, theFileOffset, "Board.mMinFPS");
}

std::vector<Byte> BoardToCanonical(
    std::span<const Byte> theSource,
    Format theSourceFormat,
    std::size_t theFileOffset,
    BoardDialect& theDialect)
{
    std::vector<Byte> aCanonical(LevelSaveFormat::DAT64_BOARD_STATE_SIZE, 0);
    if (theSourceFormat == Format::Dat64V1)
    {
        if (theSource.size() != LevelSaveFormat::DAT64_BOARD_STATE_SIZE)
        {
            Fail(theFileOffset, "Board", "棋盘固定块长度错误");
        }
        aCanonical.assign(theSource.begin(), theSource.end());
        for (std::size_t i = LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET;
             i < LevelSaveFormat::DAT64_BOARD_GAME_ID_OFFSET;
             ++i)
        {
            if (aCanonical[i] != 0)
            {
                Fail(theFileOffset + i, "Board.paddingBeforeGameID", "必须为零");
            }
        }
        theDialect =
            ReadU32At(aCanonical, BOARD_DIALECT_MARKER_OFFSET) == BOARD_DIALECT_MARKER_PRE_MIGRATION
            ? BoardDialect::PreMigration
            : BoardDialect::Canonical;
    }
    else if (theSource.size() == LEGACY_BOARD_STATE_SIZE)
    {
        theDialect = BoardDialect::Canonical;
        std::memcpy(aCanonical.data(), theSource.data(),
            LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET);
        const std::int64_t aGameID = ReadI32At(
            theSource,
            LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET);
        WriteU64At(
            aCanonical,
            LevelSaveFormat::DAT64_BOARD_GAME_ID_OFFSET,
            std::bit_cast<std::uint64_t>(aGameID));
        std::memcpy(
            aCanonical.data() + LevelSaveFormat::DAT64_BOARD_AFTER_GAME_ID_OFFSET,
            theSource.data() + LevelSaveFormat::LEGACY_BOARD_AFTER_GAME_ID_OFFSET,
            LevelSaveFormat::BOARD_AFTER_GAME_ID_SIZE);
        WriteU32At(aCanonical, BOARD_DIALECT_MARKER_OFFSET,
            BOARD_DIALECT_MARKER_CANONICAL);
    }
    else if (theSource.size() == LEGACY_BOARD_STATE_SIZE_PRE_MIGRATION)
    {
        // 旧工程方言：canonical 22024 处插入 16 字节 GOTY 统计字段初值（全
        // 零），其余整体前移 16 字节；int 版 mGameID 在旧偏移 22060。
        theDialect = BoardDialect::PreMigration;
        std::memcpy(
            aCanonical.data(),
            theSource.data(),
            BOARD_GOTY_STATS_OFFSET);
        // [22024,22040) 保持全零（安全默认值）。
        std::memcpy(
            aCanonical.data() + BOARD_GOTY_STATS_OFFSET + BOARD_GOTY_STATS_SIZE,
            theSource.data() + BOARD_GOTY_STATS_OFFSET,
            LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET -
                (BOARD_GOTY_STATS_OFFSET + BOARD_GOTY_STATS_SIZE));
        const std::int64_t aGameID = ReadI32At(
            theSource,
            LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET - BOARD_GOTY_STATS_SIZE);
        WriteU64At(
            aCanonical,
            LevelSaveFormat::DAT64_BOARD_GAME_ID_OFFSET,
            std::bit_cast<std::uint64_t>(aGameID));
        std::memcpy(
            aCanonical.data() + LevelSaveFormat::DAT64_BOARD_AFTER_GAME_ID_OFFSET,
            theSource.data() + LevelSaveFormat::LEGACY_BOARD_AFTER_GAME_ID_OFFSET - BOARD_GOTY_STATS_SIZE,
            LevelSaveFormat::BOARD_AFTER_GAME_ID_SIZE);
        WriteU32At(aCanonical, BOARD_DIALECT_MARKER_OFFSET,
            BOARD_DIALECT_MARKER_PRE_MIGRATION);
    }
    else
    {
        Fail(theFileOffset, "Board",
            "棋盘固定块长度错误（既不是 1073 规范的 22108，也不是本工程旧构建的 22092）");
    }

    ValidateCanonicalBoard(aCanonical, theFileOffset);
    return aCanonical;
}

bool BoardGotyStatsAreDefault(std::span<const Byte> theCanonical)
{
    for (std::uint32_t i = 0; i < BOARD_GOTY_STATS_SIZE; ++i)
    {
        if (theCanonical[BOARD_GOTY_STATS_OFFSET + i] != 0)
        {
            return false;
        }
    }
    return true;
}

std::vector<Byte> BoardFromCanonical(
    std::span<const Byte> theCanonical,
    Format theTargetFormat,
    BoardDialect& theDialect)
{
    if (theTargetFormat == Format::Dat64V1)
    {
        return std::vector<Byte>(theCanonical.begin(), theCanonical.end());
    }

    const bool isPreMigration =
        ReadU32At(theCanonical, BOARD_DIALECT_MARKER_OFFSET) ==
            BOARD_DIALECT_MARKER_PRE_MIGRATION &&
        BoardGotyStatsAreDefault(theCanonical);
    theDialect = isPreMigration ? BoardDialect::PreMigration : BoardDialect::Canonical;

    const std::int64_t aGameID = std::bit_cast<std::int64_t>(
        ReadU64At(theCanonical, LevelSaveFormat::DAT64_BOARD_GAME_ID_OFFSET));
    if (aGameID < std::numeric_limits<std::int32_t>::min() ||
        aGameID > std::numeric_limits<std::int32_t>::max())
    {
        Fail("Board.mGameID 超出 1073 的 int32 范围，不能反向转换为 .dat");
    }

    if (isPreMigration)
    {
        std::vector<Byte> aLegacy(LEGACY_BOARD_STATE_SIZE_PRE_MIGRATION, 0);
        std::memcpy(
            aLegacy.data(),
            theCanonical.data(),
            BOARD_GOTY_STATS_OFFSET);
        std::memcpy(
            aLegacy.data() + BOARD_GOTY_STATS_OFFSET,
            theCanonical.data() + BOARD_GOTY_STATS_OFFSET + BOARD_GOTY_STATS_SIZE,
            LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET - BOARD_GOTY_STATS_OFFSET -
                BOARD_GOTY_STATS_SIZE);
        WriteU32At(
            aLegacy,
            LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET - BOARD_GOTY_STATS_SIZE,
            std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(aGameID)));
        std::memcpy(
            aLegacy.data() + LevelSaveFormat::LEGACY_BOARD_AFTER_GAME_ID_OFFSET - BOARD_GOTY_STATS_SIZE,
            theCanonical.data() + LevelSaveFormat::DAT64_BOARD_AFTER_GAME_ID_OFFSET,
            LevelSaveFormat::BOARD_AFTER_GAME_ID_SIZE);
        return aLegacy;
    }

    std::vector<Byte> aLegacy(LevelSaveFormat::LEGACY_BOARD_STATE_SIZE, 0);
    std::memcpy(aLegacy.data(), theCanonical.data(),
        LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET);
    WriteU32At(
        aLegacy,
        LevelSaveFormat::LEGACY_BOARD_GAME_ID_OFFSET,
        std::bit_cast<std::uint32_t>(static_cast<std::int32_t>(aGameID)));
    std::memcpy(
        aLegacy.data() + LevelSaveFormat::LEGACY_BOARD_AFTER_GAME_ID_OFFSET,
        theCanonical.data() + LevelSaveFormat::DAT64_BOARD_AFTER_GAME_ID_OFFSET,
        LevelSaveFormat::BOARD_AFTER_GAME_ID_SIZE);
    return aLegacy;
}

// ---------------------------------------------------------------------------
// SeedBank（外层记录 + 内嵌 10 个 SeedPacket）
// ---------------------------------------------------------------------------

std::vector<Byte> SeedBankToCanonical(
    std::span<const Byte> theSource,
    Format theSourceFormat,
    std::size_t theFileOffset)
{
    const std::size_t anExpected = theSourceFormat == Format::Legacy1073Dat
        ? LevelSaveFormat::LEGACY_SEED_BANK_SIZE
        : LevelSaveFormat::DAT64_SEED_BANK_SIZE;
    if (theSource.size() != anExpected)
    {
        Fail(theFileOffset, "SeedBank", "长度错误");
    }

    std::vector<Byte> aCanonical(LevelSaveFormat::DAT64_SEED_BANK_SIZE, 0);
    const RecordLayout& aPacketLayout =
        LevelSaveFormat::GetRecordLayout(RecordKind::SeedPacket);
    const std::size_t aPacketSourceOffset = theSourceFormat == Format::Legacy1073Dat
        ? LevelSaveFormat::LEGACY_SEED_BANK_PACKET_OFFSET
        : LevelSaveFormat::DAT64_SEED_BANK_PACKET_OFFSET;
    const std::size_t aPacketSourceSize = theSourceFormat == Format::Legacy1073Dat
        ? aPacketLayout.mLegacySize
        : aPacketLayout.mDat64Size;

    if (theSourceFormat == Format::Dat64V1)
    {
        // 整块即 canonical 布局（含非语义寄存/游戏写的零），原样保留；
        // 仍逐包跑一遍语义校验。
        aCanonical.assign(theSource.begin(), theSource.end());
        for (std::size_t i = 0; i < LevelSaveFormat::SEED_BANK_PACKET_COUNT; ++i)
        {
            const std::size_t aSourceOffset = aPacketSourceOffset + i * aPacketSourceSize;
            ToCanonicalRecord(
                theSource.subspan(aSourceOffset, aPacketSourceSize),
                theSourceFormat,
                RecordKind::SeedPacket,
                theFileOffset + aSourceOffset,
                "SeedBank.mSeedPackets[" + std::to_string(i) + "]");
        }
        return aCanonical;
    }

    std::memcpy(aCanonical.data() + 16, theSource.data() + 8, 28);
    std::memcpy(aCanonical.data() + 48, theSource.data() + 36, 4);
    // 外层 8 字节指针区寄存到 dat64 16 字节指针区（[8,16) 恒零）。
    std::memcpy(aCanonical.data(), theSource.data(), 8);
    for (std::size_t i = 0; i < LevelSaveFormat::SEED_BANK_PACKET_COUNT; ++i)
    {
        const std::size_t aSourceOffset = aPacketSourceOffset + i * aPacketSourceSize;
        const std::vector<Byte> aPacket = ToCanonicalRecord(
            theSource.subspan(aSourceOffset, aPacketSourceSize),
            theSourceFormat,
            RecordKind::SeedPacket,
            theFileOffset + aSourceOffset,
            "SeedBank.mSeedPackets[" + std::to_string(i) + "]");
        std::memcpy(
            aCanonical.data() + LevelSaveFormat::DAT64_SEED_BANK_PACKET_OFFSET +
                i * aPacketLayout.mDat64Size,
            aPacket.data(),
            aPacket.size());
    }
    std::memcpy(
        aCanonical.data() + LevelSaveFormat::DAT64_SEED_BANK_TAIL_OFFSET,
        theSource.data() + LevelSaveFormat::LEGACY_SEED_BANK_TAIL_OFFSET,
        LevelSaveFormat::SEED_BANK_TAIL_SIZE);
    return aCanonical;
}

std::vector<Byte> SeedBankFromCanonical(
    std::span<const Byte> theCanonical,
    Format theTargetFormat)
{
    if (theTargetFormat == Format::Dat64V1)
    {
        return std::vector<Byte>(theCanonical.begin(), theCanonical.end());
    }

    std::vector<Byte> aLegacy(LevelSaveFormat::LEGACY_SEED_BANK_SIZE, 0);
    std::memcpy(aLegacy.data() + 8, theCanonical.data() + 16, 28);
    std::memcpy(aLegacy.data() + 36, theCanonical.data() + 48, 4);
    std::memcpy(aLegacy.data(), theCanonical.data(), 8);
    const RecordLayout& aPacketLayout =
        LevelSaveFormat::GetRecordLayout(RecordKind::SeedPacket);
    for (std::size_t i = 0; i < LevelSaveFormat::SEED_BANK_PACKET_COUNT; ++i)
    {
        const auto aPacket = FromCanonicalRecord(
            std::span<const Byte>(
                theCanonical.data() +
                    LevelSaveFormat::DAT64_SEED_BANK_PACKET_OFFSET +
                    i * aPacketLayout.mDat64Size,
                aPacketLayout.mDat64Size),
            Format::Legacy1073Dat,
            RecordKind::SeedPacket);
        std::memcpy(
            aLegacy.data() + LevelSaveFormat::LEGACY_SEED_BANK_PACKET_OFFSET +
                i * aPacketLayout.mLegacySize,
            aPacket.data(),
            aPacket.size());
    }
    std::memcpy(
        aLegacy.data() + LevelSaveFormat::LEGACY_SEED_BANK_TAIL_OFFSET,
        theCanonical.data() + LevelSaveFormat::DAT64_SEED_BANK_TAIL_OFFSET,
        LevelSaveFormat::SEED_BANK_TAIL_SIZE);
    return aLegacy;
}

// ---------------------------------------------------------------------------
// 数组池与退役 Trail 池
// ---------------------------------------------------------------------------

struct SlotSnapshot
{
    std::vector<Byte> mCanonicalRecord;
    std::vector<Byte> mV7State;
    std::vector<Byte> mV7Migration;
    std::int64_t mHistoricalLaunchRemainingNs = 0;
    bool mNeedsClockBinding = false;
    std::uint32_t mID{};
    bool operator==(const SlotSnapshot& other) const
    {
        return mCanonicalRecord == other.mCanonicalRecord && mV7State == other.mV7State && mID == other.mID;
    }
};

struct ArraySnapshot
{
    RecordKind mKind{};
    std::uint32_t mFreeListHead{};
    std::uint32_t mMaxUsedCount{};
    std::uint32_t mSize{};
    std::vector<SlotSnapshot> mSlots;
    bool operator==(const ArraySnapshot&) const = default;
};

struct RetiredTrailPool
{
    std::uint32_t mFreeListHead{};
    std::uint32_t mMaxUsedCount{};
    std::uint32_t mSize{};
    std::vector<Byte> mRawBlock;
    std::vector<std::int32_t> mDefinitionIDs;
    bool operator==(const RetiredTrailPool&) const = default;

    bool IsEmpty() const
    {
        return mFreeListHead == 0 && mMaxUsedCount == 0 && mSize == 0 &&
            mRawBlock.empty() && mDefinitionIDs.empty();
    }
};

struct EmitterSupplement
{
    std::int32_t mDefinitionIndex{};
    std::int32_t mImageResourceID{};
    std::vector<std::uint32_t> mParticleIDs;
    bool operator==(const EmitterSupplement&) const = default;
};

struct ParticleSystemSupplement
{
    std::int32_t mDefinitionID{};
    std::vector<std::uint32_t> mEmitterIDs;
    std::vector<EmitterSupplement> mEmitters;
    bool operator==(const ParticleSystemSupplement&) const = default;
};

struct ReanimationSupplement
{
    std::int32_t mDefinitionID{};
    std::vector<std::vector<Byte>> mCanonicalTracks;
    std::vector<std::int32_t> mImageResourceIDs;
    bool operator==(const ReanimationSupplement&) const = default;
};

struct Document
{
    std::vector<std::uint64_t> mNanoseconds;
    std::vector<Byte> mCanonicalBoard;
    std::array<ArraySnapshot, ARRAY_DESCRIPTORS.size()> mArrays;
    RetiredTrailPool mRetiredTrailPool;
    std::vector<ParticleSystemSupplement> mParticleSystems;
    std::vector<ReanimationSupplement> mReanimations;
    std::vector<Byte> mCursorObject;
    std::vector<Byte> mCursorPreview;
    std::vector<Byte> mMessageWidget;
    std::vector<Byte> mSeedBank;
    std::vector<Byte> mChallenge;
    std::vector<Byte> mMusic;
    bool operator==(const Document&) const = default;
};

std::int64_t SavedBattleClock(const Document& document)
{
    const auto fields = Dat64TimeSchema::BoardFields();
    for (std::size_t index = 0; index < fields.size(); ++index)
        if (std::string_view(fields[index].mName) == "mMainCounter")
        {
            if (index >= document.mNanoseconds.size() || document.mNanoseconds[index] > INT64_MAX)
                Fail("saved battle clock exceeds signed nanosecond range");
            return static_cast<std::int64_t>(document.mNanoseconds[index]);
        }
    Fail("saved Board has no battle clock");
}

std::int64_t CheckedClockAdd(std::int64_t left, std::int64_t right)
{
    if ((right > 0 && left > INT64_MAX - right) ||
        (right < 0 && left < INT64_MIN - right))
        Fail("migrated absolute clock exceeds signed nanosecond range");
    return left + right;
}

void MigrateBowlingClock(SlotSnapshot& slot, std::int64_t now, std::span<const Byte> board)
{
    WriteU64At(slot.mV7State, 24, 0);
    const auto state = ReadI32At(slot.mCanonicalRecord, 72);
    if (!(slot.mID & DATA_ARRAY_KEY_MASK) || (state != 28 && state != 29)) return;
    const auto row = ReadI32At(slot.mCanonicalRecord, 28);
    if (board.size() < 21484 || row < 0 || row >= 6)
        Fail("historical bowling row or Board is invalid");
    const auto background = ReadI32At(board, 21480);
    const bool roof = background == int(BackgroundType::BACKGROUND_5_ROOF) ||
        background == int(BackgroundType::BACKGROUND_6_BOSS);
    const bool pool = background == int(BackgroundType::BACKGROUND_3_POOL) ||
        background == int(BackgroundType::BACKGROUND_4_FOG);
    auto targetY = row * (roof || pool ? 85 : 100) + 80 + (roof ? 90 : 0);
    if (ReadI32At(board, 4 + row * 4) == int(GridSquareType::GRIDSQUARE_HIGH_GROUND))
        targetY -= 30;
    const auto remaining = V7SaveSchema::Rounded(
        std::abs(double(targetY) - V7SaveSchema::Float(slot.mV7State, 4)) * 5'000'000.0);
    WriteU64At(slot.mV7State, 24, static_cast<std::uint64_t>(CheckedClockAdd(now, remaining)));
}

void BindHistoricalClock(SlotSnapshot& slot, RecordKind kind, std::int64_t now,
    std::span<const Byte> board = {})
{
    if (!slot.mNeedsClockBinding) return;
    auto& state = slot.mV7State;
    if (kind == RecordKind::Plant)
    {
        WriteU64At(state, 32, static_cast<std::uint64_t>(
            CheckedClockAdd(now, slot.mHistoricalLaunchRemainingNs)));
        WriteU64At(state, 40, static_cast<std::uint64_t>(now));
        MigrateBowlingClock(slot, now, board);
        // The historical auxiliary slot already retains ZenGarden duration or
        // bowling count in its original unit; no additional tail field is used.
    }
    else if (kind == RecordKind::Projectile)
    {
        const auto birthDraft = std::bit_cast<std::int64_t>(ReadU64At(state, 0));
        WriteU64At(state, 0, static_cast<std::uint64_t>(CheckedClockAdd(now, birthDraft)));
    }
    else if (kind == RecordKind::Reanimation)
    {
        const auto startDraft = std::bit_cast<std::int64_t>(ReadU64At(state, 4));
        WriteU32At(state, 0, 1);
        WriteU64At(state, 4, static_cast<std::uint64_t>(CheckedClockAdd(now, startDraft)));
        WriteU64At(state, 64, static_cast<std::uint64_t>(now));
    }
    slot.mNeedsClockBinding = false;
}

void PrepareV7Output(Document& document, Format target)
{
    const auto now = HasV7State(target) ? SavedBattleClock(document) : 0;
    const ArraySnapshot* animations = nullptr;
    for (const auto& array : document.mArrays)
        if (array.mKind == RecordKind::Reanimation) animations = &array;
    for (auto& array : document.mArrays)
        for (auto& slot : array.mSlots)
        {
            if (!HasV7State(target))
            {
                slot.mV7State.clear();
                continue;
            }
            const auto size = V7SaveSchema::Size(array.mKind);
            if (size == 0) continue;
            if (!slot.mV7State.empty())
            {
                BindHistoricalClock(slot, array.mKind, now, document.mCanonicalBoard);
                continue;
            }
            slot.mV7State = slot.mV7Migration;
            if (slot.mV7State.empty()) slot.mV7State.resize(size, 0);
            if (slot.mV7State.size() != size) Fail("historical migration state differs from v4 schema");
            BindHistoricalClock(slot, array.mKind, now, document.mCanonicalBoard);
            if (array.mKind == RecordKind::GridItem && (slot.mID & DATA_ARRAY_KEY_MASK))
            {
                const auto id = ReadU32At(slot.mCanonicalRecord, 60);
                const auto index = id & DATA_ARRAY_INDEX_MASK;
                const auto count = animations && index < animations->mSlots.size() &&
                    animations->mSlots[index].mID == id
                    ? ReadU32At(animations->mSlots[index].mCanonicalRecord, 36) : 1U;
                for (std::size_t i = 0; i < 12; ++i)
                {
                    const auto clock = std::bit_cast<std::int64_t>(ReadU64At(slot.mV7State, i * 16 + 8));
                    if (count && (clock > std::numeric_limits<std::int64_t>::max() / count ||
                        clock < std::numeric_limits<std::int64_t>::min() / count))
                        Fail("historical trail phase exceeds v7 range");
                    WriteU64At(slot.mV7State, i * 16 + 8,
                        static_cast<std::uint64_t>(clock * count));
                }
            }
            if (slot.mID & DATA_ARRAY_KEY_MASK) V7SaveSchema::Validate(array.mKind, slot.mV7State);
        }
}

const auto& AppearanceDefinition(std::int32_t type)
{
    const auto& entries = ReanimAppearanceSaveCatalog::Definitions;
    const auto found = std::find_if(std::begin(entries), std::end(entries),
        [&](const auto& entry) { return type >= 0 && entry.type == static_cast<std::uint32_t>(type); });
    if (found == std::end(entries))
        Fail("Reanimation appearance catalog has no root type " + std::to_string(type));
    return *found;
}

constexpr std::int32_t NormalizeReanimationImageResourceID(std::int32_t resource)
{
    return FrozenSaveResources::NormalizeImageAlias(resource);
}

std::vector<std::int32_t> AppearanceImages(
    std::int32_t type, std::uint32_t variant, std::size_t trackCount)
{
    const auto& definition = AppearanceDefinition(type);
    if (trackCount != definition.trackCount || variant >= definition.variantCount)
        Fail("Reanimation type " + std::to_string(type) +
            ": track count or appearance variant index differs from the catalog");
    std::vector<std::int32_t> images(trackCount, LevelSaveRemap::kResourceIdSentinel);
    for (std::uint32_t i = 0; i < definition.axisCount; ++i)
    {
        const auto& axis = ReanimAppearanceSaveCatalog::Axes[definition.axisOffset + i];
        const auto digit = (variant / axis.stride) % axis.radix;
        bool found = false;
        for (std::uint32_t choice = 0; choice < axis.radix; ++choice)
        {
            const auto& entry = ReanimAppearanceSaveCatalog::Choices[axis.choiceOffset + choice];
            if (entry.digit == digit)
            {
                images.at(axis.trackIndex) = entry.canonicalModernResourceId;
                found = true;
                break;
            }
        }
        if (!found) Fail("Reanimation appearance catalog is missing an export choice");
    }
    return images;
}

std::uint32_t AppearanceVariant(std::int32_t type, std::span<const std::int32_t> images)
{
    const auto& definition = AppearanceDefinition(type);
    if (images.size() != definition.trackCount)
        Fail("Reanimation type " + std::to_string(type) +
            ": saved track count differs from the appearance catalog");
    std::vector<bool> handled(images.size(), false);
    std::uint64_t variant = 0;
    const auto unknown = [&](std::size_t track, std::int32_t resource) {
        Fail("Cannot migrate Reanimation appearance: root type=" + std::to_string(type) +
            ", track index=" + std::to_string(track) + ", canonical ResourceId=" +
            std::to_string(resource) + " is not a declared appearance choice; no output was written");
    };
    for (std::uint32_t i = 0; i < definition.axisCount; ++i)
    {
        const auto& axis = ReanimAppearanceSaveCatalog::Axes[definition.axisOffset + i];
        const auto resource = NormalizeReanimationImageResourceID(images[axis.trackIndex]);
        bool found = false;
        for (std::uint32_t choice = 0; choice < axis.radix; ++choice)
        {
            const auto& entry = ReanimAppearanceSaveCatalog::Choices[axis.choiceOffset + choice];
            if (resource >= 0 && entry.canonicalModernResourceId == static_cast<std::uint32_t>(resource))
            {
                variant += std::uint64_t(entry.digit) * axis.stride;
                found = true;
                break;
            }
        }
        if (!found) unknown(axis.trackIndex, resource);
        handled[axis.trackIndex] = true;
    }
    for (std::size_t track = 0; track < images.size(); ++track)
        if (!handled[track] && images[track] != LevelSaveRemap::kResourceIdSentinel)
            unknown(track, images[track]);
    if (variant >= definition.variantCount)
        Fail("Reanimation appearance mixed-radix index exceeds the catalog");
    return static_cast<std::uint32_t>(variant);
}

template<class Fn> void VisitReanimationSupplements(Document& document, Fn visit)
{
    std::size_t index = 0;
    for (auto& slot : document.mArrays[9].mSlots)
    {
        if (!(slot.mID & DATA_ARRAY_KEY_MASK)) continue;
        auto& supplement = document.mReanimations.at(index++);
        const auto type = ReadI32At(slot.mCanonicalRecord, 0);
        if (type != supplement.mDefinitionID)
            Fail("Reanimation fixed type and supplemental definition ID disagree");
        visit(slot.mCanonicalRecord, supplement);
    }
    if (index != document.mReanimations.size())
        Fail("Reanimation supplement count differs from the live pool");
}

void RestoreAppearanceImages(Document& document)
{
    VisitReanimationSupplements(document, [](auto& record, auto& supplement) {
        supplement.mImageResourceIDs = AppearanceImages(supplement.mDefinitionID,
            ReadU32At(record, 12), supplement.mCanonicalTracks.size());
    });
}

void PrepareAppearanceOutput(Document& document, Format target)
{
    VisitReanimationSupplements(document, [&](auto& record, auto& supplement) {
        if (HasReanimationVariants(target))
        {
            const auto variant = AppearanceVariant(supplement.mDefinitionID, supplement.mImageResourceIDs);
            WriteU32At(record, 12, variant);
            supplement.mImageResourceIDs = AppearanceImages(supplement.mDefinitionID,
                variant, supplement.mCanonicalTracks.size());
        }
        else
            WriteU32At(record, 12, 0);
    });
}

struct ParticleImageVariant
{
    std::int32_t mVariant;
    std::int32_t mBase;
    std::int32_t mImage;
};

constexpr ParticleImageVariant PARTICLE_IMAGE_VARIANTS[] = {
#define PARTICLE_IMAGE_VARIANT(VariantEnum, BaseEnum, ComponentName, ImageResourceToken) \
    {FrozenParticleId(#VariantEnum), FrozenParticleId(#BaseEnum), static_cast<std::int32_t>(FrozenSaveResources::ResourceId::ImageResourceToken##_ID)},
#include "Sexy.TodLib/ParticleImageVariants.inc"
#undef PARTICLE_IMAGE_VARIANT
};
constexpr std::int32_t NO_PARTICLE_IMAGE =
    static_cast<std::int32_t>(FrozenSaveResources::ResourceId::RESOURCE_ID_MAX);

// Internally every appearance is a definition, regardless of input version.
// This also lets ConvertFile compare old/new representations semantically.
void NormalizeParticleImages(Document& document)
{
    auto& pool = document.mArrays[6];
    std::size_t systemIndex = 0;
    for (auto& slot : pool.mSlots)
    {
        if (!(slot.mID & DATA_ARRAY_KEY_MASK))
            continue;
        auto& system = document.mParticleSystems.at(systemIndex++);
        const auto variant = std::find_if(std::begin(PARTICLE_IMAGE_VARIANTS),
            std::end(PARTICLE_IMAGE_VARIANTS), [&](const auto& entry) {
                return entry.mVariant == system.mDefinitionID;
            });
        if (!system.mEmitters.empty())
        {
            const auto image = system.mEmitters.front().mImageResourceID;
            if (std::any_of(system.mEmitters.begin(), system.mEmitters.end(),
                [&](const auto& emitter) { return emitter.mImageResourceID != image; }))
                Fail("粒子图片覆写混合（包括部分 null）：无法无损转换为统一定义变体");
            if (image != NO_PARTICLE_IMAGE)
            {
                if (variant != std::end(PARTICLE_IMAGE_VARIANTS))
                    Fail("粒子定义已经是图片变体，却仍带图片覆写：无法确定原始外观");
                const auto match = std::find_if(std::begin(PARTICLE_IMAGE_VARIANTS),
                    std::end(PARTICLE_IMAGE_VARIANTS), [&](const auto& entry) {
                        return entry.mBase == system.mDefinitionID && entry.mImage == image;
                    });
                if (match == std::end(PARTICLE_IMAGE_VARIANTS))
                    Fail("粒子图片覆写没有对应定义变体，无法无损迁移（definition=" +
                        std::to_string(system.mDefinitionID) + ", image=" + std::to_string(image) + "）");
                system.mDefinitionID = match->mVariant;
                for (auto& emitter : system.mEmitters)
                    emitter.mImageResourceID = NO_PARTICLE_IMAGE;
            }
        }
        WriteU32At(slot.mCanonicalRecord, 0, static_cast<std::uint32_t>(system.mDefinitionID));
    }
}

Document ParticleImagesForOutput(const Document& source, Format format)
{
    Document output = source;
    if (HasParticleVariants(format))
        return output;
    std::size_t systemIndex = 0;
    for (auto& slot : output.mArrays[6].mSlots)
    {
        if (!(slot.mID & DATA_ARRAY_KEY_MASK))
            continue;
        auto& system = output.mParticleSystems.at(systemIndex++);
        const auto variant = std::find_if(std::begin(PARTICLE_IMAGE_VARIANTS),
            std::end(PARTICLE_IMAGE_VARIANTS), [&](const auto& entry) {
                return entry.mVariant == system.mDefinitionID;
            });
        if (variant == std::end(PARTICLE_IMAGE_VARIANTS))
            continue;
        // With no emitter the legacy representation cannot carry the image.
        if (system.mEmitters.empty())
            Fail("没有发射器的粒子图片变体无法无损导出到 legacy/dat64-v1/v2");
        system.mDefinitionID = variant->mBase;
        for (auto& emitter : system.mEmitters)
            emitter.mImageResourceID = variant->mImage;
        WriteU32At(slot.mCanonicalRecord, 0, static_cast<std::uint32_t>(system.mDefinitionID));
    }
    return output;
}

template<class Callback>
void VisitTimeRecords(Document& document, Callback callback)
{
    callback(document.mCanonicalBoard, Dat64TimeSchema::BoardFields());
    for (auto& array : document.mArrays)
        for (auto& slot : array.mSlots)
            callback(slot.mCanonicalRecord, Dat64TimeSchema::GetFields(array.mKind));
    for (auto& reanimation : document.mReanimations)
        for (auto& track : reanimation.mCanonicalTracks)
            callback(track, Dat64TimeSchema::GetFields(RecordKind::ReanimatorTrackInstance));
    callback(document.mCursorObject, Dat64TimeSchema::GetFields(RecordKind::CursorObject));
    callback(document.mCursorPreview, Dat64TimeSchema::GetFields(RecordKind::CursorPreview));
    callback(document.mMessageWidget, Dat64TimeSchema::GetFields(RecordKind::MessageWidget));
    callback(document.mSeedBank, Dat64TimeSchema::SeedBankFields());
    callback(document.mChallenge, Dat64TimeSchema::GetFields(RecordKind::Challenge));
    callback(document.mMusic, Dat64TimeSchema::GetFields(RecordKind::Music));
}

void CaptureLegacyTimes(Document& document)
{
    document.mNanoseconds.clear();
    VisitTimeRecords(document, [&](auto& bytes, Dat64TimeSchema::Fields fields)
    {
        for (const auto& field : fields)
            document.mNanoseconds.push_back(
                Dat64TimeSchema::LegacyToNanoseconds(field, ReadU32At(bytes, field.mOffset)));
    });
}

void QuantizeDocumentTimes(Document& document, bool round)
{
    std::size_t index = 0;
    VisitTimeRecords(document, [&](auto& bytes, Dat64TimeSchema::Fields fields)
    {
        for (const auto& field : fields)
        {
            if (index >= document.mNanoseconds.size()) Fail("内部错误：时间字段缺失");
            const auto ticks = Dat64TimeSchema::QuantizeNanoseconds(
                field, document.mNanoseconds[index], round);
            WriteU32At(bytes, field.mOffset, ticks);
            document.mNanoseconds[index++] = Dat64TimeSchema::LegacyToNanoseconds(field, ticks);
        }
    });
    if (index != document.mNanoseconds.size()) Fail("内部错误：时间字段多余");
}

void QuantizeMusicTempo(Document& document)
{
    // Integer-only exports round tempo numerically; v3 keeps fractional values.
    for (const auto offset : {36U, 40U})
        WriteU32At(document.mMusic, offset, std::bit_cast<std::uint32_t>(
            static_cast<float>(std::lround(ReadFloatAt(document.mMusic, offset)))));
}

std::vector<std::uint32_t> LiveIDs(const ArraySnapshot& theArray)
{
    std::vector<std::uint32_t> aResult;
    for (const SlotSnapshot& aSlot : theArray.mSlots)
    {
        if (aSlot.mID & DATA_ARRAY_KEY_MASK)
        {
            aResult.push_back(aSlot.mID);
        }
    }
    return aResult;
}

void ValidateFreeList(
    const ArraySnapshot& theArray,
    const ArrayDescriptor& theDescriptor,
    std::size_t theFileOffset)
{
    std::vector<bool> aFree(theArray.mMaxUsedCount, false);
    std::uint32_t aCursor = theArray.mFreeListHead;
    while (aCursor != theArray.mMaxUsedCount)
    {
        if (aCursor >= theArray.mMaxUsedCount)
        {
            Fail(theFileOffset, std::string(theDescriptor.mName) + ".mFreeListHead", "自由链索引越界");
        }
        if (aFree[aCursor])
        {
            Fail(theFileOffset, std::string(theDescriptor.mName) + ".freeList", "自由链存在环");
        }
        const SlotSnapshot& aSlot = theArray.mSlots[aCursor];
        if (aSlot.mID & DATA_ARRAY_KEY_MASK)
        {
            Fail(theFileOffset, std::string(theDescriptor.mName) + ".freeList", "自由链指向存活对象");
        }
        aFree[aCursor] = true;
        aCursor = aSlot.mID;
    }

    std::uint32_t aLiveCount = 0;
    for (std::uint32_t i = 0; i < theArray.mMaxUsedCount; ++i)
    {
        const bool isLive = (theArray.mSlots[i].mID & DATA_ARRAY_KEY_MASK) != 0;
        if (isLive)
        {
            ++aLiveCount;
            if ((theArray.mSlots[i].mID & DATA_ARRAY_INDEX_MASK) != i)
            {
                Fail(theFileOffset, std::string(theDescriptor.mName) + ".mID", "存活 ID 的低 16 位不等于槽索引");
            }
        }
        else if (!aFree[i])
        {
            Fail(theFileOffset, std::string(theDescriptor.mName) + ".freeList", "空闲槽没有出现在自由链中");
        }
    }
    if (aLiveCount != theArray.mSize)
    {
        Fail(theFileOffset, std::string(theDescriptor.mName) + ".mSize", "存活槽计数与 mSize 不一致");
    }
}

std::uint32_t LegacySlotSizeFor(
    const ArrayDescriptor& theDescriptor,
    BoardDialect theDialect)
{
    if (theDescriptor.mKind == RecordKind::Zombie &&
        theDialect == BoardDialect::PreMigration)
    {
        return LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION;
    }
    return LevelSaveFormat::GetRecordLayout(theDescriptor.mKind).mLegacyArraySlotSize;
}

ArraySnapshot ReadArray(
    Reader& theReader,
    Format theFormat,
    const ArrayDescriptor& theDescriptor,
    BoardDialect theDialect)
{
    const std::size_t aHeaderOffset = theReader.mPosition;
    ArraySnapshot aResult;
    aResult.mKind = theDescriptor.mKind;
    aResult.mFreeListHead = theReader.U32(std::string(theDescriptor.mName) + ".mFreeListHead");
    aResult.mMaxUsedCount = theReader.U32(std::string(theDescriptor.mName) + ".mMaxUsedCount");
    aResult.mSize = theReader.U32(std::string(theDescriptor.mName) + ".mSize");
    if (aResult.mMaxUsedCount > theDescriptor.mCapacity ||
        aResult.mSize > aResult.mMaxUsedCount ||
        aResult.mFreeListHead > aResult.mMaxUsedCount)
    {
        Fail(aHeaderOffset, theDescriptor.mName, "DataArray 元数据越界");
    }

    const RecordLayout& aLayout = LevelSaveFormat::GetRecordLayout(theDescriptor.mKind);
    std::uint32_t aSlotSize = theFormat == Format::Legacy1073Dat
        ? LegacySlotSizeFor(theDescriptor, theDialect)
        : aLayout.mDat64ArraySlotSize;
    // 记录长度与 ID 偏移用布局表而不是“槽长-4”：Coin 这类尾部有对齐衬垫
    // 的槽（legacy 216 = 记录 208 + ID 4 + 衬垫 4）ID 不在槽尾。
    std::uint32_t anObjectSize = theFormat == Format::Legacy1073Dat
        ? aLayout.mLegacySize
        : aLayout.mDat64Size;
    const auto timeFields = Dat64TimeSchema::GetFields(theDescriptor.mKind);
    if (IsNanosecondFormat(theFormat))
    {
        aSlotSize = static_cast<std::uint32_t>(Dat64TimeSchema::V2Size(aSlotSize, timeFields));
        anObjectSize = static_cast<std::uint32_t>(Dat64TimeSchema::V2Size(anObjectSize, timeFields));
    }
    const std::size_t aV7Size = HasV7State(theFormat)
        ? V7SaveSchema::Size(theDescriptor.mKind) : 0;
    aSlotSize += static_cast<std::uint32_t>(aV7Size);

    const std::size_t aLengthOffset = theReader.mPosition;
    const std::uint32_t aBlockLength = theReader.U32(std::string(theDescriptor.mName) + ".slots.size");
    if (theFormat == Format::Legacy1073Dat &&
        theDescriptor.mKind == RecordKind::Zombie &&
        aResult.mMaxUsedCount > 0)
    {
        // Zombie 是唯一一个旧工程方言里记录更小的池（348 vs 360）。块长度
        // 与 mMaxUsedCount 都在文件里，由此推断实际槽步长并与棋盘方言对账。
        if (aBlockLength % aResult.mMaxUsedCount != 0)
        {
            Fail(aLengthOffset, theDescriptor.mName, "Zombie 槽块长度不是 mMaxUsedCount 的整数倍");
        }
        const std::uint32_t anInferred = aBlockLength / aResult.mMaxUsedCount;
        if (anInferred != LEGACY_ZOMBIE_SLOT_SIZE &&
            anInferred != LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION)
        {
            std::ostringstream aReason;
            aReason << "Zombie 槽步长为 " << anInferred << "，既不是 1073 规范的 360，也不是本工程旧构建的 348";
            Fail(aLengthOffset, theDescriptor.mName, aReason.str());
        }
        const bool aPoolPreMigration = anInferred == LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION;
        if (aPoolPreMigration != (theDialect == BoardDialect::PreMigration))
        {
            Fail(aLengthOffset, theDescriptor.mName, "Zombie 槽步长与棋盘方言不一致");
        }
        aSlotSize = anInferred;
        if (aPoolPreMigration)
        {
            anObjectSize = LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION - 4U;
        }
    }
    const std::uint64_t aBlockSize64 =
        static_cast<std::uint64_t>(aResult.mMaxUsedCount) * aSlotSize;
    if (aBlockSize64 > std::numeric_limits<std::uint32_t>::max())
    {
        Fail(aHeaderOffset, theDescriptor.mName, "DataArray 块过大");
    }
    if (aBlockLength != aBlockSize64)
    {
        std::ostringstream aReason;
        aReason << "槽块长度为 " << aBlockLength << "，应为 " << aBlockSize64;
        Fail(aLengthOffset, std::string(theDescriptor.mName) + ".slots", aReason.str());
    }
    const std::size_t aBlockOffset = theReader.mPosition;
    const auto aBlock = theReader.Bytes(aBlockLength, std::string(theDescriptor.mName) + ".slots");

    aResult.mSlots.reserve(aResult.mMaxUsedCount);
    for (std::uint32_t i = 0; i < aResult.mMaxUsedCount; ++i)
    {
        const std::size_t aSlotOffset = static_cast<std::size_t>(i) * aSlotSize;
        const auto aStoredSlot = aBlock.subspan(aSlotOffset, aSlotSize);
        std::vector<Byte> aBaseSlot;
        std::span<const Byte> aWireSlot = aStoredSlot;
        if (aV7Size)
        {
            aBaseSlot.insert(aBaseSlot.end(), aStoredSlot.begin(), aStoredSlot.begin() + anObjectSize);
            aBaseSlot.insert(aBaseSlot.end(), aStoredSlot.begin() + anObjectSize + aV7Size, aStoredSlot.end());
            aWireSlot = aBaseSlot;
        }
        const auto aCollapsed = ReadTimeRecord(theReader, aWireSlot, timeFields, theFormat);
        const std::span<const Byte> aSlotBytes(aCollapsed);
        const std::size_t aV1ObjectSize = IsNanosecondFormat(theFormat)
            ? aLayout.mDat64Size : anObjectSize;
        SlotSnapshot aSlot;
        if (aV7Size)
        {
            aSlot.mV7State.assign(aStoredSlot.begin() + anObjectSize,
                aStoredSlot.begin() + anObjectSize + aV7Size);
        }
        aSlot.mID = ReadU32At(aSlotBytes, aV1ObjectSize);
        if (theDescriptor.mKind == RecordKind::Plant)
        {
            const auto offset = Dat64TimeSchema::RecordField(RecordKind::Plant, 0x58,
                "historical launch remaining").mOffset;
            aSlot.mHistoricalLaunchRemainingNs = IsNanosecondFormat(theFormat)
                ? std::bit_cast<std::int64_t>(V7SaveSchema::Time(aWireSlot, RecordKind::Plant, offset))
                : std::int64_t(ReadI32At(aSlotBytes, theFormat == Format::Legacy1073Dat ? 0x58 : offset)) *
                    10'000'000;
        }
        if (aSlot.mID & DATA_ARRAY_KEY_MASK)
        {
            if (aV7Size) V7SaveSchema::Validate(theDescriptor.mKind, aSlot.mV7State);
            if (theDescriptor.mKind == RecordKind::Zombie &&
                aSlotSize == LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION)
            {
                Fail(aBlockOffset + aSlotOffset, theDescriptor.mName,
                    "旧构建（348 槽）Zombie 记录存在存活对象：该布局缺 12 字节字段的映射，无法迁移到 dat64");
            }
            aSlot.mCanonicalRecord = ToCanonicalRecord(
                aSlotBytes.first(aV1ObjectSize),
                IsNanosecondFormat(theFormat) && theDescriptor.mKind != RecordKind::ParticleSystem &&
                    theDescriptor.mKind != RecordKind::Reanimation
                    ? Format::Dat64V1 : theFormat,
                theDescriptor.mKind,
                aBlockOffset + aSlotOffset,
                std::string(theDescriptor.mName) + "[" + std::to_string(i) + "]");
        }
        else
        {
            // 死槽：记录字节是历史残留，读端只看 ID。按原样寄存在 dat64 记
            // 录区开头，保证回环逐字节一致；游戏对死槽记录从不按字段访问。
            aSlot.mCanonicalRecord.assign(aLayout.mDat64Size, 0);
            const std::size_t aRawSize = theFormat == Format::Legacy1073Dat
                ? anObjectSize
                : aLayout.mDat64Size;
            std::memcpy(
                aSlot.mCanonicalRecord.data(),
                aSlotBytes.data(),
                std::min<std::size_t>(aRawSize, aLayout.mDat64Size));
        }

        if (theFormat != Format::Legacy1073Dat)
        {
            for (std::size_t p = aV1ObjectSize + 4; p < aSlotBytes.size(); ++p)
            {
                if (aSlotBytes[p] != 0)
                {
                    Fail(aBlockOffset + aSlotOffset + p,
                        std::string(theDescriptor.mName) + ".slotPadding",
                        "必须为零");
                }
            }
        }
        if (!aV7Size && V7SaveSchema::Size(theDescriptor.mKind) && (aSlot.mID & DATA_ARRAY_KEY_MASK))
        {
            aSlot.mNeedsClockBinding = true;
            if (IsNanosecondFormat(theFormat))
                aSlot.mV7Migration = V7SaveSchema::FromHistorical(theDescriptor.mKind,
                    aSlot.mCanonicalRecord, aWireSlot);
            else
            {
                const auto expanded = Dat64TimeSchema::Expand(aSlot.mCanonicalRecord, timeFields,
                    [](const auto& field, std::uint32_t ticks) {
                        return Dat64TimeSchema::LegacyToNanoseconds(field, ticks);
                    });
                aSlot.mV7Migration = V7SaveSchema::FromHistorical(theDescriptor.mKind,
                    aSlot.mCanonicalRecord, expanded);
            }
        }
        aResult.mSlots.push_back(std::move(aSlot));
    }

    ValidateFreeList(aResult, theDescriptor, aHeaderOffset);
    return aResult;
}

RetiredTrailPool ReadRetiredTrailPool(Reader& theReader, std::uint32_t theSlotSize)
{
    const std::size_t aHeaderOffset = theReader.mPosition;
    RetiredTrailPool aResult;
    aResult.mFreeListHead = theReader.U32("Trail.mFreeListHead");
    aResult.mMaxUsedCount = theReader.U32("Trail.mMaxUsedCount");
    aResult.mSize = theReader.U32("Trail.mSize");
    if (aResult.mMaxUsedCount > LEGACY_TRAIL_CAPACITY ||
        aResult.mSize > aResult.mMaxUsedCount ||
        aResult.mFreeListHead > aResult.mMaxUsedCount)
    {
        Fail(aHeaderOffset, "Trail", "退役 Trail 池元数据越界");
    }
    const std::size_t aLengthOffset = theReader.mPosition;
    const std::uint32_t aBlockLength = theReader.U32("Trail.slots.size");
    const std::uint64_t anExpected =
        static_cast<std::uint64_t>(aResult.mMaxUsedCount) * theSlotSize;
    if (aBlockLength != anExpected)
    {
        std::ostringstream aReason;
        aReason << "槽块长度为 " << aBlockLength << "，应为 " << anExpected;
        Fail(aLengthOffset, "Trail.slots", aReason.str());
    }
    const auto aBlock = theReader.Bytes(aBlockLength, "Trail.slots");
    aResult.mRawBlock.assign(aBlock.begin(), aBlock.end());

    std::uint32_t aLiveCount = 0;
    std::vector<bool> aFree(aResult.mMaxUsedCount, false);
    for (std::uint32_t i = 0; i < aResult.mMaxUsedCount; ++i)
    {
        const std::uint32_t anID =
            ReadU32At(aBlock, static_cast<std::size_t>(i) * theSlotSize +
                theSlotSize - 4U);
        if (anID & DATA_ARRAY_KEY_MASK)
        {
            ++aLiveCount;
            if ((anID & DATA_ARRAY_INDEX_MASK) != i)
            {
                Fail(aHeaderOffset, "Trail.mID", "存活 ID 的低 16 位不等于槽索引");
            }
        }
    }
    std::uint32_t aCursor = aResult.mFreeListHead;
    while (aCursor != aResult.mMaxUsedCount)
    {
        if (aCursor >= aResult.mMaxUsedCount || aFree[aCursor])
        {
            Fail(aHeaderOffset, "Trail.freeList", "自由链越界或存在环");
        }
        aFree[aCursor] = true;
        aCursor = ReadU32At(
            aBlock,
            static_cast<std::size_t>(aCursor) * theSlotSize +
                theSlotSize - 4U);
    }
    if (aLiveCount != aResult.mSize)
    {
        Fail(aHeaderOffset, "Trail.mSize", "存活槽计数与 mSize 不一致");
    }
    return aResult;
}

void WriteRetiredTrailPool(Writer& theWriter, const RetiredTrailPool& thePool)
{
    theWriter.U32(thePool.mFreeListHead);
    theWriter.U32(thePool.mMaxUsedCount);
    theWriter.U32(thePool.mSize);
    theWriter.SyncBytes(thePool.mRawBlock);
}

std::vector<std::uint32_t> ReadIDList(
    Reader& theReader,
    const std::string& theField)
{
    const std::size_t aCountOffset = theReader.mPosition;
    const std::int32_t aCount = theReader.I32(theField + ".count");
    if (aCount < 0 || aCount > 1024)
    {
        Fail(aCountOffset, theField, "ID 列表数量不在 0..1024");
    }
    std::vector<std::uint32_t> aResult;
    aResult.reserve(aCount);
    for (int i = 0; i < aCount; ++i)
    {
        const auto anID = theReader.SyncBytes(4, theField + "[" + std::to_string(i) + "]");
        aResult.push_back(ReadU32At(anID, 0));
    }
    return aResult;
}

// 磁盘上的图像 ID 属于"写入方引擎"的 ResourceId 空间：legacy = 1073 exe 序，
// dat64 v1/v2 = 冻结历史枚举序（两序 99 号起分岔，仅 101/879 同位）。
// 校验用磁盘空间的"是图像"谓词，Document 规范态一律存 modern 空间 id。
void ValidateResourceID(
    std::int32_t theResourceID,
    std::size_t theOffset,
    const std::string& theField,
    Format theFileFormat)
{
    const int aMaximum = static_cast<int>(FrozenSaveResources::ResourceId::RESOURCE_ID_MAX);
    if (theResourceID < 0 || theResourceID > aMaximum)
    {
        Fail(theOffset, theField, "资源 ID 越界");
    }
    if (theResourceID == aMaximum)
    {
        return;
    }
    const bool isImage = theFileFormat == Format::Legacy1073Dat
        ? LevelSaveRemap::kExeIsImage[theResourceID]
        : FrozenSaveResources::IsImageResourceId(static_cast<FrozenSaveResources::ResourceId>(theResourceID));
    if (!isImage)
    {
        Fail(theOffset, theField, "资源 ID 不是图片（字体或音效）");
    }
}

std::int32_t RemapResourceToCanonical(std::int32_t theResourceID, Format theFileFormat)
{
    if (theResourceID == static_cast<std::int32_t>(FrozenSaveResources::ResourceId::RESOURCE_ID_MAX))
    {
        return theResourceID;
    }
    return theFileFormat == Format::Legacy1073Dat
        ? LevelSaveRemap::kExeToModern[theResourceID]
        : theResourceID;
}

std::int32_t RemapResourceFromCanonical(std::int32_t theResourceID, Format theTargetFormat)
{
    if (theResourceID == static_cast<std::int32_t>(FrozenSaveResources::ResourceId::RESOURCE_ID_MAX))
    {
        return theResourceID;
    }
    return theTargetFormat == Format::Legacy1073Dat
        ? LevelSaveRemap::kModernToExe[theResourceID]
        : theResourceID;
}

struct ReanimationChoiceKey
{
    std::uint32_t mIndex;
    std::size_t mPosition;
    bool operator==(const ReanimationChoiceKey&) const = default;
};

struct ReanimationChoiceHash
{
    std::size_t operator()(const ReanimationChoiceKey& theKey) const noexcept
    {
        return std::hash<std::size_t>{}(theKey.mPosition) ^
            (static_cast<std::size_t>(theKey.mIndex) * 0x9E3779B97F4A7C15ULL);
    }
};

std::vector<ReanimationSupplement> ReadReanimations(
    Reader& theReader,
    Format theFormat,
    std::uint32_t theCount,
    std::size_t theEnd)
{
    const auto aData = theReader.mData;
    const std::size_t aStart = theReader.mPosition;
    const RecordLayout& aTrackLayout =
        LevelSaveFormat::GetRecordLayout(RecordKind::ReanimatorTrackInstance);
    const auto trackFields = Dat64TimeSchema::GetFields(RecordKind::ReanimatorTrackInstance);
    const std::size_t aTrackBaseSize = aTrackLayout.mDat64Size -
        (HasReanimationVariants(theFormat) ? 8 : 0);
    const std::size_t aTrackSize = theFormat == Format::Legacy1073Dat
        ? aTrackLayout.mLegacySize
        : IsNanosecondFormat(theFormat)
            ? Dat64TimeSchema::V2Size(aTrackBaseSize, trackFields)
            : aTrackBaseSize;
    const std::size_t aTrackBoolOffset = theFormat == Format::Legacy1073Dat ? 92
        : IsNanosecondFormat(theFormat) ? Dat64TimeSchema::V2Offset(
            HasReanimationVariants(theFormat) ? 100 : 108, trackFields) : 108;
    const int aResourceMaximum = static_cast<int>(FrozenSaveResources::ResourceId::RESOURCE_ID_MAX);

    std::unordered_map<ReanimationChoiceKey, int, ReanimationChoiceHash> aMemo;
    std::function<bool(std::uint32_t, std::size_t)> Solve =
        [&](std::uint32_t theIndex, std::size_t thePosition) -> bool
    {
        const ReanimationChoiceKey aKey{theIndex, thePosition};
        if (const auto anIt = aMemo.find(aKey); anIt != aMemo.end())
        {
            return anIt->second >= 0;
        }
        if (theIndex == theCount)
        {
            const bool isDone = thePosition == theEnd;
            aMemo.emplace(aKey, isDone ? 0 : -2);
            return isDone;
        }
        if (thePosition + 4 > theEnd)
        {
            aMemo.emplace(aKey, -2);
            return false;
        }
        const std::int32_t aDefinition = ReadI32At(aData, thePosition);
        if (aDefinition < 0 || aDefinition >= NUM_REANIMS)
        {
            aMemo.emplace(aKey, -2);
            return false;
        }

        if (thePosition + 8 <= theEnd)
        {
            const std::uint32_t aByteCount = ReadU32At(aData, thePosition + 4);
            if (aByteCount > 0 && aByteCount % aTrackSize == 0)
            {
                const std::uint32_t aTrackCount = aByteCount / static_cast<std::uint32_t>(aTrackSize);
                const std::size_t aTrackStart = thePosition + 8;
                const std::size_t anImageStart = aTrackStart + aByteCount;
                const std::size_t aNext = anImageStart + (HasReanimationVariants(theFormat)
                    ? 0 : static_cast<std::size_t>(aTrackCount) * 4);
                bool isPlausible = aTrackCount <= 4096 && aNext <= theEnd;
                for (std::uint32_t i = 0; isPlausible && i < aTrackCount; ++i)
                {
                    const std::size_t aTrack = aTrackStart + static_cast<std::size_t>(i) * aTrackSize;
                    for (std::size_t b = 0; b < 4; ++b)
                    {
                        if (aData[aTrack + aTrackBoolOffset + b] > 1)
                        {
                            isPlausible = false;
                            break;
                        }
                    }
                    if (HasReanimationVariants(theFormat)) continue;
                    const std::int32_t aResource = ReadI32At(aData, anImageStart + i * 4);
                    const bool aResourceOk = aResource == aResourceMaximum ||
                        (aResource >= 0 && aResource < aResourceMaximum &&
                            (theFormat == Format::Legacy1073Dat
                                ? LevelSaveRemap::kExeIsImage[aResource]
                                : FrozenSaveResources::IsImageResourceId(static_cast<FrozenSaveResources::ResourceId>(aResource))));
                    if (!aResourceOk)
                    {
                        isPlausible = false;
                    }
                }
                if (isPlausible && Solve(theIndex + 1, aNext))
                {
                    aMemo[aKey] = static_cast<int>(aTrackCount);
                    return true;
                }
            }
        }

        if (Solve(theIndex + 1, thePosition + 4))
        {
            aMemo[aKey] = 0;
            return true;
        }
        aMemo[aKey] = -2;
        return false;
    };

    if (!Solve(0, aStart))
    {
        Fail(aStart, "Reanimation supplements", "无法按全部字段解析到固定尾块");
    }

    std::vector<ReanimationSupplement> aResult;
    aResult.reserve(theCount);
    for (std::uint32_t index = 0; index < theCount; ++index)
    {
        const std::size_t aDefinitionOffset = theReader.mPosition;
        ReanimationSupplement aReanimation;
        aReanimation.mDefinitionID = theReader.I32("Reanimation.definition");
        if (aReanimation.mDefinitionID < 0 || aReanimation.mDefinitionID >= NUM_REANIMS)
        {
            Fail(aDefinitionOffset, "Reanimation.definition", "定义 ID 越界");
        }
        const ReanimationChoiceKey aKey{index, aDefinitionOffset};
        const int aTrackCount = aMemo.at(aKey);
        if (aTrackCount > 0)
        {
            const std::size_t aTrackBlockOffset = theReader.mPosition + 4;
            const auto aTrackBlock = theReader.SyncBytes(
                static_cast<std::size_t>(aTrackCount) * aTrackSize,
                "Reanimation.tracks");
            aReanimation.mCanonicalTracks.reserve(aTrackCount);
            aReanimation.mImageResourceIDs.reserve(aTrackCount);
            for (int track = 0; track < aTrackCount; ++track)
            {
                auto bytes = ReadTimeRecord(theReader,
                    aTrackBlock.subspan(static_cast<std::size_t>(track) * aTrackSize, aTrackSize),
                    trackFields, theFormat);
                if (HasReanimationVariants(theFormat))
                    bytes.insert(bytes.begin() + 80, 8, 0);
                aReanimation.mCanonicalTracks.push_back(ToCanonicalRecord(
                    bytes,
                    IsNanosecondFormat(theFormat) ? Format::Dat64V1 : theFormat,
                    RecordKind::ReanimatorTrackInstance,
                    aTrackBlockOffset + static_cast<std::size_t>(track) * aTrackSize,
                    "Reanimation.track[" + std::to_string(track) + "]"));
            }
            for (int track = 0; !HasReanimationVariants(theFormat) && track < aTrackCount; ++track)
            {
                const std::size_t anImageOffset = theReader.mPosition;
                const std::int32_t anImage = theReader.I32("Reanimation.trackImage");
                ValidateResourceID(anImage, anImageOffset, "Reanimation.trackImage", theFormat);
                aReanimation.mImageResourceIDs.push_back(RemapResourceToCanonical(anImage, theFormat));
            }
        }
        aResult.push_back(std::move(aReanimation));
    }
    if (theReader.mPosition != theEnd)
    {
        Fail(theReader.mPosition, "Reanimation supplements", "没有正好结束在 Trail 定义之前");
    }
    return aResult;
}

void ValidateReferences(const Document& theDocument)
{
    const std::vector<std::uint32_t> anEmitterIDs = LiveIDs(theDocument.mArrays[7]);
    const std::vector<std::uint32_t> aParticleIDs = LiveIDs(theDocument.mArrays[8]);
    const std::unordered_set<std::uint32_t> anEmitterSet(anEmitterIDs.begin(), anEmitterIDs.end());
    const std::unordered_set<std::uint32_t> aParticleSet(aParticleIDs.begin(), aParticleIDs.end());
    std::unordered_set<std::uint32_t> seenEmitters;
    std::unordered_set<std::uint32_t> seenParticles;

    for (const ParticleSystemSupplement& aSystem : theDocument.mParticleSystems)
    {
        for (const std::uint32_t anEmitterID : aSystem.mEmitterIDs)
        {
            if (!anEmitterSet.contains(anEmitterID))
            {
                Fail("粒子系统引用了不存在的 ParticleEmitter ID");
            }
            if (!seenEmitters.insert(anEmitterID).second)
            {
                Fail("同一个 ParticleEmitter ID 被多个粒子系统重复拥有");
            }
        }
        for (const EmitterSupplement& anEmitter : aSystem.mEmitters)
        {
            for (const std::uint32_t aParticleID : anEmitter.mParticleIDs)
            {
                if (!aParticleSet.contains(aParticleID))
                {
                    Fail("粒子发射器引用了不存在的 Particle ID");
                }
                if (!seenParticles.insert(aParticleID).second)
                {
                    Fail("同一个 Particle ID 被多个发射器重复拥有");
                }
            }
        }
    }
    if (seenEmitters.size() != anEmitterSet.size())
    {
        Fail("存在没有归属粒子系统的存活 ParticleEmitter");
    }
    if (seenParticles.size() != aParticleSet.size())
    {
        Fail("存在没有归属发射器的存活 Particle");
    }
}

Document ParsePayload(
    Reader& theReader,
    Format theFormat,
    Dat64Grammar theDat64Grammar = Dat64Grammar::Current)
{
    Document aDocument;
    const std::size_t aBoardOffset = theReader.mPosition + 4;
    const std::size_t aBoardSize = theFormat == Format::Legacy1073Dat
        ? LevelSaveFormat::LEGACY_BOARD_STATE_SIZE
        : LevelSaveFormat::DAT64_BOARD_STATE_SIZE;
    if (theFormat != Format::Legacy1073Dat)
    {
        BoardDialect anUnusedDialect = BoardDialect::Canonical;
        const auto fields = Dat64TimeSchema::BoardFields();
        const auto bytes = ReadTimeRecord(theReader,
            theReader.SyncBytes(IsNanosecondFormat(theFormat)
                ? Dat64TimeSchema::V2Size(aBoardSize, fields) : aBoardSize, "Board"),
            fields, theFormat);
        aDocument.mCanonicalBoard = BoardToCanonical(
            bytes, Format::Dat64V1, aBoardOffset, anUnusedDialect);
    }
    else
    {
        // legacy 棋盘块有两种长度：1073 规范 22108，本工程旧构建 22092。
        const std::size_t aLengthOffset = theReader.mPosition;
        const std::uint32_t anActualSize = theReader.U32("Board.size");
        if (anActualSize != LEGACY_BOARD_STATE_SIZE &&
            anActualSize != LEGACY_BOARD_STATE_SIZE_PRE_MIGRATION)
        {
            std::ostringstream aReason;
            aReason << "SyncBytes 长度为 " << anActualSize << "，应为 "
                    << LEGACY_BOARD_STATE_SIZE << " 或（本工程旧构建）"
                    << LEGACY_BOARD_STATE_SIZE_PRE_MIGRATION;
            Fail(aLengthOffset, "Board", aReason.str());
        }
        BoardDialect aDialect = BoardDialect::Canonical;
        aDocument.mCanonicalBoard = BoardToCanonical(
            theReader.Bytes(anActualSize, "Board"),
            theFormat,
            aBoardOffset,
            aDialect);

        for (std::size_t i = 0; i + 1 < ARRAY_DESCRIPTORS.size(); ++i)
        {
            aDocument.mArrays[i] = ReadArray(theReader, theFormat, ARRAY_DESCRIPTORS[i], aDialect);
        }
        aDocument.mRetiredTrailPool = ReadRetiredTrailPool(theReader, LEGACY_TRAIL_SLOT_SIZE);
        aDocument.mArrays[ARRAY_DESCRIPTORS.size() - 1] =
            ReadArray(theReader, theFormat, ARRAY_DESCRIPTORS.back(), aDialect);

        const std::vector<std::uint32_t> aParticleSystemIDs = LiveIDs(aDocument.mArrays[6]);
        aDocument.mParticleSystems.reserve(aParticleSystemIDs.size());
        for (std::size_t system = 0; system < aParticleSystemIDs.size(); ++system)
        {
            ParticleSystemSupplement aSystem;
            const std::size_t aDefinitionOffset = theReader.mPosition;
            aSystem.mDefinitionID = ParticleIdToCanonical(
                theReader.I32("ParticleSystem.definition"), theFormat);
            if (aSystem.mDefinitionID < 0 || aSystem.mDefinitionID >= FROZEN_PARTICLE_COUNT)
            {
                Fail(aDefinitionOffset, "ParticleSystem.definition", "定义 ID 越界");
            }
            aSystem.mEmitterIDs = ReadIDList(theReader, "ParticleSystem.emitters");
            aSystem.mEmitters.reserve(aSystem.mEmitterIDs.size());
            for (std::size_t emitter = 0; emitter < aSystem.mEmitterIDs.size(); ++emitter)
            {
                EmitterSupplement anEmitter;
                const std::size_t anEmitterDefOffset = theReader.mPosition;
                anEmitter.mDefinitionIndex = theReader.I32("ParticleEmitter.definitionIndex");
                if (anEmitter.mDefinitionIndex < 0 || anEmitter.mDefinitionIndex >= 4096)
                {
                    Fail(anEmitterDefOffset, "ParticleEmitter.definitionIndex", "索引越界");
                }
                const std::size_t anImageOffset = theReader.mPosition;
                anEmitter.mImageResourceID = theReader.I32("ParticleEmitter.imageOverride");
                ValidateResourceID(anEmitter.mImageResourceID, anImageOffset, "ParticleEmitter.imageOverride", theFormat);
                anEmitter.mImageResourceID = RemapResourceToCanonical(anEmitter.mImageResourceID, theFormat);
                anEmitter.mParticleIDs = ReadIDList(theReader, "ParticleEmitter.particles");
                aSystem.mEmitters.push_back(std::move(anEmitter));
            }
            aDocument.mParticleSystems.push_back(std::move(aSystem));
        }

        const std::size_t aFixedTailSize = LEGACY_FIXED_TAIL_SIZE;
        if (theReader.Size() < aFixedTailSize)
        {
            Fail(theReader.mPosition, "fixedTail", "文件短于固定尾块");
        }
        const std::size_t aFixedTailOffset = theReader.Size() - aFixedTailSize;
        const std::uint32_t aTrailCount = aDocument.mRetiredTrailPool.mSize;
        if (aFixedTailOffset < static_cast<std::size_t>(aTrailCount) * 4 ||
            aFixedTailOffset - static_cast<std::size_t>(aTrailCount) * 4 < theReader.mPosition)
        {
            Fail(theReader.mPosition, "dynamicTail", "动态块与固定尾块重叠");
        }
        const std::size_t aReanimationEnd =
            aFixedTailOffset - static_cast<std::size_t>(aTrailCount) * 4;
        aDocument.mReanimations = ReadReanimations(
            theReader,
            theFormat,
            aDocument.mArrays[9].mSize,
            aReanimationEnd);

        aDocument.mRetiredTrailPool.mDefinitionIDs.reserve(aTrailCount);
        for (std::uint32_t i = 0; i < aTrailCount; ++i)
        {
            aDocument.mRetiredTrailPool.mDefinitionIDs.push_back(
                theReader.I32("Trail.definition(retired)"));
        }
        if (theReader.mPosition != aFixedTailOffset)
        {
            Fail(theReader.mPosition, "fixedTail", "Trail 定义没有正好到达固定尾块");
        }
    }

    if (theFormat != Format::Legacy1073Dat)
    {
        for (std::size_t i = 0; i + 1 < ARRAY_DESCRIPTORS.size(); ++i)
        {
            aDocument.mArrays[i] = ReadArray(theReader, theFormat, ARRAY_DESCRIPTORS[i], BoardDialect::Canonical);
        }
        if (theDat64Grammar == Dat64Grammar::Transitional)
        {
            // 过渡方言：Reanimation 之后是退役 Trail 池（dat64 槽 256），必须
            // 为空——dat64 现行写端没有这段，非空无迁移路径。
            RetiredTrailPool aTrail =
                ReadRetiredTrailPool(theReader, DAT64_TRAIL_SLOT_SIZE);
            if (!aTrail.IsEmpty())
            {
                Fail(theReader.mPosition, "Trail",
                    "过渡 dat64 方言的 Trail 池非空：现行 dat64 没有对应区段，无法迁移");
            }
        }
        aDocument.mArrays[ARRAY_DESCRIPTORS.size() - 1] =
            ReadArray(theReader, theFormat, ARRAY_DESCRIPTORS.back(), BoardDialect::Canonical);

        const std::vector<std::uint32_t> aParticleSystemIDs = LiveIDs(aDocument.mArrays[6]);
        aDocument.mParticleSystems.reserve(aParticleSystemIDs.size());
        for (std::size_t system = 0; system < aParticleSystemIDs.size(); ++system)
        {
            ParticleSystemSupplement aSystem;
            const std::size_t aDefinitionOffset = theReader.mPosition;
            // V3 has one authoritative definition ID, in the fixed system record.
            aSystem.mDefinitionID = HasParticleVariants(theFormat)
                ? ReadI32At(aDocument.mArrays[6].mSlots[
                    aParticleSystemIDs[system] & DATA_ARRAY_INDEX_MASK].mCanonicalRecord, 0)
                : ParticleIdToCanonical(theReader.I32("ParticleSystem.definition"), theFormat);
            if (aSystem.mDefinitionID < 0 || aSystem.mDefinitionID >= FROZEN_PARTICLE_COUNT)
            {
                Fail(aDefinitionOffset, "ParticleSystem.definition", "定义 ID 越界");
            }
            aSystem.mEmitterIDs = ReadIDList(theReader, "ParticleSystem.emitters");
            aSystem.mEmitters.reserve(aSystem.mEmitterIDs.size());
            for (std::size_t emitter = 0; emitter < aSystem.mEmitterIDs.size(); ++emitter)
            {
                EmitterSupplement anEmitter;
                const std::size_t anEmitterDefOffset = theReader.mPosition;
                anEmitter.mDefinitionIndex = theReader.I32("ParticleEmitter.definitionIndex");
                if (anEmitter.mDefinitionIndex < 0 || anEmitter.mDefinitionIndex >= 4096)
                {
                    Fail(anEmitterDefOffset, "ParticleEmitter.definitionIndex", "索引越界");
                }
                anEmitter.mImageResourceID = NO_PARTICLE_IMAGE;
                if (!HasParticleVariants(theFormat))
                {
                    const std::size_t anImageOffset = theReader.mPosition;
                    anEmitter.mImageResourceID = theReader.I32("ParticleEmitter.imageOverride");
                    ValidateResourceID(anEmitter.mImageResourceID, anImageOffset, "ParticleEmitter.imageOverride", theFormat);
                    anEmitter.mImageResourceID = RemapResourceToCanonical(anEmitter.mImageResourceID, theFormat);
                }
                anEmitter.mParticleIDs = ReadIDList(theReader, "ParticleEmitter.particles");
                aSystem.mEmitters.push_back(std::move(anEmitter));
            }
            aDocument.mParticleSystems.push_back(std::move(aSystem));
        }

        std::size_t aFixedTailSize = DAT64_FIXED_TAIL_SIZE;
        if (IsNanosecondFormat(theFormat))
        {
            aFixedTailSize += 4 * (
                Dat64TimeSchema::GetFields(RecordKind::CursorObject).size() +
                Dat64TimeSchema::GetFields(RecordKind::CursorPreview).size() +
                Dat64TimeSchema::GetFields(RecordKind::MessageWidget).size() +
                Dat64TimeSchema::SeedBankFields().size() +
                Dat64TimeSchema::GetFields(RecordKind::Challenge).size() +
                Dat64TimeSchema::GetFields(RecordKind::Music).size());
        }
        if (theReader.Size() < aFixedTailSize)
        {
            Fail(theReader.mPosition, "fixedTail", "文件短于固定尾块");
        }
        const std::size_t aFixedTailOffset = theReader.Size() - aFixedTailSize;
        if (aFixedTailOffset < theReader.mPosition)
        {
            Fail(theReader.mPosition, "dynamicTail", "动态块与固定尾块重叠");
        }
        aDocument.mReanimations = ReadReanimations(
            theReader,
            theFormat,
            aDocument.mArrays[9].mSize,
            aFixedTailOffset);
    }

    const auto ReadFixed = [&](RecordKind theKind, const std::string& theName)
    {
        const RecordLayout& aLayout = LevelSaveFormat::GetRecordLayout(theKind);
        const std::size_t aSize = theFormat == Format::Legacy1073Dat
            ? aLayout.mLegacySize
            : aLayout.mDat64Size;
        const std::size_t anOffset = theReader.mPosition + 4;
        const auto fields = Dat64TimeSchema::GetFields(theKind);
        const auto bytes = ReadTimeRecord(theReader,
            theReader.SyncBytes(IsNanosecondFormat(theFormat)
                ? Dat64TimeSchema::V2Size(aSize, fields) : aSize, theName),
            fields, theFormat);
        return ToCanonicalRecord(
            bytes,
            IsNanosecondFormat(theFormat) && theKind != RecordKind::Music ? Format::Dat64V1 : theFormat,
            theKind,
            anOffset,
            theName);
    };

    aDocument.mCursorObject = ReadFixed(RecordKind::CursorObject, "CursorObject");
    aDocument.mCursorPreview = ReadFixed(RecordKind::CursorPreview, "CursorPreview");
    aDocument.mMessageWidget = ReadFixed(RecordKind::MessageWidget, "MessageWidget");

    const std::size_t aSeedBankSize = theFormat == Format::Legacy1073Dat
        ? LevelSaveFormat::LEGACY_SEED_BANK_SIZE
        : LevelSaveFormat::DAT64_SEED_BANK_SIZE;
    const std::size_t aSeedBankOffset = theReader.mPosition + 4;
    const auto seedFields = Dat64TimeSchema::SeedBankFields();
    const auto seedBytes = ReadTimeRecord(theReader,
        theReader.SyncBytes(IsNanosecondFormat(theFormat)
            ? Dat64TimeSchema::V2Size(aSeedBankSize, seedFields) : aSeedBankSize, "SeedBank"),
        seedFields, theFormat);
    aDocument.mSeedBank = SeedBankToCanonical(
        seedBytes,
        IsNanosecondFormat(theFormat) ? Format::Dat64V1 : theFormat,
        aSeedBankOffset);

    aDocument.mChallenge = ReadFixed(RecordKind::Challenge, "Challenge");
    aDocument.mMusic = ReadFixed(RecordKind::Music, "Music");

    if (theFormat == Format::Legacy1073Dat)
    {
        const std::size_t anEndOffset = theReader.mPosition;
        if (theReader.U32("legacy.endMarker") != LEGACY_MAGIC)
        {
            Fail(anEndOffset, "legacy.endMarker", "不是 0xFEEDDEAD");
        }
    }
    else
    {
        const auto anEnd = theReader.SyncBytes(8, "dat64.endMarker");
        if (ReadU64At(anEnd, 0) != LevelSaveFormat::DAT64_END_MARKER)
        {
            Fail(theReader.mPosition - 8, "dat64.endMarker", "结束标记错误");
        }
    }
    if (theReader.Remaining() != 0)
    {
        Fail(theReader.mPosition, "file", "结束标记后仍有多余字节");
    }

    if (HasReanimationVariants(theFormat))
        RestoreAppearanceImages(aDocument);
    else
        VisitReanimationSupplements(aDocument, [](auto&, auto&) {});
    ValidateReferences(aDocument);
    NormalizeParticleImages(aDocument);
    if (IsNanosecondFormat(theFormat))
        aDocument.mNanoseconds = std::move(theReader.mNanoseconds);
    else
        CaptureLegacyTimes(aDocument);
    return aDocument;
}

Document ParseLegacy(std::span<const Byte> theFile, std::uint32_t& theDateEcho)
{
    Reader aReader(theFile);
    const auto aHeader = aReader.SyncBytes(12, "legacy.header");
    if (ReadU32At(aHeader, 0) != LEGACY_MAGIC ||
        ReadU32At(aHeader, 4) != LEGACY_VERSION ||
        (ReadU32At(aHeader, 8) != LEGACY_DATE_OFFICIAL &&
            ReadU32At(aHeader, 8) != LEGACY_DATE_PROJECT))
    {
        Fail(4, "legacy.header", "不是 PvZ 1.2.0.1073 关卡存档");
    }
    theDateEcho = ReadU32At(aHeader, 8);
    Document aDocument = ParsePayload(aReader, Format::Legacy1073Dat);
    // 源档日期戳回记到 canonical SeedBank 衬垫，随 dat64 一起旅行，回环时
    // 原样写回 legacy 头。
    WriteU32At(aDocument.mSeedBank, SEED_BANK_DATE_ECHO_OFFSET, theDateEcho);
    return aDocument;
}

Document ParseDat64(std::span<const Byte> theFile, std::uint32_t& theDateEcho)
{
    if (theFile.size() < LevelSaveFormat::DAT64_HEADER_SIZE)
    {
        Fail(0, "dat64.header", "文件短于 48 字节头部");
    }
    if (!std::equal(
            LevelSaveFormat::DAT64_MAGIC.begin(),
            LevelSaveFormat::DAT64_MAGIC.end(),
            theFile.begin()))
    {
        Fail(0, "dat64.magic", "不是 PVZDAT64");
    }
    const Format format = Dat64Format(theFile);
    if (!LevelSaveFormat::IsSupportedDat64Header(theFile))
    {
        Fail(8, "dat64.header", "版本、权威版本、flags、schema 或保留字段错误");
    }
    const std::uint64_t aPayloadSize = ReadU64At(theFile, 24);
    if (aPayloadSize != theFile.size() - LevelSaveFormat::DAT64_HEADER_SIZE)
    {
        Fail(24, "dat64.payloadSize", "与实际文件长度不一致");
    }
    const auto aPayload = theFile.subspan(LevelSaveFormat::DAT64_HEADER_SIZE);
    // The former payload CRC at 0x20 is ignored for v1/v2/v4.
    // 现行 11 池语法优先；失败则按“过渡 dat64”方言（2026-09-30 便携构建：
    // 12 池含退役 Trail 池，其余一致）重试一次。
    Reader aReader(aPayload);
    Document aDocument = [&]()
    {
        try
        {
            return ParsePayload(aReader, format, Dat64Grammar::Current);
        }
        catch (const std::exception& aCurrentError)
        {
            if (IsNanosecondFormat(format)) throw;
            try
            {
                Reader aRetry(aPayload);
                return ParsePayload(aRetry, Format::Dat64V1, Dat64Grammar::Transitional);
            }
            catch (const std::exception& aTransitionalError)
            {
                std::ostringstream aMessage;
                aMessage << aCurrentError.what()
                         << "（按过渡 dat64 方言重试亦失败："
                         << aTransitionalError.what() << "）";
                Fail(aMessage.str());
            }
        }
    }();
    // 方言标记归一：仅当标记为 22092 且 GOTY 统计字段仍为默认零时才保留
    // 旧方言；游戏经手改写过的档（衬垫可能带垃圾值）一律归回规范。归一
    // 发生在 canonical 上，保证回环的 Document 比较稳定成立。
    if (ReadU32At(aDocument.mCanonicalBoard, BOARD_DIALECT_MARKER_OFFSET) !=
            BOARD_DIALECT_MARKER_PRE_MIGRATION ||
        !BoardGotyStatsAreDefault(aDocument.mCanonicalBoard))
    {
        WriteU32At(
            aDocument.mCanonicalBoard,
            BOARD_DIALECT_MARKER_OFFSET,
            BOARD_DIALECT_MARKER_CANONICAL);
    }
    // SeedBank 衬垫里回记的 legacy 头日期戳（转换器写入）；无效值（游戏自
    // 写的 dat64 该处为零或垃圾）归一为官方值。
    std::uint32_t anEcho = ReadU32At(aDocument.mSeedBank, SEED_BANK_DATE_ECHO_OFFSET);
    if (anEcho != LEGACY_DATE_OFFICIAL && anEcho != LEGACY_DATE_PROJECT)
    {
        anEcho = LEGACY_DATE_OFFICIAL;
        WriteU32At(aDocument.mSeedBank, SEED_BANK_DATE_ECHO_OFFSET, anEcho);
    }
    theDateEcho = anEcho;
    return aDocument;
}

Document Parse(std::span<const Byte> theFile, Format theFormat, std::uint32_t& theDateEcho)
{
    return theFormat == Format::Legacy1073Dat
        ? ParseLegacy(theFile, theDateEcho)
        : ParseDat64(theFile, theDateEcho);
}

void WriteIDList(Writer& theWriter, const std::vector<std::uint32_t>& theIDs)
{
    theWriter.I32(static_cast<std::int32_t>(theIDs.size()));
    for (const std::uint32_t anID : theIDs)
    {
        std::array<Byte, 4> aBytes{};
        WriteU32At(aBytes, 0, anID);
        theWriter.SyncBytes(aBytes);
    }
}

void WriteArray(
    Writer& theWriter,
    const ArraySnapshot& theArray,
    Format theFormat,
    const ArrayDescriptor& theDescriptor,
    BoardDialect theDialect)
{
    const RecordLayout& aLayout = LevelSaveFormat::GetRecordLayout(theArray.mKind);
    const auto timeFields = Dat64TimeSchema::GetFields(theArray.mKind);
    const std::size_t aV1SlotSize = theFormat == Format::Legacy1073Dat
        ? LegacySlotSizeFor(theDescriptor, theDialect)
        : aLayout.mDat64ArraySlotSize;
    const std::size_t aV7Size = HasV7State(theFormat)
        ? V7SaveSchema::Size(theArray.mKind) : 0;
    const std::size_t aSlotSize = (IsNanosecondFormat(theFormat)
        ? Dat64TimeSchema::V2Size(aV1SlotSize, timeFields) : aV1SlotSize) + aV7Size;
    // 记录长度/ID 偏移与 ReadArray 同规则（Coin 尾衬垫；旧方言 Zombie 344）。
    const std::size_t anObjectSize = theFormat == Format::Legacy1073Dat
        ? (theArray.mKind == RecordKind::Zombie &&
                   theDialect == BoardDialect::PreMigration
               ? LEGACY_ZOMBIE_SLOT_SIZE_PRE_MIGRATION - 4U
               : aLayout.mLegacySize)
        : aLayout.mDat64Size;
    theWriter.U32(theArray.mFreeListHead);
    theWriter.U32(theArray.mMaxUsedCount);
    theWriter.U32(theArray.mSize);
    std::vector<Byte> aBlock(static_cast<std::size_t>(theArray.mMaxUsedCount) * aSlotSize, 0);
    for (std::size_t i = 0; i < theArray.mSlots.size(); ++i)
    {
        const SlotSnapshot& aSlot = theArray.mSlots[i];
        std::vector<Byte> aV1Slot(aV1SlotSize, 0);
        Byte* aDestination = aV1Slot.data();
        if (aSlot.mID & DATA_ARRAY_KEY_MASK)
        {
            const std::vector<Byte> aRecord = FromCanonicalRecord(
                aSlot.mCanonicalRecord,
                IsNanosecondFormat(theFormat) && theDescriptor.mKind != RecordKind::ParticleSystem
                    ? Format::Dat64V1 : theFormat,
                theArray.mKind);
            std::memcpy(aDestination, aRecord.data(), aRecord.size());
        }
        else
        {
            // 死槽的原始字节按读入时的原样写回（见 ReadArray）。
            std::memcpy(
                aDestination,
                aSlot.mCanonicalRecord.data(),
                std::min<std::size_t>(anObjectSize, aSlot.mCanonicalRecord.size()));
        }
        WriteU32At(
            std::span<Byte>(aDestination, aV1SlotSize),
            anObjectSize,
            aSlot.mID);
        auto wireSlot = WriteTimeRecord(theWriter, aV1Slot, timeFields, theFormat);
        if (aV7Size)
        {
            const std::size_t offset = Dat64TimeSchema::V2Size(anObjectSize, timeFields);
            if (aSlot.mV7State.size() != aV7Size) Fail("v3.3 exact-state size differs from its schema");
            wireSlot.insert(wireSlot.begin() + offset, aSlot.mV7State.begin(), aSlot.mV7State.end());
        }
        std::memcpy(aBlock.data() + i * aSlotSize, wireSlot.data(), wireSlot.size());
    }
    theWriter.SyncBytes(aBlock);
}

std::vector<Byte> WritePayload(
    const Document& theSourceDocument,
    Format theFormat)
{
    const Document theDocument = ParticleImagesForOutput(theSourceDocument, theFormat);
    BoardDialect aDialect = BoardDialect::Canonical;
    Writer aWriter;
    aWriter.mNanoseconds = &theDocument.mNanoseconds;
    // Document image IDs use modern enum space. Supplement IDs are remapped
    // below when writing the legacy exe space.
    const std::vector<Byte> aBoard =
        BoardFromCanonical(theDocument.mCanonicalBoard,
            IsNanosecondFormat(theFormat) ? Format::Dat64V1 : theFormat, aDialect);
    aWriter.SyncBytes(WriteTimeRecord(aWriter, aBoard, Dat64TimeSchema::BoardFields(), theFormat));

    for (std::size_t i = 0; i + 1 < ARRAY_DESCRIPTORS.size(); ++i)
    {
        WriteArray(aWriter, theDocument.mArrays[i], theFormat, ARRAY_DESCRIPTORS[i], aDialect);
    }
    if (theFormat == Format::Legacy1073Dat)
    {
        WriteRetiredTrailPool(aWriter, theDocument.mRetiredTrailPool);
    }
    else if (!theDocument.mRetiredTrailPool.IsEmpty())
    {
        Fail("退役 Trail 池非空：dat64 格式没有对应区段，无法迁移");
    }
    WriteArray(
        aWriter,
        theDocument.mArrays[ARRAY_DESCRIPTORS.size() - 1],
        theFormat,
        ARRAY_DESCRIPTORS.back(),
        aDialect);

    for (const ParticleSystemSupplement& aSystem : theDocument.mParticleSystems)
    {
        if (!HasParticleVariants(theFormat))
            aWriter.I32(ParticleIdFromCanonical(aSystem.mDefinitionID, theFormat));
        WriteIDList(aWriter, aSystem.mEmitterIDs);
        for (const EmitterSupplement& anEmitter : aSystem.mEmitters)
        {
            aWriter.I32(anEmitter.mDefinitionIndex);
            if (!HasParticleVariants(theFormat))
                aWriter.I32(RemapResourceFromCanonical(anEmitter.mImageResourceID, theFormat));
            WriteIDList(aWriter, anEmitter.mParticleIDs);
        }
    }

    for (const ReanimationSupplement& aReanimation : theDocument.mReanimations)
    {
        aWriter.I32(aReanimation.mDefinitionID);
        if (!aReanimation.mCanonicalTracks.empty())
        {
            const RecordLayout& aLayout =
                LevelSaveFormat::GetRecordLayout(RecordKind::ReanimatorTrackInstance);
            const auto fields = Dat64TimeSchema::GetFields(RecordKind::ReanimatorTrackInstance);
            const std::size_t aTrackBaseSize = aLayout.mDat64Size -
                (HasReanimationVariants(theFormat) ? 8 : 0);
            const std::size_t aTrackSize = theFormat == Format::Legacy1073Dat
                ? aLayout.mLegacySize
                : IsNanosecondFormat(theFormat)
                    ? Dat64TimeSchema::V2Size(aTrackBaseSize, fields) : aTrackBaseSize;
            std::vector<Byte> aTrackBlock(
                aReanimation.mCanonicalTracks.size() * aTrackSize,
                0);
            for (std::size_t track = 0; track < aReanimation.mCanonicalTracks.size(); ++track)
            {
                std::vector<Byte> aV1Track = FromCanonicalRecord(
                    aReanimation.mCanonicalTracks[track],
                    IsNanosecondFormat(theFormat) ? Format::Dat64V1 : theFormat,
                    RecordKind::ReanimatorTrackInstance);
                if (HasReanimationVariants(theFormat))
                    aV1Track.erase(aV1Track.begin() + 80, aV1Track.begin() + 88);
                const auto aTrack = WriteTimeRecord(aWriter, aV1Track, fields, theFormat);
                std::memcpy(
                    aTrackBlock.data() + track * aTrackSize,
                    aTrack.data(),
                    aTrack.size());
            }
            aWriter.SyncBytes(aTrackBlock);
            if (!HasReanimationVariants(theFormat))
            for (const std::int32_t anImageID : aReanimation.mImageResourceIDs)
            {
                aWriter.I32(RemapResourceFromCanonical(anImageID, theFormat));
            }
        }
    }

    if (theFormat == Format::Legacy1073Dat)
    {
        for (const std::int32_t aDefinition : theDocument.mRetiredTrailPool.mDefinitionIDs)
        {
            aWriter.I32(aDefinition);
        }
    }

    const auto WriteFixed = [&](RecordKind theKind, const std::vector<Byte>& theRecord)
    {
        const auto bytes = FromCanonicalRecord(theRecord,
            IsNanosecondFormat(theFormat) && theKind != RecordKind::Music ? Format::Dat64V1 : theFormat, theKind);
        aWriter.SyncBytes(WriteTimeRecord(aWriter, bytes, Dat64TimeSchema::GetFields(theKind), theFormat));
    };
    WriteFixed(RecordKind::CursorObject, theDocument.mCursorObject);
    WriteFixed(RecordKind::CursorPreview, theDocument.mCursorPreview);
    WriteFixed(RecordKind::MessageWidget, theDocument.mMessageWidget);

    if (theFormat != Format::Legacy1073Dat)
    {
        // SeedBank canonical 里已带日期戳回记（见 Parse*），整块写出。
        aWriter.SyncBytes(WriteTimeRecord(aWriter, theDocument.mSeedBank,
            Dat64TimeSchema::SeedBankFields(), theFormat));
    }
    else
    {
        aWriter.SyncBytes(SeedBankFromCanonical(theDocument.mSeedBank, theFormat));
    }
    WriteFixed(RecordKind::Challenge, theDocument.mChallenge);
    WriteFixed(RecordKind::Music, theDocument.mMusic);

    if (theFormat == Format::Legacy1073Dat)
    {
        aWriter.U32(LEGACY_MAGIC);
    }
    else
    {
        std::array<Byte, 8> anEnd{};
        WriteU64At(anEnd, 0, LevelSaveFormat::DAT64_END_MARKER);
        aWriter.SyncBytes(anEnd);
    }
    if (IsNanosecondFormat(theFormat) && aWriter.mTimeIndex != theDocument.mNanoseconds.size())
        Fail("内部错误：写出时间字段多余");
    return aWriter.mData;
}

std::vector<Byte> WriteFileImage(
    const Document& theDocument,
    Format theFormat,
    std::uint32_t theDateEcho)
{
    const std::vector<Byte> aPayload = WritePayload(theDocument, theFormat);
    Writer aFile;
    if (theFormat == Format::Legacy1073Dat)
    {
        std::array<Byte, 12> aHeader{};
        WriteU32At(aHeader, 0, LEGACY_MAGIC);
        WriteU32At(aHeader, 4, LEGACY_VERSION);
        // 回记的源档日期戳优先（保证回环逐字节一致）；没有回记信息时写官方值。
        WriteU32At(aHeader, 8, theDateEcho != 0 ? theDateEcho : LEGACY_DATE_OFFICIAL);
        aFile.SyncBytes(aHeader);
    }
    else
    {
        aFile.Bytes(LevelSaveFormat::DAT64_MAGIC);
        aFile.U32(HasV7State(theFormat) ? LevelSaveFormat::DAT64_VERSION :
            theFormat == Format::Dat64V2 ? LevelSaveFormat::DAT64_V2_VERSION : LevelSaveFormat::DAT64_V1_VERSION);
        aFile.U32(LevelSaveFormat::DAT64_HEADER_SIZE);
        aFile.U32(LevelSaveFormat::DAT64_AUTHORITY_BUILD);
        aFile.U32(IsNanosecondFormat(theFormat) ? Dat64TimeSchema::FLAGS : LevelSaveFormat::DAT64_V1_FLAGS);
        aFile.U64(aPayload.size());
        // v1 exports retain a CRC for older readers; v2/v4 reserve the slot
        // as zero. No version requires a matching CRC on input.
        aFile.U32(theFormat == Format::Dat64V1 ? Crc32(aPayload) : 0);
        aFile.U32(theFormat == Format::Dat64V4 ? LevelSaveFormat::DAT64_SCHEMA_ID :
            theFormat == Format::Dat64V2 ? LevelSaveFormat::DAT64_V2_SCHEMA_ID : LevelSaveFormat::DAT64_V1_SCHEMA_ID);
        aFile.U32(theFormat == Format::Dat64V4 ? LevelSaveFormat::DAT64_MINOR_VERSION : 0);
        aFile.U32(0);
        if (aFile.Position() != LevelSaveFormat::DAT64_HEADER_SIZE)
        {
            Fail("内部错误：dat64 头部不是 48 字节");
        }
    }
    aFile.Bytes(aPayload);
    return aFile.mData;
}

void AtomicWrite(
    const std::filesystem::path& theOutput,
    std::span<const Byte> theData)
{
    const std::filesystem::path aParent = theOutput.parent_path();
    if (!aParent.empty() && !std::filesystem::is_directory(aParent))
    {
        Fail("输出目录不存在：" + aParent.string());
    }
    std::filesystem::path aTemporary = theOutput;
#ifdef _WIN32
    aTemporary += ".tmp." + std::to_string(GetCurrentProcessId());
#else
    aTemporary += ".tmp";
#endif
    if (std::filesystem::exists(aTemporary))
    {
        std::filesystem::remove(aTemporary);
    }

    try
    {
        std::ofstream aFile(aTemporary, std::ios::binary | std::ios::trunc);
        if (!aFile)
        {
            Fail("无法创建临时输出文件：" + aTemporary.string());
        }
        if (!theData.empty())
        {
            aFile.write(
                reinterpret_cast<const char*>(theData.data()),
                static_cast<std::streamsize>(theData.size()));
        }
        aFile.flush();
        if (!aFile)
        {
            Fail("写入临时输出文件失败：" + aTemporary.string());
        }
        aFile.close();

#ifdef _WIN32
        if (!MoveFileExW(
                aTemporary.c_str(),
                theOutput.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
        {
            const DWORD anError = GetLastError();
            std::ostringstream aReason;
            aReason << "原子替换目标文件失败，Win32 错误 " << anError;
            Fail(aReason.str());
        }
#else
        std::filesystem::rename(aTemporary, theOutput);
#endif
    }
    catch (...)
    {
        std::error_code anIgnored;
        std::filesystem::remove(aTemporary, anIgnored);
        throw;
    }
}

// ---------------------------------------------------------------------------
// user<ID>.dat / user<ID>.dat64 —— 玩家明细档
// ---------------------------------------------------------------------------

// 字段序=PlayerInfo.cpp SyncDetails（GOTY 1073 0046BE80）。转换器自包含，
// 不 include 游戏侧 PlayerInfo.h，尺寸常量与之对拍（kPottedPlantSize=88、
// kZombatarSize=72、MAX_POTTED_PLANTS=200、MAX_NUM_ZOMBATARS=100）。
constexpr std::uint32_t USER_V12_VERSION = 12;
constexpr std::uint32_t USER_V13_VERSION = 13;
constexpr std::size_t USER_RECORD_COUNT = 100;
constexpr std::size_t USER_PURCHASE_COUNT = 80;
constexpr std::size_t USER_SCALAR_COUNT = 18;  // mHasUsedCheatKeys..mPlaceHolderPlayerStats
constexpr std::uint32_t USER_MAX_POTTED_PLANTS = 200;
constexpr std::size_t USER_POTTED_PLANT_SIZE = 88;
constexpr std::size_t USER_ACHIEVEMENT_BYTES = 40;  // 20 × (earned, shown) 交织字节
constexpr std::uint32_t USER_MAX_ZOMBATARS = 100;
constexpr std::size_t USER_ZOMBATAR_SIZE = 72;  // 9 页 × (i32 item, i32 color)
constexpr std::size_t USER_MINIGAME_COUNT = 20;
// v1 小游戏在旧枚举里是 16..35；v3 的 MINIGAME 区间是 1..20（槽=值-1）。
constexpr std::size_t USER_V1_MINIGAME_FIRST = 16;

struct UserDocument
{
    std::int32_t mLevel{};
    std::int32_t mCoins{};
    std::int32_t mFinishedAdventure{};
    // 槽序=GameMode v3 直下标（canonical）；死槽（73..99）恒零。
    std::array<std::int32_t, USER_RECORD_COUNT> mChallengeRecords{};
    std::array<std::int32_t, USER_PURCHASE_COUNT> mPurchases{};
    std::int32_t mPlayTimeActivePlayer{};
    std::int32_t mPlayTimeInactivePlayer{};
    std::array<std::int32_t, USER_SCALAR_COUNT> mScalars{};
    std::vector<std::array<Byte, USER_POTTED_PLANT_SIZE>> mPottedPlants;
    std::array<Byte, USER_ACHIEVEMENT_BYTES> mAchievements{};
    Byte mAcceptedZombatarULA{};
    std::vector<std::array<Byte, USER_ZOMBATAR_SIZE>> mZombatars;
    // 槽序=v3 值-1（MINIGAME 区间即槽）。
    std::array<Byte, USER_MINIGAME_COUNT> mMiniGamesCompleted{};
    Byte mShownZombatarDesktopMessage{};
    bool operator==(const UserDocument&) const = default;
};

UserDocument ReadUserWire(Reader& theReader)
{
    UserDocument aDocument;
    aDocument.mLevel = theReader.I32("User.mLevel");
    aDocument.mCoins = theReader.I32("User.mCoins");
    aDocument.mFinishedAdventure = theReader.I32("User.mFinishedAdventure");
    for (std::size_t i = 0; i < USER_RECORD_COUNT; ++i)
    {
        aDocument.mChallengeRecords[i] = theReader.I32("User.mChallengeRecords");
    }
    for (std::size_t i = 0; i < USER_PURCHASE_COUNT; ++i)
    {
        aDocument.mPurchases[i] = theReader.I32("User.mPurchases");
    }
    aDocument.mPlayTimeActivePlayer = theReader.I32("User.mPlayTimeActivePlayer");
    aDocument.mPlayTimeInactivePlayer = theReader.I32("User.mPlayTimeInactivePlayer");
    for (std::size_t i = 0; i < USER_SCALAR_COUNT; ++i)
    {
        aDocument.mScalars[i] = theReader.I32("User.scalar");
    }
    const std::size_t aNumPottedPlantsOffset = theReader.mPosition;
    const std::int32_t aNumPottedPlants = theReader.I32("User.mNumPottedPlants");
    if (aNumPottedPlants < 0 ||
        aNumPottedPlants > static_cast<std::int32_t>(USER_MAX_POTTED_PLANTS))
    {
        Fail(aNumPottedPlantsOffset, "User.mNumPottedPlants",
            "不在 0..200（变长段，字段级走读不可字节手术）");
    }
    aDocument.mPottedPlants.resize(static_cast<std::size_t>(aNumPottedPlants));
    for (std::size_t i = 0; i < aDocument.mPottedPlants.size(); ++i)
    {
        const auto aPlant = theReader.Bytes(USER_POTTED_PLANT_SIZE, "User.mPottedPlant");
        std::memcpy(aDocument.mPottedPlants[i].data(), aPlant.data(), USER_POTTED_PLANT_SIZE);
    }
    const std::size_t anAchievementsOffset = theReader.mPosition;
    const auto anAchievements = theReader.Bytes(
        USER_ACHIEVEMENT_BYTES, "User.achievements");
    std::memcpy(
        aDocument.mAchievements.data(), anAchievements.data(), USER_ACHIEVEMENT_BYTES);
    for (std::size_t i = 0; i < USER_ACHIEVEMENT_BYTES; ++i)
    {
        ValidateBool(
            aDocument.mAchievements, i, anAchievementsOffset, "User.achievementPair");
    }
    const std::size_t anULAOffset = theReader.mPosition;
    aDocument.mAcceptedZombatarULA = theReader.Bytes(1, "User.mAcceptedZombatarULA")[0];
    ValidateBool(
        std::span<const Byte>(&aDocument.mAcceptedZombatarULA, 1),
        0, anULAOffset, "User.mAcceptedZombatarULA");
    const std::size_t aNumZombatarsOffset = theReader.mPosition;
    const std::int32_t aNumZombatars = theReader.I32("User.mNumZombatars");
    if (aNumZombatars < 0 ||
        aNumZombatars > static_cast<std::int32_t>(USER_MAX_ZOMBATARS))
    {
        Fail(aNumZombatarsOffset, "User.mNumZombatars", "不在 0..100");
    }
    aDocument.mZombatars.resize(static_cast<std::size_t>(aNumZombatars));
    for (std::size_t i = 0; i < aDocument.mZombatars.size(); ++i)
    {
        const auto aZombatar = theReader.Bytes(USER_ZOMBATAR_SIZE, "User.mZombatars");
        std::memcpy(aDocument.mZombatars[i].data(), aZombatar.data(), USER_ZOMBATAR_SIZE);
    }
    const std::size_t aMiniGamesOffset = theReader.mPosition;
    const auto aMiniGames = theReader.Bytes(USER_MINIGAME_COUNT, "User.mMiniGamesCompleted");
    std::memcpy(
        aDocument.mMiniGamesCompleted.data(), aMiniGames.data(), USER_MINIGAME_COUNT);
    for (std::size_t i = 0; i < USER_MINIGAME_COUNT; ++i)
    {
        ValidateBool(
            aDocument.mMiniGamesCompleted, i, aMiniGamesOffset, "User.mMiniGamesCompleted");
    }
    const std::size_t aDesktopOffset = theReader.mPosition;
    aDocument.mShownZombatarDesktopMessage =
        theReader.Bytes(1, "User.mShownZombatarDesktopMessage")[0];
    ValidateBool(
        std::span<const Byte>(&aDocument.mShownZombatarDesktopMessage, 1),
        0, aDesktopOffset, "User.mShownZombatarDesktopMessage");
    return aDocument;
}

void WriteUserWire(Writer& theWriter, const UserDocument& theDocument, std::uint32_t theVersion)
{
    theWriter.I32(static_cast<std::int32_t>(theVersion));
    theWriter.I32(theDocument.mLevel);
    theWriter.I32(theDocument.mCoins);
    theWriter.I32(theDocument.mFinishedAdventure);
    for (const std::int32_t aRecord : theDocument.mChallengeRecords)
    {
        theWriter.I32(aRecord);
    }
    for (const std::int32_t aPurchase : theDocument.mPurchases)
    {
        theWriter.I32(aPurchase);
    }
    theWriter.I32(theDocument.mPlayTimeActivePlayer);
    theWriter.I32(theDocument.mPlayTimeInactivePlayer);
    for (const std::int32_t aScalar : theDocument.mScalars)
    {
        theWriter.I32(aScalar);
    }
    theWriter.I32(static_cast<std::int32_t>(theDocument.mPottedPlants.size()));
    for (const auto& aPlant : theDocument.mPottedPlants)
    {
        theWriter.Bytes(aPlant);
    }
    theWriter.Bytes(theDocument.mAchievements);
    theWriter.Bytes(std::span<const Byte>(&theDocument.mAcceptedZombatarULA, 1));
    theWriter.I32(static_cast<std::int32_t>(theDocument.mZombatars.size()));
    for (const auto& aZombatar : theDocument.mZombatars)
    {
        theWriter.Bytes(aZombatar);
    }
    theWriter.Bytes(theDocument.mMiniGamesCompleted);
    theWriter.Bytes(std::span<const Byte>(&theDocument.mShownZombatarDesktopMessage, 1));
}

// v1 旧枚举值 → v3 新枚举值（ConstEnums.h 的 kGameModeRemapV1ToV3 单点包装，
// 带界检）。v1 值域 0..71；越界即坏档/坏文件名。
int ConstEnumsV1ToV3(int theV1Mode)
{
    if (theV1Mode < 0 || theV1Mode >= 72)
    {
        std::ostringstream aReason;
        aReason << "v1 模式值 " << theV1Mode << " 越界（0..71）";
        Fail(aReason.str());
    }
    return static_cast<int>(kGameModeRemapV1ToV3[static_cast<std::size_t>(theV1Mode)]);
}

// v3 新枚举值 → v1 旧枚举值（逆置换，线性查 72 项）。v3 值 72（CUSTOM_LEVEL）
// 无 v1 落点，返回 -1。
int ConstEnumsV3ToV1(int theV3Mode)
{
    if (theV3Mode < 0 || theV3Mode > 72)
    {
        std::ostringstream aReason;
        aReason << "v3 模式值 " << theV3Mode << " 越界（0..72）";
        Fail(aReason.str());
    }
    for (int m = 0; m < 72; ++m)
    {
        if (static_cast<int>(kGameModeRemapV1ToV3[static_cast<std::size_t>(m)]) == theV3Mode)
        {
            return m;
        }
    }
    return -1;
}

// game<profileId>_<mode> 文件名解析；不匹配该形态返回 nullopt（自定义名走
// 纯扩展名互换，不动数字）。
struct GameSaveNameParts
{
    std::string mStemPrefix;  // "game<id>_"
    int mModeDigit;
};
std::optional<GameSaveNameParts> ParseGameSaveName(const std::string& theFilename)
{
    const std::size_t anUnderscore = theFilename.find('_');
    if (anUnderscore == std::string::npos || anUnderscore < 5 ||
        !theFilename.starts_with("game"))
    {
        return std::nullopt;
    }
    const std::string anId = theFilename.substr(4, anUnderscore - 4);
    std::size_t aDigitEnd = anUnderscore + 1;
    while (aDigitEnd < theFilename.size() &&
        std::isdigit(static_cast<unsigned char>(theFilename[aDigitEnd])) != 0)
    {
        ++aDigitEnd;
    }
    if (anId.empty() || aDigitEnd == anUnderscore + 1 ||
        aDigitEnd != theFilename.size())
    {
        return std::nullopt;
    }
    if (!std::all_of(anId.begin(), anId.end(), [](unsigned char c) {
            return std::isdigit(c) != 0;
        }))
    {
        return std::nullopt;
    }
    return GameSaveNameParts{
        theFilename.substr(0, anUnderscore + 1),
        std::stoi(theFilename.substr(anUnderscore + 1))};
}

// v12（槽=旧模式值-1）→ v13（槽=GameMode v3 直下标）。v12 死槽 71..99 是
// 历史垃圾（旧构建作弊键曾整表刷 20），语义上任何版本都不可读，直接归零。
UserDocument RemapUserRecordsV12ToV13(UserDocument theDocument)
{
    std::array<std::int32_t, USER_RECORD_COUNT> anOld = theDocument.mChallengeRecords;
    theDocument.mChallengeRecords.fill(0);
    for (int m = 1; m <= 71; ++m)
    {
        const auto aNewSlot = static_cast<std::size_t>(
            static_cast<int>(ConstEnumsV1ToV3(m)));
        theDocument.mChallengeRecords[aNewSlot] = anOld[static_cast<std::size_t>(m - 1)];
    }

    std::array<Byte, USER_MINIGAME_COUNT> anOldMini = theDocument.mMiniGamesCompleted;
    theDocument.mMiniGamesCompleted.fill(0);
    for (std::size_t i = 0; i < USER_MINIGAME_COUNT; ++i)
    {
        const auto aNewSlot = static_cast<std::size_t>(
            static_cast<int>(ConstEnumsV1ToV3(
                static_cast<int>(USER_V1_MINIGAME_FIRST + i))) - 1);
        theDocument.mMiniGamesCompleted[aNewSlot] = anOldMini[i];
    }
    return theDocument;
}

// v13 → v12：v13 槽 0（ADVENTURE）与 72（CUSTOM_LEVEL）没有 v1 落点，非零
// 即真实数据无法表达，明确拒绝；死槽 73..99 归零。
UserDocument RemapUserRecordsV13ToV12(UserDocument theDocument)
{
    if (theDocument.mChallengeRecords[0] != 0)
    {
        Fail("v13 挑战记录槽 0（ADVENTURE）非零，v12 user.dat 没有对应槽位");
    }
    if (theDocument.mChallengeRecords[72] != 0)
    {
        Fail("v13 挑战记录槽 72（CUSTOM_LEVEL）非零，v12 user.dat 没有对应槽位");
    }
    std::array<std::int32_t, USER_RECORD_COUNT> aNew;
    aNew.fill(0);
    for (int m = 1; m <= 71; ++m)
    {
        const auto aV13Slot = static_cast<std::size_t>(
            static_cast<int>(ConstEnumsV1ToV3(m)));
        aNew[static_cast<std::size_t>(m - 1)] = theDocument.mChallengeRecords[aV13Slot];
    }
    theDocument.mChallengeRecords = aNew;

    std::array<Byte, USER_MINIGAME_COUNT> aNewMini{};
    for (std::size_t i = 0; i < USER_MINIGAME_COUNT; ++i)
    {
        const auto aV13Slot = static_cast<std::size_t>(
            static_cast<int>(ConstEnumsV1ToV3(
                static_cast<int>(USER_V1_MINIGAME_FIRST + i))) - 1);
        aNewMini[i] = theDocument.mMiniGamesCompleted[aV13Slot];
    }
    theDocument.mMiniGamesCompleted = aNewMini;
    return theDocument;
}

UserDocument ParseUserV12(std::span<const Byte> theFile)
{
    Reader aReader(theFile);
    const std::size_t aVersionOffset = aReader.mPosition;
    const std::uint32_t aVersion = aReader.U32("User.version");
    if (aVersion != USER_V12_VERSION)
    {
        std::ostringstream aReason;
        aReason << "版本为 " << aVersion << "，应为 12（更旧的档先在原版里过一关升级）";
        Fail(aVersionOffset, "User.version", aReason.str());
    }
    UserDocument aDocument = ReadUserWire(aReader);
    if (aReader.Remaining() != 0)
    {
        Fail(aReader.mPosition, "User", "明细走读结束后仍有剩余字节");
    }
    return RemapUserRecordsV12ToV13(std::move(aDocument));
}

UserDocument ParseUserV13(std::span<const Byte> theFile)
{
    if (theFile.size() < LevelSaveFormat::USER64_HEADER_SIZE ||
        !std::equal(
            LevelSaveFormat::USER64_MAGIC.begin(),
            LevelSaveFormat::USER64_MAGIC.end(),
            theFile.begin()))
    {
        Fail(0, "user64.magic", "不是 PVZUSR64");
    }
    Reader aReader(theFile.subspan(LevelSaveFormat::USER64_HEADER_SIZE));
    const std::size_t aVersionOffset = aReader.mPosition;
    const std::uint32_t aVersion = aReader.U32("User.version");
    if (aVersion != USER_V13_VERSION)
    {
        std::ostringstream aReason;
        aReason << "版本为 " << aVersion << "，应为 13";
        Fail(aVersionOffset, "User.version", aReason.str());
    }
    UserDocument aDocument = ReadUserWire(aReader);
    if (aReader.Remaining() != 0)
    {
        Fail(aReader.mPosition, "User", "明细走读结束后仍有剩余字节");
    }
    return aDocument;
}

std::vector<Byte> WriteUserV12Image(const UserDocument& theDocument)
{
    Writer aWriter;
    WriteUserWire(aWriter, RemapUserRecordsV13ToV12(theDocument), USER_V12_VERSION);
    return aWriter.mData;
}

std::vector<Byte> WriteUserV13Image(const UserDocument& theDocument)
{
    Writer aWriter;
    aWriter.Bytes(LevelSaveFormat::USER64_MAGIC);
    WriteUserWire(aWriter, theDocument, USER_V13_VERSION);
    return aWriter.mData;
}
}

const char* FormatName(Format theFormat)
{
    switch (theFormat)
    {
    case Format::Legacy1073Dat: return "PvZ 1.2.0.1073 game*.dat";
    case Format::Dat64V1: return "Legacy dat64 v1";
    case Format::Dat64V2: return "Nanosecond dat64 v2";
    case Format::Dat64V4: return "Frame-cursor dat64 v4";
    }
    return "Unknown";
}

Format DetectFormat(const std::filesystem::path& thePath)
{
    const std::vector<Byte> aData = ReadFileFully(thePath);
    if (aData.size() >= LevelSaveFormat::DAT64_MAGIC.size() &&
        std::equal(
            LevelSaveFormat::DAT64_MAGIC.begin(),
            LevelSaveFormat::DAT64_MAGIC.end(),
            aData.begin()))
    {
        std::uint32_t aDateEcho = 0;
        ParseDat64(aData, aDateEcho);
        return Dat64Format(aData);
    }
    if (aData.size() >= 16 && ReadU32At(aData, 0) == 12 &&
        ReadU32At(aData, 4) == LEGACY_MAGIC)
    {
        std::uint32_t aDateEcho = 0;
        ParseLegacy(aData, aDateEcho);
        return Format::Legacy1073Dat;
    }
    Fail(0, "file.magic", "既不是 1073 game*.dat，也不是 PVZDAT64");
}

std::filesystem::path DefaultOutputPath(
    const std::filesystem::path& theInput,
    Format theInputFormat)
{
    // game<id>_<mode> 文件名里的模式数字随格式换系：dat（v1 数字）→ dat64
    //（v3 数字），反向同理。存档体内不含模式值（mGameMode 在 LawnApp，只进
    // 文件名），文件名就是模式数字的唯一载体——不搬数字的话转换出的 dat64
    // 会按 v3 读数指向完全不同的玩法（v1 5=生存普通5 → v3 5=最终Boss）。
    // 解析按主名（去扩展名）做；不匹配该形态的自定义名走纯扩展名互换。
    const auto aParts = ParseGameSaveName(theInput.stem().string());
    std::filesystem::path aResult = theInput;
    if (aParts)
    {
        int aTargetDigit;
        if (theInputFormat == Format::Legacy1073Dat)
        {
            aTargetDigit = ConstEnumsV1ToV3(aParts->mModeDigit);
        }
        else
        {
            aTargetDigit = ConstEnumsV3ToV1(aParts->mModeDigit);
            if (aTargetDigit < 0)
            {
                Fail(theInput.filename().string() +
                    "：模式 72（自制关卡）没有 v1 对应值，无法回写 .dat");
            }
        }
        aResult = theInput.parent_path() /
            (aParts->mStemPrefix + std::to_string(aTargetDigit) +
                (theInputFormat == Format::Legacy1073Dat ? ".dat64" : ".dat"));
        return aResult;
    }
    aResult.replace_extension(
        theInputFormat == Format::Legacy1073Dat ? ".dat64" : ".dat");
    return aResult;
}

ConversionResult ConvertFile(
    const std::filesystem::path& theInput,
    const std::filesystem::path& theOutput,
    const ConversionOptions& theOptions)
{
    const std::filesystem::path anInput = std::filesystem::absolute(theInput).lexically_normal();
    const std::filesystem::path anOutput = std::filesystem::absolute(theOutput).lexically_normal();
    if (anInput == anOutput)
    {
        Fail("输入和输出不能是同一个文件");
    }

    const std::vector<Byte> aSource = ReadFileFully(anInput);
    Format aSourceFormat;
    if (aSource.size() >= LevelSaveFormat::DAT64_MAGIC.size() &&
        std::equal(
            LevelSaveFormat::DAT64_MAGIC.begin(),
            LevelSaveFormat::DAT64_MAGIC.end(),
            aSource.begin()))
    {
        // Parse below checks the complete header before any output is opened.
        if (aSource.size() < LevelSaveFormat::DAT64_HEADER_SIZE)
            Fail(0, "dat64.header", "文件短于 48 字节头部");
        aSourceFormat = Dat64Format(aSource);
    }
    else if (aSource.size() >= 16 && ReadU32At(aSource, 0) == 12 &&
             ReadU32At(aSource, 4) == LEGACY_MAGIC)
    {
        aSourceFormat = Format::Legacy1073Dat;
    }
    else
    {
        Fail(0, "file.magic", "既不是 1073 game*.dat，也不是 PVZDAT64");
    }

    // Full parse and validation happens before any output file is opened.
    std::uint32_t aDateEcho = 0;
    Document aDocument = Parse(aSource, aSourceFormat, aDateEcho);
    const Format aTargetFormat = theOptions.mTargetFormat.value_or(
        aSourceFormat == Format::Legacy1073Dat ? Format::Dat64V4 : Format::Legacy1073Dat);
    PrepareAppearanceOutput(aDocument, aTargetFormat);
    if (aSourceFormat == Format::Dat64V4 && aTargetFormat != Format::Dat64V4)
    {
        std::size_t index = 0;
        VisitTimeRecords(aDocument, [&](auto& bytes, Dat64TimeSchema::Fields fields) {
            for (const auto& field : fields)
            {
                if (&bytes == &aDocument.mSeedBank && std::string_view(field.mName) == "mSlotMachineCountDown" &&
                    std::bit_cast<std::int64_t>(aDocument.mNanoseconds[index]) < 0)
                {
                    // Old readers represent final-turn waiting as one 10ms
                    // interval. Current readers retain the exact expired time.
                    aDocument.mNanoseconds[index] = 10'000'000;
                    WriteU32At(bytes, field.mOffset, 1);
                }
                ++index;
            }
        });
    }
    if (!IsNanosecondFormat(aTargetFormat))
    {
        QuantizeDocumentTimes(aDocument, theOptions.mRoundTimeToNearestTick);
    }
    if (!HasParticleVariants(aTargetFormat))
        QuantizeMusicTempo(aDocument);
    PrepareV7Output(aDocument, aTargetFormat);
    // Earlier magnet tails held start vectors and lookup parameters. They are
    // not velocities: discard those transient trajectory parameters explicitly.
    if (aTargetFormat == Format::Dat64V4 && aSourceFormat != Format::Dat64V4)
        for (auto& array : aDocument.mArrays)
            if (array.mKind == RecordKind::Plant)
                for (auto& slot : array.mSlots)
                    if (slot.mV7State.size() == V7SaveSchema::Size(RecordKind::Plant))
                        std::fill(slot.mV7State.begin() + 56, slot.mV7State.end(), Byte{0});
    const std::vector<Byte> aTarget = WriteFileImage(aDocument, aTargetFormat, aDateEcho);

    // Parse the generated target with the opposite grammar and compare every
    // canonical field before the atomic write.
    std::uint32_t aRoundTripDateEcho = 0;
    const Document aRoundTrip = Parse(aTarget, aTargetFormat, aRoundTripDateEcho);
    if (aRoundTrip != aDocument)
    {
        Fail("内部错误：生成文件虽能解析，但逐字段语义与输入不一致");
    }

    AtomicWrite(anOutput, aTarget);
    return {
        aSourceFormat,
        aTargetFormat,
        anInput,
        anOutput,
        aSource.size(),
        aTarget.size()
    };
}

const char* UserFormatName(UserFormat theFormat)
{
    return theFormat == UserFormat::LegacyUserDat
        ? "PvZ 1.2.0.1073 user*.dat（v12 明细）"
        : "user*.dat64（PVZUSR64 + v13 明细）";
}

UserFormat DetectUserFormat(const std::filesystem::path& thePath)
{
    const std::vector<Byte> aData = ReadFileFully(thePath);
    if (aData.size() >= LevelSaveFormat::USER64_MAGIC.size() &&
        std::equal(
            LevelSaveFormat::USER64_MAGIC.begin(),
            LevelSaveFormat::USER64_MAGIC.end(),
            aData.begin()))
    {
        ParseUserV13(aData);
        return UserFormat::UserDat64;
    }
    if (aData.size() >= 4 && ReadU32At(aData, 0) == USER_V12_VERSION)
    {
        ParseUserV12(aData);
        return UserFormat::LegacyUserDat;
    }
    Fail(0, "user.magic", "既不是 v12 user*.dat，也不是 PVZUSR64 user*.dat64");
}

std::filesystem::path DefaultUserOutputPath(
    const std::filesystem::path& theInput,
    UserFormat theInputFormat)
{
    std::filesystem::path aResult = theInput;
    aResult.replace_extension(
        theInputFormat == UserFormat::LegacyUserDat ? ".dat64" : ".dat");
    return aResult;
}

UserConversionResult ConvertUserFile(
    const std::filesystem::path& theInput,
    const std::filesystem::path& theOutput)
{
    const std::filesystem::path anInput = std::filesystem::absolute(theInput).lexically_normal();
    const std::filesystem::path anOutput = std::filesystem::absolute(theOutput).lexically_normal();
    if (anInput == anOutput)
    {
        Fail("输入和输出不能是同一个文件");
    }

    const std::vector<Byte> aSource = ReadFileFully(anInput);
    UserFormat aSourceFormat;
    if (aSource.size() >= LevelSaveFormat::USER64_MAGIC.size() &&
        std::equal(
            LevelSaveFormat::USER64_MAGIC.begin(),
            LevelSaveFormat::USER64_MAGIC.end(),
            aSource.begin()))
    {
        aSourceFormat = UserFormat::UserDat64;
    }
    else if (aSource.size() >= 4 && ReadU32At(aSource, 0) == USER_V12_VERSION)
    {
        aSourceFormat = UserFormat::LegacyUserDat;
    }
    else
    {
        Fail(0, "user.magic", "既不是 v12 user*.dat，也不是 PVZUSR64 user*.dat64");
    }

    const UserDocument aDocument = aSourceFormat == UserFormat::LegacyUserDat
        ? ParseUserV12(aSource)
        : ParseUserV13(aSource);
    const UserFormat aTargetFormat = aSourceFormat == UserFormat::LegacyUserDat
        ? UserFormat::UserDat64
        : UserFormat::LegacyUserDat;
    const std::vector<Byte> aTarget = aTargetFormat == UserFormat::UserDat64
        ? WriteUserV13Image(aDocument)
        : WriteUserV12Image(aDocument);

    // 生成文件按对向语法完整重解析并逐字段比对（与 game*.dat 转换同门）。
    const UserDocument aRoundTrip = aTargetFormat == UserFormat::UserDat64
        ? ParseUserV13(aTarget)
        : ParseUserV12(aTarget);
    if (aRoundTrip != aDocument)
    {
        Fail("内部错误：生成的 user 档虽能解析，但逐字段语义与输入不一致");
    }

    AtomicWrite(anOutput, aTarget);
    return {
        aSourceFormat,
        aTargetFormat,
        anInput,
        anOutput,
        aSource.size(),
        aTarget.size()
    };
}
}
