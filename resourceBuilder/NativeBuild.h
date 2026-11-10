#pragma once
#include <nlohmann/json.hpp>
#include <array>
#include <cstdint>
#include <filesystem>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pvz::build
{
namespace fs = std::filesystem;
using Json = nlohmann::ordered_json;
using Bytes = std::vector<unsigned char>;
using Files = std::map<std::string, fs::path>;
using Records = std::map<std::string, Bytes>;
struct Reader
{
    std::span<const unsigned char> data;
    std::size_t at = 0;
    explicit Reader(std::span<const unsigned char> bytes);
    unsigned Word();
    int Int();
    float Float();
    double Double();
    Bytes Take(std::size_t count);
    std::string String();
    void Skip(std::size_t count);
    void Finish() const;
};
void Require(bool condition, std::string_view message);
Bytes Read(const fs::path &path);
std::string Text(const fs::path &path);
Json ReadJson(const fs::path &path);
void Publish(const fs::path &path, std::span<const unsigned char> data);
void Publish(const fs::path &path, std::string_view text);
void Word(Bytes &bytes, unsigned value);
void Integer(Bytes &bytes, int value);
void Float(Bytes &bytes, float value);
void Double(Bytes &bytes, double value);
void String(Bytes &bytes, std::string_view value);
void Append(Bytes &bytes, std::span<const unsigned char> value);
void SetWord(Bytes &bytes, std::size_t offset, unsigned value);
unsigned GetWord(std::span<const unsigned char> bytes, std::size_t offset);
std::string Lower(std::string value);
std::string Upper(std::string value);
std::string CleanComments(std::string_view value);
std::vector<std::string> StringLiterals(std::string_view value);
std::string Key(std::string value);
std::string Sha256(std::span<const unsigned char> data);
unsigned Crc(std::span<const unsigned char> data);
std::string CppFloat(double value);
std::string CppString(std::string_view value);
std::string Region(const Json &rect, int width, int height);
Json DrawingRegion(const Json &rect, const Json &report);
void Run(const fs::path &executable, const std::vector<std::string> &args);
void Zip(const fs::path &path, const Records &records);
void Compact(const fs::path &source, const fs::path &output, std::string_view mode, unsigned workers = 2);
Bytes Envelope(unsigned cookie, const Bytes &payload);
Files Inventory(const fs::path &root);

struct AtlasSettings
{
    unsigned padding = 2, maxSide = 4096, workers = 1;
    double seconds = 0;
    bool requireOptimal = true;
    bool fast = false;
};
struct Config
{
    fs::path path;
    std::map<std::string, std::vector<fs::path>> sources;
    std::map<std::string, std::vector<std::pair<std::string, std::string>>> order;
    std::map<std::string, std::vector<std::string>> groups;
    std::map<std::string, AtlasSettings> settings;
    AtlasSettings defaults;
};
Config LoadConfig(const fs::path &path);
void ValidateCatalogs(const Config &config, const fs::path &root);
std::map<std::string, std::vector<std::string>> ResolveGroups(const Config &config,
                                                              const std::set<std::string> &names);
std::string TypeMap(const Config &config);

struct FontCorpus
{
    Json fonts;
    std::map<std::string, unsigned> tags;
};
FontCorpus LoadFonts(const fs::path &directory);
Records CompileFonts(const FontCorpus &corpus);

struct ResourcePlan
{
    Bytes record;
    Json groups;
    std::set<std::string> declared, live;
    unsigned contract = 0, payload = 0;
};
ResourcePlan CompileResources(const fs::path &root, const Json &author,
                              const std::map<std::string, unsigned> &tags);
Records ResourceHeaders(const ResourcePlan &plan);

struct ImageDescriptor
{
    std::string path, alpha, alphaGrid;
    fs::path highResolution;
    unsigned logicalWidth=0, logicalHeight=0;
    unsigned flags = 0, rows = 1, cols = 1, color = 0xffffff;
};
struct Image
{
    unsigned width = 0, height = 0;
    unsigned logicalWidth = 0, logicalHeight = 0;
    Bytes rgba;
    std::set<std::string> files;
};
std::map<std::string, ImageDescriptor> ImageDescriptors(const Json &author, const Files &files,
                                                        const std::set<std::string> &names);
Image DecodeImage(const ImageDescriptor &descriptor, const Files &files);
void ApplyHighResolutionImages(const fs::path &root, std::map<std::string, ImageDescriptor> &descriptors);
Bytes EncodePng(const Image &image);
Records PackFontImages(FontCorpus &corpus, const Files &files);
Image PackImages(const std::vector<const Image *> &images, const Json &layout);
std::vector<std::size_t> ImageOffsets(std::string_view kind, std::span<const unsigned char> body);
Json NativeLayouts(const fs::path &compiler, const std::vector<Json> &sizes, const fs::path &work);
Json ExactLayout(const Json &sizes, const Json &initial, const AtlasSettings &settings, const fs::path &cache,
                 const fs::path &seed);
void PackAtlas(const fs::path &manifest, const fs::path &compiler, const fs::path &output,
               const fs::path &cache);
Json SceneParse(std::string_view text);
Json CommonUI();
Json PrepareScenes(Json documents, const Json &author, const Files &files,
                   std::map<std::string, ImageDescriptor> &sources);
Records GenerateScenes(const Json &reports);
Json CompileScene(const Json &document, const Json &layout, const std::vector<std::string> &symbols,
                  std::string_view atlasPath, const std::map<std::string, Image> &images);
Records GenerateZombatar(const Json &document, const Json &report);
Bytes CompileProjectiles(const Json &document, const Json &layout, const std::vector<std::string> &symbols,
                         const std::map<std::string, Image> &images);

struct Definition
{
    Bytes bytes;
    Json atlas;
};
Definition CompileDefinition(std::string_view name, const Bytes &source, const Json &layout,
                             const std::vector<std::string> &symbols,
                             const std::map<unsigned, std::string> &names,
                             const std::map<std::string, ImageDescriptor> &descriptors, std::string_view path,
                             const std::map<std::string, Image> &images);
Definition ReanimVariants(const Bytes &source, const Json &metadata, const Json &variants,
                          const Json &actions, const std::map<unsigned, std::string> &names,
                          const std::map<std::string, ImageDescriptor> &descriptors);
Definition ParticleVariants(const Bytes &source, const std::vector<Definition> &variants);
Json ParseAtlas(const Bytes &data);
struct Generated
{
    Records files, runtime;
    Json summary;
};
Generated GenerateDefinitions(const Config &config, const Records &records, const Json &reports,
                              const fs::path &generated);
std::string AttacherMasks(std::string_view mphf, std::string_view tracks);
void Build(const fs::path &root, const fs::path &compiler, const fs::path &config, const fs::path &cache,
           const fs::path &generated);
} // namespace pvz::build
