#include "NativeBuild.h"
#include <iostream>

using namespace pvz::build;

int main(int argc, char **argv)
{
    try
    {
        Require(argc == 2, "Expected fixture output directory");
        const fs::path root = fs::absolute(argv[1]);
        fs::create_directories(root / "assets/high-resolution");
        fs::create_directories(root / "original");
        Image original;
        original.width = 12;
        original.height = 8;
        original.rgba.assign(12 * 8 * 4, 255);
        Publish(root / "original/image.png", EncodePng(original));
        Image hd;
        hd.width = 48;
        hd.height = 32;
        hd.rgba.assign(48 * 32 * 4, 128);
        Publish(root / "assets/high-resolution/image.png", EncodePng(hd));
        Publish(root / "assets/high-resolution/images.json",
                Json{{"format", "pvz-high-resolution-images-v1"},
                     {"images", {{"images/test", "image.png"}}}}.dump());
        ImageDescriptor descriptor;
        descriptor.path = "images/test";
        descriptor.cols = 3;
        ImageDescriptor program;
        program.flags=1;
        std::map<std::string, ImageDescriptor> descriptors{{"TEST", descriptor},{"PROGRAM",program}};
        ApplyHighResolutionImages(root, descriptors);
        const auto image = DecodeImage(descriptors.at("TEST"), {{"images/test.png", root / "original/image.png"}});
        Require(image.width == 48 && image.height == 32 && image.logicalWidth == 12 && image.logicalHeight == 8,
                "Physical and logical image extents were conflated");
        Require(image.rgba[3] == 128, "Replacement alpha was changed");
        Publish(root / "assets/high-resolution/images.json",
                Json{{"format", "pvz-high-resolution-images-v1"},
                     {"images", {{"images/test", "image.png"}}},
                     {"logicalSizes", {{"images/test", {12, 16}}}}}.dump());
        ApplyHighResolutionImages(root, descriptors);
        const auto resized = DecodeImage(descriptors.at("TEST"),
                                         {{"images/test.png", root / "original/image.png"}});
        Require(resized.width == 48 && resized.height == 32 &&
                    resized.logicalWidth == 12 && resized.logicalHeight == 16,
                "HD logical canvas override was ignored by definition atlas images");
        const Json report{{"images", Json::array({{{"region", {10, 20, 48, 32, 1, 3}},
                                                   {"width", 48}, {"height", 32},
                                                   {"logical_width", 12}, {"logical_height", 8}}})}};
        const auto cell = DrawingRegion(Json{26, 20, 16, 32}, report);
        Require(cell[4] == 4 && cell[5] == 8, "Sprite cell changed logical drawing size");
        const auto expected = "{" + CppFloat(.26) + ", " + CppFloat(.2) + ", " +
                              CppFloat(.42) + ", " + CppFloat(.52) + ", 4, 8}";
        Require(Region(cell, 100, 100) == expected, "Sprite cell UV is incorrect");
        Require(DrawingRegion(Json{0, 0, 0, 0}, report) == Json{0, 0, 0, 0}, "Null image slot changed");
        bool rejected = false;
        try { DrawingRegion(Json{0, 0, 1, 1}, report); }
        catch (const std::exception &) { rejected = true; }
        Require(rejected, "Unowned atlas rectangle was accepted");
        const Json scene{{"name", "test"}, {"sprites", Json::array({{{"name", "crop"},
            {"source", "TEST"}, {"rect", {4, 2, 4, 3}}}})}};
        const Json layout{{"width", 100}, {"height", 100}, {"regions", Json::array({{10, 20, 48, 32}})}};
        const auto compiled = CompileScene(scene, layout, {"TEST"}, "test.png", {{"TEST", image}});
        Require(compiled["sprites"][0]["region"] == Json{26.0, 28.0, 16.0, 12.0, 4, 3},
                "Scene crop did not convert logical pixels to physical UV coordinates");
        auto legacy = DecodeImage(descriptor, {{"images/test.png", root / "original/image.png"}});
        Require(legacy.logicalWidth == 12 && legacy.logicalHeight == 8 && legacy.width == 12,
                "Image without an HD replacement changed");
        descriptor.logicalWidth=6;
        descriptor.logicalHeight=4;
        auto direct=DecodeImage(descriptor,{{"images/test.png",root/"original/image.png"}});
        Require(direct.width==12 && direct.height==8 && direct.logicalWidth==6 && direct.logicalHeight==4,
                "Direct atlas source ignored its logical dimensions");
        Json sceneReports=Json::object();
        for(const auto &module:{"magnet","almanac","achievements","desktopui","landscapeui","commonui"})
            sceneReports["compiled/scene/"+std::string(module)+".compiled"]={
                {"atlas_width",100},{"atlas_height",100},
                {"atlas_path","compiled/atlases/"+std::string(module)+".atlas.png"},
                {"sprites",Json::array({{{"name","TEST"},{"region",{0,0,4,4,4,4}},{"scale",1.0}}})},
                {"images",Json::array()}};
        auto &cardRows=sceneReports["compiled/scene/commonui.compiled"]["sprites"];
        cardRows.push_back({{"name","SEED_CARD_59"},{"region",{10,20,16,32,50,70}},{"scale",1.0}});
        auto generated=GenerateScenes(sceneReports);
        auto header=generated.at("SceneAtlasData.h");
        auto implementation=generated.at("SceneAtlasData.cpp");
        Require(std::string(header.begin(),header.end()).find("SeedCards[60]")!=std::string::npos,
                "Special card 59 was omitted from generated direct-index table");
        Require(std::string(implementation.begin(),implementation.end()).find(Region(Json{10,20,16,32,50,70},100,100))!=std::string::npos,
                "Special card region lost its logical extent");
        std::cout << "High-resolution extents, sprite cells, alpha and sparse special card atlas indexing verified\n";
        return 0;
    }
    catch (const std::exception &error)
    {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
