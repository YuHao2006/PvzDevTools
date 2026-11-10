#include "NativeBuild.h"
#include <algorithm>
#include <cmath>
#include <regex>

namespace pvz::build
{
namespace
{
const std::vector<std::string> modules = {"Magnet",    "Almanac",     "Achievements",
                                          "DesktopUI", "LandscapeUI", "CommonUI"};
std::string ScenePath(const std::string &name)
{
    return "compiled/scene/" + Lower(name) + ".compiled";
}
Records FilesFrom(const std::map<std::string, std::string> &sources)
{
    Records out;
    for (auto &[name, text] : sources)
        out[name] = Bytes(text.begin(), text.end());
    return out;
}
std::string Number(const Json &value)
{
    return std::to_string(value.get<unsigned>());
}
} // namespace

Json SceneParse(std::string_view text)
{
    std::string source(text);
    std::regex token(
        R"xxx(\s+|//[^\n]*|\#[^\n]*|/\*[\s\S]*?\*/|"(?:\\.|[^"\\])*"|[A-Za-z_]\w*|-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[{}:;,\[\]])xxx");
    std::vector<std::string> tokens;
    std::size_t at = 0;
    while (at < source.size())
    {
        std::smatch match;
        auto tail = source.substr(at);
        Require(std::regex_search(tail, match, token, std::regex_constants::match_continuous),
                "Invalid scene DSL at " + std::to_string(at));
        auto value = match.str();
        at += value.size();
        if (std::isspace(static_cast<unsigned char>(value[0])) || value.starts_with("//") ||
            value.starts_with("/*") || value.starts_with('#'))
            continue;
        tokens.push_back(value);
    }
    at = 0;
    auto take = [&](const std::string &expected = "") {
        Require(at < tokens.size(), "Truncated scene DSL");
        auto value = tokens[at++];
        Require(expected.empty() || value == expected, "Expected " + expected + ", got " + value);
        return value;
    };
    take("export");
    take("atlas");
    auto name = take();
    Require(std::find(modules.begin(), modules.end() - 1, name) != modules.end() - 1,
            "Unsupported scene module " + name);
    take("{");
    Json sprites = Json::array();
    std::set<std::string> names;
    while (at < tokens.size() && tokens[at] != "}")
    {
        take("sprite");
        auto sprite = take();
        if (sprite.starts_with("IMAGE_"))
            sprite.erase(0, 6);
        Require(std::regex_match(sprite, std::regex("[A-Za-z_]\\w*")) && sprite != "Count" &&
                    sprite != "None" && names.insert(sprite).second,
                "Invalid scene sprite " + sprite);
        take("{");
        Json row = {{"name", sprite}};
        while (at < tokens.size() && tokens[at] != "}")
        {
            auto key = take();
            Require((key == "image" || key == "rect" || key == "scale") && !row.contains(key),
                    "Invalid scene field " + key);
            take(":");
            if (key == "image")
            {
                auto value = take();
                Require(value.starts_with('"'), "Scene image must be quoted");
                row[key] = Json::parse(value);
            }
            else if (key == "rect")
            {
                take("[");
                Json rect = Json::array();
                for (int i = 0; i < 4; ++i)
                {
                    auto value = take();
                    Require(std::regex_match(value, std::regex("-?\\d+")), "Scene rect requires integers");
                    rect.push_back(std::stoi(value));
                    if (i != 3)
                        take(",");
                }
                take("]");
                Require(rect[0].get<int>() >= 0 && rect[1].get<int>() >= 0 && rect[2].get<int>() > 0 &&
                            rect[3].get<int>() > 0,
                        "Invalid scene rectangle");
                row[key] = rect;
            }
            else
            {
                auto value = std::stod(take());
                Require(std::isfinite(value) && value > 0, "Invalid scene scale");
                row[key] = value;
            }
            take(";");
        }
        take("}");
        Require(row.contains("image") && !row["image"].get<std::string>().empty(),
                "Scene sprite has no image");
        if (!row.contains("scale"))
            row["scale"] = 1.0;
        Require(name == "Magnet" || row["scale"].get<double>() == 1.0, "UI images must not be scaled");
        auto image = row["image"].get<std::string>();
        if (!image.starts_with("IMAGE_"))
        {
            Key(image);
            auto ext = Lower(fs::path(image).extension().string());
            Require(ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".gif" || ext == ".bmp",
                    "Scene image must name an image file");
        }
        sprites.push_back(row);
    }
    take("}");
    Require(at == tokens.size() && !sprites.empty() && sprites.size() < 65536, "Empty or trailing scene DSL");
    return {{"name", name}, {"sprites", sprites}};
}

Json CommonUI()
{
    Json sprites = Json::array();
    for (auto name : {"BUTTON_LEFT",
                      "BUTTON_MIDDLE",
                      "BUTTON_RIGHT",
                      "BUTTON_DOWN_LEFT",
                      "BUTTON_DOWN_MIDDLE",
                      "BUTTON_DOWN_RIGHT",
                      "DIALOG_TOPLEFT",
                      "DIALOG_TOPMIDDLE",
                      "DIALOG_TOPRIGHT",
                      "DIALOG_CENTERLEFT",
                      "DIALOG_CENTERMIDDLE",
                      "DIALOG_CENTERRIGHT",
                      "DIALOG_BOTTOMLEFT",
                      "DIALOG_BOTTOMMIDDLE",
                      "DIALOG_BOTTOMRIGHT",
                      "DIALOG_BIGBOTTOMLEFT",
                      "DIALOG_BIGBOTTOMMIDDLE",
                      "DIALOG_BIGBOTTOMRIGHT",
                      "DIALOG_HEADER",
                      "SEEDCHOOSER_BUTTON",
                      "SEEDCHOOSER_BUTTON_DISABLED",
                      "SEEDCHOOSER_BUTTON_GLOW",
                      "SEEDCHOOSER_BUTTON2",
                      "SEEDCHOOSER_BUTTON2_GLOW"})
        sprites.push_back({{"name", name}, {"image", "IMAGE_" + std::string(name)}, {"scale", 1.0}});
    const std::array<std::string, 4> corners = {"TL", "TR", "BL", "BR"};
    const std::array<Json, 4> rectangles = {Json{0, 0, 42, 36}, Json{62, 0, 42, 36}, Json{0, 108, 42, 36},
                                            Json{62, 108, 42, 36}};
    for (unsigned i = 0; i < 4; ++i)
        sprites.push_back({{"name", "TOUCH_FRAME_" + corners[i]},
                           {"image", "IMAGE_TOUCH_SELECTION_FRAME_BLUE"},
                           {"rect", rectangles[i]},
                           {"scale", 1.0}});
    return {{"name", "CommonUI"}, {"sprites", sprites}};
}

Json PrepareScenes(Json documents, const Json &author, const Files &files,
                   std::map<std::string, ImageDescriptor> &sources)
{
    std::set<std::string> labels;
    std::string prefix;
    for (auto &group : author["manifest"]["children"])
        for (auto &node : group["children"])
        {
            const auto &a = node["attrs"];
            if (node["tag"] == "SetDefaults")
            {
                prefix = a.value("idprefix", prefix);
                while (!prefix.empty() && prefix.back() == '/')
                    prefix.pop_back();
            }
            else if (node["tag"] == "Image")
            {
                auto p = a["path"].get<std::string>();
                std::replace(p.begin(), p.end(), '\\', '/');
                labels.insert(prefix + a.value("id", fs::path(p).stem().string()));
            }
        }
    auto authored = ImageDescriptors(author, files, labels);
    std::map<std::string, std::vector<ImageDescriptor>> paths;
    for (auto &[name, d] : authored)
        if (!d.path.empty())
            paths[Key(d.path)].push_back(d);
    auto recipe = [](const ImageDescriptor &d) {
        return std::make_tuple(d.path, d.alpha, d.alphaGrid, d.flags, d.color,d.logicalWidth,d.logicalHeight);
    };
    Json result = Json::object();
    for (auto &document : documents)
    {
        auto module = document["name"].get<std::string>(), path = ScenePath(module);
        Require(std::find(modules.begin(), modules.end(), module) != modules.end() && !result.contains(path),
                "Duplicate scene module");
        for (auto &row : document["sprites"])
        {
            auto image = row["image"].get<std::string>();
            ImageDescriptor d;
            if (image.starts_with("IMAGE_"))
            {
                Require(authored.contains(image), "Unknown scene image label " + image);
                d = authored.at(image);
            }
            else
            {
                auto key = Key(image), stem = fs::path(key).replace_extension().generic_string();
                auto found = paths.find(key);
                if (found == paths.end())
                    found = paths.find(stem);
                if (found != paths.end())
                {
                    d = found->second.front();
                    for (auto &other : found->second)
                        Require(recipe(other) == recipe(d), "Ambiguous scene decode recipe");
                }
                else
                {
                    d.path = image;
                    d.flags = 1024;
                }
            }
            if(row.contains("logical_size"))
            {
                d.logicalWidth=row["logical_size"][0].get<unsigned>();
                d.logicalHeight=row["logical_size"][1].get<unsigned>();
                Require(d.logicalWidth && d.logicalHeight,"Invalid scene logical size");
            }
            Require(!(d.flags & (1 | 8192)) && (d.alphaGrid.empty() || (d.rows == 1 && d.cols == 1)),
                    "Unavailable scene source");
            auto key = "scene:" + Key(d.path);
            d.rows = d.cols = 1;
            Require(!sources.contains(key) || recipe(sources[key]) == recipe(d),
                    "Conflicting scene decode recipe");
            sources[key] = d;
            row["source"] = key;
        }
        result[path] = document;
    }
    return result;
}

Json CompileScene(const Json &document, const Json &layout, const std::vector<std::string> &symbols,
                  std::string_view atlasPath, const std::map<std::string, Image> &images)
{
    std::map<std::string, Json> regions;
    for (unsigned i = 0; i < symbols.size(); ++i)
        regions[symbols[i]] = layout["regions"][i];
    std::set<std::string> sources, used;
    Json rows = Json::array(), sprites = Json::array();
    unsigned sourceBytes = 0, paddedBytes = 0;
    for (auto &row : document["sprites"])
        sources.insert(row["source"].get<std::string>());
    for (auto &source : sources)
    {
        const auto &image = images.at(source);
        rows.push_back({{"source", source},
                        {"width", image.width},
                        {"height", image.height},
                        {"rows", 1},
                        {"cols", 1},
                        {"rgba_bytes", image.rgba.size()},
                        {"rgba_sha256", Sha256(image.rgba)},
                        {"files", image.files},
                        {"region", regions.at(source)}});
        used.insert(image.files.begin(), image.files.end());
        sourceBytes += unsigned(image.rgba.size());
        paddedBytes += (image.width + 4) * (image.height + 4) * 4;
    }
    for (auto row : document["sprites"])
    {
        auto source = row["source"].get<std::string>();
        const auto &image = images.at(source);
        auto rect = row.value("rect", Json{0, 0, image.logicalWidth, image.logicalHeight});
        int x = rect[0], y = rect[1], w = rect[2], h = rect[3];
        Require(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= int(image.logicalWidth) && y + h <= int(image.logicalHeight),
                "Scene sprite outside source " + row["name"].get<std::string>());
        row["source_rect"] = rect;
        const double sx = double(image.width) / image.logicalWidth;
        const double sy = double(image.height) / image.logicalHeight;
        row["region"] = {regions.at(source)[0].get<int>() + x * sx,
                         regions.at(source)[1].get<int>() + y * sy, w * sx, h * sy, w, h};
        sprites.push_back(row);
    }
    return {{"name", document["name"]},
            {"atlas_path", atlasPath},
            {"atlas_width", layout["width"]},
            {"atlas_height", layout["height"]},
            {"atlas_shared", true},
            {"region_count", rows.size()},
            {"source_files", used},
            {"source_rgba_bytes", sourceBytes},
            {"atlas_rgba_bytes", layout["width"].get<unsigned>() * layout["height"].get<unsigned>() * 4},
            {"padded_region_rgba_bytes", paddedBytes},
            {"images", rows},
            {"sprites", sprites},
            {"packing", layout}};
}

Records GenerateScenes(const Json &reports)
{
    std::string header =
        "// Generated scene atlas geometry and direct texture bindings.\n#pragma once\n#include "
        "\"SexyAppFramework/graphics/GpuBackend.h\"\n#include "
        "\"SexyAppFramework/graphics/TextureRegion.h\"\n#include <cstddef>\n#include <cstdint>\n";
    std::string cpp = "#include \"SceneAtlasData.h\"\n#include \"Sexy.TodLib/DefinitionImageRegion.h\"\n";
    std::vector<std::string> extents, sheets;
    for (auto &module : modules)
    {
        const auto &report = reports.at(ScenePath(module));
        const auto &rows = report["sprites"];
        int width = report["atlas_width"], height = report["atlas_height"];
        auto space = module + "Atlas";
        header += "namespace " + space +
                  " {\nextern Sexy::GpuTexture* Texture;\nenum class Sprite : std::uint16_t {\n";
        for (unsigned i = 0; i < rows.size(); ++i)
            header += "    " + rows[i]["name"].get<std::string>() + " = " + std::to_string(i) + ",\n";
        header += "    Count = " + std::to_string(rows.size()) +
                  ",\n};\ninline constexpr std::size_t Count = " + std::to_string(rows.size()) +
                  ";\nextern const Sexy::TextureRegion Images[Count];\n";
        cpp += "namespace " + space +
               " {\nSexy::GpuTexture* Texture = nullptr;\nconst Sexy::TextureRegion Images[Count] = {\n";
        for (auto &row : rows)
            cpp += "    " + Region(row["region"], width, height) + ",\n";
        cpp += "};\n";
        if(module=="CommonUI")
        {
            std::map<unsigned,Json> cards;
            for(const auto &row:rows)
            {
                const auto name=row["name"].get<std::string>();
                if(name.starts_with("SEED_CARD_"))cards[std::stoul(name.substr(10))]=row["region"];
            }
            header+="inline constexpr bool HasSeedCards="+std::string(cards.empty()?"false":"true")+";\n";
            const unsigned cardCount=cards.empty()?53u:std::max(53u,cards.rbegin()->first+1);
            header+="extern const Sexy::TextureRegion SeedCards["+std::to_string(cardCount)+"];\n";
            cpp+="const Sexy::TextureRegion SeedCards["+std::to_string(cardCount)+"] = {\n";
            for(unsigned i=0;i<cardCount;++i)
                cpp+="    "+Region(cards.contains(i)?cards.at(i):Json{0,0,0,0},width,height)+",\n";
            cpp+="};\n";
        }
        if (module == "Achievements")
        {
            std::map<unsigned, Json> icons;
            std::regex pattern("ICON_(\\d+)");
            for (auto &row : rows)
            {
                auto name = row["name"].get<std::string>();
                std::smatch m;
                if (std::regex_match(name, m, pattern))
                    Require(icons.emplace(std::stoul(m[1]), row).second, "Duplicate achievement icon");
            }
            header += "inline constexpr std::size_t IconCount = " + std::to_string(icons.size()) + ";\n";
            if (!icons.empty())
            {
                header += "extern const Sexy::TextureRegion Icons[IconCount];\n";
                cpp += "const Sexy::TextureRegion Icons[IconCount] = {\n";
                for (unsigned i = 0; i < icons.size(); ++i)
                {
                    Require(icons.contains(i), "Noncontiguous achievement icons");
                    cpp += "    " + Region(icons.at(i)["region"], width, height) + ",\n";
                }
                cpp += "};\n";
            }
        }
        if (std::any_of(rows.begin(), rows.end(),
                        [](const Json &row) { return row["scale"].get<double>() != 1.0; }))
        {
            header += "extern const float Scales[Count];\n";
            cpp += "const float Scales[Count] = {\n    ";
            bool first = true;
            for (auto &row : rows)
            {
                if (!first)
                    cpp += ", ";
                first = false;
                cpp += CppFloat(row["scale"]);
            }
            cpp += "\n};\n";
        }
        header += "void Load();\nvoid Free();\n}\n";
        auto path = report["atlas_path"].get<std::string>();
        cpp += "void Load() {\n    DefinitionBindAtlasPath(Texture, " + std::to_string(width) + ", " +
               std::to_string(height) + ", " + CppString(path) +
               ");\n}\nvoid Free() { DefinitionUnbindAtlas(Texture); }\n}\n";
        extents.push_back("    {" + CppString(path) + ", " + std::to_string(width) + "u, " +
                          std::to_string(height) + "u},\n");
        for (auto &image : report["images"])
        {
            std::string row =
                "    {" + CppString(path) + ", " + CppString(image["source"].get<std::string>());
            for (auto &v : image["region"])
                row += ", " + Number(v) + "u";
            row += ", " + CppString(image["rgba_sha256"].get<std::string>()) + "},\n";
            sheets.push_back(row);
        }
    }
    header += "void LoadSceneAtlases(bool landscapeUI);\nvoid FreeSceneAtlases();\n";
    cpp += "void LoadSceneAtlases(bool landscapeUI) {\n";
    for (auto &module : modules)
        if (module != "DesktopUI" && module != "LandscapeUI")
            cpp += "    " + module + "Atlas::Load();\n";
    cpp += "    if (landscapeUI) LandscapeUIAtlas::Load();\n    else DesktopUIAtlas::Load();\n}\nvoid "
           "FreeSceneAtlases() {\n";
    for (auto i = modules.rbegin(); i != modules.rend(); ++i)
        cpp += "    " + *i + "Atlas::Free();\n";
    cpp += "}\n";
    std::string audit = "// Offline scene extent audit; not included by the game.\n#pragma once\n#include "
                        "<array>\nnamespace SceneAtlasBindings {\nstruct Extent { const char* image; "
                        "unsigned width,height; };\ninline constexpr std::array<Extent, " +
                        std::to_string(extents.size()) + "> Extents{{\n";
    for (auto &row : extents)
        audit += row;
    audit += "}};\nstruct Sheet { const char* image; const char* source; unsigned x,y,width,height; const "
             "char* rgbaSha256; };\ninline constexpr std::array<Sheet, " +
             std::to_string(sheets.size()) + "> Sheets{{\n";
    for (auto &row : sheets)
        audit += row;
    audit += "}};\n}\n";
    return FilesFrom(
        {{"SceneAtlasData.h", header}, {"SceneAtlasData.cpp", cpp}, {"SceneAtlasBindings.h", audit}});
}

Records GenerateZombatar(const Json &document, const Json &report)
{
    Require(document["images"].size() == 209 && report["images"].size() == 209,
            "Expected 209 Zombatar images");
    std::string header = "// Generated from authored Zombatar sources. Do not edit.\n#pragma once\n#include "
                         "<cstddef>\n#include <cstdint>\nenum class ZombatarImage : std::uint16_t {\n";
    for (unsigned i = 0; i < 209; ++i)
    {
        Require(document["images"][i]["name"] == report["images"][i]["name"], "Zombatar enum order differs");
        header +=
            "    " + document["images"][i]["name"].get<std::string>() + " = " + std::to_string(i) + ",\n";
    }
    header += "    Count = 209,\n    None = Count\n};\n";
    int width = report["atlas_width"], height = report["atlas_height"];
    std::string cpp =
        "// Generated static Zombatar atlas and image ownership bindings. Do not edit.\n#include "
        "\"Lawn/Widget/ZombatarResources.h\"\n#include \"Sexy.TodLib/DefinitionImageRegion.h\"\nconstinit "
        "ZombatarAtlasDefinition gZombatarAtlas {\n    {{\n";
    for (auto &row : report["images"])
    {
        Json r = Json::array();
        for (unsigned i = 0; i < 4; ++i)
            r.push_back(row["region"][i]);
        cpp += "        " + Region(r, width, height) + ",\n";
    }
    cpp += "    }}, nullptr\n};\nstatic_assert(static_cast<std::size_t>(ZombatarImage::Count) == 209);\nvoid "
           "ZombatarLoadResources() {\n    DefinitionBindAtlasPath(gZombatarAtlas.mTexture, " +
           std::to_string(width) + ", " + std::to_string(height) + ", " +
           CppString(report["atlas_path"].get<std::string>()) +
           ");\n}\nvoid ZombatarFreeResources() {\n    DefinitionUnbindAtlas(gZombatarAtlas.mTexture);\n}\n";
    return FilesFrom({{"ZombatarImage.h", header}, {"ZombatarAtlasData.cpp", cpp}});
}

Bytes CompileProjectiles(const Json &document, const Json &layout, const std::vector<std::string> &symbols,
                         const std::map<std::string, Image> &images)
{
    Require(document["format"] == "pvz-projectile-sources-v1" && document["projectiles"].size() == 14 &&
                document["shadows"].size() == 2,
            "Invalid projectile document");
    std::map<std::string, Json> regions;
    for (unsigned i = 0; i < symbols.size(); ++i)
        regions[symbols[i]] = layout["regions"][i];
    Bytes bytes;
    for (unsigned v :
         {0x314a5250u, 1u, 14u, layout["width"].get<unsigned>(), layout["height"].get<unsigned>()})
        Word(bytes, v);
    for (auto &row : document["projectiles"])
    {
        auto rect = row["image"].is_null() ? Json{0, 0, 0, 0} : regions.at(row["image"].get<std::string>());
        for (auto &v : rect)
            Integer(bytes, v.get<int>());
        auto scale = row["scale"].get<float>();
        Require(std::isfinite(scale) && scale > 0, "Invalid projectile scale");
        Float(bytes, scale);
    }
    for (auto &row : document["shadows"])
    {
        auto r = regions.at(row["image"].get<std::string>()), rect = row["rect"];
        const auto &image = images.at(row["image"].get<std::string>());
        int x = rect[0], y = rect[1], w = rect[2], h = rect[3];
        Require(x >= 0 && y >= 0 && w > 0 && h > 0 && x + w <= int(image.logicalWidth) && y + h <= int(image.logicalHeight),
                "Projectile shadow outside source");
        Integer(bytes, r[0].get<int>() + int(x * 1ULL * image.width / image.logicalWidth));
        Integer(bytes, r[1].get<int>() + int(y * 1ULL * image.height / image.logicalHeight));
        Integer(bytes, int(w * 1ULL * image.width / image.logicalWidth));
        Integer(bytes, int(h * 1ULL * image.height / image.logicalHeight));
    }
    return bytes;
}
} // namespace pvz::build
