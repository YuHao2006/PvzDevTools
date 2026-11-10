#include "NativeBuild.h"
#include <algorithm>
#include <cmath>
#include <iomanip>
#include <regex>
#include <sstream>

namespace pvz::build
{
namespace
{
using Lines = std::vector<std::string>;
std::string Join(const Lines &rows, std::string_view separator = "\n")
{
    std::string out;
    for (auto &row : rows)
    {
        if (!out.empty())
            out += separator;
        out += row;
    }
    return out;
}
Bytes Source(const Lines &rows)
{
    auto text = Join(rows) + "\n";
    return Bytes(text.begin(), text.end());
}
std::string Array(Lines &declarations, Lines &code, const std::string &type, const std::string &name,
                  const Lines &values, bool readonly = false)
{
    if (values.empty())
        return "nullptr";
    auto qualifier = readonly ? "const " : "";
    declarations.push_back("extern " + std::string(qualifier) + type + " " + name + "[" +
                           std::to_string(values.size()) + "];");
    code.push_back("constinit " + std::string(qualifier) + type + " " + name + "[" +
                   std::to_string(values.size()) + "] = {");
    for (auto &row : values)
        code.push_back("    " + row + ",");
    code.push_back("};");
    return name;
}
struct Frame
{
    std::array<float, 8> values;
    int cel = 0;
    unsigned slot = 0, font = 0;
    std::string text;
    bool hasText = false;
};
struct Track
{
    unsigned id = 0, flags = 0;
    std::vector<Frame> frames;
};
struct Segment
{
    std::array<unsigned, 9> values;
    float rate = 0;
};
struct Reanim
{
    Json atlas;
    float fps = 0;
    std::vector<Track> tracks;
    std::vector<Segment> segments;
};
Reanim ParseReanim(const Bytes &bytes)
{
    Reanim out;
    out.atlas = ParseAtlas(bytes);
    auto start = out.atlas["body_offset"].get<unsigned>(),
         end = out.atlas["appearance_offset"].get<unsigned>();
    Reader r(std::span(bytes).subspan(start, end - start));
    Require(r.Word() == 0x46544449, "Expected authored reanim behavior");
    r.Skip(4);
    Require(r.Word() == 0, "Global image IDs are forbidden");
    r.Skip(4);
    out.fps = r.Float();
    auto count = r.Word();
    Require(std::isfinite(out.fps) && out.fps > 0 && count > 0 && count <= 1 << 20,
            "Invalid static reanim header");
    for (unsigned t = 0; t < count; ++t)
    {
        Track track;
        track.id = r.Word();
        auto frames = r.Word();
        Require(r.Word() == 32 && frames > 0 && frames <= 1 << 20, "Invalid static reanim track");
        track.flags = r.Word();
        Require((track.flags & ~7u) == 0 && (track.flags >> 1) < 3, "Invalid reanim storage flags");
        Frame previous;
        previous.values = {0, 0, 0, 0, 1, 1, 0, 1};
        for (unsigned f = 0; f < frames; ++f)
        {
            Frame frame = previous;
            for (unsigned i = 0; i < 8; ++i)
            {
                if (i == 6)
                {
                    auto cel = r.Int();
                    if (cel != -10000)
                        frame.cel = cel;
                    frame.values[6] = float(frame.cel);
                }
                else
                {
                    auto value = r.Float();
                    Require(std::isfinite(value), "Nonfinite reanim transform");
                    if (value != -10000)
                        frame.values[i] = value;
                }
            }
            track.frames.push_back(frame);
            previous = frame;
        }
        out.tracks.push_back(std::move(track));
    }
    for (auto &track : out.tracks)
    {
        unsigned font = 0;
        std::string text;
        bool hasText = false;
        for (auto &frame : track.frames)
        {
            frame.slot = r.Word();
            auto nextFont = r.Word();
            Require(frame.slot < out.atlas["slot_count"].get<unsigned>() && nextFont <= 1,
                    "Invalid static reanim resource slot");
            if (nextFont)
                font = nextFont;
            if (track.flags & 1)
            {
                auto next = r.String();
                Require(next.find('\0') == next.npos, "NUL in caption");
                if (!next.empty())
                {
                    text = next;
                    hasText = true;
                }
            }
            frame.font = font;
            frame.text = text;
            frame.hasText = hasText;
        }
    }
    auto segmentTracks = r.Word();
    int lastTrack = -1;
    for (unsigned t = 0; t < segmentTracks; ++t)
    {
        auto index = r.Word(), count = r.Word();
        Require(index < out.tracks.size() && int(index) > lastTrack && !(out.tracks[index].flags & 1),
                "Invalid attacher track");
        lastTrack = int(index);
        unsigned previousEnd = 0;
        for (unsigned s = 0; s < count; ++s)
        {
            Segment row{};
            row.values[0] = index;
            for (unsigned i = 1; i < 5; ++i)
                row.values[i] = r.Word();
            row.rate = r.Float();
            for (unsigned i = 6; i < 9; ++i)
                row.values[i] = r.Word();
            Require(previousEnd <= row.values[1] && row.values[1] < row.values[2] &&
                        row.values[2] <= out.tracks[index].frames.size() && row.values[7] <= row.values[1] &&
                        row.values[2] <= row.values[8] && std::isfinite(row.rate) && row.values[6] <= 2,
                    "Invalid attacher segment");
            previousEnd = row.values[2];
            out.segments.push_back(row);
        }
    }
    r.Finish();
    return out;
}

struct Node
{
    int numerator = 0, denominator = 1;
    float low = 0, high = 0;
    unsigned curve = 0, distribution = 0;
};
using Curve = std::vector<Node>;
Curve ReadCurve(Reader &r)
{
    auto count = r.Word();
    Require(count <= 100000, "Too many particle nodes");
    Curve nodes;
    for (unsigned i = 0; i < count; ++i)
    {
        Node n;
        n.numerator = r.Int();
        n.denominator = r.Int();
        n.low = r.Float();
        n.high = r.Float();
        n.curve = r.Word();
        n.distribution = r.Word();
        Require(n.denominator > 0 && std::isfinite(n.low) && std::isfinite(n.high) && n.curve < 14 &&
                    n.distribution < 14,
                "Invalid particle node");
        nodes.push_back(n);
    }
    return nodes;
}
void WriteCurve(Bytes &bytes, const Curve &nodes)
{
    Word(bytes, unsigned(nodes.size()));
    for (auto &n : nodes)
    {
        Double(bytes, double(n.numerator) / n.denominator);
        Float(bytes, n.low);
        Float(bytes, n.high);
        Word(bytes, n.curve);
        Word(bytes, n.distribution);
    }
}
Bytes ParticleRecord(const Bytes &source, const Json &reports, unsigned names, unsigned hash, Json &summary)
{
    auto atlas = ParseAtlas(source);
    Require(atlas["emitter_name_hash"] == hash, "Particle name registry mismatch");
    unsigned start = 20, end = atlas["appearance_offset"], emitters = atlas["emitter_count"],
             variants = atlas["variant_count"], slots = atlas["slot_count"];
    Reader r(std::span(source).subspan(start, end - start));
    Require(r.Word() == 0x46524449 && r.Word() == slots && r.Word() == hash && r.Word() == emitters,
            "Invalid particle behavior header");
    auto size = r.Word();
    auto body = r.Take(size);
    Reader b(body);
    struct Field
    {
        unsigned type;
        Curve x, y;
    };
    struct Emitter
    {
        std::array<unsigned, 9> fields{};
        std::array<Curve, 22> system;
        std::array<std::vector<Field>, 2> forces;
        std::array<Curve, 16> particle;
    };
    std::vector<Emitter> rows;
    for (unsigned index = 0; index < emitters; ++index)
    {
        Emitter row;
        for (unsigned i = 0; i < 6; ++i)
            row.fields[i] = b.Word();
        Require(row.fields[0] == atlas["slot_offsets"][index] &&
                    row.fields[1] == atlas["frame_counts"][index] && row.fields[2] > 0 &&
                    row.fields[3] <= 1 && row.fields[4] < (1 << 12) && row.fields[5] <= 4,
                "Invalid particle emitter");
        for (auto &curve : row.system)
            curve = ReadCurve(b);
        for (auto &forces : row.forces)
        {
            auto count = b.Word();
            Require(count <= 4, "Too many particle fields");
            for (unsigned f = 0; f < count; ++f)
            {
                Field force;
                force.type = b.Word();
                Require(force.type > 0 && force.type < 12, "Invalid force type");
                force.x = ReadCurve(b);
                force.y = ReadCurve(b);
                forces.push_back(force);
            }
        }
        for (auto &curve : row.particle)
            curve = ReadCurve(b);
        auto setDefault = [](Curve &curve, float value) {
            if (curve.empty())
                curve.push_back({0, 1, value, value, 0, 1});
        };
        for (unsigned i : {3u, 4u, 5u})
            setDefault(row.system[i], -1);
        setDefault(row.system[14], 100);
        for (unsigned i = 17; i < 22; ++i)
            setDefault(row.system[i], 1);
        for (unsigned i : {0u, 1u, 2u, 3u, 4u, 7u, 8u})
            setDefault(row.particle[i], 1);
        unsigned mask = 0;
        for (unsigned i = 0; i < 5; ++i)
            if (!row.system[17 + i].empty() && row.system[17 + i][0].curve != 0 &&
                row.system[17 + i][0].curve != 8)
                mask |= 1 << i;
        const std::array<unsigned, 9> render = {0, 1, 2, 3, 4, 7, 8, 6, 5};
        for (unsigned i = 0; i < render.size(); ++i)
            if (!row.particle[render[i]].empty() && row.particle[render[i]][0].curve != 0 &&
                row.particle[render[i]][0].curve != 8)
                mask |= 1 << (5 + i);
        row.fields[8] = mask;
        rows.push_back(row);
    }
    b.Finish();
    Require(r.Word() == 0x33544449 && r.Word() == emitters, "Missing particle duration section");
    for (auto &row : rows)
    {
        row.fields[7] = r.Word();
        Require(row.fields[7] == 0xffffffff || row.fields[7] < emitters, "Invalid duration target");
    }
    Require(r.Word() == 0x35544449 && r.Word() == emitters, "Missing particle name section");
    for (auto &row : rows)
    {
        row.fields[6] = r.Word();
        Require(row.fields[6] < names, "Invalid particle emitter name");
    }
    r.Finish();
    Bytes out;
    for (unsigned v : {0x31435450u, names, hash, emitters, variants, slots})
        Word(out, v);
    for (auto &row : rows)
    {
        for (auto v : row.fields)
            Word(out, v);
        for (auto &curve : row.system)
        {
            WriteCurve(out, curve);
            summary["nodes"] = summary["nodes"].get<unsigned>() + curve.size();
        }
        for (auto &fields : row.forces)
        {
            Word(out, unsigned(fields.size()));
            summary["fields"] = summary["fields"].get<unsigned>() + fields.size();
            for (auto &field : fields)
            {
                Word(out, field.type);
                WriteCurve(out, field.x);
                WriteCurve(out, field.y);
                summary["nodes"] = summary["nodes"].get<unsigned>() + field.x.size() + field.y.size();
            }
        }
        for (auto &curve : row.particle)
        {
            WriteCurve(out, curve);
            summary["nodes"] = summary["nodes"].get<unsigned>() + curve.size();
        }
    }
    Require(reports.size() == variants, "Particle appearance metadata mismatch");
    for (unsigned v = 0; v < variants; ++v)
    {
        unsigned w = atlas["variant_dimensions"][v][0], h = atlas["variant_dimensions"][v][1];
        Word(out, w);
        Word(out, h);
        String(out, reports[v]["atlas_path"].get<std::string>());
        for (auto &physicalRect : atlas["variant_rectangles"][v])
        {
            auto rect = DrawingRegion(physicalRect, reports[v]);
            int x = rect[0], y = rect[1], a = rect[2], b = rect[3];
            for (double coordinate : {w ? double(x) / w : 0, h ? double(y) / h : 0, w ? double(x + a) / w : 0,
                                      h ? double(y + b) / h : 0})
                Float(out, float(coordinate));
            Integer(out, rect.size() == 6 ? rect[4].get<int>() : a);
            Integer(out, rect.size() == 6 ? rect[5].get<int>() : b);
        }
    }
    summary["emitters"] = summary["emitters"].get<unsigned>() + emitters;
    summary["variants"] = summary["variants"].get<unsigned>() + variants;
    summary["rectangles"] = summary["rectangles"].get<unsigned>() + variants * slots;
    return out;
}
} // namespace

Generated GenerateDefinitions(const Config &config, const Records &records, const Json &reports,
                              const fs::path &generated)
{
    Generated result;
    const auto &reanimOrder = config.order.at("reanim");
    const auto &particleOrder = config.order.at("particle");
    unsigned rc = unsigned(reanimOrder.size()), pc = unsigned(particleOrder.size());
    std::vector<Reanim> definitions;
    std::set<std::string> expected;
    for (auto &[id, name] : reanimOrder)
    {
        auto path = "compiled/reanim/" + Lower(name) + ".compiled";
        expected.insert(path);
        definitions.push_back(ParseReanim(records.at(path)));
    }
    std::array<Lines, 8> shards;
    for (auto &shard : shards)
        shard = {"#include \"../GeneratedDefinitions.h\"", "#include \"../GeneratedDefinitionStorage.h\"", "",
                 "namespace GeneratedDefinitions {"};
    Lines declarations, initializers, fontBindings, fontResets, attacherInitializers;
    Json rs = {{"definitions", rc},
               {"tracks", 0},
               {"frames", 0},
               {"variants", 0},
               {"rectangles", 0},
               {"attacher_segments", 0},
               {"font_frames", 0},
               {"storage_tracks", {{"both", 0}, {"cpu", 0}, {"gpu", 0}}},
               {"unindexed_records", Json::array()}};
    for (auto &[path, data] : records)
        if (path.starts_with("compiled/reanim/") && path.ends_with(".compiled") && !expected.contains(path))
            rs["unindexed_records"].push_back(path);
    for (unsigned index = 0; index < rc; ++index)
    {
        const auto &definition = definitions[index];
        const auto path = "compiled/reanim/" + Lower(reanimOrder[index].second) + ".compiled";
        const auto &atlas = definition.atlas;
        auto &code = shards[index % 8];
        auto prefix = "Reanim" + std::to_string(index);
        auto emit = [&](const std::string &type, const std::string &suffix, const Lines &values,
                        bool readonly = false) {
            return Array(declarations, code, type, prefix + suffix, values, readonly);
        };
        Lines trackValues;
        unsigned count = unsigned(definition.tracks.size());
        for (unsigned t = 0; t < count; ++t)
        {
            const auto &track = definition.tracks[t];
            auto storage = std::array<const char *, 3>{"both", "cpu", "gpu"}.at(track.flags >> 1);
            rs["storage_tracks"][storage] = rs["storage_tracks"][storage].get<unsigned>() + 1;
            Lines values;
            for (unsigned f = 0; f < track.frames.size(); ++f)
            {
                const auto &frame = track.frames[f];
                Lines fields;
                for (unsigned i = 0; i < 8; ++i)
                    fields.push_back(i == 6 ? std::to_string(frame.cel) : CppFloat(frame.values[i]));
                fields.push_back(std::to_string(frame.slot) + "u");
                fields.push_back("nullptr");
                fields.push_back(frame.hasText ? CppString(frame.text) : "nullptr");
                values.push_back("{" + Join(fields, ", ") + "}");
                if (frame.font)
                {
                    auto target = prefix + "Frames" + std::to_string(t) + "[" + std::to_string(f) + "].mFont";
                    fontBindings.push_back(target + " = font;");
                    fontResets.push_back(target + " = nullptr;");
                }
            }
            auto frames = emit("ReanimatorSourceTransform", "Frames" + std::to_string(t), values);
            trackValues.push_back("{static_cast<ReanimTrackId>(" + std::to_string(track.id) + "u), {" +
                                  frames + ", " + std::to_string(values.size()) + "}}");
            rs["frames"] = rs["frames"].get<unsigned>() + values.size();
        }
        auto tracks = emit("ReanimatorTrack", "Tracks", trackValues);
        unsigned variants = atlas["variant_count"], slots = atlas["slot_count"],
                 actions = atlas["action_count"];
        auto textures = emit("Sexy::GpuTexture*", "Textures", Lines(variants, "nullptr"));
        Lines rectValues;
        for (unsigned v = 0; v < variants; ++v)
            for (auto &rect : atlas["variant_rectangles"][v])
                rectValues.push_back(
                    Region(DrawingRegion(rect, reports.at(path).at("variants").at(v)),
                           atlas["variant_dimensions"][v][0], atlas["variant_dimensions"][v][1]));
        auto rects = emit("Sexy::TextureRegion", "Rects", rectValues, true);
        Lines transitionValues;
        for (auto &v : atlas["transitions"])
            transitionValues.push_back(std::to_string(v.get<unsigned>()) + "u");
        auto transitions = emit("std::uint32_t", "Transitions", transitionValues);
        std::vector<unsigned> starts(count), counts(count), options(variants * count);
        std::map<unsigned, unsigned> firstTracks;
        for (unsigned t = 0; t < count; ++t)
            firstTracks.try_emplace(definition.tracks[t].id, t);
        for (unsigned a = 0; a < actions; ++a)
        {
            auto t = firstTracks.at(atlas["action_keys"][a][0].get<unsigned>());
            if (counts[t] == 0)
                starts[t] = a;
            ++counts[t];
            for (unsigned v = 0; v < variants; ++v)
                if (atlas["transitions"][v * actions + a] == v)
                    options[v * count + t] = atlas["action_keys"][a][1];
        }
        auto integers = [](const std::vector<unsigned> &values) {
            Lines result;
            for (auto v : values)
                result.push_back(std::to_string(v) + "u");
            return result;
        };
        auto startName = emit("std::uint32_t", "ActionStarts", integers(starts)),
             countName = emit("std::uint32_t", "ActionCounts", integers(counts)),
             optionName = emit("std::uint32_t", "Options", integers(options));
        initializers.push_back(
            "{" +
            Join({"{" + tracks + ", " + std::to_string(count) + "}", CppFloat(definition.fps), textures,
                  std::to_string(variants) + "u", std::to_string(slots) + "u", std::to_string(actions) + "u",
                  rects, transitions, startName, countName, optionName},
                 ", ") +
            "}");
        Lines segmentValues;
        for (auto &segment : definition.segments)
        {
            auto target = segment.values[3], action = segment.values[4];
            int first = -1, length = -1;
            if (target != 0xffffffff && action)
            {
                Require(target < definitions.size(), "Attacher target outside catalog");
                auto &targetTracks = definitions[target].tracks;
                auto found = std::find_if(targetTracks.begin(), targetTracks.end(),
                                          [&](const Track &t) { return t.id == action; });
                Require(found != targetTracks.end(), "Attacher action missing in target");
                int last = -1;
                for (unsigned f = 0; f < found->frames.size(); ++f)
                    if (found->frames[f].cel >= 0)
                    {
                        if (first < 0)
                            first = int(f);
                        last = int(f);
                    }
                if (first < 0)
                {
                    first = 0;
                    length = 1;
                }
                else
                    length = last - first + 1;
            }
            Lines fields;
            for (unsigned i = 0; i < 9; ++i)
                fields.push_back(i == 5 ? CppFloat(segment.rate) : std::to_string(segment.values[i]) + "u");
            fields.push_back(std::to_string(first));
            fields.push_back(std::to_string(length));
            segmentValues.push_back("{" + Join(fields, ", ") + "}");
        }
        auto segments = emit("AttacherSegmentRT", "Segments", segmentValues);
        attacherInitializers.push_back(
            segmentValues.empty() ? "{}"
                                  : "{" + segments + ", " + std::to_string(segmentValues.size()) + "}");
        for (auto [key, value] : std::vector<std::pair<std::string, unsigned>>{
                 {"tracks", count},
                 {"variants", variants},
                 {"rectangles", variants * slots},
                 {"attacher_segments", unsigned(segmentValues.size())}})
            rs[key] = rs[key].get<unsigned>() + value;
    }
    rs["font_frames"] = fontBindings.size();
    rs["rectangle_bytes"] = rs["rectangles"].get<unsigned>() * 24;
    for (unsigned i = 0; i < 8; ++i)
    {
        shards[i].push_back("}");
        result.files["definitions/ReanimData0" + std::to_string(i) + ".cpp"] = Source(shards[i]);
    }
    auto registry = Text(generated / "ParticleEmitterName.h");
    std::smatch nameCount, nameHash;
    Require(std::regex_search(registry, nameCount, std::regex(R"(\btop\s*=\s*(\d+))")) &&
                std::regex_search(registry, nameHash,
                                  std::regex(R"(\bkParticleEmitterNameTableHash\s*=\s*(\d+))")),
            "Missing particle name registry");
    unsigned names = std::stoul(nameCount[1]), hash = std::stoul(nameHash[1]);
    Json ps = {{"definitions", pc},    {"emitters", 0}, {"nodes", 0},
               {"fields", 0},          {"variants", 0}, {"rectangles", 0},
               {"rectangle_bytes", 0}, {"shards", 0},   {"unindexed_records", Json::array()}};
    expected.clear();
    for (auto &[id, name] : particleOrder)
    {
        auto path = "compiled/particles/" + Lower(name) + ".compiled";
        Require(expected.insert(path).second, "Duplicate particle root");
        const auto &report = reports.at(path);
        auto variants = report.contains("variants") ? report["variants"] : Json::array({report});
        result.runtime["compiled/particle-runtime/" + Lower(name) + ".compiled"] =
            ParticleRecord(records.at(path), variants, names, hash, ps);
    }
    ps["rectangle_bytes"] = ps["rectangles"].get<unsigned>() * 24;
    for (auto &[path, data] : records)
        if (path.starts_with("compiled/particles/") && path.ends_with(".compiled") &&
            !expected.contains(path))
            ps["unindexed_records"].push_back(path);
    Lines header = {"// Generated native definitions and loading functions. Do not edit.",
                    "#pragma once",
                    "#include \"Sexy.TodLib/Reanimator.h\"",
                    "#include \"Sexy.TodLib/TodParticle.h\"",
                    "#include \"Lawn/ProjectileResources.h\"",
                    "#include <span>",
                    "namespace GeneratedDefinitions {",
                    "inline constexpr int ReanimationCount=" + std::to_string(rc) +
                        ", ParticleCount=" + std::to_string(pc) + ";",
                    "extern ReanimatorDefinition Reanimations[ReanimationCount];",
                    "extern TodParticleDefinition Particles[ParticleCount];",
                    "extern const char* const ParticlePaths[ParticleCount];",
                    "extern ProjectileAtlasDefinition Projectiles;",
                    "extern std::span<AttacherSegmentRT> AttacherSegments[ReanimationCount];",
                    "void LoadReanimationImages(int type);",
                    "void LoadParticleImages();",
                    "void LoadProjectileImages();",
                    "void LoadAllImages();",
                    "void BindFonts();",
                    "void ReleaseReanimationImages();",
                    "void ReleaseParticleImages();",
                    "void ReleaseProjectileImages();",
                    "void ReleaseImages();",
                    "}"};
    Lines storage = {"// Generated backing storage declarations; no runtime wrappers.", "#pragma once",
                     "#include \"GeneratedDefinitions.h\"", "namespace GeneratedDefinitions {"};
    storage.insert(storage.end(), declarations.begin(), declarations.end());
    storage.push_back("}");
    Lines objects = {"// Generated complete static definitions.", "#include \"GeneratedDefinitionStorage.h\"",
                     "namespace GeneratedDefinitions {",
                     "constinit ReanimatorDefinition Reanimations[ReanimationCount] = {"};
    for (auto &row : initializers)
        objects.push_back("    " + row + ",");
    objects.insert(objects.end(), {"};", "TodParticleDefinition Particles[ParticleCount]{};",
                                   "constinit const char* const ParticlePaths[ParticleCount] = {"});
    for (auto &[id, name] : particleOrder)
        objects.push_back("    " + CppString("compiled/particle-runtime/" + Lower(name) + ".compiled") + ",");
    objects.insert(objects.end(),
                   {"};", "constinit std::span<AttacherSegmentRT> AttacherSegments[ReanimationCount] = {"});
    for (auto &row : attacherInitializers)
        objects.push_back("    " + row + ",");
    objects.push_back("};");
    Reader projectile(records.at("compiled/projectiles/projectiles.compiled"));
    Require(projectile.Word() == 0x314a5250 && projectile.Word() == 1 && projectile.Word() == 14,
            "Invalid projectile record");
    unsigned pw = projectile.Word(), ph = projectile.Word();
    const auto &projectileReport = reports.at("compiled/projectiles/projectiles.compiled");
    Lines visuals, shadows;
    for (unsigned i = 0; i < 14; ++i)
    {
        int x = projectile.Int(), y = projectile.Int(), w = projectile.Int(), h = projectile.Int();
        auto scale = projectile.Float();
        visuals.push_back("{" + Region(DrawingRegion(Json{x, y, w, h}, projectileReport), pw, ph) + "," + CppFloat(scale) + "}");
    }
    for (unsigned i = 0; i < 2; ++i)
    {
        int x = projectile.Int(), y = projectile.Int(), w = projectile.Int(), h = projectile.Int();
        shadows.push_back(Region(DrawingRegion(Json{x, y, w, h}, projectileReport), pw, ph));
    }
    projectile.Finish();
    objects.insert(objects.end(), {"constinit ProjectileAtlasDefinition Projectiles = {",
                                   "    {{" + Join(visuals, ",") + "}},",
                                   "    {{" + Join(shadows, ",") + "}}, nullptr", "};"});
    for (auto kind : {"reanim", "particle"})
    {
        auto type = std::string(kind) == "reanim" ? "ReanimationType" : "ParticleEffect",
             count = std::string(kind) == "reanim" ? "NUM_REANIMS" : "NUM_PARTICLES";
        const auto &order = config.order.at(kind);
        objects.push_back("static_assert(static_cast<int>(" + std::string(type) + "::" + count +
                          ")==" + std::to_string(order.size()) + ");");
        for (unsigned i = 0; i < order.size(); ++i)
            objects.push_back("static_assert(static_cast<int>(" + std::string(type) + "::" + order[i].first +
                              ")==" + std::to_string(i) + ");");
    }
    objects.push_back("}");
    Lines load = {"// Generated image ownership bindings; behavior is already static.",
                  "#include \"GeneratedDefinitionStorage.h\"",
                  "#include \"Sexy.TodLib/DefinitionImageRegion.h\"",
                  "#include \"Sexy.TodLib/ParticleDefinitionCompiled.h\"",
                  "#include \"Sexy.TodLib/TodCommon.h\"",
                  "#include <algorithm>",
                  "#include <stdexcept>",
                  "namespace GeneratedDefinitions {",
                  "namespace {",
                  "constinit bool reanimationImages[ReanimationCount]{};",
                  "constinit bool particleImages=false, projectileImages=false, fonts=false;",
                  "}",
                  "void BindFonts() {",
                  "    if (fonts) return;"};
    if (!fontBindings.empty())
    {
        load.push_back("    auto* font=TodLoadCreditsFont();");
        load.push_back(
            "    if (!font) throw std::runtime_error(\"Static reanimation font is unavailable\");");
        for (auto &row : fontBindings)
            load.push_back("    " + row);
    }
    load.insert(load.end(),
                {"    fonts=true;", "}", "void LoadReanimationImages(int type) {",
                 "    if (type<0 || type>=ReanimationCount) throw std::out_of_range(\"Reanimation type\");",
                 "    if (reanimationImages[type]) return;", "    switch(type) {"});
    std::set<std::string> atlasPaths;
    for (unsigned index = 0; index < rc; ++index)
    {
        load.push_back("    case " + std::to_string(index) + ":");
        const auto &report = reports.at("compiled/reanim/" + Lower(reanimOrder[index].second) + ".compiled");
        const auto &rows = report["variants"];
        for (unsigned v = 0; v < rows.size(); ++v)
        {
            auto path = rows[v]["atlas_path"].get<std::string>();
            if (path.empty())
                continue;
            atlasPaths.insert(path);
            load.push_back(
                "        DefinitionBindAtlasPath(Reanimations[" + std::to_string(index) + "].mTextures[" +
                std::to_string(v) + "], " + std::to_string(rows[v]["atlas_width"].get<unsigned>()) + ", " +
                std::to_string(rows[v]["atlas_height"].get<unsigned>()) + ", " + CppString(path) + ");");
        }
        load.push_back("        break;");
    }
    load.insert(load.end(), {"    }", "    reanimationImages[type]=true;", "}", "void LoadParticleImages() {",
                             "    if (particleImages) return;"});
    for (auto &[id, name] : particleOrder)
        for (auto &row : reports.at("compiled/particles/" + Lower(name) + ".compiled")["variants"])
        {
            auto path = row["atlas_path"].get<std::string>();
            if (!path.empty())
                atlasPaths.insert(path);
        }
    load.insert(load.end(),
                {"    try {",
                 "        for(int i=0;i<ParticleCount;++i) "
                 "TodParticleLoadCompiledDefinition(Particles[i],ParticlePaths[i]);",
                 "    } catch (...) {",
                 "        for(auto& definition:Particles) TodParticleFreeCompiledDefinition(definition);",
                 "        throw;", "    }", "    particleImages=true;", "}", "void LoadProjectileImages() {",
                 "    if (projectileImages) return;"});
    auto path = reports.at("compiled/projectiles/projectiles.compiled")["atlas_path"].get<std::string>();
    atlasPaths.insert(path);
    load.push_back("    DefinitionBindAtlasPath(Projectiles.mTexture, " + std::to_string(pw) + ", " +
                   std::to_string(ph) + ", " + CppString(path) + ");");
    load.insert(load.end(), {"    projectileImages=true;", "}", "void LoadAllImages() {", "    BindFonts();",
                             "    for(int i=0;i<ReanimationCount;++i) LoadReanimationImages(i);",
                             "    LoadParticleImages();", "    LoadProjectileImages();", "}",
                             "void ReleaseReanimationImages() {", "    for(auto& definition:Reanimations)",
                             "        for(unsigned v=0;v<definition.mVariantCount;++v) "
                             "DefinitionUnbindAtlas(definition.mTextures[v]);"});
    for (auto &row : fontResets)
        load.push_back("    " + row);
    load.insert(load.end(),
                {"    std::fill(std::begin(reanimationImages),std::end(reanimationImages),false);",
                 "    fonts=false;", "}", "void ReleaseParticleImages() {",
                 "    for(auto& definition:Particles) TodParticleFreeCompiledDefinition(definition);",
                 "    particleImages=false;", "}", "void ReleaseProjectileImages() {",
                 "    DefinitionUnbindAtlas(Projectiles.mTexture);", "    projectileImages=false;", "}",
                 "void ReleaseImages() {", "    ReleaseReanimationImages();", "    ReleaseParticleImages();",
                 "    ReleaseProjectileImages();", "}", "}"});
    result.files["GeneratedDefinitions.h"] = Source(header);
    result.files["GeneratedDefinitionStorage.h"] = Source(storage);
    result.files["GeneratedDefinitions.cpp"] = Source(objects);
    result.files["GeneratedDefinitionLoading.cpp"] = Source(load);
    Json generatedFiles = Json::object();
    for (auto &[name, data] : result.files)
        generatedFiles[name] = {{"bytes", data.size()}, {"sha256", Sha256(data)}};
    result.summary = {{"format", "pvz-static-definitions-v1"},
                      {"reanim", rs},
                      {"particle", ps},
                      {"projectile_types", 14},
                      {"atlases", atlasPaths},
                      {"generated_files", generatedFiles}};
    return result;
}

std::string AttacherMasks(std::string_view mphf, std::string_view)
{
    std::string text(mphf);
    std::regex slot(R"(\{0x([0-9a-fA-F]+)ull,\s*(\d+)\})");
    std::map<unsigned, unsigned> trackCounts;
    std::smatch block;
    Require(std::regex_search(text, block, std::regex(R"(kSlots\[kSlotCount\]\s*=\s*\{([\s\S]*?)\n    \};)")),
            "Missing MPHF slots");
    auto slotText = block[1].str();
    for (auto i = std::sregex_iterator(slotText.begin(), slotText.end(), slot); i != std::sregex_iterator();
         ++i)
    {
        auto key = std::stoull((*i)[1], nullptr, 16);
        if (key)
            trackCounts[unsigned(key >> 32)] =
                std::max(trackCounts[unsigned(key >> 32)], unsigned(std::stoul((*i)[2]) + 1));
    }
    auto number = [&](const std::string &name) {
        std::smatch match;
        Require(std::regex_search(text, match, std::regex(name + R"(\s*=\s*(\d+)\s*;)")),
                "Missing attacher mask constant");
        return unsigned(std::stoul(match[1]));
    };
    auto count = number("kAttacherMaskTypeCount"), words = number("kAttacherMaskWords");
    unsigned maxTracks = 0;
    for (auto [id, n] : trackCounts)
        maxTracks = std::max(maxTracks, n);
    Require(words == (maxTracks + 63) / 64, "Attacher mask capacity differs");
    Require(
        std::regex_search(
            text, block,
            std::regex(
                R"(kAttacherMaskByType\[kAttacherMaskTypeCount\]\[kAttacherMaskWords\]\s*=\s*\{([\s\S]*?)\n\};)")),
        "Missing attacher masks");
    auto masks = block[1].str();
    std::regex row(R"(\{([^}]*)\})"), word(R"(0x([0-9a-fA-F]+)ull)");
    Lines rows;
    for (auto i = std::sregex_iterator(masks.begin(), masks.end(), row); i != std::sregex_iterator(); ++i)
    {
        auto content = (*i)[1].str();
        Lines values;
        for (auto j = std::sregex_iterator(content.begin(), content.end(), word); j != std::sregex_iterator();
             ++j)
        {
            std::ostringstream out;
            out << "0x" << std::hex << std::setw(16) << std::setfill('0') << std::stoull((*j)[1], nullptr, 16)
                << "ull";
            values.push_back(out.str());
        }
        Require(values.size() == words, "Attacher mask word count differs");
        rows.push_back("    {" + Join(values, ", ") + "},  // type " + std::to_string(rows.size()) + " (" +
                       std::to_string(trackCounts[unsigned(rows.size())]) + " tracks)");
    }
    Require(rows.size() == count, "Attacher mask row count differs");
    Lines output = {"// Generated by pvz-resource-build from ReanimTrackMphfGlobal.h. Do not edit.",
                    "#pragma once",
                    "#include <cstdint>",
                    "inline constexpr int kMaxReanimTracks = " + std::to_string(maxTracks) + ";",
                    "inline constexpr int kReanimAttacherTypeCount = " + std::to_string(count) + ";",
                    "inline constexpr int kReanimAttacherMaskWords = " + std::to_string(words) + ";",
                    "inline constexpr std::uint64_t "
                    "kReanimAttacherMasks[kReanimAttacherTypeCount][kReanimAttacherMaskWords] = {"};
    output.insert(output.end(), rows.begin(), rows.end());
    output.push_back("};");
    return Join(output) + "\n";
}
} // namespace pvz::build
