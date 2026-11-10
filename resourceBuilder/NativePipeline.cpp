#include "NativeBuild.h"
#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <fstream>
#include <iostream>
#include <mutex>
#include <sstream>
#include <thread>

namespace pvz::build
{
namespace
{
struct Unit
{
    std::string name, path;
    std::vector<std::string> members, symbols;
    AtlasSettings settings;
};
std::vector<std::string> Lines(const std::string &text)
{
    std::vector<std::string> lines;
    std::istringstream stream(text);
    std::string line;
    while (std::getline(stream, line))
    {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        lines.push_back(line);
    }
    return lines;
}
std::string List(const std::vector<fs::path> &paths)
{
    std::string text;
    for (auto &path : paths)
        text += path.string() + "\n";
    return text;
}
Json GeometryReport(const std::vector<std::string> &symbols, const Json &layout,
                    const std::vector<std::string> &order, const std::map<std::string, Image> &images,
                    const std::map<std::string, ImageDescriptor> &descriptors, const std::string &path,
                    bool shared)
{
    std::map<std::string, Json> regions;
    for (unsigned i = 0; i < order.size(); ++i)
        regions[order[i]] = layout["regions"][i];
    Json rows = Json::array();
    std::set<std::string> files;
    unsigned sourceBytes = 0, paddedBytes = 0;
    for (auto &symbol : symbols)
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
        files.insert(image.files.begin(), image.files.end());
        sourceBytes += unsigned(image.rgba.size());
        paddedBytes += (image.width + 4) * (image.height + 4) * 4;
    }
    unsigned w = symbols.empty() ? 0 : layout["width"].get<unsigned>(),
             h = symbols.empty() ? 0 : layout["height"].get<unsigned>();
    auto packing = layout;
    packing.erase("regions");
    packing.erase("width");
    packing.erase("height");
    return {{"atlas_width", w},
            {"atlas_height", h},
            {"atlas_path", symbols.empty() ? "" : path},
            {"atlas_shared", shared && !symbols.empty()},
            {"packing", packing},
            {"atlas_rgba_bytes", w * h * 4},
            {"padded_region_rgba_bytes", paddedBytes},
            {"source_rgba_bytes", sourceBytes},
            {"region_count", rows.size()},
            {"images", rows},
            {"frames", Json::array()},
            {"source_files", files}};
}
std::set<std::string> SourceSymbols(const Json &variant)
{
    std::set<std::string> result;
    for (auto &track : variant.at("images"))
        for (auto &value : track)
            if (!value.is_null())
                result.insert(value.get<std::string>());
    return result;
}
void Insert(Records &records, const Records &source)
{
    for (auto &[name, data] : source)
        Require(records.emplace(name, data).second, "Duplicate generated file " + name);
}
std::string Bindings(const Json &reports)
{
    std::vector<std::string> names;
    for (auto &[name, row] : reports.items())
        names.push_back(name);
    std::sort(names.begin(), names.end());
    std::vector<std::string> rows;
    std::map<std::string, std::pair<unsigned, unsigned>> extents;
    for (auto &name : names)
    {
        const auto &report = reports.at(name);
        auto variants = report.contains("variants") ? report["variants"] : Json::array({report});
        for (unsigned i = 0; i < variants.size(); ++i)
        {
            const auto &row = variants[i];
            auto path = row["atlas_path"].get<std::string>();
            rows.push_back("    {" + CppString(name) + ", " + std::to_string(i) + "u, " + CppString(path) +
                           "},\n");
            if (!path.empty())
            {
                std::pair<unsigned, unsigned> dimensions = {row["atlas_width"], row["atlas_height"]};
                Require(!extents.contains(path) || extents[path] == dimensions,
                        "Shared atlas dimensions disagree");
                extents[path] = dimensions;
            }
        }
    }
    std::string text =
        "// Generated offline asset verification metadata; not included by the game.\n#pragma once\n#include "
        "<array>\n#include <cstdint>\n#include <string_view>\n\nnamespace DefinitionImageBindings {\nstruct "
        "Binding { std::string_view definition; std::uint32_t variant; std::string_view image; };\ninline "
        "constexpr std::array<Binding, " +
        std::to_string(rows.size()) + "> Bindings{{\n";
    for (auto &row : rows)
        text += row;
    text += "}};\nstruct Extent { std::string_view image; unsigned width,height; };\ninline constexpr "
            "std::array<Extent, " +
            std::to_string(extents.size()) + "> Extents{{\n";
    for (auto &[path, dimensions] : extents)
        text += "    {" + CppString(path) + ", " + std::to_string(dimensions.first) + "u, " +
                std::to_string(dimensions.second) + "u},\n";
    return text + "}};\n}\n";
}
std::set<std::string> Physical(const std::string &kind, const std::string &value,
                               const std::set<std::string> &available)
{
    auto name = Key(value);
    std::set<std::string> result;
    auto add = [&](const std::string &candidate) {
        if (available.contains(candidate))
            result.insert(candidate);
    };
    if (kind == "atlas")
        add(name);
    else if (kind == "sound")
        for (auto ext : {".wav", ".ogg", ".au"})
            add(name + ext);
    else
    {
        auto image = [&](const std::string &stem) {
            auto ext = fs::path(stem).extension().string();
            if (ext.empty())
                for (auto e : {".tga", ".jpg", ".png", ".gif"})
                    add(stem + e);
            else if (ext == ".tga" || ext == ".jpg" || ext == ".png" || ext == ".gif")
                add(stem);
        };
        image(name);
        image((fs::path(name).parent_path() / ("_" + fs::path(name).filename().string())).generic_string());
        image(name + "_");
    }
    Require(!result.empty(), "Missing referenced " + kind + " asset: " + value);
    return result;
}
} // namespace

void Build(const fs::path &root, const fs::path &compiler, const fs::path &configuration,
           const fs::path &cache, const fs::path &generated)
{
    auto config = LoadConfig(configuration);
    ValidateCatalogs(config, root);
    fs::create_directories(cache);
    auto definitions = root / "assets/definitions";
    auto work = cache / ("native-work-" +
                         std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
    Require(fs::create_directory(work), "Cannot create private build workspace");
    struct Cleanup
    {
        fs::path path;
        ~Cleanup()
        {
            std::error_code ec;
            fs::remove_all(path, ec);
        }
    } cleanup{work};
    auto staged = work / "generated";
    fs::create_directories(staged);
    Records deferred;
    Publish(work / "reanim-inputs.txt", List(config.sources.at("reanim")));
    Publish(work / "particle-inputs.txt", List(config.sources.at("particle")));
    Publish(work / "reanim-types.txt", TypeMap(config));
    auto metadataRoot = definitions / "toolchain";
    // These are game-specific DSL inputs, not compiler implementation files.
    auto input = [&](const std::string &name) {
        auto path = metadataRoot / name;
        Require(fs::is_regular_file(path), "Missing game compiler input: " + path.string());
        return path.string();
    };
    Run(compiler,
        {"compile-v2", "--input-list", (work / "reanim-inputs.txt").string(), "--output",
         (work / "reanim").string(), "--type-map", (work / "reanim-types.txt").string(), "--family-file",
         input("reanim_track_families.txt"), "--font-registry", input("font_symbol_ids.bin")});
    Run(compiler, {"compile-particle-v2", "--input-list", (work / "particle-inputs.txt").string(), "--output",
                   (work / "particles").string(), "--gold64", (definitions / "particle-reference").string()});
    Require(config.sources.at("projectile").size() == 1 && config.sources.at("zombatar").size() == 1,
            "Production build requires projectile and Zombatar definitions");
    Run(compiler, {"compile-projectile", "--input", config.sources.at("projectile")[0].string(), "--output",
                   (work / "projectiles.json").string()});
    auto projectiles = ReadJson(work / "projectiles.json"),
         zombatar = ReadJson(config.sources.at("zombatar")[0]);
    Run(compiler, {"bake-strings", "--input", (definitions / "LawnStrings.txt").string(), "--output-dir",
                   (work / "strings").string()});
    for (auto name : {"ReanimTrackId.h", "ReanimTrackMphfGlobal.h"})
        deferred["../" + std::string(name)] = Read(work / "reanim" / name);
    for (auto name : {"ReanimVariantId.h"})
        deferred[name] = Read(work / "reanim" / name);
    for (auto name : {"ParticleEmitterName.h", "ParticleVariantId.h"})
    {
        deferred[name] = Read(work / "particles" / name);
        Publish(staged / name, deferred.at(name));
    }
    for (auto name : {"LawnStrings.h", "LawnStrings.cpp"})
        deferred[name] = Read(work / "strings" / name);
    const std::set<std::string> fixtures = {"artchallengeplant.compiled", "circleevenspacing.compiled",
                                            "kernelsplat.compiled",       "perftest.compiled",
                                            "seedpacketpickup.compiled",  "simpleparticle.compiled"};
    Records records;
    std::map<std::string, std::map<unsigned, std::string>> sourceNames;
    for (auto kind : {"reanim", "particles"})
    {
        auto directory = work / kind;
        std::set<std::string> used;
        unsigned index = 1;
        for (auto &name : Lines(Text(directory / "image_sources.txt")))
        {
            Require(!name.empty() && used.insert(name).second, "Invalid compiler image sources");
            sourceNames[kind][index++] = name;
        }
        for (auto &entry : fs::directory_iterator(directory))
            if (entry.is_regular_file() && entry.path().extension() == ".source-record")
            {
                auto name = Lower(entry.path().stem().string()) + ".compiled";
                if (!fixtures.contains(name))
                    records["compiled/" + std::string(kind) + "/" + name] = Read(entry.path());
            }
    }
    auto particleMetadata = ReadJson(work / "particles/particle_variants.json").at("roots"),
         reanimMetadata = ReadJson(work / "reanim/reanim_variants.json").at("roots");
    auto inheritance = ReadJson(work / "particles/particle_inheritance.json");
    for (auto &[child, parent] : inheritance.items())
        Require(records.contains(child) && records.contains(parent.get<std::string>()),
                "Invalid particle inheritance sidecar");
    Json actions = ReadJson(definitions / "reanim-variant-actions.json"), virtualVariants = Json::object();
    for (auto &[rootName, metadata] : reanimMetadata.items())
        for (auto &variant : metadata["variants"])
        {
            auto path = "compiled/reanim/" + Lower(variant["name"].get<std::string>()) + ".compiled";
            Require(!virtualVariants.contains(path), "Duplicate reanim appearance");
            virtualVariants[path] = {{"root", rootName}, {"variant", variant}};
        }
    auto corpus = LoadFonts(definitions / "fonts");
    auto rawFiles = Inventory(root);
    Insert(records, PackFontImages(corpus, rawFiles));
    Insert(records, CompileFonts(corpus));
    auto author = ReadJson(definitions / "resources.json");
    auto plan = CompileResources(root, author, corpus.tags);
    Json sceneInput = Json::array();
    for (auto &source : config.sources.at("scene"))
        sceneInput.push_back(SceneParse(Text(source)));
    auto commonUI=CommonUI();
    const auto hdManifest=root/"assets/high-resolution/images.json";
    if(fs::exists(hdManifest))
    {
        const auto cards=ReadJson(hdManifest).value("cards",Json::object());
        for(const auto &[id,image]:cards.items())
        {
            Require(std::stoul(id)<65536,"High-resolution seed card index exceeds atlas index range");
            commonUI["sprites"].push_back({{"name","SEED_CARD_"+id},{"image",image},
                                           {"logical_size",{50,70}},{"scale",1.0}});
        }
    }
    sceneInput.push_back(commonUI);
    std::map<std::string, ImageDescriptor> sourceImages;
    auto scenes = PrepareScenes(sceneInput, author, rawFiles, sourceImages);
    Require(scenes.size() == 6, "Production build needs all six scene modules");
    std::set<std::string> labels;
    for (auto &[kind, names] : sourceNames)
        for (auto &[index, name] : names)
            labels.insert(name);
    std::set<std::string> projectileImages, zombatarImages;
    for (auto &row : projectiles["projectiles"])
        if (!row["image"].is_null())
            projectileImages.insert(row["image"].get<std::string>());
    for (auto &row : projectiles["shadows"])
        projectileImages.insert(row["image"].get<std::string>());
    labels.insert(projectileImages.begin(), projectileImages.end());
    auto authorImages = ImageDescriptors(author, rawFiles, labels);
    for (auto &[name, d] : authorImages)
        Require(sourceImages.emplace(name, d).second, "Duplicate scene/source decode key");
    for (auto &name : projectileImages)
    {
        Key(name);
        Require(Lower(fs::path(name).extension().string()) == ".png", "Projectile path must name a PNG");
        ImageDescriptor d;
        d.path = name;
        d.flags = 1024;
        sourceImages[name] = d;
    }
    Require(zombatar["format"] == "pvz-zombatar-sources-v1" && zombatar["images"].size() == 209,
            "Invalid Zombatar source corpus");
    std::set<std::string> zombatarNames;
    for (auto &row : zombatar["images"])
    {
        auto name = row["image"].get<std::string>();
        Key(name);
        Require(zombatarImages.insert(name).second &&
                    zombatarNames.insert(row["name"].get<std::string>()).second &&
                    row["auto_alpha"].is_boolean(),
                "Duplicate Zombatar source or name");
        ImageDescriptor d;
        d.path = name;
        d.flags = row["auto_alpha"].get<bool>() ? 1024 : 0;
        sourceImages[name] = d;
    }
    ApplyHighResolutionImages(root, sourceImages);
    // Resolution is an offline asset policy. A deferred module can share source
    // art with a completed module without forcing either module's texture size.
    std::map<std::string,std::map<unsigned,std::string>> originalDefinitionNames;
    if(fs::exists(hdManifest))
    {
        const auto deferred=ReadJson(hdManifest).value("originalDefinitions",std::set<std::string>{});
        for(auto &[rootName,metadata]:reanimMetadata.items())
        {
            if(!deferred.contains(Lower(rootName)))continue;
            std::map<std::string,std::string> aliases;
            for(auto &variant:metadata["variants"])
            {
                for(auto &track:variant["images"])
                    for(auto &image:track)
                    {
                        if(image.is_null())continue;
                        const auto original=image.get<std::string>();
                        const auto alias="ORIGINAL_RESOLUTION_"+original;
                        auto descriptor=sourceImages.at(original);
                        descriptor.highResolution.clear();
                        sourceImages[alias]=descriptor;
                        aliases[original]=alias;
                        image=alias;
                    }
                const auto member="compiled/reanim/"+Lower(variant["name"].get<std::string>())+".compiled";
                virtualVariants[member]["variant"]=variant;
            }
            auto names=sourceNames.at("reanim");
            for(auto &[id,symbol]:names)
                if(aliases.contains(symbol))symbol=aliases.at(symbol);
            originalDefinitionNames[rootName]=std::move(names);
        }
    }
    const std::string projectilePath = "compiled/projectiles/projectiles.compiled",
                      zombatarPath = "compiled/zombatar/zombatar.compiled";
    records[projectilePath] = {};
    std::set<std::string> definitionNames = {zombatarPath, projectilePath};
    for (auto &[name, data] : records)
        if (name.starts_with("compiled/reanim/") || name.starts_with("compiled/particles/"))
            definitionNames.insert(name);
    for (auto &[name, row] : virtualVariants.items())
        definitionNames.insert(name);
    for (auto &[name, row] : scenes.items())
        definitionNames.insert(name);
    std::map<std::string, std::vector<std::string>> definitionSymbols;
    std::map<std::string, Image> images;
    for (auto &name : definitionNames)
    {
        std::set<std::string> symbols;
        if (scenes.contains(name))
            for (auto &row : scenes[name]["sprites"])
                symbols.insert(row["source"].get<std::string>());
        else if (name == zombatarPath)
            symbols = zombatarImages;
        else if (name == projectilePath)
            symbols = projectileImages;
        else if (virtualVariants.contains(name))
            symbols = SourceSymbols(virtualVariants[name]["variant"]);
        else
        {
            auto kind = name.starts_with("compiled/reanim/") ? "reanim" : "particles";
            const auto &source = records.at(name);
            for (auto offset : ImageOffsets(kind, std::span(source).subspan(12)))
            {
                auto handle = GetWord(source, offset + 12);
                if (handle)
                    symbols.insert(sourceNames.at(kind).at(handle));
            }
        }
        for (auto &symbol : symbols)
            if (!images.contains(symbol))
                images[symbol] = DecodeImage(sourceImages.at(symbol), rawFiles);
        definitionSymbols[name] = {symbols.begin(), symbols.end()};
    }
    std::vector<Unit> units;
    std::set<std::string> grouped;
    for (auto &[name, members] : ResolveGroups(config, definitionNames))
    {
        Unit unit;
        unit.name = name;
        unit.path = "compiled/atlases/" + name + ".atlas.png";
        unit.members = members;
        unit.settings = config.settings.contains(name) ? config.settings.at(name) : config.defaults;
        units.push_back(unit);
        grouped.insert(members.begin(), members.end());
    }
    if (!grouped.contains("compiled/scene/commonui.compiled"))
    {
        Require(!config.groups.contains("common_ui"), "common_ui group name is reserved");
        units.push_back({"common_ui",
                         "compiled/atlases/common_ui.atlas.png",
                         {"compiled/scene/commonui.compiled"},
                         {},
                         config.defaults});
        grouped.insert("compiled/scene/commonui.compiled");
    }
    for (auto &name : definitionNames)
        if (!grouped.contains(name))
            units.push_back({name, "", {name}, {}, config.defaults});
    std::vector<Json> batches;
    for (auto &unit : units)
    {
        std::set<std::string> symbols;
        for (auto &member : unit.members)
            symbols.insert(definitionSymbols.at(member).begin(), definitionSymbols.at(member).end());
        unit.symbols = {symbols.begin(), symbols.end()};
        Json sizes = Json::array();
        for (auto &name : unit.symbols)
            sizes.push_back({images.at(name).width, images.at(name).height});
        batches.push_back(sizes);
    }
    auto layouts = NativeLayouts(compiler, batches, work);
    auto started = std::chrono::steady_clock::now();
    auto elapsed = [&] {
        return std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
    };
    auto progressPath = cache / "atlas-proof-progress.json";
    for (unsigned index = 0; index < units.size(); ++index)
    {
        auto &unit = units[index];
        std::cout << "Solving atlas " << index + 1 << '/' << units.size() << ": " << unit.name << " ("
                  << unit.symbols.size() << " images)" << std::endl;
        try
        {
            std::mutex mutex;
            auto heartbeat = [&] {
                std::lock_guard lock(mutex);
                Publish(progressPath,
                        Json{{"atlas", unit.name},
                             {"index", index + 1},
                             {"total", units.size()},
                             {"phase", "solving"},
                             {"elapsed_seconds", elapsed()},
                             {"require_optimal", unit.settings.requireOptimal},
                             {"time_limit_seconds",
                              unit.settings.seconds ? Json(unit.settings.seconds) : Json(nullptr)},
                             {"publication", "previous package and generated headers retained"}}
                                .dump(2) +
                            "\n");
            };
            heartbeat();
            std::jthread pulse([&](std::stop_token stop) {
                std::mutex timerMutex;
                std::unique_lock timerLock(timerMutex);
                std::condition_variable_any timer;
                while (!stop.stop_requested())
                {
                    timer.wait_for(timerLock, stop, std::chrono::seconds(1), [] { return false; });
                    if (!stop.stop_requested())
                        heartbeat();
                }
            });
            layouts[index] = ExactLayout(batches[index], layouts[index], unit.settings,
                                         cache / "atlas-layout-cache", definitions / "atlas-cache");
            const auto &exact = layouts[index]["exact"];
            Require(unit.settings.fast || exact["optimal"].get<bool>() ||
                        (!unit.settings.requireOptimal && unit.settings.seconds > 0 &&
                         (exact["status"] == "FEASIBLE" || exact["status"] == "UNKNOWN")),
                    "Atlas violates publication policy");
            std::cout << "Atlas result " << unit.name << ": " << exact["status"]
                      << ", area=" << exact["objective_area"] << ", bound=" << exact["best_bound"]
                      << ", gap=" << exact["gap"] << std::endl;
        }
        catch (const std::exception &error)
        {
            Publish(progressPath, Json{{"atlas", unit.name},
                                       {"phase", "failed"},
                                       {"error", error.what()},
                                       {"elapsed_seconds", elapsed()},
                                       {"publication", "previous package and generated headers retained"}}
                                          .dump(2) +
                                      "\n");
            throw;
        }
    }
    Json reports = Json::object(), sceneReports = Json::object(), variantLayouts = Json::object(),
         variantReports = Json::object();
    for (unsigned index = 0; index < units.size(); ++index)
    {
        const auto &unit = units[index];
        const auto &layout = layouts[index];
        auto path = unit.path.empty() ? unit.members[0].substr(0, unit.members[0].size() - 9) + ".atlas.png"
                                      : unit.path;
        if (!unit.symbols.empty())
        {
            std::vector<const Image *> sources;
            for (auto &name : unit.symbols)
                sources.push_back(&images.at(name));
            records[path] = EncodePng(PackImages(sources, layout));
        }
        for (auto &name : unit.members)
        {
            if (scenes.contains(name))
            {
                sceneReports[name] = CompileScene(scenes[name], layout, unit.symbols, path, images);
                continue;
            }
            if (virtualVariants.contains(name))
            {
                variantLayouts[name] = {{"layout", layout}, {"symbols", unit.symbols}};
                auto report = GeometryReport(definitionSymbols.at(name), layout, unit.symbols, images,
                                             sourceImages, path, !unit.path.empty());
                report["name"] = virtualVariants[name]["variant"]["name"];
                variantReports[name] = report;
                continue;
            }
            if (name == zombatarPath || name == projectilePath)
            {
                std::vector<std::string> order = definitionSymbols.at(name);
                if (name == zombatarPath)
                {
                    order.clear();
                    for (auto &row : zombatar["images"])
                        order.push_back(row["image"].get<std::string>());
                }
                auto report = GeometryReport(order, layout, unit.symbols, images, sourceImages, path, true);
                if (name == zombatarPath)
                    for (unsigned i = 0; i < 209; ++i)
                        report["images"][i]["name"] = zombatar["images"][i]["name"];
                else
                {
                    records[name] = CompileProjectiles(projectiles, layout, unit.symbols, images);
                    Reader p(records[name]);
                    p.Skip(20);
                    Json rows = Json::array(), shadows = Json::array();
                    for (auto row : projectiles["projectiles"])
                    {
                        int x = p.Int(), y = p.Int(), w = p.Int(), h = p.Int();
                        p.Float();
                        row["rect"] = {x, y, w, h};
                        rows.push_back(row);
                    }
                    for (auto row : projectiles["shadows"])
                    {
                        int x = p.Int(), y = p.Int(), w = p.Int(), h = p.Int();
                        shadows.push_back({{"name", row["name"]}, {"rect", {x, y, w, h}}});
                    }
                    report["projectiles"] = rows;
                    report["shadows"] = shadows;
                }
                reports[name] = report;
                continue;
            }
            auto result = CompileDefinition(
                name, records.at(name), layout, unit.symbols,
                sourceNames.at(name.starts_with("compiled/reanim/") ? "reanim" : "particles"), sourceImages,
                path, images);
            result.atlas["atlas_shared"] =
                !unit.path.empty() && !result.atlas["atlas_path"].get<std::string>().empty();
            records[name] = std::move(result.bytes);
            reports[name] = std::move(result.atlas);
        }
    }
    for (auto &[rootName, metadata] : reanimMetadata.items())
    {
        auto name = "compiled/reanim/" + Lower(rootName) + ".compiled";
        Json variants = Json::array(), rows = Json::array();
        for (auto &variant : metadata["variants"])
        {
            auto member = "compiled/reanim/" + Lower(variant["name"].get<std::string>()) + ".compiled";
            variants.push_back(variantLayouts.at(member));
            rows.push_back(variantReports.at(member));
        }
        auto result = ReanimVariants(records.at(name), metadata, variants, actions.value(rootName, Json()),
                                     originalDefinitionNames.contains(rootName)?originalDefinitionNames.at(rootName):sourceNames.at("reanim"), sourceImages);
        records[name] = std::move(result.bytes);
        auto report = rows[0];
        report.update(result.atlas);
        report["variants"] = rows;
        reports[name] = report;
    }
    for (auto &[name, metadata] : particleMetadata.items())
    {
        if (!records.contains(name))
            continue;
        std::vector<Definition> variants;
        Json rows = Json::array(), names = Json::array();
        for (auto &variant : metadata["variants"])
        {
            auto member = variant["path"].get<std::string>();
            variants.push_back({records.at(member), {}});
            auto row = reports.at(member);
            row["name"] = variant["name"];
            row["path"] = member;
            rows.push_back(row);
            names.push_back(variant["name"]);
        }
        auto result = ParticleVariants(records.at(name), variants);
        records[name] = std::move(result.bytes);
        auto report = rows[0];
        report.update(result.atlas);
        report["variants"] = rows;
        report["variant_names"] = names;
        reports[name] = report;
        for (unsigned v = 1; v < metadata["variants"].size(); ++v)
        {
            auto member = metadata["variants"][v]["path"].get<std::string>();
            records.erase(member);
            reports.erase(member);
        }
    }
    auto native = GenerateDefinitions(config, records, reports, staged);
    Insert(native.files, GenerateZombatar(zombatar, reports.at(zombatarPath)));
    Insert(native.files, GenerateScenes(sceneReports));
    std::set<std::string> runtimeRoots = {projectilePath, zombatarPath},
                          runtimeAtlases = native.summary["atlases"].get<std::set<std::string>>();
    runtimeAtlases.insert(reports[zombatarPath]["atlas_path"].get<std::string>());
    for (auto &row : sceneReports)
        runtimeAtlases.insert(row["atlas_path"].get<std::string>());
    for (auto kind : {"reanim", "particle"})
    {
        auto directory = std::string(kind) == "reanim" ? "reanim" : "particles";
        unsigned index = 0;
        for (auto &[id, name] : config.order.at(kind))
        {
            auto path = "compiled/" + std::string(directory) + "/" + Lower(name) + ".compiled";
            runtimeRoots.insert(path);
            auto atlas = ParseAtlas(records.at(path));
            reports[path]["definition_id"] = index++;
            reports[path]["slot_count"] = atlas["slot_count"];
            for (unsigned v = 0; v < reports[path]["variants"].size(); ++v)
                reports[path]["variants"][v]["frame_rectangles"] = atlas["variant_rectangles"][v];
        }
    }
    for (auto i = reports.begin(); i != reports.end();)
        if (!runtimeRoots.contains(i.key()))
            i = reports.erase(i);
        else
            ++i;
    auto bindings = Bindings(reports);
    native.files["DefinitionImageBindings.h"] = Bytes(bindings.begin(), bindings.end());
    auto masks =
        AttacherMasks(Text(work / "reanim/ReanimTrackMphfGlobal.h"), Text(work / "reanim/ReanimTrackId.h"));
    native.files["ReanimAttacherMasks.h"] = Bytes(masks.begin(), masks.end());
    Insert(native.files, ResourceHeaders(plan));
    Insert(native.files, deferred);
    for (auto i = records.begin(); i != records.end();)
        if (((i->first.starts_with("compiled/reanim/") || i->first.starts_with("compiled/particles/")) &&
             i->first.ends_with(".compiled")) ||
            i->first == projectilePath ||
            (i->first.ends_with(".atlas.png") && !runtimeAtlases.contains(i->first)))
            i = records.erase(i);
        else
            ++i;
    Insert(records, native.runtime);
    records["compiled/resources.compiled"] = plan.record;
    std::set<std::string> available, selected;
    for (auto &[name, path] : rawFiles)
        available.insert(name);
    for (auto &[name, data] : records)
        available.insert(name);
    auto retain = [&](const std::string &kind, const std::string &path) {
        auto files = Physical(kind, path, available);
        selected.insert(files.begin(), files.end());
    };
    for (auto &font : corpus.fonts)
        for (auto &layer : font["layers"])
            retain("image", layer["image"].get<std::string>());
    for (auto &group : plan.groups)
        for (auto &record : group["records"])
        {
            auto bytes = record["data"].get_binary();
            Reader reader(bytes);
            auto kind = reader.Word(), flags = reader.Word();
            reader.Skip(8);
            std::array<std::string, 4> paths;
            for (auto &path : paths)
                path = reader.String();
            if (flags & (1 | 4))
                continue;
            if (kind == 0)
            {
                retain("image", paths[0]);
                for (unsigned i = 1; i < 3; ++i)
                    if (!paths[i].empty())
                        retain("image", paths[i]);
            }
            else if (kind == 1)
                retain("sound", paths[0]);
            else if (!paths[3].empty())
                retain("image", paths[3]);
        }
    for (auto &entry : fs::recursive_directory_iterator(root / "pvzPerf"))
    {
        if (!entry.is_regular_file())
            continue;
        auto path = entry.path();
        if (path.extension() != ".cpp" && path.extension() != ".h" && path.extension() != ".hpp" &&
            path.extension() != ".inl")
            continue;
        bool generatedFile = false;
        for (auto &part : path)
            generatedFile |= part == "generated";
        if (generatedFile)
            continue;
        for (auto literal : StringLiterals(Text(path)))
        {
            std::replace(literal.begin(), literal.end(), '\\', '/');
            auto normalized = Lower(literal);
            if (available.contains(normalized))
                selected.insert(normalized);
            else if (normalized.starts_with("images/") || normalized.starts_with("reanim/") ||
                     normalized.starts_with("particles/") || normalized.starts_with("data/"))
            {
                try
                {
                    retain("image", normalized);
                }
                catch (const std::exception &)
                {
                }
            }
            else if (normalized.starts_with("sounds/"))
            {
                try
                {
                    retain("sound", normalized);
                }
                catch (const std::exception &)
                {
                }
            }
        }
    }
    const std::set<std::string> media = {".png", ".jpg", ".jpeg", ".gif", ".bmp",
                                         ".ogg", ".wav", ".au",   ".mo3"};
    for (auto &name : available)
        if (!media.contains(Lower(fs::path(name).extension().string())) &&
            name != "properties/lawnstrings.txt")
            selected.insert(name);
    for (auto &name : selected)
        if (rawFiles.contains(name))
            records[name] = Read(rawFiles.at(name));
    Json files = Json::object();
    for (auto &[name, data] : records)
        files[name] = {{"bytes", data.size()}, {"sha256", Sha256(data)}};
    std::set<std::string> omitted, retired;
    for (auto &[name, path] : rawFiles)
        if (!selected.contains(name))
            omitted.insert(name);
    std::set_difference(plan.declared.begin(), plan.declared.end(), plan.live.begin(), plan.live.end(),
                        std::inserter(retired, retired.end()));
    Json groups = Json::object();
    unsigned long long atlasBytes = 0, sourceBytes = 0, paddedBytes = 0, shelfBytes = 0, attempts = 0,
                       micros = 0;
    std::map<std::tuple<std::string, unsigned, unsigned>, unsigned long long> unique, overlap;
    for (unsigned i = 0; i < units.size(); ++i)
    {
        const auto &unit = units[i];
        const auto &layout = layouts[i];
        auto path = unit.path.empty() ? unit.members[0].substr(0, unit.members[0].size() - 9) + ".atlas.png"
                                      : unit.path;
        if (!runtimeAtlases.contains(path))
            continue;
        groups[unit.name] = {
            {"members", unit.members},
            {"path", unit.path},
            {"images", unit.symbols.size()},
            {"width", layout["width"]},
            {"height", layout["height"]},
            {"solver", unit.settings.fast ? "maxrects" : "exact"},
            {"time_limit_seconds", unit.settings.seconds ? Json(unit.settings.seconds) : Json(nullptr)},
            {"require_optimal", unit.settings.requireOptimal},
            {"exact", layout["exact"]}};
        atlasBytes +=
            layout["width"].get<unsigned long long>() * layout["height"].get<unsigned long long>() * 4;
        shelfBytes += layout["shelf_area"].get<unsigned long long>() * 4;
        attempts += layout["search_attempts"].get<unsigned long long>();
        micros += layout["search_microseconds"].get<unsigned long long>();
        for (auto &symbol : unit.symbols)
        {
            const auto &image = images.at(symbol);
            sourceBytes += image.rgba.size();
            paddedBytes += (image.width + 4ULL) * (image.height + 4) * 4;
        }
    }
    for (auto &collection : {reports, sceneReports})
        for (auto &report : collection)
        {
            auto rows = report.contains("variants") ? report["variants"] : Json::array({report});
            for (auto &row : rows)
                for (auto &image : row["images"])
                {
                    auto key =
                        std::make_tuple(image["rgba_sha256"].get<std::string>(),
                                        image["width"].get<unsigned>(), image["height"].get<unsigned>());
                    unique[key] = image["rgba_bytes"].get<unsigned long long>();
                    if (plan.live.contains(image["source"].get<std::string>()))
                        overlap[key] = unique[key];
                }
        }
    unsigned long long uniqueBytes = 0, overlapBytes = 0;
    for (auto &[key, value] : unique)
        uniqueBytes += value;
    for (auto &[key, value] : overlap)
        overlapBytes += value;
    native.summary["zombatar_images"] = 209;
    native.summary["atlases"] = runtimeAtlases;
    for (auto &[name, data] : native.files)
        native.summary["generated_files"][name] = {{"bytes", data.size()}, {"sha256", Sha256(data)}};
    Json report = {{"format", "pvz-resource-build-static-v1"},
                   {"definition_source_format", "IDTF-IDRF-float-v1"},
                   {"numeric_counts",
                    {{"reanim", 0},
                     {"particles", 0},
                     {"fonts", corpus.fonts.size()},
                     {"resources", 1},
                     {"projectiles", 0}}},
                   {"code_images", plan.live},
                   {"retained_program_images", plan.live},
                   {"runtime_inputs", rawFiles.size()},
                   {"runtime_included", rawFiles.size() - omitted.size()},
                   {"runtime_omitted", omitted},
                   {"retired_images", retired},
                   {"definition_atlases", reports},
                   {"scene_atlases", sceneReports},
                   {"static_definitions", native.summary},
                   {"build_config", configuration.lexically_relative(root).generic_string()},
                   {"atlas_groups", groups},
                   {"atlas_memory",
                    {{"rgba_bytes", atlasBytes},
                     {"source_occurrence_rgba_bytes", sourceBytes},
                     {"unique_source_rgba_bytes", uniqueBytes},
                     {"duplicate_source_rgba_bytes", sourceBytes - uniqueBytes},
                     {"source_pixels_also_retained_globally_rgba_bytes", overlapBytes},
                     {"padding_and_unused_rgba_bytes", atlasBytes - sourceBytes},
                     {"extrusion_rgba_bytes", paddedBytes - sourceBytes},
                     {"unused_gap_rgba_bytes", atlasBytes - paddedBytes},
                     {"padded_region_occupancy", atlasBytes ? double(paddedBytes) / atlasBytes : 1.0},
                     {"definition_atlas_count", runtimeAtlases.size()},
                     {"packing_shelf_rgba_bytes", shelfBytes},
                     {"packing_saved_rgba_bytes", shelfBytes - atlasBytes},
                     {"packing_search_attempts", attempts},
                     {"packing_search_microseconds", micros}}},
                   {"files", files}};
    // No game source or package is published until every definition, atlas,
    // static table and physical reference has passed validation above.
    Zip(cache / "main.pak64", records);
    for (auto &[name, data] : native.files)
        Publish(generated / name, data);
    Publish(cache / "manifest.json", report.dump(2) + "\n");
    Publish(cache / "resources.stamp", "complete\n");
    // Unlike generated sources, the stamp records successful execution even
    // when its contents are unchanged. Otherwise newer compiler/input files
    // keep the multi-output Ninja rule dirty on every subsequent build.
    fs::last_write_time(cache / "resources.stamp", fs::file_time_type::clock::now());
    Json unproven = Json::array();
    for (unsigned i = 0; i < units.size(); ++i)
        if (!layouts[i]["exact"]["optimal"].get<bool>())
            unproven.push_back(units[i].name);
    Publish(progressPath, Json{{"phase", "published"},
                               {"total", units.size()},
                               {"elapsed_seconds", elapsed()},
                               {"publication", "all configured atlas policies passed"},
                               {"unproven_atlases", unproven}}
                                  .dump(2) +
                              "\n");
    std::cout << "Resource package: " << (cache / "main.pak64").string() << "; " << records.size()
              << " entries; omitted " << omitted.size() << " inputs" << std::endl;
}
} // namespace pvz::build
