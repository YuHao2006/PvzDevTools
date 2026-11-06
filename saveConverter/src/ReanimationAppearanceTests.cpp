// The offline regression exercises production wire grammars and catalog mapping.
#include "LevelSaveConverter.cpp"
#include <iostream>

namespace
{
using namespace PvzDat64;
void Require(bool value, const char* message)
{
    if (!value) throw std::runtime_error(message);
}
template<class Fn> void Reject(Fn operation, const char* message)
{
    try { operation(); }
    catch (const std::runtime_error&) { return; }
    throw std::runtime_error(message);
}
void CheckCatalog()
{
    std::set<std::int32_t> types;
    for (const auto& definition : ReanimAppearanceSaveCatalog::Definitions)
    {
        Require(types.insert(definition.type).second && definition.variantCount > 0,
            "Appearance catalog has duplicate types or no default appearance");
        std::uint64_t product = 1;
        std::set<std::uint32_t> tracks;
        for (std::uint32_t i = 0; i < definition.axisCount; ++i)
        {
            const auto& axis = ReanimAppearanceSaveCatalog::Axes[definition.axisOffset + i];
            Require(axis.trackIndex < definition.trackCount && axis.radix > 1 &&
                tracks.insert(axis.trackIndex).second && axis.stride == product,
                "Appearance axes do not have unique tracks and mixed-radix strides");
            std::set<std::uint32_t> digits;
            std::set<std::int32_t> resources;
            for (std::uint32_t j = 0; j < axis.radix; ++j)
            {
                const auto& choice = ReanimAppearanceSaveCatalog::Choices[axis.choiceOffset + j];
                Require(choice.digit < axis.radix && digits.insert(choice.digit).second &&
                    resources.insert(choice.canonicalModernResourceId).second,
                    "Appearance choices are ambiguous");
                if (choice.digit == 0)
                    Require(choice.canonicalModernResourceId == LevelSaveRemap::kResourceIdSentinel,
                        "Default appearance is not the null override sentinel");
            }
            product *= axis.radix;
        }
        Require(product == definition.variantCount, "Mixed-radix product differs from variant count");
        for (std::uint32_t variant = 0; variant < definition.variantCount; ++variant)
        {
            const auto images = AppearanceImages(definition.type, variant, definition.trackCount);
            Require(AppearanceVariant(definition.type, images) == variant,
                "Appearance choices do not map reversibly");
            auto aliased = images;
            for (auto& resource : aliased)
                if (resource == 0) resource = 2;
            Require(AppearanceVariant(definition.type, aliased) == variant,
                "Proven IMAGE_PARTNER_LOGO/IMAGE_BLANK pointer alias changed appearance");
        }
        Reject([&] { AppearanceImages(definition.type, definition.variantCount, definition.trackCount); },
            "Out-of-range variant was accepted");
    }
    Require(types.size() == NUM_REANIMS, "Appearance catalog omits a root definition");
}
Document SingleReanimation(Document document, std::int32_t type, std::uint32_t variant)
{
    const auto& definition = AppearanceDefinition(type);
    auto& pool = document.mArrays[9];
    pool.mFreeListHead = pool.mMaxUsedCount = pool.mSize = 1;
    pool.mSlots = {{std::vector<Byte>(176, 0), {}, {}, 0, false, 0x10000}};
    WriteU32At(pool.mSlots[0].mCanonicalRecord, 0, static_cast<std::uint32_t>(type));
    WriteU32At(pool.mSlots[0].mCanonicalRecord, 36, 1);
    pool.mSlots[0].mV7Migration = V7SaveSchema::FromHistorical(RecordKind::Reanimation,
        pool.mSlots[0].mCanonicalRecord, {});
    pool.mSlots[0].mNeedsClockBinding = true;
    ReanimationSupplement supplement;
    supplement.mDefinitionID = type;
    supplement.mCanonicalTracks.resize(definition.trackCount, std::vector<Byte>(112, 0));
    supplement.mImageResourceIDs = AppearanceImages(type, variant, definition.trackCount);
    for (std::size_t i = 0; i < supplement.mCanonicalTracks.size(); ++i)
    {
        auto& track = supplement.mCanonicalTracks[i];
        WriteU32At(track, 0, static_cast<std::uint32_t>(i + 1));
        WriteU32At(track, 4, static_cast<std::uint32_t>(i + 2));
        WriteU32At(track, 64, std::bit_cast<std::uint32_t>(1.25f));
        WriteU32At(track, 88, 0x12345678); // Trailing fields must shift intact.
        WriteU32At(track, 92, 0xabcdef12);
        track[108] = track[111] = 1;
    }
    document.mReanimations = {std::move(supplement)};
    CaptureLegacyTimes(document);
    PrepareAppearanceOutput(document, Format::Dat64V4);
    PrepareV7Output(document, Format::Dat64V4);
    return document;
}
void CheckWire(const Document& fixture)
{
    const auto found = std::find_if(std::begin(ReanimAppearanceSaveCatalog::Definitions),
        std::end(ReanimAppearanceSaveCatalog::Definitions), [](const auto& entry) {
            return entry.variantCount > 1 && entry.trackCount > 0;
        });
    Require(found != std::end(ReanimAppearanceSaveCatalog::Definitions),
        "No nondefault appearance exists for the wire regression");
    const auto expected = SingleReanimation(fixture, found->type, found->variantCount - 1);
    const auto current = WriteFileImage(expected, Format::Dat64V4, LEGACY_DATE_OFFICIAL);
    Require(ReadU32At(current, 8) == 4 && ReadU32At(current, 36) == 0x10745 &&
        ReadU32At(current, 40) == 0 && ReadU32At(current, 44) == 0,
        "v4 header does not identify the new grammar");
    Require(LevelSaveFormat::IsSupportedDat64Header(current) &&
        LevelSaveFormat::IsCurrentDat64Header(current), "Current v4 was rejected");
    auto old = expected;
    PrepareAppearanceOutput(old, Format::Dat64V2);
    PrepareV7Output(old, Format::Dat64V2);
    const auto previous = WriteFileImage(old, Format::Dat64V2, LEGACY_DATE_OFFICIAL);
    std::size_t oldParticleSupplementBytes = expected.mParticleSystems.size() * 4;
    for (const auto& system : expected.mParticleSystems)
        oldParticleSupplementBytes += system.mEmitters.size() * 4;
    std::size_t tails = 0;
    for (const auto& pool : expected.mArrays) for (const auto& slot : pool.mSlots) tails += slot.mV7State.size();
    Require(previous.size() + tails == current.size() + found->trackCount * 12 + oldParticleSupplementBytes,
        "Current format did not physically remove the 8-byte gap and 4-byte image ID per track");
    Require(!LevelSaveFormat::IsCurrentDat64Header(previous) &&
        LevelSaveFormat::IsSupportedDat64Header(previous), "Old v2 must require offline upgrade");
    for (const auto [minor, schema] :
         {std::pair{0U, 0x10733U}, std::pair{1U, 0x10734U},
          std::pair{0U, 0x10735U}, std::pair{1U, 0x10735U},
          std::pair{2U, 0x10734U}, std::pair{3U, 0x10735U}})
    {
        auto unsupported = current;
        WriteU32At(unsupported, 36, schema);
        WriteU32At(unsupported, 40, minor);
        Require(!LevelSaveFormat::IsSupportedDat64Header(unsupported) &&
            !LevelSaveFormat::IsCurrentDat64Header(unsupported),
            "Unsupported intermediate or mismatched header was accepted");
        Reject([&] { Dat64Format(unsupported); }, "Unsupported header was auto-detected as current");
        std::uint32_t echo = 0;
        Reject([&] { Parse(unsupported, Format::Dat64V4, echo); },
            "Unsupported input grammar was accepted");
    }
    for (const auto format :
         {Format::Legacy1073Dat, Format::Dat64V1, Format::Dat64V2, Format::Dat64V4})
    {
        auto input = expected;
        PrepareAppearanceOutput(input, format);
        PrepareV7Output(input, format);
        const auto wire = WriteFileImage(input, format, LEGACY_DATE_OFFICIAL);
        std::uint32_t echo = 0;
        auto parsed = Parse(wire, format, echo);
        PrepareAppearanceOutput(parsed, Format::Dat64V4);
        PrepareV7Output(parsed, Format::Dat64V4);
        Require(parsed == expected, "Appearance or instance state changed during an old-format import");
        const auto upgraded = WriteFileImage(parsed, Format::Dat64V4, echo);
        auto reparsed = Parse(upgraded, Format::Dat64V4, echo);
        Require(reparsed == expected &&
            WriteFileImage(reparsed, Format::Dat64V4, echo) == upgraded,
            "v4 field or byte roundtrip is not stable");
    }
    auto edited = current;
    WriteU32At(edited, 32, 0xdeadbeef);
    std::uint32_t echo = 0;
    Require(Parse(edited, Format::Dat64V4, echo) == expected,
        "The ignored old CRC field became a content checksum");
    auto invalid = expected;
    invalid.mReanimations[0].mImageResourceIDs[0] =
        static_cast<int>(FrozenSaveResources::ResourceId::IMAGE_POPCAP_LOGO_ID);
    Reject([&] { PrepareAppearanceOutput(invalid, Format::Dat64V4); },
        "Unknown override was silently dropped");
    // Exercise a genuine original-format record, not only a canonical helper:
    // the image ID is a valid image resource but absent from this root's choices.
    auto originalUnknown = invalid;
    PrepareAppearanceOutput(originalUnknown, Format::Legacy1073Dat);
    PrepareV7Output(originalUnknown, Format::Legacy1073Dat);
    const auto originalUnknownWire = WriteFileImage(
        originalUnknown, Format::Legacy1073Dat, LEGACY_DATE_OFFICIAL);
    auto parsedOriginalUnknown = Parse(originalUnknownWire, Format::Legacy1073Dat, echo);
    Reject([&] { PrepareAppearanceOutput(parsedOriginalUnknown, Format::Dat64V4); },
        "An unsupported original-format override was silently converted");
    invalid = expected;
    ++invalid.mReanimations[0].mDefinitionID;
    Reject([&] { PrepareAppearanceOutput(invalid, Format::Dat64V4); },
        "Conflicting fixed and supplemental identities were accepted");
    invalid = expected;
    WriteU32At(invalid.mArrays[9].mSlots[0].mCanonicalRecord, 12, found->variantCount);
    const auto badVariant = WriteFileImage(invalid, Format::Dat64V4, LEGACY_DATE_OFFICIAL);
    Reject([&] { Parse(badVariant, Format::Dat64V4, echo); },
        "v4 parser accepted a variant outside the root definition");
}
}
int main(int argc, char** argv)
{
    try
    {
        Require(argc == 2, "Pass an old DAT64 v1 regression fixture");
        std::uint32_t echo = 0;
        const auto fixture = Parse(ReadFileFully(argv[1]), Format::Dat64V1, echo);
        CheckCatalog();
        CheckWire(fixture);
        std::cout << "PASS reanimation appearance catalog, legacy/v1/v2/v4, unsupported v3/v3.1 rejection, "
            "physical 12-byte-per-track removal, identity, state, CRC independence and roundtrips\n";
    }
    catch (const std::exception& error)
    {
        std::cerr << "FAIL reanimation appearance: " << error.what() << '\n';
        return 1;
    }
}
