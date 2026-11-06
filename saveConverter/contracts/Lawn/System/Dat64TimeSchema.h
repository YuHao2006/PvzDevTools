#pragma once
#include "LevelSaveFormat.h"
#include <algorithm>
#include <bit>
#include <limits>
#include <span>
#include <stdexcept>
#include <string>
#include <vector>

// v2 retains the v1 byte grammar, replacing each described 32-bit time with
// a little-endian 64-bit nanosecond value. Offsets always refer to v1 records.
namespace Dat64TimeSchema
{
inline constexpr std::uint32_t VERSION = LevelSaveFormat::DAT64_VERSION;
inline constexpr std::uint32_t SCHEMA_ID = LevelSaveFormat::DAT64_SCHEMA_ID;
inline constexpr std::uint32_t FLAGS = LevelSaveFormat::DAT64_FLAGS;
struct Field
{
    std::uint32_t mOffset;
    bool mUnsigned;
    const char* mName;
    std::int64_t mTickNs = 10'000'000;
};
using Fields = std::span<const Field>;

// Populated from the audited persistent time fields; deliberately excludes
// animation frames, array indexes, counts and simulation step identifiers.
constexpr Field BoardField(std::uint32_t legacyObjectOffset, bool isUnsigned,
    const char* name, std::int64_t tickNs = 10'000'000)
{
    return {legacyObjectOffset - 0x164, isUnsigned, name, tickNs};
}
inline constexpr Field BOARD_FIELDS[] = {
    BoardField(0x5d4, false, "mFogBlownCountDown"),
    BoardField(0x624, false, "mIceTimer[0]"), BoardField(0x628, false, "mIceTimer[1]"),
    BoardField(0x62c, false, "mIceTimer[2]"), BoardField(0x630, false, "mIceTimer[3]"),
    BoardField(0x634, false, "mIceTimer[4]"), BoardField(0x638, false, "mIceTimer[5]"),
    BoardField(0x5538, false, "mSunCountDown"), BoardField(0x5540, false, "mShakeCounter"),
    BoardField(0x5568, true, "mMainCounter"), BoardField(0x556c, true, "mEffectCounter"),
    BoardField(0x5574, false, "mRiseFromGraveCounter"), BoardField(0x5578, false, "mOutOfMoneyCounter"),
    BoardField(0x558c, false, "mTutorialTimer"), BoardField(0x559c, false, "mZombieCountDown"),
    BoardField(0x55a0, false, "mZombieCountDownStart"), BoardField(0x55a4, false, "mHugeWaveCountDown"),
    BoardField(0x55f4, false, "mCoinBankFadeCount"), BoardField(0x5600, false, "mBoardFadeOutCounter"),
    BoardField(0x5604, false, "mNextSurvivalStageCounter"), BoardField(0x5608, false, "mScoreNextMowerCounter"),
    BoardField(0x5614, false, "mFlagRaiseCounter"), BoardField(0x5618, false, "mIceTrapCounter"),
    BoardField(0x5744, false, "mFwooshCountDown"), BoardField(0x5748, false, "mTimeStopCounter"),
    BoardField(0x5750, false, "mFinalWaveSoundCounter"), BoardField(0x5754, false, "mCobCannonCursorDelayCounter"),
    // GOTY adds 16 bytes before this tail; the old member comments there are stale.
    BoardField(0x5780, false, "mPlayTimeActiveLevel", 1'000'000'000),
    BoardField(0x5784, false, "mPlayTimeInactiveLevel", 1'000'000'000),
    BoardField(0x578c, true, "mStartDrawTime", 1'000'000),
    BoardField(0x5790, true, "mIntervalDrawTime", 1'000'000),
    BoardField(0x579c, false, "mPreloadTime", 1'000'000)
};
inline Fields BoardFields() { return BOARD_FIELDS; }

constexpr Field RecordField(LevelSaveFormat::RecordKind kind, std::uint32_t legacyOffset,
    const char* name)
{
    const auto& layout = LevelSaveFormat::GetRecordLayout(kind);
    for (std::size_t i = 0; i < layout.mSegmentCount; ++i)
    {
        const auto& segment = layout.mSegments[i];
        if (legacyOffset >= segment.mLegacyOffset &&
            legacyOffset + 4 <= segment.mLegacyOffset + segment.mSize)
            return {segment.mDat64Offset + legacyOffset - segment.mLegacyOffset, false, name};
    }
    throw "time field is outside persistent record segments";
}
#define DAT64_TIME(kind, offset, name) RecordField(LevelSaveFormat::RecordKind::kind, offset, #name)
inline constexpr Field ZOMBIE_FIELDS[] = {
    DAT64_TIME(Zombie, 0x38, mAnimCounter), DAT64_TIME(Zombie, 0x3c, mGroanCounter),
    DAT64_TIME(Zombie, 0x40, mAnimTicksPerFrame), DAT64_TIME(Zombie, 0x54, mJustGotShotCounter),
    DAT64_TIME(Zombie, 0x58, mShieldJustGotShotCounter), DAT64_TIME(Zombie, 0x5c, mShieldRecoilCounter),
    DAT64_TIME(Zombie, 0x60, mZombieAge), DAT64_TIME(Zombie, 0x68, mPhaseCounter),
    DAT64_TIME(Zombie, 0x74, mZombieFade), DAT64_TIME(Zombie, 0xac, mChilledCounter),
    DAT64_TIME(Zombie, 0xb0, mButteredCounter), DAT64_TIME(Zombie, 0xb4, mIceTrapCounter),
    DAT64_TIME(Zombie, 0xc0, mYuckyFaceCounter), DAT64_TIME(Zombie, 0x114, mSummonCounter),
    DAT64_TIME(Zombie, 0x134, mBossBungeeCounter), DAT64_TIME(Zombie, 0x138, mBossStompCounter),
    DAT64_TIME(Zombie, 0x13c, mBossHeadCounter)
};
inline constexpr Field PLANT_FIELDS[] = {
    DAT64_TIME(Plant, 0x2c, mAnimCounter), DAT64_TIME(Plant, 0x34, mFrameLength),
    DAT64_TIME(Plant, 0x4c, mDisappearCountdown), DAT64_TIME(Plant, 0x50, mDoSpecialCountdown),
    DAT64_TIME(Plant, 0x54, mStateCountdown), DAT64_TIME(Plant, 0x58, mLaunchCounter),
    DAT64_TIME(Plant, 0x5c, mLaunchRate), DAT64_TIME(Plant, 0x90, mShootingCounter),
    DAT64_TIME(Plant, 0xb0, mBlinkCountdown), DAT64_TIME(Plant, 0xb4, mRecentlyEatenCountdown),
    DAT64_TIME(Plant, 0xb8, mEatenFlashCountdown), DAT64_TIME(Plant, 0xbc, mBeghouledFlashCountdown),
    DAT64_TIME(Plant, 0x130, mWakeUpCounter)
};
inline constexpr Field PROJECTILE_FIELDS[] = {
    DAT64_TIME(Projectile, 0x2c, mAnimCounter), DAT64_TIME(Projectile, 0x54, mAnimTicksPerFrame),
    DAT64_TIME(Projectile, 0x60, mProjectileAge), DAT64_TIME(Projectile, 0x64, mClickBackoffCounter)
};
inline constexpr Field COIN_FIELDS[] = {
    DAT64_TIME(Coin, 0x3c, mFadeCount), DAT64_TIME(Coin, 0x4c, mCoinAge),
    DAT64_TIME(Coin, 0x54, mDisappearCounter)
};
inline constexpr Field MOWER_FIELDS[] = {
    DAT64_TIME(LawnMower, 0x18, mAnimTicksPerFrame), DAT64_TIME(LawnMower, 0x20, mChompCounter),
    DAT64_TIME(LawnMower, 0x24, mRollingInCounter), DAT64_TIME(LawnMower, 0x28, mSquishedCounter)
};
inline constexpr Field GRID_FIELDS[] = {
    DAT64_TIME(GridItem, 0x18, mGridItemCounter), DAT64_TIME(GridItem, 0x4c, mTransparentCounter)
};
inline constexpr Field PARTICLE_FIELDS[] = {
    DAT64_TIME(Particle, 0x4, mParticleDuration), DAT64_TIME(Particle, 0x8, mParticleAge),
    DAT64_TIME(Particle, 0x38, mCrossFadeDuration)
};
inline constexpr Field EMITTER_FIELDS[] = {
    DAT64_TIME(ParticleEmitter, 0x28, mSystemAge), DAT64_TIME(ParticleEmitter, 0x2c, mSystemDuration),
    DAT64_TIME(ParticleEmitter, 0x5c, mEmitterCrossFadeCountDown)
};
inline constexpr Field TRACK_FIELDS[] = {
    DAT64_TIME(ReanimatorTrackInstance, 0x0, mBlendCounter),
    DAT64_TIME(ReanimatorTrackInstance, 0x4, mBlendTime)
};
inline constexpr Field CURSOR_FIELDS[] = {DAT64_TIME(CursorObject, 0x44, mHammerDownCounter)};
inline constexpr Field MESSAGE_FIELDS[] = {
    DAT64_TIME(MessageWidget, 0x84, mDisplayTime), DAT64_TIME(MessageWidget, 0x88, mDuration),
    DAT64_TIME(MessageWidget, 0x294, mSlideOffTime)
};
inline constexpr Field SEED_FIELDS[] = {
    DAT64_TIME(SeedPacket, 0x24, mRefreshCounter), DAT64_TIME(SeedPacket, 0x28, mRefreshTime),
    DAT64_TIME(SeedPacket, 0x3c, mSlotMachineCountDown)
};
inline constexpr Field CHALLENGE_FIELDS[] = {
    DAT64_TIME(Challenge, 0x58, mChallengeStateCounter), DAT64_TIME(Challenge, 0x5c, mConveyorBeltCounter),
    DAT64_TIME(Challenge, 0x90, mCloudsCounter[0]), DAT64_TIME(Challenge, 0x94, mCloudsCounter[1]),
    DAT64_TIME(Challenge, 0x98, mCloudsCounter[2]), DAT64_TIME(Challenge, 0x9c, mCloudsCounter[3]),
    DAT64_TIME(Challenge, 0xa0, mCloudsCounter[4]), DAT64_TIME(Challenge, 0xa4, mCloudsCounter[5]),
    DAT64_TIME(Challenge, 0xb4, mRainCounter)
};
inline constexpr Field MUSIC_FIELDS[] = {
    DAT64_TIME(Music, 0x28, mBurstStateCounter), DAT64_TIME(Music, 0x34, mDrumsStateCounter),
    DAT64_TIME(Music, 0x44, mFadeOutCounter), DAT64_TIME(Music, 0x48, mFadeOutDuration)
};
#undef DAT64_TIME
inline constexpr auto SEED_BANK_FIELDS = [] {
    std::array<Field, 31> fields{};
    for (std::size_t packet = 0; packet < 10; ++packet)
        for (std::size_t timer = 0; timer < 3; ++timer)
        {
            fields[packet * 3 + timer] = SEED_FIELDS[timer];
            fields[packet * 3 + timer].mOffset += 56 + static_cast<std::uint32_t>(packet) * 96;
        }
    fields[30] = {1020, false, "mConveyorBeltCounter"};
    return fields;
}();
inline Fields SeedBankFields() { return SEED_BANK_FIELDS; }
inline Fields GetFields(LevelSaveFormat::RecordKind kind)
{
    using enum LevelSaveFormat::RecordKind;
    switch (kind)
    {
    case Zombie: return ZOMBIE_FIELDS;
    case Plant: return PLANT_FIELDS;
    case Projectile: return PROJECTILE_FIELDS;
    case Coin: return COIN_FIELDS;
    case LawnMower: return MOWER_FIELDS;
    case GridItem: return GRID_FIELDS;
    case ParticleEmitter: return EMITTER_FIELDS;
    case Particle: return PARTICLE_FIELDS;
    case CursorObject: return CURSOR_FIELDS;
    case MessageWidget: return MESSAGE_FIELDS;
    case SeedPacket: return SEED_FIELDS;
    case Challenge: return CHALLENGE_FIELDS;
    case Music: return MUSIC_FIELDS;
    case ReanimatorTrackInstance: return TRACK_FIELDS;
    default: return {};
    }
}

inline std::size_t V2Size(std::size_t theV1Size, Fields theFields)
{
    return theV1Size + 4 * theFields.size();
}
inline std::size_t V2Offset(std::size_t theV1Offset, Fields theFields)
{
    return theV1Offset + 4 * static_cast<std::size_t>(std::count_if(
        theFields.begin(), theFields.end(),
        [=](const Field& f) { return f.mOffset < theV1Offset; }));
}
inline std::uint64_t Read(std::span<const std::uint8_t> b, std::size_t p, unsigned n)
{
    if (p > b.size() || n > b.size() - p) throw std::runtime_error("time field outside record");
    std::uint64_t v = 0;
    for (unsigned i = 0; i < n; ++i) v |= std::uint64_t(b[p + i]) << (i * 8);
    return v;
}
inline void Append(std::vector<std::uint8_t>& b, std::uint64_t v, unsigned n)
{
    for (unsigned i = 0; i < n; ++i) b.push_back(static_cast<std::uint8_t>(v >> (i * 8)));
}
inline std::uint64_t LegacyToNanoseconds(const Field& f, std::uint32_t v)
{
    if (f.mUnsigned) return std::uint64_t(v) * static_cast<std::uint64_t>(f.mTickNs);
    return std::bit_cast<std::uint64_t>(
        std::int64_t(std::bit_cast<std::int32_t>(v)) * f.mTickNs);
}
// Round to nearest, ties away from zero. Signed sentinels remain signed values:
// -1 legacy tick is -10ms, and consequently round-trips exactly.
inline std::uint32_t QuantizeNanoseconds(const Field& f, std::uint64_t v, bool round = false)
{
    const auto scale = static_cast<std::uint64_t>(f.mTickNs);
    const bool negative = !f.mUnsigned && std::bit_cast<std::int64_t>(v) < 0;
    const std::uint64_t magnitude = negative ? std::uint64_t(0) - v : v;
    std::uint64_t ticks = magnitude / scale;
    const auto remainder = magnitude % scale;
    if (!round && remainder != 0)
        throw std::runtime_error(std::string(f.mName) + ": nanoseconds are not an exact legacy time unit; use --round-time nearest");
    if (round && remainder >= (scale + 1) / 2) ++ticks;
    const std::uint64_t maximum = f.mUnsigned
        ? std::numeric_limits<std::uint32_t>::max()
        : negative ? std::uint64_t(2147483648ULL) : std::uint64_t(2147483647ULL);
    if (ticks > maximum)
        throw std::runtime_error(std::string(f.mName) + ": time exceeds the legacy 32-bit range");
    return negative ? std::uint32_t(0) - static_cast<std::uint32_t>(ticks)
                    : static_cast<std::uint32_t>(ticks);
}
template<class Callback>
std::vector<std::uint8_t> Expand(std::span<const std::uint8_t> b, Fields fields, Callback callback)
{
    std::vector<std::uint8_t> out;
    out.reserve(V2Size(b.size(), fields));
    std::size_t p = 0;
    for (const Field& f : fields)
    {
        if (f.mOffset < p || f.mOffset > b.size() || b.size() - f.mOffset < 4)
            throw std::runtime_error("invalid time schema offset");
        out.insert(out.end(), b.begin() + p, b.begin() + f.mOffset);
        Append(out, callback(f, static_cast<std::uint32_t>(Read(b, f.mOffset, 4))), 8);
        p = f.mOffset + 4;
    }
    out.insert(out.end(), b.begin() + p, b.end());
    return out;
}
template<class Callback>
std::vector<std::uint8_t> Collapse(std::span<const std::uint8_t> b, Fields fields, Callback callback)
{
    if (b.size() < 4 * fields.size()) throw std::runtime_error("short v2 time record");
    std::vector<std::uint8_t> out;
    out.reserve(b.size() - 4 * fields.size());
    std::size_t p = 0;
    for (std::size_t i = 0; i < fields.size(); ++i)
    {
        const Field& f = fields[i];
        const std::size_t at = f.mOffset + i * 4;
        if (at < p || at > b.size() || b.size() - at < 8)
            throw std::runtime_error("invalid v2 time schema offset");
        out.insert(out.end(), b.begin() + p, b.begin() + at);
        Append(out, callback(f, Read(b, at, 8)), 4);
        p = at + 8;
    }
    out.insert(out.end(), b.begin() + p, b.end());
    return out;
}
}
