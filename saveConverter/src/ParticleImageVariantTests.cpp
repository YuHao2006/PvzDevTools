// Include the implementation so the standalone regression exercises the same
// canonical model and wire parsers without exposing test-only production APIs.
#include "LevelSaveConverter.cpp"
#include <iostream>

namespace
{
using namespace PvzDat64;

void Require(bool condition, const char* reason)
{
    if (!condition) throw std::runtime_error(reason);
}

template<class Fn>
void Reject(Fn operation, const char* reason)
{
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(reason);
}

void ReplacePool(Document& document, std::size_t index, std::size_t count)
{
    auto& pool = document.mArrays[index];
    pool.mFreeListHead = pool.mMaxUsedCount = pool.mSize = static_cast<std::uint32_t>(count);
    pool.mSlots.clear();
    for (std::size_t i = 0; i < count; ++i)
        pool.mSlots.push_back({
            std::vector<Byte>(LevelSaveFormat::GetRecordLayout(pool.mKind).mDat64Size, 0),
            {}, {}, 0, false, 0x10000U + static_cast<std::uint32_t>(i)});
}

Document WithParticles(Document document, const ParticleImageVariant& entry)
{
    ReplacePool(document, 9, 0);
    document.mReanimations.clear();
    ReplacePool(document, 6, 1);
    ReplacePool(document, 7, 2);
    ReplacePool(document, 8, 2);
    document.mParticleSystems = {{
        entry.mBase, {0x10000U, 0x10001U},
        {{0, entry.mImage, {0x10000U}}, {1, entry.mImage, {0x10001U}}}}};
    WriteU32At(document.mArrays[6].mSlots[0].mCanonicalRecord, 0, entry.mBase);
    // Distinct frames, emitter state, and particle ages must survive image migration.
    for (std::size_t i = 0; i < 2; ++i)
    {
        auto& emitter = document.mArrays[7].mSlots[i].mCanonicalRecord;
        WriteU32At(emitter, 128, i == 0 ? 0xffffffffU : 7);
        WriteU32At(emitter, 60, 39U + static_cast<std::uint32_t>(i));
        WriteU32At(document.mArrays[8].mSlots[i].mCanonicalRecord, 12, 23U + static_cast<std::uint32_t>(i));
    }
    CaptureLegacyTimes(document);
    return document;
}

void CheckWireRoundTrips(const Document& fixture)
{
    for (const auto& entry : PARTICLE_IMAGE_VARIANTS)
    {
        Document expected = WithParticles(fixture, entry);
        NormalizeParticleImages(expected);
        Require(expected.mParticleSystems[0].mDefinitionID == entry.mVariant,
            "Legacy image did not resolve to the variant definition");
        Require(ReadU32At(expected.mArrays[6].mSlots[0].mCanonicalRecord, 0) ==
            static_cast<std::uint32_t>(entry.mVariant), "Raw system effect identity was not migrated");
        // Old game builds already wrote v2 with image overrides. The v2 input
        // version alone must not cause those records to bypass migration.
        const auto oldV2 = WriteFileImage(WithParticles(fixture, entry),
            Format::Dat64V2, LEGACY_DATE_OFFICIAL);
        std::uint32_t oldEcho = 0;
        Require(Parse(oldV2, Format::Dat64V2, oldEcho) == expected,
            "Old v2 image override did not migrate");
        const auto oldRepresentation = ParticleImagesForOutput(expected, Format::Dat64V2);
        Require(oldRepresentation.mParticleSystems[0].mDefinitionID == entry.mBase &&
            oldRepresentation.mParticleSystems[0].mEmitters[0].mImageResourceID == entry.mImage,
            "Explicit v2 export did not restore base definition and image override");
        const auto v2Wire = WriteFileImage(expected, Format::Dat64V2, LEGACY_DATE_OFFICIAL);
        auto modern = expected;
        PrepareV7Output(modern, Format::Dat64V4);
        const auto v3Wire = WriteFileImage(modern, Format::Dat64V4, LEGACY_DATE_OFFICIAL);
        std::size_t tails = 0;
        for (const auto& pool : modern.mArrays) for (const auto& slot : pool.mSlots) tails += slot.mV7State.size();
        Require(v2Wire.size() + tails == v3Wire.size() + 12,
            "V4 did not physically omit one system definition and two image override IDs");
        for (const auto format : {Format::Legacy1073Dat, Format::Dat64V1, Format::Dat64V2,
                                  Format::Dat64V4})
        {
            auto outgoing = expected;
            PrepareV7Output(outgoing, format);
            const auto image = WriteFileImage(outgoing, format, LEGACY_DATE_OFFICIAL);
            std::uint32_t dateEcho = 0;
            auto parsed = Parse(image, format, dateEcho);
            // Legacy stores the build-date echo in canonical SeedBank padding.
            auto comparable = outgoing;
            WriteU32At(comparable.mSeedBank, SEED_BANK_DATE_ECHO_OFFSET,
                ReadU32At(parsed.mSeedBank, SEED_BANK_DATE_ECHO_OFFSET));
            Require(parsed == comparable, "Variant/frames/state/time changed through wire roundtrip");
            PrepareV7Output(parsed, Format::Dat64V4);
            auto v2 = WriteFileImage(parsed, Format::Dat64V4, dateEcho);
            std::uint32_t echoAgain = 0;
            const auto repeated = Parse(v2, Format::Dat64V4, echoAgain);
            Require(repeated == parsed, "Legacy/v1 import did not retain canonical variant");
            Require(WriteFileImage(repeated, Format::Dat64V4, echoAgain) == v2,
                "v4-to-v4 conversion is not idempotent");
        }
    }
}

void CheckRejections(const Document& fixture)
{
    auto old = WithParticles(fixture, PARTICLE_IMAGE_VARIANTS[0]);
    old.mParticleSystems[0].mEmitters[1].mImageResourceID = NO_PARTICLE_IMAGE;
    Reject([&] { NormalizeParticleImages(old); }, "Mixed null/image emitters were accepted");
    const auto mixedOriginal = WriteFileImage(old, Format::Legacy1073Dat, LEGACY_DATE_OFFICIAL);
    std::uint32_t echo = 0;
    Reject([&] { Parse(mixedOriginal, Format::Legacy1073Dat, echo); },
        "Original-format mixed particle appearance was silently dropped");
    old = WithParticles(fixture, PARTICLE_IMAGE_VARIANTS[0]);
    old.mParticleSystems[0].mEmitters[1].mImageResourceID = PARTICLE_IMAGE_VARIANTS[1].mImage;
    Reject([&] { NormalizeParticleImages(old); }, "Distinct emitter images were accepted");
    old = WithParticles(fixture, PARTICLE_IMAGE_VARIANTS[0]);
    for (auto& emitter : old.mParticleSystems[0].mEmitters)
        emitter.mImageResourceID = static_cast<int>(FrozenSaveResources::ResourceId::IMAGE_BLANK_ID);
    Reject([&] { NormalizeParticleImages(old); }, "Unsupported image silently lost appearance");
    old = WithParticles(fixture, PARTICLE_IMAGE_VARIANTS[0]);
    old.mParticleSystems[0].mDefinitionID = PARTICLE_IMAGE_VARIANTS[0].mVariant;
    Reject([&] { NormalizeParticleImages(old); }, "Variant with redundant override was accepted");
    old = WithParticles(fixture, PARTICLE_IMAGE_VARIANTS[0]);
    for (auto& emitter : old.mParticleSystems[0].mEmitters)
        emitter.mImageResourceID = NO_PARTICLE_IMAGE;
    NormalizeParticleImages(old);
    Require(old.mParticleSystems[0].mDefinitionID == PARTICLE_IMAGE_VARIANTS[0].mBase,
        "Unmodified base definition was not retained");
    old = WithParticles(fixture, PARTICLE_IMAGE_VARIANTS[0]);
    NormalizeParticleImages(old);
    old.mParticleSystems[0].mEmitters.clear();
    old.mParticleSystems[0].mEmitterIDs.clear();
    ReplacePool(old, 7, 0);
    ReplacePool(old, 8, 0);
    Reject([&] { ParticleImagesForOutput(old, Format::Legacy1073Dat); },
        "Emitterless variant lost its image during legacy export");
    Reject([&] { ParticleImagesForOutput(old, Format::Dat64V2); },
        "Emitterless variant lost its image during v2 export");
    const auto unchanged = ParticleImagesForOutput(old, Format::Dat64V4);
    Require(unchanged == old, "Emitterless modern variant was not retained");
}

void CheckMusicTempo(const Document& fixture)
{
    auto oldV2 = fixture;
    ReplacePool(oldV2, 9, 0);
    oldV2.mReanimations.clear();
    CaptureLegacyTimes(oldV2);
    WriteU32At(oldV2.mMusic, 36, std::bit_cast<std::uint32_t>(155.0f));
    WriteU32At(oldV2.mMusic, 40, std::bit_cast<std::uint32_t>(3.0f));
    const auto oldWire = WriteFileImage(oldV2, Format::Dat64V2, LEGACY_DATE_OFFICIAL);
    std::uint32_t echo = 0;
    const auto upgraded = Parse(oldWire, Format::Dat64V2, echo);
    Require(ReadFloatAt(upgraded.mMusic, 36) == 155.0f &&
        ReadFloatAt(upgraded.mMusic, 40) == 3.0f,
        "Old integer v2 tempos were reinterpreted instead of numerically converted");
    Require(ReadU32At(oldWire, 8) == 2 && ReadU32At(oldWire, 36) == 0x10732,
        "Tempo migration changed the v2 header");

    auto precise = upgraded;
    WriteU32At(precise.mMusic, 36, std::bit_cast<std::uint32_t>(155.25f));
    WriteU32At(precise.mMusic, 40, std::bit_cast<std::uint32_t>(3.5f));
    PrepareV7Output(precise, Format::Dat64V4);
    const auto newWire = WriteFileImage(precise, Format::Dat64V4, LEGACY_DATE_OFFICIAL);
    Require(ReadU32At(newWire, 8) == 4 && ReadU32At(newWire, 36) == 0x10745 &&
        ReadU32At(newWire, 40) == 0, "New saves did not use the v4 version/schema");
    Require(Parse(newWire, Format::Dat64V4, echo) == precise,
        "Fractional float tempos changed on a v4 roundtrip");
    Require(WriteFileImage(Parse(newWire, Format::Dat64V4, echo),
        Format::Dat64V4, LEGACY_DATE_OFFICIAL) == newWire,
        "Float-tempo v4 normalization is not idempotent");
    auto transitionalV2 = oldWire;
    const auto musicOffset = transitionalV2.size() - 12 - 104;
    WriteU32At(transitionalV2, musicOffset + 36, std::bit_cast<std::uint32_t>(155.25f));
    WriteU32At(transitionalV2, musicOffset + 40, std::bit_cast<std::uint32_t>(3.5f));
    auto preciseWithoutTail = precise;
    PrepareV7Output(preciseWithoutTail, Format::Dat64V2);
    Require(Parse(transitionalV2, Format::Dat64V2, echo) == preciseWithoutTail,
        "Transitional float-tempo v2 input was not preserved offline");
    for (const auto format : {Format::Legacy1073Dat, Format::Dat64V1, Format::Dat64V2})
    {
        const auto integerRecord = FromCanonicalRecord(precise.mMusic, format, RecordKind::Music);
        const auto restored = ToCanonicalRecord(integerRecord, format, RecordKind::Music, 0, "Music");
        Require(ReadFloatAt(restored, 36) == 155.0f && ReadFloatAt(restored, 40) == 4.0f,
            "Float tempos were not rounded numerically for an integer export");
    }
    auto quantized = precise;
    QuantizeMusicTempo(quantized);
    Require(ReadFloatAt(quantized.mMusic, 36) == 155.0f &&
        ReadFloatAt(quantized.mMusic, 40) == 4.0f,
        "Legacy export comparison retained unrepresentable fractional tempos");
    for (const auto value : {0.0f, -1.0f, 1001.0f,
                            std::numeric_limits<float>::infinity(),
                            std::numeric_limits<float>::quiet_NaN()})
    {
        auto invalid = precise.mMusic;
        WriteU32At(invalid, 36, std::bit_cast<std::uint32_t>(value));
        Reject([&] { ToCanonicalRecord(invalid, Format::Dat64V4, RecordKind::Music, 0, "Music"); },
            "Invalid float tempo accepted");
    }
}

void CheckInternalParticleAppearances()
{
    std::set<std::uint32_t> oldIds;
    std::set<std::pair<std::uint32_t, std::uint32_t>> pairs;
    for (const auto& entry : ParticleAppearanceSaveCatalog::Mappings)
    {
        Require(entry.oldV3Effect < FROZEN_PARTICLE_COUNT && entry.baseEffect < 106 &&
            oldIds.insert(entry.oldV3Effect).second &&
            pairs.emplace(entry.baseEffect, entry.variant).second,
            "Particle appearance catalog is not a bijection over historical identities");
        Require(static_cast<std::uint32_t>(ParticleAppearanceToCanonical(entry.baseEffect, entry.variant)) ==
            entry.oldV3Effect,
            "Particle base/variant reverse mapping changed");
        std::vector<Byte> record(72, 0);
        WriteU32At(record, 0, entry.oldV3Effect);
        const auto wire = FromCanonicalRecord(record, Format::Dat64V4, RecordKind::ParticleSystem);
        Require(ReadU32At(wire, 0) == entry.baseEffect && ReadU32At(wire, 4) == entry.variant &&
            wire.size() == 72, "Current particle record does not store base plus internal variant");
        Require(ToCanonicalRecord(wire, Format::Dat64V4, RecordKind::ParticleSystem, 0, "Particle") == record,
            "Particle appearance mapping changed record state");
    }
    Require(oldIds.size() == 162, "Particle catalog omitted an old independent appearance");
    for (std::uint32_t base = 0; base < 106; ++base)
    {
        const auto& entry = ParticleAppearanceFromCanonical(LEGACY_PARTICLE_IDS[base]);
        Require(entry.baseEffect == base && entry.variant == 0,
            "Original 106-effect ordering/default appearance changed");
    }
    Reject([&] { ParticleAppearanceToCanonical(106, 0); }, "Unknown base effect was accepted");
    Reject([&] { ParticleAppearanceToCanonical(0, 0xffffffffU); }, "Unknown variant was accepted");
}

void CheckFrozenParticleIds()
{
    Require(LEGACY_PARTICLE_IDS[0] == FrozenParticleId("PARTICLE_MELONSPLASH") &&
        LEGACY_PARTICLE_IDS[6] == FrozenParticleId("PARTICLE_ZOMBIE_HEAD") &&
        LEGACY_PARTICLE_IDS[105] == FrozenParticleId("PARTICLE_PERSENT_PICK_UP_ARROW") &&
        LEGACY_PARTICLE_IDS[106] == FrozenParticleId("PARTICLE_ZOMBIE_HEAD_POLEVAULTER") &&
        LEGACY_PARTICLE_IDS[161] == FrozenParticleId("PARTICLE_ZOMBIE_BOSS_ICEBALL_BURST"),
        "Frozen pre-v3 particle identity anchors changed");
    for (std::size_t old = 0; old < std::size(LEGACY_PARTICLE_IDS); ++old)
    {
        const auto current = ParticleIdToCanonical(static_cast<int>(old), Format::Dat64V2);
        Require(current == LEGACY_PARTICLE_IDS[old] &&
            ParticleIdFromCanonical(current, Format::Dat64V2) == old,
            "v2 particle ID remapping is not reversible");
        if (old < 106)
            for (const auto format : {Format::Legacy1073Dat, Format::Dat64V1})
                Require(ParticleIdToCanonical(static_cast<int>(old), format) == current &&
                    ParticleIdFromCanonical(current, format) == old,
                    "Retail/v1 particle ID remapping is not reversible");
    }
    Require(ParticleIdToCanonical(-1, Format::Legacy1073Dat) == -1 &&
        ParticleIdFromCanonical(-1, Format::Dat64V4) == -1,
        "PARTICLE_NONE changed its sentinel");
    Reject([&] { ParticleIdToCanonical(106, Format::Legacy1073Dat); },
        "Retail format accepted an appended v2 definition ID");
}
}

int main(int argc, char** argv)
{
    try
    {
        if (argc != 2) throw std::runtime_error("usage: particle-image-variant-tests fixture.dat64");
        std::uint32_t echo = 0;
        const auto bytes = ReadFileFully(std::filesystem::path(argv[1]));
        const auto fixture = Parse(bytes, Format::Dat64V1, echo);
        CheckWireRoundTrips(fixture);
        CheckRejections(fixture);
        CheckMusicTempo(fixture);
        CheckFrozenParticleIds();
        CheckInternalParticleAppearances();
        std::cout << "PASS: " << std::size(PARTICLE_IMAGE_VARIANTS)
            << " particle variants across legacy/v1/v2/v4; 162 canonical identities to 106 roots/internal variants, no image IDs, frames, state, time, tempo floats, idempotence\n";
        return 0;
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL: " << error.what() << '\n';
        return 1;
    }
}
