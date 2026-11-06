#pragma once
#include "LevelSaveFormat.h"
#include "Dat64TimeSchema.h"
#include <bit>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <span>
#include <stdexcept>
#include <utility>
#include <vector>

// v4 retains the frozen canonical records for offline conversion, followed by
// these exact v7 state fields before the DataArray ID. Never copy native padding.
namespace V7SaveSchema
{
using Kind = LevelSaveFormat::RecordKind;
using Byte = unsigned char;
inline constexpr std::int64_t Frame = 1'000'000'000'000LL;
inline constexpr std::int64_t SourceFrame = 1'000'000'000LL;
constexpr std::size_t Size(Kind kind)
{
    switch (kind)
    {
    case Kind::Zombie: return 105;
    case Kind::Plant: return 156;
    case Kind::Projectile: return 8;
    case Kind::Coin: return 0;
    case Kind::LawnMower: return 0;
    case Kind::Reanimation: return 72;
    case Kind::ParticleSystem: return 4;
    case Kind::ParticleEmitter: return 44;
    case Kind::Particle: return 20;
    case Kind::GridItem: return 240;
    default: return 0;
    }
}
inline std::uint64_t Read(std::span<const Byte> bytes, std::size_t offset, std::size_t width)
{
    if (offset > bytes.size() || width > bytes.size() - offset)
        throw std::runtime_error("v7 save field exceeds record");
    std::uint64_t value = 0;
    for (std::size_t i = 0; i < width; ++i) value |= std::uint64_t(bytes[offset + i]) << (i * 8);
    return value;
}
inline void Write(std::span<Byte> bytes, std::size_t offset, std::uint64_t value, std::size_t width)
{
    if (offset > bytes.size() || width > bytes.size() - offset)
        throw std::runtime_error("v7 save field exceeds record");
    for (std::size_t i = 0; i < width; ++i) bytes[offset + i] = Byte(value >> (i * 8));
}
inline float Float(std::span<const Byte> bytes, std::size_t offset)
{
    return std::bit_cast<float>(std::uint32_t(Read(bytes, offset, 4)));
}
inline void PutFloat(std::span<Byte> bytes, std::size_t offset, float value)
{
    Write(bytes, offset, std::bit_cast<std::uint32_t>(value), 4);
}
inline double Double(std::span<const Byte> bytes, std::size_t offset)
{
    return std::bit_cast<double>(Read(bytes, offset, 8));
}
inline void PutDouble(std::span<Byte> bytes, std::size_t offset, double value)
{
    Write(bytes, offset, std::bit_cast<std::uint64_t>(value), 8);
}
inline std::int64_t Rounded(double value)
{
    if (!std::isfinite(value) ||
        value < -9'223'372'036'854'775'808.0 || value >= 9'223'372'036'854'775'808.0)
        throw std::runtime_error("historical animation value exceeds v7 integer range");
    return std::llround(value);
}
inline void Validate(Kind kind, std::span<const Byte> state)
{
    if (state.size() != Size(kind)) throw std::runtime_error("v7 state length differs from schema");
    const auto Signed = [&](std::size_t at) { return std::bit_cast<std::int64_t>(Read(state, at, 8)); };
    const auto Require = [](bool valid, const char* message) {
        if (!valid) throw std::runtime_error(message);
    };
    if (kind == Kind::Reanimation)
    {
        Require(Read(state, 0, 4) <= 1, "animation time domain is invalid");
        const auto period = Signed(24), framePeriod = Signed(16);
        Require(period > 0 && framePeriod > 0 && period % framePeriod == 0,
            "animation periods are invalid");
        const auto frame = std::int32_t(Read(state, 12, 4));
        Require(frame >= 0 && frame <= period / framePeriod, "animation frame outside action");
        Require(std::isfinite(Double(state, 48)), "animation root displacement is not finite");
        Require(Read(state, 56, 1) <= 1, "animation history flag is not boolean");
        if (Read(state, 56, 1)) Require(Signed(40) >= 0 && Signed(40) <= period,
            "previous animation phase is outside period");
        Require(Signed(64) >= 0 && Signed(4) <= Signed(64) &&
            std::uint64_t(Signed(64)) - std::uint64_t(Signed(4)) <= std::uint64_t(INT64_MAX),
            "animation frame time is invalid");

    }
    else if (kind == Kind::ParticleEmitter)
    {
        Require(Signed(0) > -Frame && Signed(0) < Frame, "particle spawn remainder exceeds its unit");
    }
    else if (kind == Kind::Particle)
    {
        Require(Signed(0) >= 0, "particle animation time is negative");
        Require(Signed(8) >= 0 && Signed(8) < Frame, "particle animation anchor exceeds its period");
    }
    else if (kind == Kind::Plant || kind == Kind::Zombie)
    {
        Require(std::isfinite(Float(state, 0)) && std::isfinite(Float(state, 4)) &&
            std::isfinite(Double(state, 8)) && std::isfinite(Float(state, 20)),
            "actor coordinate or motion anchor is not finite");
        if (kind == Kind::Plant)
        {
            Require(Signed(24) >= 0, "bowling row arrival time is negative");
        }
        else
            for (const std::size_t at : {24U, 52U, 80U})
            {
                Require(std::isfinite(Double(state, at)) &&
                    std::isfinite(Float(state, at + 16)),
                    "zombie velocity segment is not finite");
                Require(Read(state, at + 20, 1) <= 1, "zombie velocity binding is not boolean");
            }
    }
    if (kind == Kind::GridItem)
    {
        Require(std::isfinite(Double(state, 192)) && std::isfinite(Double(state, 200)) &&
            std::isfinite(Float(state, 220)), "root-motion coordinate is not finite");
        Require(Read(state, 232, 1) <= 1, "root-motion chocolate flag is not boolean");
    }
}
inline std::uint64_t Time(std::span<const Byte> expanded, Kind kind, std::size_t canonicalOffset)
{
    const auto fields = Dat64TimeSchema::GetFields(kind);
    std::size_t offset = canonicalOffset;
    for (const auto& field : fields)
        if (field.mOffset < canonicalOffset) offset += 4;
    return Read(expanded, offset, 8);
}

// Migration is deliberately explicit. Historical float precision cannot be
// recovered; preserve its value once, then future saves retain exact state.
inline std::vector<Byte> FromHistorical(Kind kind, std::span<const Byte> canonical,
    std::span<const Byte> expanded)
{
    std::vector<Byte> state(Size(kind), 0);
    const auto Put = [&](std::size_t offset, std::int64_t value, std::size_t width = 8) {
        Write(state, offset, static_cast<std::uint64_t>(value), width);
    };
    if (kind == Kind::Zombie || kind == Kind::Plant)
    {
        const auto coordinate = [&](std::size_t axis) {
            const float value = kind == Kind::Zombie
                ? Float(canonical, 56 + axis * 4)
                : static_cast<float>(std::int32_t(Read(canonical, 16 + axis * 4, 4)));
            if (!std::isfinite(value))
                throw std::runtime_error("historical coordinate is not finite");
            return value;
        };
        PutFloat(state, 0, coordinate(0));
        PutFloat(state, 4, coordinate(1));
    }
    else if (kind == Kind::Projectile)
    {
        const auto age = std::bit_cast<std::int64_t>(Time(expanded, kind, 108));
        if (age < 0) throw std::runtime_error("historical projectile age is negative");
        Put(0, -age); // Relative draft; converter binds the creation time to Board now.
    }
    else if (kind == Kind::Reanimation)
    {
        const auto count = std::int32_t(Read(canonical, 36, 4));
        const double rate = Float(canonical, 8);
        const double phase = Float(canonical, 4);
        const double last = Float(canonical, 168);
        if (!std::isfinite(rate) || !std::isfinite(phase) || !std::isfinite(last))
            throw std::runtime_error("historical animation clock is not finite");
        const auto loop = Read(canonical, 24, 4);
        const bool full = loop == 1 || loop == 4 || loop == 5;
        const int intervals = std::max(1, full ? count : count - 1);
        const auto original = count > 0 && rate != 0
            ? Rounded(std::floor(double(count) * 1e9 / std::abs(rate)))
            : std::int64_t(std::max(count, 1)) * 1'000'000'000LL;
        const auto q = original / intervals;
        if (q <= 0) throw std::runtime_error("historical keyframe duration is below one nanosecond");
        const auto period = q * intervals;
        const auto at = std::clamp<std::int64_t>(Rounded(phase * period), 0, period);
        const auto frame = at / q, remainder = at % q;
        const bool reverse = rate < 0;
        const auto age = reverse && remainder != 0 ? q - remainder : remainder;
        Put(4, -age);
        Put(12, frame + (reverse && remainder != 0), 4);
        Put(16, q);
        Put(24, period);
        Put(40, last >= 0 ? std::clamp<std::int64_t>(Rounded(last * period), 0, period) : 0);
        const bool wrapped = last >= 0 && (reverse ? phase > last : phase < last);
        Put(32, wrapped ? (reverse ? -1 : 1) : 0);
        // Saved actor coordinates already contain historical completed motion.
        // Bind the new displacement origin at load, without replaying old loops.
        PutDouble(state, 48, 0);
        Put(56, last >= 0, 1);

    }
    else if (kind == Kind::ParticleEmitter)
    {
        Put(0, Rounded(double(Float(canonical, 48)) * Frame));
        for (std::size_t vector = 0; vector < 2; ++vector)
            std::memcpy(state.data() + 8 + vector * 8, canonical.data() + 52, 8);
        // No authored curve origin exists in historical records. The runtime
        // binds it from the saved center and age after resolving the definition.
    }
    else if (kind == Kind::Particle)
    {
        Put(0, 0);
        Put(8, Rounded(double(Float(canonical, 24)) * Frame));
        Put(16, 0x9E3779B9U, 4);
    }
    else if (kind == Kind::ParticleSystem)
    {
        Put(0, 0x6D2B79F5U, 4);
    }
    else if (kind == Kind::GridItem)
    {
        // Motion-trail phases are normalized legacy values. The exact frame
        // count is resolved by the converter's reanimation catalog later.
        for (std::size_t i = 0; i < 12; ++i)
        {
            std::memcpy(state.data() + i * 16, canonical.data() + 92 + i * 12, 8);
            Put(i * 16 + 8, Rounded(double(Float(canonical, 100 + i * 12)) * SourceFrame));
        }
    }
    return state;
}
}
