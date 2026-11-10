#include "NativeBuild.h"
#include <algorithm>
#include <bit>
#include <tuple>

namespace pvz::build
{
namespace
{
Bytes Body(const Bytes &bytes, unsigned cookie)
{
    Reader reader(bytes);
    Require(reader.Word() == cookie, "Invalid definition envelope cookie");
    auto size = reader.Word(), crc = reader.Word();
    Require(size == bytes.size() - 12, "Invalid definition envelope size");
    auto body = reader.Take(size);
    Require(Crc(body) == crc, "Invalid definition envelope CRC");
    return body;
}
void SkipTrack(Reader &r)
{
    auto count = r.Word();
    Require(count <= 100000, "Particle node count exceeds limit");
    r.Skip(std::size_t(count) * 24);
}
void SkipEmitter(Reader &r)
{
    r.Skip(16);
    for (int i = 0; i < 22; ++i)
        SkipTrack(r);
    for (int i = 0; i < 2; ++i)
    {
        auto count = r.Word();
        Require(count <= 4, "Too many particle fields");
        for (unsigned j = 0; j < count; ++j)
        {
            r.Skip(4);
            SkipTrack(r);
            SkipTrack(r);
        }
    }
    for (int i = 0; i < 16; ++i)
        SkipTrack(r);
}
Json ReadRects(Reader &r, unsigned count)
{
    Json rects = Json::array();
    for (unsigned i = 0; i < count; ++i)
    {
        int x = r.Int(), y = r.Int(), w = r.Int(), h = r.Int();
        rects.push_back({x, y, w, h});
    }
    return rects;
}
} // namespace

std::vector<std::size_t> ImageOffsets(std::string_view kind, std::span<const unsigned char> body)
{
    Reader reader(body);
    std::vector<std::size_t> offsets;
    if (kind == "reanim")
    {
        Require(reader.Word() == 0x46544449, "Expected authored IDTF");
        reader.Skip(16);
        auto count = reader.Word();
        std::vector<std::pair<unsigned, unsigned>> tracks;
        for (unsigned i = 0; i < count; ++i)
        {
            reader.Skip(4);
            auto frames = reader.Word(), stride = reader.Word(), flags = reader.Word();
            Require(stride == 32 && (flags & ~7u) == 0, "Invalid reanim track");
            reader.Skip(std::size_t(frames) * stride);
            tracks.emplace_back(frames, flags);
        }
        for (auto [frames, flags] : tracks)
            for (unsigned i = 0; i < frames; ++i)
            {
                offsets.push_back(reader.at);
                reader.Skip(8);
                if (flags & 1)
                    reader.Skip(reader.Word());
            }
    }
    else
    {
        Require(kind == "particles" && reader.Word() == 0x46524449, "Expected authored IDRF");
        reader.Skip(8);
        auto count = reader.Word(), size = reader.Word();
        auto end = reader.at + size;
        for (unsigned i = 0; i < count; ++i)
        {
            auto frames = reader.Word();
            Require(frames <= 100000, "Too many particle frames");
            for (unsigned f = 0; f < frames; ++f)
            {
                offsets.push_back(reader.at);
                Require(reader.Word() != 0, "Null particle image handle");
            }
            SkipEmitter(reader);
        }
        Require(reader.at == end, "Particle body size mismatch");
    }
    return offsets;
}

Definition CompileDefinition(std::string_view name, const Bytes &source, const Json &layout,
                             const std::vector<std::string> &symbols,
                             const std::map<unsigned, std::string> &names,
                             const std::map<std::string, ImageDescriptor> &descriptors,
                             std::string_view atlasPath, const std::map<std::string, Image> &images)
{
    bool reanim = name.starts_with("compiled/reanim/");
    auto body = Body(source, reanim ? 0xdeadfed7 : 0xdeadfed8);
    auto offsets = ImageOffsets(reanim ? "reanim" : "particles", body);
    std::map<std::string, Json> regions;
    Require(symbols.size() == layout["regions"].size(), "Atlas symbol/layout count differs");
    for (std::size_t i = 0; i < symbols.size(); ++i)
        regions[symbols[i]] = layout["regions"][i];
    std::set<std::string> usedNames, usedFiles;
    for (auto offset : offsets)
    {
        auto handle = GetWord(body, offset);
        if (handle)
            usedNames.insert(names.at(handle));
    }
    Json rows = Json::array(), frames = Json::array();
    unsigned sourceBytes = 0, paddedBytes = 0;
    for (auto &symbol : usedNames)
    {
        const auto &image = images.at(symbol);
        const auto &d = descriptors.at(symbol);
        auto region = regions.at(symbol);
        region.push_back(d.rows);
        region.push_back(d.cols);
        rows.push_back({{"source", symbol},
                        {"width", image.width},
                        {"height", image.height},
                        {"logical_width", image.logicalWidth},
                        {"logical_height", image.logicalHeight},
                        {"rows", d.rows},
                        {"cols", d.cols},
                        {"rgba_bytes", image.rgba.size()},
                        {"rgba_sha256", Sha256(image.rgba)},
                        {"files", image.files},
                        {"region", region}});
        usedFiles.insert(image.files.begin(), image.files.end());
        sourceBytes += unsigned(image.rgba.size());
        paddedBytes += (image.width + 4) * (image.height + 4) * 4;
    }
    SetWord(body, reanim ? 8 : 4, 0);
    Bytes transformed;
    std::size_t previous = 0;
    for (auto offset : offsets)
    {
        auto handle = GetWord(body, offset);
        Append(transformed, std::span(body).subspan(previous, offset - previous));
        if (handle)
        {
            auto symbol = names.at(handle);
            auto r = regions.at(symbol);
            for (auto &v : r)
                Integer(transformed, v.get<int>());
            if (reanim)
            {
                Word(transformed, descriptors.at(symbol).rows);
                Word(transformed, descriptors.at(symbol).cols);
            }
            else
                frames.push_back(
                    {{"source", symbol}, {"source_rect", {0, 0, r[2], r[3]}}, {"atlas_rect", r}});
        }
        else
            transformed.insert(transformed.end(), reanim ? 24 : 16, 0);
        previous = offset + 4;
    }
    Append(transformed, std::span(body).subspan(previous));
    if (!reanim)
    {
        SetWord(transformed, 4, unsigned(offsets.size()));
        SetWord(transformed, 16, GetWord(body, 16) + 12 * unsigned(offsets.size()));
    }
    unsigned w = usedNames.empty() ? 0 : layout["width"].get<unsigned>(),
             h = usedNames.empty() ? 0 : layout["height"].get<unsigned>();
    Bytes payload;
    Word(payload, reanim ? 0x35564152 : 0x35564150);
    Word(payload, w);
    Word(payload, h);
    Append(payload, transformed);
    auto packing = layout;
    packing.erase("regions");
    packing.erase("width");
    packing.erase("height");
    return {Envelope(reanim ? 0xdeadfed9 : 0xdeadfeda, payload),
            {{"atlas_width", w},
             {"atlas_height", h},
             {"atlas_path", usedNames.empty() ? "" : std::string(atlasPath)},
             {"atlas_shared", !atlasPath.empty() && !usedNames.empty()},
             {"packing", packing},
             {"atlas_rgba_bytes", w * h * 4},
             {"padded_region_rgba_bytes", paddedBytes},
             {"source_rgba_bytes", sourceBytes},
             {"region_count", usedNames.size()},
             {"images", rows},
             {"frames", frames},
             {"source_files", usedFiles}}};
}

Definition ReanimVariants(const Bytes &source, const Json &metadata, const Json &variants,
                          const Json &actions, const std::map<unsigned, std::string> &names,
                          const std::map<std::string, ImageDescriptor> &descriptors)
{
    auto body = Body(source, 0xdeadfed7);
    Reader reader(body);
    Require(reader.Word() == 0x46544449, "Expected IDTF variant behavior");
    reader.Skip(4);
    Require(reader.Word() == 0, "Source images must be local");
    reader.Skip(8);
    auto count = reader.Word();
    Require(count == metadata["tracks"].size(), "Variant track count differs");
    struct Track
    {
        unsigned id, frames, flags;
        Bytes values;
    };
    std::vector<Track> tracks;
    for (unsigned i = 0; i < count; ++i)
    {
        Track t;
        t.id = reader.Word();
        t.frames = reader.Word();
        Require(reader.Word() == 32, "Invalid reanim stride");
        t.flags = reader.Word();
        Require((t.flags & ~7u) == 0, "Invalid track storage");
        t.values = reader.Take(std::size_t(t.frames) * 32);
        tracks.push_back(std::move(t));
    }
    Bytes behavior(body.begin(), body.begin() + reader.at);
    using Slot = std::tuple<unsigned, unsigned, std::string, unsigned>;
    std::vector<Slot> slots(1);
    std::map<std::tuple<unsigned, std::string, unsigned>, unsigned> slotIds;
    for (unsigned t = 0; t < count; ++t)
    {
        std::string previous;
        int previousCel = 0;
        const auto &originals = metadata["variants"][0]["images"][t];
        for (unsigned f = 0; f < tracks[t].frames; ++f)
        {
            auto handle = reader.Word();
            if (handle)
                previous = names.at(handle);
            auto cel = std::bit_cast<int>(GetWord(tracks[t].values, std::size_t(f) * 32 + 24));
            if (cel == -10000)
                cel = previousCel;
            previousCel = cel;
            unsigned slot = 0;
            if (!previous.empty())
            {
                auto cols = descriptors.at(previous).cols;
                unsigned cell = cel < 0 ? 0 : unsigned(cel) % cols;
                auto key = std::make_tuple(t, previous, cell);
                if (!slotIds.contains(key))
                {
                    auto found = std::find(originals.begin(), originals.end(), Json(previous));
                    Require(found != originals.end(), "Root image missing from author slots");
                    slotIds[key] = unsigned(slots.size());
                    slots.emplace_back(t, unsigned(found - originals.begin()), previous, cell);
                }
                slot = slotIds.at(key);
            }
            Word(behavior, slot);
            Word(behavior, reader.Word());
            if (tracks[t].flags & 1)
                String(behavior, reader.String());
        }
    }
    Append(behavior, reader.Take(body.size() - reader.at));
    Require(variants.size() == metadata["variants"].size(), "Variant row count differs");
    std::map<std::string, unsigned> indices, trackIds;
    Json variantNames = Json::array();
    for (unsigned i = 0; i < variants.size(); ++i)
    {
        auto name = metadata["variants"][i]["name"].get<std::string>();
        Require(indices.emplace(name, i).second, "Duplicate variant name");
        variantNames.push_back(name);
    }
    for (unsigned i = 0; i < count; ++i)
        trackIds[metadata["tracks"][i].get<std::string>()] = tracks[i].id;
    std::vector<unsigned> actionOrder;
    Json keys = Json::array(), transitions = Json::array();
    if (!actions.is_null() && !actions.empty())
    {
        for (unsigned i = 0; i < actions.at("actions").size(); ++i)
            actionOrder.push_back(i);
        auto actionKey = [&](unsigned i) {
            const auto &a = actions["actions"][i];
            return std::pair(trackIds.at(a["track"].get<std::string>()), a["option"].get<unsigned>());
        };
        std::sort(actionOrder.begin(), actionOrder.end(),
                  [&](unsigned a, unsigned b) { return actionKey(a) < actionKey(b); });
        for (auto i : actionOrder)
        {
            auto [track, option] = actionKey(i);
            keys.push_back({track, option});
        }
        Require(actions["transitions"].size() == indices.size(), "Incomplete reanim transitions");
        for (auto &name : variantNames)
        {
            const auto &targets = actions["transitions"].at(name.get<std::string>());
            Require(targets.size() == actionOrder.size(), "Incomplete transition actions");
            for (auto i : actionOrder)
                transitions.push_back(indices.at(targets[i].get<std::string>()));
        }
    }
    Json dimensions = Json::array(), rectangles = Json::array();
    for (unsigned v = 0; v < variants.size(); ++v)
    {
        const auto &row = variants[v];
        const auto &images = metadata["variants"][v]["images"];
        std::map<std::string, Json> regions;
        for (unsigned i = 0; i < row["symbols"].size(); ++i)
            regions[row["symbols"][i].get<std::string>()] = row["layout"]["regions"][i];
        bool hasImages = false;
        for (auto &track : images)
            for (auto &image : track)
                hasImages |= !image.is_null();
        dimensions.push_back(hasImages ? Json{row["layout"]["width"], row["layout"]["height"]} : Json{0, 0});
        Json rects = Json::array();
        rects.push_back({0, 0, 0, 0});
        for (unsigned i = 1; i < slots.size(); ++i)
        {
            const auto &[t, author, original, cell] = slots[i];
            const auto &replacement = images[t][author];
            if (replacement.is_null())
            {
                rects.push_back({0, 0, 0, 0});
                continue;
            }
            auto r = regions.at(replacement.get<std::string>());
            if (replacement == original)
            {
                const auto &d = descriptors.at(original);
                int cw = r[2].get<int>() / d.cols, ch = r[3].get<int>() / d.rows;
                r = {r[0].get<int>() + int(cell % d.cols) * cw, r[1].get<int>() + int(cell / d.cols) * ch, cw,
                     ch};
            }
            rects.push_back(r);
        }
        rectangles.push_back(rects);
    }
    Bytes payload;
    Word(payload, 0x37564152);
    Word(payload, unsigned(behavior.size()));
    Append(payload, behavior);
    Word(payload, unsigned(variants.size()));
    Word(payload, unsigned(slots.size()));
    Word(payload, unsigned(keys.size()));
    for (auto &pair : dimensions)
    {
        Word(payload, pair[0].get<unsigned>());
        Word(payload, pair[1].get<unsigned>());
    }
    for (auto &pair : keys)
    {
        Word(payload, pair[0].get<unsigned>());
        Word(payload, pair[1].get<unsigned>());
    }
    for (auto &target : transitions)
        Word(payload, target.get<unsigned>());
    for (auto &row : rectangles)
        for (auto &rect : row)
            for (auto &v : rect)
                Integer(payload, v.get<int>());
    return {Envelope(0xdeadfedb, payload),
            {{"variant_count", variants.size()},
             {"slot_count", slots.size()},
             {"action_count", keys.size()},
             {"variant_names", variantNames}}};
}

Json ParseAtlas(const Bytes &bytes)
{
    Require(bytes.size() >= 24, "Truncated atlas record");
    auto cookie = GetWord(bytes, 0);
    auto body = Body(bytes, cookie);
    Reader r(body);
    auto magic = r.Word();
    Json result;
    if (magic == 0x37564152 || magic == 0x36564150)
    {
        auto size = r.Word();
        auto behavior = r.Take(size);
        result["body_offset"] = 20;
        result["appearance_offset"] = 20 + size;
        auto variants = r.Word(), slots = r.Word(), actions = magic == 0x37564152 ? r.Word() : 0;
        result["variant_count"] = variants;
        result["slot_count"] = slots;
        result["action_count"] = actions;
        Json dimensions = Json::array(), keys = Json::array(), transitions = Json::array(),
             rectangles = Json::array();
        for (unsigned i = 0; i < variants; ++i)
        {
            auto w = r.Word(), h = r.Word();
            dimensions.push_back({w, h});
        }
        for (unsigned i = 0; i < actions; ++i)
        {
            auto track = r.Word(), option = r.Word();
            keys.push_back({track, option});
        }
        for (unsigned i = 0; i < variants * actions; ++i)
            transitions.push_back(r.Word());
        for (unsigned i = 0; i < variants; ++i)
            rectangles.push_back(ReadRects(r, slots));
        r.Finish();
        result["variant_dimensions"] = dimensions;
        result["variant_rectangles"] = rectangles;
        result["action_keys"] = keys;
        result["transitions"] = transitions;
        if (magic == 0x36564150)
        {
            Reader b(behavior);
            Require(b.Word() == 0x46524449, "Expected authored particle behavior");
            Require(b.Word() == slots, "Particle slot count differs");
            result["emitter_name_hash"] = b.Word();
            auto count = b.Word(), size = b.Word();
            auto end = b.at + size;
            Json offsets = Json::array(), counts = Json::array();
            unsigned total = 0;
            for (unsigned i = 0; i < count; ++i)
            {
                auto offset = b.Word(), frames = b.Word();
                Require(offset == total, "Particle slot offsets differ");
                offsets.push_back(offset);
                counts.push_back(frames);
                total += frames;
                SkipEmitter(b);
            }
            Require(b.at == end && total == slots, "Particle behavior size differs");
            result["slot_offsets"] = offsets;
            result["frame_counts"] = counts;
            result["emitter_count"] = count;
        }
    }
    else
    {
        Require(magic == 0x35564150, "Expected PAV5 atlas");
        auto w = r.Word(), h = r.Word();
        result["width"] = w;
        result["height"] = h;
        Require(r.Word() == 0x46524449, "Expected authored particle frames");
        auto total = r.Word(), hash = r.Word(), count = r.Word(), size = r.Word();
        auto end = r.at + size;
        Json frames = Json::array(), rectangles = Json::array();
        for (unsigned i = 0; i < count; ++i)
        {
            auto n = r.Word();
            frames.push_back(n);
            auto list = ReadRects(r, n);
            for (auto &rect : list)
                rectangles.push_back(rect);
            SkipEmitter(r);
        }
        Require(r.at == end && rectangles.size() == total, "Particle atlas frame count differs");
        result["frame_counts"] = frames;
        result["frame_fields4I"] = rectangles;
        result["emitter_name_hash"] = hash;
        result["emitter_count"] = count;
    }
    return result;
}

Definition ParticleVariants(const Bytes &source, const std::vector<Definition> &variants)
{
    auto atlas = ParseAtlas(source);
    Require(!variants.empty(), "Particle root requires appearances");
    Reader sourceReader(std::span(source).subspan(24));
    auto magic = sourceReader.Word(), total = sourceReader.Word(), hash = sourceReader.Word(),
         count = sourceReader.Word(), size = sourceReader.Word();
    Require(magic == 0x46524449, "Expected authored particle root");
    auto end = sourceReader.at + size;
    Bytes body;
    unsigned offset = 0;
    for (auto &n : atlas["frame_counts"])
    {
        unsigned frames = n;
        Require(sourceReader.Word() == frames, "Particle emitter frame count differs");
        sourceReader.Skip(std::size_t(frames) * 16);
        auto start = sourceReader.at;
        SkipEmitter(sourceReader);
        Word(body, offset);
        Word(body, frames);
        Append(body, sourceReader.data.subspan(start, sourceReader.at - start));
        offset += frames;
    }
    Require(sourceReader.at == end && offset == total, "Particle root behavior size differs");
    Bytes behavior;
    for (unsigned v : {magic, total, hash, count, unsigned(body.size())})
        Word(behavior, v);
    Append(behavior, body);
    Append(behavior, sourceReader.Take(sourceReader.data.size() - sourceReader.at));
    Bytes payload;
    Word(payload, 0x36564150);
    Word(payload, unsigned(behavior.size()));
    Append(payload, behavior);
    Word(payload, unsigned(variants.size()));
    Word(payload, total);
    std::vector<Json> rows;
    for (auto &variant : variants)
    {
        auto row = ParseAtlas(variant.bytes);
        Require(row["frame_counts"] == atlas["frame_counts"], "Particle appearance frame counts differ");
        Word(payload, row["width"].get<unsigned>());
        Word(payload, row["height"].get<unsigned>());
        rows.push_back(row);
    }
    for (auto &row : rows)
        for (auto &rect : row["frame_fields4I"])
            for (auto &v : rect)
                Integer(payload, v.get<int>());
    return {Envelope(0xdeadfedc, payload), {{"variant_count", variants.size()}, {"slot_count", total}}};
}
} // namespace pvz::build
