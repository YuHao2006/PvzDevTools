#include "NativeBuild.h"
#include <algorithm>
#include <cmath>
#include <regex>

namespace pvz::build
{
namespace
{
using Command = std::pair<std::string, std::vector<std::string>>;

std::vector<Command> Commands(std::string_view text)
{
    std::vector<std::string> tokens;
    for (std::size_t at = 0; at < text.size();)
    {
        if (std::isspace(static_cast<unsigned char>(text[at])))
        {
            ++at;
            continue;
        }
        if (text[at] == '#')
        {
            while (at < text.size() && text[at] != '\n')
                ++at;
            continue;
        }
        if (text[at] == '(' || text[at] == ')')
        {
            tokens.emplace_back(1, text[at++]);
            continue;
        }
        auto start = at;
        if (text[at] == '"')
        {
            ++at;
            while (at < text.size() && text[at] != '"')
            {
                if (text[at++] == '\\')
                {
                    Require(at < text.size(), "Unclosed quoted argument");
                    ++at;
                }
            }
            Require(at < text.size(), "Unclosed quoted argument");
            tokens.push_back(Json::parse(text.substr(start, ++at - start)).get<std::string>());
        }
        else
        {
            while (at < text.size() && !std::isspace(static_cast<unsigned char>(text[at])) &&
                   text[at] != '(' && text[at] != ')' && text[at] != '#' && text[at] != '"')
                ++at;
            Require(start != at, "Invalid build argument");
            tokens.emplace_back(text.substr(start, at - start));
        }
    }
    std::vector<Command> result;
    for (std::size_t at = 0; at < tokens.size();)
    {
        auto name = tokens[at++];
        Require(at < tokens.size() && tokens[at++] == "(", "Expected '(' after " + name);
        std::vector<std::string> args;
        while (at < tokens.size() && tokens[at] != ")")
        {
            Require(tokens[at] != "(", "Nested build commands are forbidden");
            args.push_back(tokens[at++]);
        }
        Require(at < tokens.size(), "Unclosed build command " + name);
        ++at;
        result.emplace_back(name, std::move(args));
    }
    return result;
}

unsigned Positive(std::string_view value)
{
    Require(!value.empty() && value.find_first_not_of("0123456789") == value.npos,
            "Expected positive integer setting");
    auto number = std::stoull(std::string(value));
    Require(number <= 0xffffffffULL, "Integer setting out of range");
    return static_cast<unsigned>(number);
}

AtlasSettings Settings(AtlasSettings base, std::span<const std::string> args)
{
    Require(args.size() % 2 == 0, "Atlas settings require key/value pairs");
    std::set<std::string> seen;
    for (std::size_t i = 0; i < args.size(); i += 2)
    {
        const auto &key = args[i];
        const auto &value = args[i + 1];
        Require(seen.insert(key).second, "Duplicate atlas setting " + key);
        if (key == "solver")
        {
            Require(value == "exact" || value == "maxrects", "Unknown atlas solver");
            base.fast = value == "maxrects";
        }
        else if (key == "require_optimal")
        {
            Require(value == "true" || value == "false", "Invalid require_optimal value");
            base.requireOptimal = value == "true";
        }
        else if (key == "time_limit_seconds")
        {
            if (value == "unlimited")
                base.seconds = 0;
            else
            {
                std::size_t consumed = 0;
                base.seconds = std::stod(value, &consumed);
                Require(consumed == value.size() && std::isfinite(base.seconds) && base.seconds > 0 &&
                            base.seconds <= 86400,
                        "Invalid atlas time budget");
            }
        }
        else if (key == "padding")
            base.padding = Positive(value);
        else if (key == "max_side")
            base.maxSide = Positive(value);
        else if (key == "workers")
            base.workers = Positive(value);
        else
            Require(false, "Unknown atlas setting " + key);
    }
    Require(base.padding == 2 && base.maxSide >= 1 && base.maxSide <= 16384 && base.workers >= 1 &&
                base.workers <= 64,
            "Invalid atlas settings");
    Require(base.fast || base.requireOptimal || base.seconds > 0, "Unproven layout requires a finite budget");
    return base;
}

std::string Directory(const std::string &kind)
{
    if (kind == "particle")
        return "particles";
    if (kind == "projectile")
        return "projectiles";
    return kind;
}

bool Glob(std::string_view pattern, std::string_view value)
{
    // Python pathlib glob semantics: '*' does not cross a directory boundary.
    std::string expression = "^";
    for (std::size_t i = 0; i < pattern.size(); ++i)
    {
        char c = pattern[i];
        if (c == '*' && i + 1 < pattern.size() && pattern[i + 1] == '*')
        {
            ++i;
            if (i + 1 < pattern.size() && pattern[i + 1] == '/')
            {
                ++i;
                expression += "(?:.*/)?";
            }
            else
                expression += ".*";
        }
        else if (c == '*')
            expression += "[^/]*";
        else if (c == '?')
            expression += "[^/]";
        else
        {
            if (std::string_view(".\\+()[]{}^$|").find(c) != std::string_view::npos)
                expression += '\\';
            expression += c;
        }
    }
    return std::regex_match(value.begin(), value.end(), std::regex(expression + "$"));
}
} // namespace

Config LoadConfig(const fs::path &path)
{
    Config config;
    config.path = fs::canonical(path);
    for (auto kind : {"reanim", "particle", "projectile", "zombatar", "scene"})
        config.sources[kind];
    const auto commands = Commands(Text(path));
    Require(!commands.empty() && commands[0] == Command("resource_build", {"1"}),
            "Build configuration must begin with resource_build(1)");
    std::set<fs::path> selected;
    std::map<std::string, std::vector<std::string>> overrides;
    bool defaults = false;
    for (std::size_t index = 1; index < commands.size(); ++index)
    {
        const auto &[command, args] = commands[index];
        if (command == "sources")
        {
            Require(args.size() >= 2 && config.sources.contains(args[0]), "Invalid sources command");
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                fs::path pattern(args[i]);
                Require(!pattern.is_absolute() && pattern.generic_string().find(':') == std::string::npos,
                        "Source paths must be relative");
                for (auto &part : pattern)
                    Require(part != "..", "Source paths must stay inside configuration");
                std::vector<fs::path> matches;
                for (auto &entry : fs::recursive_directory_iterator(config.path.parent_path()))
                {
                    auto relative = entry.path().lexically_relative(config.path.parent_path());
                    if (Glob(pattern.generic_string(), relative.generic_string()))
                        matches.push_back(entry.path());
                }
                std::sort(matches.begin(), matches.end(), [](const fs::path &a, const fs::path &b) {
                    return Lower(a.generic_string()) < Lower(b.generic_string());
                });
                Require(!matches.empty(), "Source pattern matched no files: " + args[i]);
                for (auto &match : matches)
                {
                    auto source = fs::canonical(match);
                    auto relative = source.lexically_relative(config.path.parent_path());
                    Require(!relative.empty() && *relative.begin() != ".." && fs::is_regular_file(source) &&
                                source.extension() == (args[0] == "zombatar" ? ".json" : ".dsl"),
                            "Invalid definition source " + source.string());
                    Require(selected.insert(source).second, "Definition source selected more than once");
                    config.sources[args[0]].push_back(source);
                }
            }
        }
        else if (command == "definition_order")
        {
            Require(args.size() >= 2 && (args[0] == "reanim" || args[0] == "particle") &&
                        !config.order.contains(args[0]),
                    "Invalid or duplicate definition_order");
            std::set<std::string> enums, names;
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                auto colon = args[i].find(':');
                Require(colon != std::string::npos, "Expected ENUM:Definition");
                auto symbol = args[i].substr(0, colon), name = args[i].substr(colon + 1);
                Require(std::regex_match(symbol, std::regex("[A-Z][A-Z0-9_]*")) &&
                            std::regex_match(name, std::regex("[A-Za-z0-9_-]+")) &&
                            symbol.starts_with(args[0] == "reanim" ? "REANIM_" : "PARTICLE_") &&
                            enums.insert(symbol).second && names.insert(Lower(name)).second,
                        "Invalid or duplicate definition order member " + args[i]);
                config.order[args[0]].emplace_back(symbol, name);
            }
        }
        else if (command == "atlas_defaults")
        {
            Require(!defaults, "Duplicate atlas_defaults");
            defaults = true;
            config.defaults = Settings(config.defaults, args);
        }
        else if (command == "atlas")
        {
            Require(args.size() >= 2 && std::regex_match(args[0], std::regex("[a-z][a-z0-9_-]*")) &&
                        !std::regex_match(args[0], std::regex("con|prn|aux|nul|com[1-9]|lpt[1-9]")) &&
                        !config.groups.contains(args[0]),
                    "Invalid or duplicate atlas name");
            auto &group = config.groups[args[0]];
            for (std::size_t i = 1; i < args.size(); ++i)
            {
                auto colon = args[i].find(':');
                Require(colon != std::string::npos, "Expected kind:Definition");
                auto kind = args[i].substr(0, colon), name = args[i].substr(colon + 1);
                Require(config.sources.contains(kind) &&
                            std::regex_match(name, std::regex("[A-Za-z0-9_-]+\\*?")),
                        "Invalid atlas member " + args[i]);
                auto member = "compiled/" + Directory(kind) + "/" + Lower(name) + ".compiled";
                for (const auto &[_, members] : config.groups)
                    Require(std::find(members.begin(), members.end(), member) == members.end(),
                            "Definition belongs to multiple atlas groups");
                group.push_back(member);
            }
        }
        else if (command == "atlas_settings")
        {
            Require(!args.empty() && !overrides.contains(args[0]), "Duplicate or empty atlas_settings");
            overrides[args[0]] = {args.begin() + 1, args.end()};
        }
        else
            Require(false, "Unknown resource build command " + command);
    }
    Require(!config.sources["reanim"].empty() && !config.sources["particle"].empty(),
            "Configuration must select reanim and particle sources");
    Require(config.sources["projectile"].size() <= 1 && config.sources["zombatar"].size() <= 1,
            "Only one projectile/Zombatar document is allowed");
    for (auto &[name, args] : overrides)
    {
        Require(config.groups.contains(name), "Settings refer to undefined atlas " + name);
        config.settings[name] = Settings(config.defaults, args);
    }
    return config;
}

void ValidateCatalogs(const Config &config, const fs::path &root)
{
    for (auto kind : {std::string("reanim"), std::string("particle")})
    {
        bool reanim = kind == "reanim";
        auto text = CleanComments(
            Text(root / "pvzPerf/Sexy.TodLib" / (reanim ? "Reanimator.cpp" : "TodParticle.cpp")));
        std::string catalog = reanim ? "gLawnReanimationArray" : "gLawnParticleArray";
        std::smatch block;
        Require(std::regex_search(text, block,
                                  std::regex("\\b" + catalog + R"(\s*\[[^;]*?\]\s*=\s*\{([\s\S]*?)\n\};)")),
                "Cannot locate runtime definition catalog " + catalog);
        const auto body = block[1].str();
        std::regex row(R"xxx(\{\s*(?:\w+::)?(\w+)\s*,\s*("(?:\\.|[^"\\])*"))xxx");
        std::vector<std::pair<std::string, std::string>> actual, expected;
        for (auto i = std::sregex_iterator(body.begin(), body.end(), row); i != std::sregex_iterator(); ++i)
        {
            auto path = Json::parse((*i)[2].str()).get<std::string>();
            std::replace(path.begin(), path.end(), '\\', '/');
            actual.emplace_back((*i)[1], Lower(fs::path(path).stem().string()));
        }
        Require(config.order.contains(kind), "Missing definition_order(" + kind + ")");
        for (auto &[symbol, name] : config.order.at(kind))
            expected.emplace_back(symbol, Lower(name));
        Require(actual == expected, "Configured " + kind + " definition order differs from runtime catalog");
    }
}

std::string TypeMap(const Config &config)
{
    Require(config.order.contains("reanim") && !config.order.at("reanim").empty(), "Missing reanim order");
    std::string out = "reanim_none REANIM_NONE 4294967295\n";
    unsigned index = 0;
    for (auto &[symbol, name] : config.order.at("reanim"))
        out += Lower(name) + " " + std::to_string(index++) + "\n";
    return out;
}

std::map<std::string, std::vector<std::string>> ResolveGroups(const Config &config,
                                                              const std::set<std::string> &names)
{
    std::map<std::string, std::string> aliases;
    for (auto &[kind, sources] : config.sources)
        for (auto &source : sources)
        {
            auto text = Text(source);
            std::smatch component, output;
            if (!std::regex_search(text, component, std::regex(R"(export\s+component\s+(\w+)\s*\{)")))
                continue;
            auto name = component[1].str(), basename = name;
            if (kind == "particle" &&
                std::regex_search(text, output,
                                  std::regex(R"xxx(source-name\s*:\s*"([A-Za-z0-9_-]+)"\s*;)xxx")))
                basename = output[1];
            auto alias = "compiled/" + Directory(kind) + "/" + Lower(name) + ".compiled";
            auto target = "compiled/" + Directory(kind) + "/" + Lower(basename) + ".compiled";
            Require(!aliases.contains(alias) || aliases[alias] == target,
                    "Ambiguous atlas component " + name);
            aliases[alias] = target;
        }
    std::set<std::string> assigned;
    std::map<std::string, std::vector<std::string>> result;
    for (auto &[name, members] : config.groups)
    {
        auto &group = result[name];
        for (auto &member : members)
        {
            if (member.find('*') != std::string::npos)
            {
                auto count = group.size();
                for (auto &candidate : names)
                    if (Glob(member, candidate))
                        group.push_back(candidate);
                Require(group.size() != count, "Atlas pattern matched no definitions: " + member);
            }
            else
                group.push_back(aliases.contains(member) ? aliases[member] : member);
        }
        for (auto &member : group)
        {
            Require(names.contains(member), "Atlas contains unknown definition " + member);
            Require(assigned.insert(member).second, "Definition belongs to multiple atlas groups");
        }
    }
    return result;
}
} // namespace pvz::build
