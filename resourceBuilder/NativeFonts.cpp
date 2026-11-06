#include "NativeBuild.h"
#include <algorithm>
#include <bit>
#include <regex>

namespace pvz::build
{
namespace
{
const std::vector<std::string> scalars = {
    "draw_mode", "offset_x",       "offset_y", "spacing",        "min_size",     "max_size",   "point_size",
    "ascent",    "ascent_padding", "height",   "default_height", "line_spacing", "base_order", "alpha"};

Json ParseList(std::string_view text, std::size_t &at, bool nested = false)
{
    Json list = Json::array();
    std::string atom;
    char quote = 0;
    bool escaped = false;
    while (at < text.size())
    {
        char ch = text[at++];
        if (escaped)
        {
            atom += ch;
            escaped = false;
            continue;
        }
        if ((ch == '"' || ch == '\'') && (!quote || ch == quote))
            quote = quote ? 0 : ch;
        if (ch == '\\')
            escaped = true;
        else if (!quote && std::string_view(" \t\n,()").find(ch) != std::string_view::npos)
        {
            Require(ch != '(' || atom.empty(), "Unexpected font list start");
            if (!atom.empty())
            {
                list.push_back(atom);
                atom.clear();
            }
            if (ch == '(')
                list.push_back(ParseList(text, at, true));
            else if (ch == ')')
            {
                Require(nested, "Unexpected font list end");
                return list;
            }
        }
        else
            atom += ch;
    }
    Require(!quote && !nested && !escaped, "Unterminated font list/quote");
    if (!atom.empty())
        list.push_back(atom);
    return list;
}

Json Descriptor(std::string_view text)
{
    Json commands = Json::array();
    std::string current;
    char quote = 0;
    bool escaped = false, lineStart = true, skip = false;
    auto append = [&] {
        if (current.find_first_not_of(" \t\n") != current.npos)
        {
            std::size_t at = 0;
            commands.push_back(ParseList(current, at));
        }
        current.clear();
    };
    for (char ch : text)
    {
        if (ch == '\r')
            continue;
        if (lineStart && std::string_view(" \t\n").find(ch) != std::string_view::npos)
            continue;
        if (lineStart)
        {
            skip = ch == '#';
            lineStart = false;
        }
        if (ch == '\n')
        {
            lineStart = true;
            if (skip)
            {
                skip = false;
                continue;
            }
        }
        if (skip)
            continue;
        if (escaped)
        {
            current += '\\';
            current += ch;
            escaped = false;
            continue;
        }
        if (ch == '\\' && quote)
        {
            escaped = true;
            continue;
        }
        if ((ch == '"' || ch == '\'') && (!quote || ch == quote))
            quote = quote ? 0 : ch;
        if (ch == ';' && !quote)
            append();
        else
            current += ch;
    }
    Require(!quote && !escaped, "Unterminated descriptor quote");
    append();
    return commands;
}

std::string Unquote(const Json &atom)
{
    auto value = atom.get<std::string>();
    if (value.empty() || (value[0] != '"' && value[0] != '\''))
        return value;
    std::string result;
    char quote = value[0];
    bool last = false;
    for (char c : value)
    {
        if (c == quote)
        {
            if (last)
                result += c;
            last = true;
        }
        else
        {
            result += c;
            last = false;
        }
    }
    return result;
}

int Int(const Json &atom)
{
    auto value = atom.get<std::string>();
    Require(std::regex_match(value, std::regex("-?(?:[0-9]+|0[xX][0-9a-fA-F]+)")), "Invalid font integer");
    auto number = std::stoll(value, nullptr, Lower(value).find('x') != value.npos ? 16 : 10);
    Require(number >= -2147483648LL && number <= 4294967295LL, "Font integer outside 32 bits");
    return std::bit_cast<int>(static_cast<unsigned>(number));
}

Json Ints(const Json &list)
{
    Json result = Json::array();
    for (auto &atom : list)
        result.push_back(Int(atom));
    return result;
}

// Reproduce the old parser's CP_UTF8 -> UTF-16 -> unsigned-char operation,
// including U+FFFD for ill-formed legacy descriptor bytes.
std::vector<unsigned> WideChars(std::string_view text)
{
    std::vector<unsigned> result;
    for (std::size_t at = 0; at < text.size() && text[at];)
    {
        unsigned lead = static_cast<unsigned char>(text[at++]), cp = lead, count = 0;
        if (lead >= 0xc2 && lead <= 0xdf)
        {
            cp = lead & 31;
            count = 1;
        }
        else if (lead >= 0xe0 && lead <= 0xef)
        {
            cp = lead & 15;
            count = 2;
        }
        else if (lead >= 0xf0 && lead <= 0xf4)
        {
            cp = lead & 7;
            count = 3;
        }
        else if (lead >= 0x80)
            cp = 0xfffd;
        for (unsigned i = 0; i < count; ++i)
        {
            if (at == text.size())
            {
                cp = 0xfffd;
                break;
            }
            unsigned c = static_cast<unsigned char>(text[at]);
            if ((c & 0xc0) != 0x80 || (i == 0 && ((lead == 0xe0 && c < 0xa0) || (lead == 0xed && c >= 0xa0) ||
                                                  (lead == 0xf0 && c < 0x90) || (lead == 0xf4 && c >= 0x90))))
            {
                cp = 0xfffd;
                break;
            }
            ++at;
            cp = (cp << 6) | (c & 63);
        }
        if (cp <= 0xffff)
            result.push_back(cp & 255);
        else
        {
            cp -= 0x10000;
            result.push_back((0xd800 + (cp >> 10)) & 255);
            result.push_back((0xdc00 + (cp & 1023)) & 255);
        }
    }
    return result;
}

Json Layer(std::string_view name, unsigned order)
{
    Json scalar = Json::object();
    for (auto &key : scalars)
        scalar[key] = 0;
    scalar["draw_mode"] = scalar["min_size"] = scalar["max_size"] = -1;
    scalar["base_order"] = order;
    scalar["alpha"] = 1;
    return {{"name", name},
            {"image", ""},
            {"required", Json::array()},
            {"excluded", Json::array()},
            {"scalars", scalar},
            {"mult", {255, 255, 255, 255}},
            {"add", {0, 0, 0, 0}},
            {"glyphs", Json::object()}};
}
Json NewGlyph()
{
    return {{"rect", {0, 0, 0, 0}}, {"offset", {0, 0}}, {"width", 0}, {"order", 0}, {"kern", Json::object()}};
}

class Compiler
{
  public:
    Json defines = Json::object(), layers = Json::array(), charMap = Json::object();
    std::map<std::string, unsigned> layerMap;
    int pointSize = 0;
    Json Values(const Json &source) const
    {
        if (!source.is_array())
        {
            auto value = defines.at(Upper(source.get<std::string>()));
            Require(value.is_array(), "Font list required");
            return value;
        }
        Json result = Json::array();
        for (auto &value : source)
        {
            if (value.is_array())
                result.push_back(Values(value));
            else
            {
                auto str = value.get<std::string>();
                if (str.empty())
                    continue;
                if (str[0] == '"' || str[0] == '\'')
                    result.push_back(Unquote(value));
                else if (std::string_view("0123456789-+").find(str[0]) != std::string_view::npos)
                    result.push_back(value);
                else
                    result.push_back(defines.at(Upper(str)));
            }
        }
        return result;
    }
    std::string Scalar(const Json &source) const
    {
        auto key = Upper(source.get<std::string>());
        auto value = defines.contains(key) ? defines.at(key) : source;
        Require(value.is_string(), "Scalar font define required");
        return Unquote(value);
    }
    Json Run(const std::string &sourcePath, std::string_view text)
    {
        const std::map<std::string, unsigned> counts = {{"define", 2},
                                                        {"createhorzspanrectlist", 3},
                                                        {"setdefaultpointsize", 1},
                                                        {"setcharmap", 2},
                                                        {"createlayer", 1},
                                                        {"createlayerfrom", 2},
                                                        {"layerrequiretags", 2},
                                                        {"layerexcludetags", 2},
                                                        {"layerpointrange", 3},
                                                        {"layersetpointsize", 2},
                                                        {"layersetheight", 2},
                                                        {"layersetimage", 2},
                                                        {"layersetdrawmode", 2},
                                                        {"layersetcolormult", 2},
                                                        {"layersetcoloradd", 2},
                                                        {"layersetascent", 2},
                                                        {"layersetascentpadding", 2},
                                                        {"layersetlinespacingoffset", 2},
                                                        {"layersetoffset", 2},
                                                        {"layersetcharwidths", 3},
                                                        {"layersetspacing", 2},
                                                        {"layersetimagemap", 3},
                                                        {"layersetcharoffsets", 3},
                                                        {"layersetkerningpairs", 3},
                                                        {"layersetbaseorder", 2},
                                                        {"layersetcharorders", 3},
                                                        {"layersetexinfo", 0}};
        const std::map<std::string, std::string> scalarCommands = {
            {"layersetpointsize", "point_size"},
            {"layersetheight", "height"},
            {"layersetdrawmode", "draw_mode"},
            {"layersetascent", "ascent"},
            {"layersetascentpadding", "ascent_padding"},
            {"layersetlinespacingoffset", "line_spacing"},
            {"layersetspacing", "spacing"},
            {"layersetbaseorder", "base_order"}};
        for (auto &command : Descriptor(text))
        {
            Require(!command.empty() && command[0].is_string(), "Empty font command");
            auto name = Lower(command[0].get<std::string>());
            Require(counts.contains(name), "Unknown font command " + name);
            Json a = Json::array();
            for (std::size_t i = 1; i < command.size(); ++i)
                a.push_back(command[i]);
            Require(name == "layersetexinfo" || a.size() == counts.at(name),
                    "Wrong argument count for " + name);
            if (name == "define")
            {
                auto key = Upper(a[0].get<std::string>());
                defines.erase(key);
                defines[key] =
                    a[1].is_array() ? Values(a[1]) : defines.value(Upper(a[1].get<std::string>()), a[1]);
            }
            else if (name == "createhorzspanrectlist")
            {
                auto rect = Ints(Values(a[1])), widths = Ints(Values(a[2]));
                Require(rect.size() == 4, "Font span rectangle needs four integers");
                int x = rect[0];
                Json spans = Json::array();
                for (auto &w : widths)
                {
                    spans.push_back({std::to_string(x), std::to_string(rect[1].get<int>()),
                                     std::to_string(w.get<int>()), std::to_string(rect[3].get<int>())});
                    x += w.get<int>();
                }
                defines[Upper(a[0].get<std::string>())] = spans;
            }
            else if (name == "setdefaultpointsize")
                pointSize = Int(a[0]);
            else if (name == "setcharmap")
            {
                auto from = Values(a[0]), to = Values(a[1]);
                Require(from.size() == to.size(), "Char map size mismatch");
                for (std::size_t i = 0; i < from.size(); ++i)
                {
                    auto f = from[i].get<std::string>(), t = to[i].get<std::string>();
                    Require(f.size() == 1 && t.size() == 1, "Single byte char map required");
                    auto key = std::to_string(static_cast<unsigned char>(f[0]));
                    if (!charMap.contains(key))
                        charMap[key] = static_cast<unsigned char>(t[0]);
                }
            }
            else if (name == "createlayer" || name == "createlayerfrom")
            {
                auto key = Upper(a[0].get<std::string>());
                Require(!layerMap.contains(key), "Duplicate font layer");
                auto layer = name == "createlayer" ? Layer(key, unsigned(layers.size()))
                                                   : layers.at(layerMap.at(Upper(a[1].get<std::string>())));
                layer["name"] = key;
                layer["scalars"]["base_order"] = layers.size();
                layerMap[key] = unsigned(layers.size());
                layers.push_back(layer);
            }
            else if (name == "layersetexinfo")
                continue;
            else
            {
                auto &layer = layers.at(layerMap.at(Upper(a[0].get<std::string>())));
                auto &scalar = layer["scalars"];
                if (scalarCommands.contains(name))
                {
                    auto v = Int(a[1]);
                    Require(name != "layersetdrawmode" || v == 0 || v == 1, "Invalid font draw mode");
                    scalar[scalarCommands.at(name)] = v;
                }
                else if (name == "layerpointrange")
                {
                    scalar["min_size"] = Int(a[1]);
                    scalar["max_size"] = Int(a[2]);
                }
                else if (name == "layersetoffset")
                {
                    auto v = Ints(Values(a[1]));
                    Require(v.size() == 2, "Font offset needs two integers");
                    scalar["offset_x"] = v[0];
                    scalar["offset_y"] = v[1];
                }
                else if (name == "layerrequiretags" || name == "layerexcludetags")
                {
                    for (auto &v : Values(a[1]))
                        layer[name == "layerrequiretags" ? "required" : "excluded"].push_back(
                            Upper(v.get<std::string>()));
                }
                else if (name == "layersetimage")
                {
                    auto path = Scalar(a[1]);
                    std::replace(path.begin(), path.end(), '\\', '/');
                    layer["image"] = path.find(':') != path.npos || path.starts_with('/')
                                         ? path
                                         : (fs::path(sourcePath).parent_path() / path).generic_string();
                }
                else if (name == "layersetcolormult" || name == "layersetcoloradd")
                {
                    Json color = Json::array();
                    if (a[1].is_array())
                    {
                        for (auto &v : Values(a[1]))
                            color.push_back(int(std::stod(v.get<std::string>()) * 255));
                        Require(color.size() == 4, "RGBA list required");
                    }
                    else
                    {
                        unsigned c = static_cast<unsigned>(Int(a[1]));
                        color = {(c >> 16) & 255, (c >> 8) & 255, c & 255, (c >> 24) ? c >> 24 : 255};
                    }
                    layer[name == "layersetcolormult" ? "mult" : "add"] = color;
                }
                else
                {
                    auto chars = Values(a[1]), values = Values(a[2]);
                    Require(chars.size() == values.size(), "Glyph list size mismatch");
                    for (std::size_t i = 0; i < chars.size(); ++i)
                    {
                        auto text = chars[i].get<std::string>();
                        std::vector<unsigned> units;
                        if (name == "layersetcharorders")
                            for (unsigned char c : text)
                                units.push_back(c);
                        else
                            units = WideChars(text);
                        Require(units.size() == (name == "layersetkerningpairs" ? 2 : 1),
                                "Invalid font character/pair");
                        Json dummy = NewGlyph();
                        auto key = std::to_string(units[0]);
                        if (units[0] < 128 && !layer["glyphs"].contains(key))
                            layer["glyphs"][key] = NewGlyph();
                        auto &glyph = units[0] < 128 ? layer["glyphs"][key] : dummy;
                        if (name == "layersetcharwidths")
                            glyph["width"] = Int(values[i]);
                        else if (name == "layersetcharorders")
                            glyph["order"] = Int(values[i]);
                        else if (name == "layersetkerningpairs")
                            glyph["kern"][std::to_string(units[1])] = Int(values[i]);
                        else if (name == "layersetimagemap" || name == "layersetcharoffsets")
                        {
                            auto v = Ints(values[i]);
                            Require(v.size() == (name == "layersetimagemap" ? 4 : 2),
                                    "Invalid font geometry");
                            glyph[name == "layersetimagemap" ? "rect" : "offset"] = v;
                        }
                        else
                            Require(false, "Unhandled font command " + name);
                    }
                    if (name == "layersetimagemap")
                    {
                        Require(!layer["image"].get<std::string>().empty(), "Image map precedes image");
                        int height = 0;
                        for (auto &glyph : layer["glyphs"])
                            height =
                                std::max(height, glyph["rect"][3].get<int>() + glyph["offset"][1].get<int>());
                        scalar["default_height"] = height;
                    }
                }
            }
        }
        Require(!layers.empty(), "Font has no layers");
        unsigned id = 0;
        for (auto &[name, index] : layerMap)
            layers[index]["id"] = id++;
        for (auto &layer : layers)
            Require(!layer["image"].get<std::string>().empty(), "Font layer needs an image");
        return {{"source", sourcePath}, {"point_size", pointSize}, {"char_map", charMap}, {"layers", layers}};
    }
};
} // namespace

FontCorpus LoadFonts(const fs::path &directory)
{
    FontCorpus result;
    result.fonts = Json::array();
    std::vector<fs::path> paths;
    std::set<std::string> tags;
    for (auto &e : fs::directory_iterator(directory))
        if (e.is_regular_file() && e.path().extension() == ".txt")
            paths.push_back(e.path());
    std::sort(paths.begin(), paths.end());
    Require(!paths.empty(), "No authored fonts");
    for (auto &path : paths)
    {
        auto text = Text(path);
        Require(std::regex_search(text, std::regex("\\bCreateLayer\\b", std::regex::icase)),
                "Invalid font descriptor");
        auto font = Compiler().Run("data/" + path.filename().string(), text);
        for (auto &layer : font["layers"])
            for (auto field : {"required", "excluded"})
                for (auto &tag : layer[field])
                    tags.insert(tag.get<std::string>());
        result.fonts.push_back(std::move(font));
    }
    for (auto &tag : tags)
        result.tags[tag] = unsigned(result.tags.size());
    return result;
}

Records CompileFonts(const FontCorpus &corpus)
{
    Records result;
    for (auto &font : corpus.fonts)
    {
        Bytes payload;
        std::map<unsigned, unsigned> charMap;
        for (auto &[ch, target] : font["char_map"].items())
            charMap[std::stoul(ch)] = target.get<unsigned>();
        for (auto &[from, to] : charMap)
        {
            Word(payload, from);
            Word(payload, to);
        }
        for (auto &layer : font["layers"])
        {
            Word(payload, layer["id"].get<unsigned>());
            String(payload, layer["image"].get<std::string>());
            for (auto key : {"required", "excluded"})
            {
                Word(payload, unsigned(layer[key].size()));
                for (auto &tag : layer[key])
                    Word(payload, corpus.tags.at(tag.get<std::string>()));
            }
            for (auto &key : scalars)
                Integer(payload, layer["scalars"][key].get<int>());
            for (auto key : {"mult", "add"})
                for (auto &v : layer[key])
                    Integer(payload, v.get<int>());
            std::map<unsigned, Json> glyphs;
            for (auto &[key, glyph] : layer["glyphs"].items())
                glyphs[std::stoul(key)] = glyph;
            Word(payload, unsigned(glyphs.size()));
            for (auto &[ch, glyph] : glyphs)
            {
                Word(payload, ch);
                for (auto key : {"rect", "offset"})
                    for (auto &v : glyph[key])
                        Integer(payload, v.get<int>());
                Integer(payload, glyph["width"].get<int>());
                Integer(payload, glyph["order"].get<int>());
                Word(payload, unsigned(glyph["kern"].size()));
                for (auto &[key, v] : glyph["kern"].items())
                {
                    Word(payload, std::stoul(key));
                    Integer(payload, v.get<int>());
                }
            }
        }
        Bytes record;
        for (unsigned v : {0x544e464eu, 1u, unsigned(payload.size()), Crc(payload)})
            Word(record, v);
        Integer(record, font["point_size"].get<int>());
        Word(record, unsigned(charMap.size()));
        Word(record, unsigned(font["layers"].size()));
        Word(record, unsigned(corpus.tags.size()));
        Append(record, payload);
        auto source = font["source"].get<std::string>();
        result["compiled/fonts/" + Lower(fs::path(source).stem().string()) + ".compiled"] = std::move(record);
    }
    return result;
}
} // namespace pvz::build
