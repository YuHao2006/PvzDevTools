#include "NativeBuild.h"
#include <algorithm>
#include <regex>
#include <sstream>
#include <iomanip>

namespace pvz::build
{
namespace
{
std::string Path(std::string value)
{
    std::replace(value.begin(), value.end(), '\\', '/');
    return value;
}
std::string StripSlash(std::string value)
{
    while (!value.empty() && value.back() == '/')
        value.pop_back();
    return value;
}
bool Excluded(const std::string &name)
{
    return name.starts_with("IMAGE_QUICKPLAY_") || name == "IMAGE_SELECTORSCREEN_MOREWAYSTOPLAY_BG";
}
std::vector<std::string> Tokens(const std::string &value)
{
    std::vector<std::string> result;
    std::regex token("[^,\\s]+");
    for (auto i = std::sregex_iterator(value.begin(), value.end(), token); i != std::sregex_iterator(); ++i)
        result.push_back(i->str());
    return result;
}
std::string Hex(unsigned value)
{
    std::ostringstream out;
    out << std::uppercase << std::hex << std::setw(8) << std::setfill('0') << value;
    return out.str();
}
Bytes Payload(const Json &groups)
{
    Bytes result;
    for (auto &group : groups)
    {
        Bytes block;
        for (auto &record : group["records"])
        {
            auto &data = record["data"].get_binary();
            Word(block, unsigned(data.size()));
            Append(block, data);
        }
        Word(result, group["group"].get<unsigned>());
        Word(result, unsigned(group["records"].size()));
        Word(result, unsigned(block.size()));
        Append(result, block);
    }
    return result;
}
} // namespace

ResourcePlan CompileResources(const fs::path &root, const Json &author,
                              const std::map<std::string, unsigned> &tags)
{
    Json highResolution;
    const auto hdManifest = root / "assets/high-resolution/images.json";
    if (fs::exists(hdManifest))
        highResolution = ReadJson(hdManifest);
    Require(author.is_object() && author.size() == 2 &&
                author.value("format", "") == "pvz-resources-authoring-v1",
            "Invalid author manifest format");
    auto validate = [](const Json &node, std::string_view tag) {
        Require(node.is_object() && node.size() == 3 && node.contains("tag") && node.contains("attrs") &&
                    node.contains("children") && node["attrs"].is_object() && node["children"].is_array(),
                "Invalid author manifest node");
        Require(tag.empty() || node["tag"].get<std::string>() == tag, "Unexpected author node");
        for (auto &[key, value] : node["attrs"].items())
            Require(value.is_string() && key.find('\0') == key.npos &&
                        value.get<std::string>().find('\0') == std::string::npos,
                    "Invalid author attribute");
    };
    const auto &manifest = author.at("manifest");
    validate(manifest, "ResourceManifest");
    ResourcePlan result;
    result.groups = Json::array();
    auto declarations = CleanComments(Text(root / "pvzPerf/Resources.h"));
    std::regex declaration(R"(\bextern\s+Image\s*\*\s*((?:IMAGE_|FONT_IMAGE_)\w+)\s*;)");
    for (auto i = std::sregex_iterator(declarations.begin(), declarations.end(), declaration);
         i != std::sregex_iterator(); ++i)
    {
        auto name = (*i)[1].str();
        Require(!result.declared.contains(name), "Duplicate image declaration");
        if (!Excluded(name))
            result.declared.insert(name);
    }
    Require(!result.declared.empty(), "No image declarations");
    const std::regex use(R"(\b(?:IMAGE_|FONT_IMAGE_)\w+)");
    for (auto &entry : fs::recursive_directory_iterator(root / "pvzPerf"))
    {
        if (!entry.is_regular_file())
            continue;
        auto path = entry.path();
        if (path.extension() != ".cpp" && path.extension() != ".h" && path.extension() != ".hpp" &&
            path.extension() != ".inl")
            continue;
        bool generated = false;
        for (auto &part : path)
            generated |= part == "generated";
        if (generated || path.filename() == "Resources.cpp" || path.filename() == "Resources.h")
            continue;
        auto text = CleanComments(Text(path));
        for (auto i = std::sregex_iterator(text.begin(), text.end(), use); i != std::sregex_iterator(); ++i)
            if (result.declared.contains(i->str()))
                result.live.insert(i->str());
    }
    std::map<std::string, unsigned> groupIds;
    auto groupText = Text(root / "pvzPerf/ResourceGroupId.h");
    auto begin = groupText.find("enum class ResourceGroupId"), end = groupText.find("};", begin);
    Require(begin != groupText.npos && end != groupText.npos, "Missing ResourceGroupId");
    groupText = groupText.substr(begin, end - begin);
    std::regex groupPattern(R"((\w+)\s*=\s*(\d+))");
    for (auto i = std::sregex_iterator(groupText.begin(), groupText.end(), groupPattern);
         i != std::sregex_iterator(); ++i)
        groupIds[(*i)[1]] = std::stoul((*i)[2]);
    Require(groupIds.size() == 30 && groupIds.at("Invalid") == 0, "Expected 29 resource groups");
    std::set<std::string> seenNames, seenImages, seenGroups;
    std::string defaultPath, defaultPrefix;
    for (auto &group : manifest["children"])
    {
        validate(group, "Resources");
        auto groupName = group["attrs"].value("id", "");
        Require(!groupName.empty(), "Group has no ID");
        if (!groupIds.contains(groupName))
        {
            for (auto &node : group["children"])
                Require(node["tag"] == "SetDefaults", "Unknown nonempty resource group " + groupName);
            continue;
        }
        Require(seenGroups.insert(groupName).second, "Duplicate resource group");
        Json plan = {{"group", groupIds.at(groupName)}, {"name", groupName}, {"records", Json::array()}};
        std::map<std::string, unsigned> localFonts;
        for (auto &node : group["children"])
        {
            validate(node, "");
            auto tag = node["tag"].get<std::string>();
            const auto &a = node["attrs"];
            Require(node["children"].empty(), "Resource must be a leaf");
            if (tag == "SetDefaults")
            {
                if (a.contains("path"))
                    defaultPath = StripSlash(Path(a["path"].get<std::string>())) + "/";
                defaultPrefix = StripSlash(a.value("idprefix", defaultPrefix));
                continue;
            }
            Require((tag == "Image" || tag == "Sound" || tag == "Font") && a.contains("path"),
                    "Invalid resource entry");
            auto original = Path(a["path"].get<std::string>());
            auto name = defaultPrefix + a.value("id", fs::path(original).stem().string());
            Require(std::regex_match(name, std::regex("[A-Za-z_]\\w*")) && seenNames.insert(name).second,
                    "Invalid or duplicate resource target " + name);
            unsigned kind = tag == "Image" ? 0 : tag == "Sound" ? 1 : 2;
            if (kind == 0)
            {
                if (!result.live.contains(name))
                    continue;
                seenImages.insert(name);
            }
            auto path = original.starts_with('!') ? original : defaultPath + original;
            unsigned flags = 0, ref = 0xffffffff;
            if (path == "!program")
            {
                flags |= 1;
                path.clear();
            }
            else if (path.starts_with("!ref:"))
            {
                Require(kind == 2 && localFonts.contains(path.substr(5)),
                        "Font reference must name a prior font in the group");
                flags |= 4;
                ref = localFonts.at(path.substr(5));
                path.clear();
            }
            else
                Require(!path.starts_with('!'), "Unsupported resource control token " + path);
            if (Lower(path).starts_with("reanim/"))
                flags |= 8;
            std::array<std::string, 4> paths = {
                path, a.contains("alphaimage") ? defaultPath + a["alphaimage"].get<std::string>() : "",
                a.contains("alphagrid") ? defaultPath + a["alphagrid"].get<std::string>() : "",
                a.value("image", "")};
            Bytes details;
            if (kind == 0)
            {
                if (!a.contains("nopal"))
                    flags |= 16;
                if (!a.contains("noalpha") && name != "IMAGE_EDITBOX")
                    flags |= 1024;
                for (auto [key, bit] : std::vector<std::pair<std::string, unsigned>>{{"a4r4g4b4", 32},
                                                                                     {"a8r8g8b8", 64},
                                                                                     {"ddsurface", 128},
                                                                                     {"nobits", 256},
                                                                                     {"minsubdivide", 512},
                                                                                     {"nobits3d", 2048},
                                                                                     {"nobits2d", 4096}})
                    if (a.contains(key))
                        flags |= bit;
                auto rows = std::stoll(a.value("rows", "1")), cols = std::stoll(a.value("cols", "1"));
                Require(rows > 0 && rows <= 2147483647 && cols > 0 && cols <= 2147483647,
                        "Invalid image grid");
                Require(!a.contains("alphagrid") || (rows == 1 && cols == 1),
                        "Alpha grid requires an offline expanded mask");
                Require(Lower(a.value("anim", "none")) == "none" &&
                            std::stoll(a.value("framedelay", "0")) == 0 &&
                            std::stoll(a.value("begindelay", "0")) == 0 &&
                            std::stoll(a.value("enddelay", "0")) == 0 &&
                            a.value("perframedelay", "").empty() && a.value("framemap", "").empty(),
                        "Unsupported image animation metadata");
                Word(details, std::stoul(a.value("alphacolor", "FFFFFF"), nullptr, 16));
            }
            else if (kind == 1)
            {
                Double(details, std::stod(a.value("volume", "-1")));
                Integer(details, std::stoi(a.value("pan", "0")));
            }
            else
            {
                Integer(details, std::stoi(a.value("size", "0")));
                unsigned style = 0;
                for (auto [key, bit] : std::vector<std::pair<std::string, unsigned>>{
                         {"bold", 1}, {"italic", 2}, {"underline", 4}, {"shadow", 8}})
                    if (a.contains(key))
                        style |= bit;
                Word(details, style);
                auto fontTags = Tokens(a.value("tags", ""));
                Word(details, unsigned(fontTags.size()));
                for (auto &t : fontTags)
                {
                    Require(tags.contains(t), "Unknown compiled font tag " + t);
                    Word(details, tags.at(t));
                }
                localFonts[name] = unsigned(plan["records"].size());
            }
            if (kind == 0 && !path.empty() && !highResolution.is_null() &&
                highResolution.at("images").contains(Key(path)))
            {
                const auto key = Key(path);
                const auto &logical = highResolution.at("logicalSizes").at(key);
                Require(logical[0].get<int>() > 0 && logical[1].get<int>() > 0,
                        "Invalid high-resolution logical dimensions");
                path = "high-resolution/" + highResolution["images"][key].get<std::string>();
                paths[0] = path;
                paths[1].clear();
                paths[2].clear();
                flags = (flags & ~1024u) | 16384u;
                Word(details, logical[0].get<unsigned>());
                Word(details, logical[1].get<unsigned>());
            }
            Bytes pathData;
            for (auto &p : paths)
                String(pathData, p);
            Bytes recipe;
            Word(recipe, flags);
            Append(recipe, pathData);
            Append(recipe, details);
            if (!a.value("variant", "").empty())
                String(recipe, a["variant"].get<std::string>());
            Bytes data;
            Word(data, kind);
            Word(data, flags);
            Word(data, 0);
            Word(data, ref);
            Append(data, pathData);
            Append(data, details);
            plan["records"].push_back({{"name", name},
                                       {"kind", kind},
                                       {"flags", flags},
                                       {"variant", 0},
                                       {"path", path},
                                       {"data", Json::binary(data)},
                                       {"_recipe", Json::binary(recipe)}});
        }
        result.groups.push_back(plan);
    }
    Require(result.live == seenImages, "Live image globals missing from author manifest");
    std::sort(result.groups.begin(), result.groups.end(), [](const Json &a, const Json &b) {
        return a["group"].get<unsigned>() < b["group"].get<unsigned>();
    });
    std::map<std::string, std::set<Bytes>> recipes;
    for (auto &group : result.groups)
        for (auto &r : group["records"])
            if (r["kind"] == 0)
                recipes[Lower(r["path"].get<std::string>())].insert(r["_recipe"].get_binary());
    Json contract = Json::array();
    unsigned images = 0, total = 0;
    for (auto &group : result.groups)
    {
        Json list = Json::array();
        for (auto &r : group["records"])
        {
            auto recipe = r["_recipe"].get_binary();
            r.erase("_recipe");
            if (r["kind"] == 0)
            {
                auto &set = recipes.at(Lower(r["path"].get<std::string>()));
                auto variant = unsigned(std::distance(set.begin(), set.find(recipe)));
                r["variant"] = variant;
                auto data = r["data"].get_binary();
                SetWord(data, 8, variant);
                r["data"] = Json::binary(data);
                ++images;
            }
            ++total;
            list.push_back({r["kind"], r["name"]});
        }
        contract.push_back({group["group"], list});
    }
    auto text = contract.dump(-1, ' ', true);
    result.contract = Crc(std::span(reinterpret_cast<const unsigned char *>(text.data()), text.size()));
    auto payload = Payload(result.groups);
    result.payload = Crc(payload);
    for (unsigned v : {0x4353524eu, 4u, images, unsigned(result.groups.size()), total, result.contract,
                       result.payload, unsigned(payload.size())})
        Word(result.record, v);
    Append(result.record, payload);
    return result;
}

Records ResourceHeaders(const ResourcePlan &plan)
{
    std::vector<Json> records;
    for (auto &group : plan.groups)
        for (auto &r : group["records"])
            records.push_back(r);
    unsigned images =
        unsigned(std::count_if(records.begin(), records.end(), [](const Json &r) { return r["kind"] == 0; }));
    std::string contract =
        "// Generated transient load-plan contract.\n#pragma once\n#include <cstdint>\ninline constexpr "
        "std::uint32_t RESOURCE_LOAD_BINDING_CRC = 0x" +
        Hex(plan.contract) + "u;\ninline constexpr std::uint32_t RESOURCE_LOAD_PLAN_CRC = 0x" +
        Hex(plan.payload) +
        "u;\ninline constexpr unsigned RESOURCE_LOAD_IMAGE_COUNT = " + std::to_string(images) +
        ";\ninline constexpr unsigned RESOURCE_LOAD_PLAN_RECORD_COUNT = " + std::to_string(records.size()) +
        ";\n";
    std::string output = "// Generated explicit resource ownership bindings. Do not edit.\nnamespace "
                         "ResourceLoadBindings\n{\nnamespace {\n";
    for (auto &r : records)
        if (r["kind"] == 0)
            output += "Sexy::SharedImageRef owner_" + r["name"].get<std::string>() + ";\n";
    output += "}\nbool LoadStep(Sexy::ResourceManager& manager, Sexy::ResourceManager::LoadPlan& plan,\n     "
              "         ResourceGroupId group, unsigned step)\n{\n    switch (group)\n    {\n";
    const std::array<std::string, 3> kinds = {"Image", "Sound", "Font"};
    for (auto &group : plan.groups)
    {
        output += "    case ResourceGroupId::" + group["name"].get<std::string>() +
                  ":\n        switch (step)\n        {\n";
        unsigned step = 0;
        for (auto &r : group["records"])
        {
            auto kind = kinds.at(r["kind"].get<unsigned>()), name = r["name"].get<std::string>();
            auto owner = r["kind"] == 0 ? ", owner_" + name : "";
            output += "        case " + std::to_string(step++) + ": return manager.Load" + kind + "(plan.m" +
                      kind + "s[plan.mNext" + kind + "++], Sexy::" + name + owner + ");\n";
        }
        output += "        default: return false;\n        }\n";
    }
    output += "    default: return false;\n    }\n}\nvoid ReleaseGroup(Sexy::ResourceManager& manager, "
              "ResourceGroupId group)\n{\n    switch (group)\n    {\n";
    for (auto &group : plan.groups)
    {
        output += "    case ResourceGroupId::" + group["name"].get<std::string>() + ":\n";
        const auto &list = group["records"];
        for (auto i = list.rbegin(); i != list.rend(); ++i)
        {
            auto kind = kinds.at((*i)["kind"].get<unsigned>()), name = (*i)["name"].get<std::string>();
            output += "        manager.Release" + kind + "(Sexy::" + name +
                      ((*i)["kind"] == 0 ? ", owner_" + name : "") + ");\n";
        }
        output += "        break;\n";
    }
    output += "    default: break;\n    }\n}\n";
    if (std::any_of(records.begin(), records.end(),
                    [](const Json &r) { return r["name"] == "IMAGE_TITLESCREEN"; }))
        output += "void ReleaseTitleImage(Sexy::ResourceManager& manager)\n{\n    "
                  "manager.ReleaseImage(Sexy::IMAGE_TITLESCREEN, owner_IMAGE_TITLESCREEN);\n}\n";
    output += "}\n";
    return {{"NumericResourceBindings.h", Bytes(contract.begin(), contract.end())},
            {"ResourceLoadBindings.inc", Bytes(output.begin(), output.end())}};
}
} // namespace pvz::build
