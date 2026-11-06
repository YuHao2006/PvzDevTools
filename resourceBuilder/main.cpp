#include "NativeBuild.h"
#include <iostream>

int main(int argc, char **argv)
{
    try
    {
        using namespace pvz::build;
        if (argc == 2 && std::string_view(argv[1]) == "--version")
        {
            std::cout << "PvzDevTools 0.1.1\n";
            return 0;
        }
        if (argc < 2 || (argc == 2 && std::string_view(argv[1]) == "--help"))
        {
            std::cout
                << "PvzDevTools resource builder 0.1.1\n"
                   "build --root GAME --compiler REANIMC --cache DIRECTORY [--config FILE] [--generated-dir "
                   "DIRECTORY]\n"
                   "      [--compact lossless|light-lossy] [--compact-output FILE] [--compact-workers 1..8]\n"
                   "config|fonts|resources --root GAME --output PATH\n"
                   "compact --input PAK64 --output PAK64 --mode lossless|light-lossy [--workers 1..8]\n"
                   "atlas --input SIZES_JSON --compiler REANIMC --output LAYOUT_JSON --cache DIRECTORY\n";
            return argc < 2 ? 1 : 0;
        }
        Require(argc >= 2, "Expected build, config, fonts or resources subcommand");
        std::string command = argv[1];
        std::map<std::string, std::string> args;
        for (int i = 2; i < argc; i += 2)
        {
            Require(i + 1 < argc && std::string_view(argv[i]).starts_with("--"), "Expected --option value");
            Require(args.emplace(argv[i], argv[i + 1]).second, "Duplicate argument");
        }
        if (command == "compact")
        {
            Compact(args.at("--input"), args.at("--output"), args.at("--mode"),
                    args.contains("--workers") ? std::stoul(args.at("--workers")) : 2);
            return 0;
        }
        if (command == "atlas")
        {
            auto sizes = ReadJson(args.at("--input"));
            auto cache = fs::absolute(args.at("--cache"));
            fs::create_directories(cache);
            auto layouts = NativeLayouts(fs::canonical(args.at("--compiler")), {sizes}, cache);
            AtlasSettings settings;
            if (args.contains("--seconds"))
                settings.seconds = std::stod(args.at("--seconds"));
            if (args.contains("--require-optimal"))
                settings.requireOptimal = args.at("--require-optimal") == "true";
            auto layout =
                ExactLayout(sizes, layouts[0], settings, cache / "atlas-layout-cache", cache / "seed");
            Publish(args.at("--output"), layout.dump(2) + "\n");
            return 0;
        }
        auto root = fs::canonical(args.at("--root"));
        if (command == "build")
        {
            auto config = args.contains("--config") ? fs::path(args.at("--config"))
                                                    : root / "assets/definitions/ResourceBuild.pvzbuild";
            auto generated = args.contains("--generated-dir") ? fs::absolute(args.at("--generated-dir"))
                                                              : root / "pvzPerf/Sexy.TodLib/generated";
            auto cache = fs::absolute(args.at("--cache"));
            Build(root, fs::canonical(args.at("--compiler")), fs::canonical(config), cache, generated);
            if (args.contains("--compact"))
                Compact(cache / "main.pak64",
                        args.contains("--compact-output") ? fs::path(args.at("--compact-output"))
                                                          : cache / "main-compact.pak64",
                        args.at("--compact"),
                        args.contains("--compact-workers") ? std::stoul(args.at("--compact-workers")) : 2);
            return 0;
        }
        auto output = fs::absolute(args.at("--output"));
        if (command == "config")
        {
            auto config =
                LoadConfig(args.contains("--config") ? fs::path(args.at("--config"))
                                                     : root / "assets/definitions/ResourceBuild.pvzbuild");
            ValidateCatalogs(config, root);
            Json result = {{"type_map", TypeMap(config)},
                           {"sources", Json::object()},
                           {"order", Json::object()},
                           {"groups", config.groups}};
            for (auto &[kind, files] : config.sources)
            {
                result["sources"][kind] = Json::array();
                for (auto &path : files)
                    result["sources"][kind].push_back(path.generic_string());
            }
            for (auto &[kind, rows] : config.order)
                result["order"][kind] = rows;
            Publish(output, result.dump(2) + "\n");
        }
        else if (command == "fonts")
        {
            auto corpus = LoadFonts(root / "assets/definitions/fonts");
            for (auto &[name, record] : CompileFonts(corpus))
                Publish(output / name, record);
            Publish(output / "font-tags.json", Json(corpus.tags).dump(2) + "\n");
        }
        else if (command == "resources")
        {
            auto corpus = LoadFonts(root / "assets/definitions/fonts");
            auto plan =
                CompileResources(root, ReadJson(root / "assets/definitions/resources.json"), corpus.tags);
            Publish(output / "resources.compiled", plan.record);
            for (auto &[name, record] : ResourceHeaders(plan))
                Publish(output / name, record);
        }
        else
            Require(false, "Unknown subcommand " + command);
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << "Resource build failed: " << error.what() << '\n';
        return 1;
    }
}
