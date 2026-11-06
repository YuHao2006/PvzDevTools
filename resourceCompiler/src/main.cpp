#include "reanim/Pak64.h"
#include "reanim/ParticleDsl.h"
#include "reanim/ProjectileDsl.h"
#include "reanim/StringIdGen.h"
#include "reanim/TrailDsl.h"
#include "reanim/AtlasPacking.h"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace fs = std::filesystem;
namespace pak64 = openpvz::reanim::pak64;
namespace particle = openpvz::reanim::particle;
namespace trail = openpvz::reanim::trail;
namespace stringid = openpvz::reanim::stringid;
using pak64::RawReanim;

namespace {

void PrintUsage()
{
    std::cout
        << "reanimc pack-atlas-layout --input BATCH --output LAYOUT\n"
        << "reanimc compile-projectile --input Projectiles.projectile.dsl --output FILE.json\n"
        << "    Compile projectile image paths, scales, and shadow crops to offline source metadata.\n"
        << "    Deterministic offline MaxRects batch layout; no rotation or pixels.\n"
        << "reanimc import --input SOURCE.reanim --output SOURCE.reanim.dsl\n"
        << "    Read a Legacy reanim XML source and emit the equivalent DSL.\n"
        << "reanimc compile --input SOURCE.reanim.dsl [--output DIR|FILE]\n"
        << "    Compile DSL to a byte-identical pak64 compiled record.\n"
        << "    Without --output the record is named after the module:\n"
        << "    export component Tallnut -> Tallnut.reanim.compiled next to\n"
        << "    the input; --output may be a directory (same naming) or an\n"
        << "    explicit file path.\n"
        << "reanimc compile-v2 --input DIR|FILE.dsl --output DIR\n"
        << "    Or --input-list FILE: definition paths selected by ResourceBuild.pvzbuild.\n"
        << "    [--header FILE] [--table FILE]\n"
        << "    Corpus compile with compile-time track-id interning: identical\n"
        << "    track names (case-folded) are replaced by one uint32 id (from 1;\n"
        << "    0 = not found, preserving the legacy fall-back-to-track-0 rule).\n"
        << "    With --font-registry, offline numeric source records are named\n"
        << "    <Module>.source-record. image_sources.txt lists their source-local\n"
        << "    image handles in sorted order (line 1 = handle 1; 0 = no image).\n"
        << "    These handles never use the game's image symbol registry.\n"
        << "    Offline v2 gold remains <Module>.reanim.v2.compiled, plus ReanimTrackId.h\n"
        << "    cpu:/gpu:/both: select storage for following tracks (default both).\n"
        << "    cpu/gpu/both before a Track selects storage for that declaration only.\n"
        << "    Non-both storage requires --font-registry and omits legacy v2 gold.\n"
        << "    (the string<->int enum) and reanim_track_ids.bin (sidecar id table).\n"
        << "    The header, table and every record come from the same build.\n"
        << "    With --type-map also emits ReanimTrackMphfGlobal.h (single global\n"
        << "    MPHF over the joint (type,id) key space of the whole corpus,\n"
        << "    plus the corpus type-membership bitmap).\n"
        << "reanimc inspect --input FILE.reanim.compiled [--table FILE]\n"
        << "    Parse a compiled record and print its structure summary.\n"
        << "    v2 records (cookie DEADFED6) additionally need --table with the\n"
        << "    matching reanim_track_ids.bin.\n"
        << "reanimc import-trail --input SOURCE.trail --output DIR/SOURCE.trail.dsl\n"
        << "    Read a Legacy trail XML source and emit the equivalent trail\n"
        << "    DSL (kind: trail). FloatTrack text is preserved verbatim inside\n"
        << "    track { ... } blocks and parsed at compile time.\n"
        << "reanimc compile-trail --input trails.root.dsl --output DIR\n"
        << "    [--verify FILE|DIR]\n"
        << "    Compile a trail DSL bundle (forward closure from the root file)\n"
        << "    to byte-identical pak64 records: <Module>.trail.compiled per\n"
        << "    module. --verify additionally compares each record against a\n"
        << "    gold file (a DIR is searched for <Module>.trail.compiled and\n"
        << "    the pak-style lower-cased name) and reports the first\n"
        << "    differing byte with its field attribution.\n"
        << "reanimc bake-strings --input LawnStrings.txt [--output-dir DIR]\n"
        << "    [--entity-tables] [--sources PATH]... [--rewrite] [--dry-run]\n"
        << "    Bake a LawnStrings.txt corpus into LawnStrings.h/.cpp:\n"
        << "    struct LawnStrings { SexyString <KEY>; ... } and a const gStr\n"
        << "    with values baked verbatim (placeholders like {FLAGS} survive).\n"
        << "    With --entity-tables, also generate EntityStrings.h/.cpp:\n"
        << "    contiguous 1..N numeric key families become indexed arrays\n"
        << "    (gCrazyDaveLines etc. with count constants) and any runtime key\n"
        << "    resolves through a sorted key table (EntityStringFind/Translate,\n"
        << "    byte-compatible with TodStringListFind/TodStringTranslate).\n"
        << "    With --sources, scan game sources and report/perform the call\n"
        << "    site rewrite _S(\"[KEY]\") -> gStr.KEY for verified-translating\n"
        << "    receivers; --rewrite writes back, --dry-run only previews.\n"
        << "reanimc import-particle --input DIR|FILE.xml --output DIR\n"
        << "    [--root particles.root.dsl]\n"
        << "    Batch-import Legacy particle XML to particle DSL (kind:\n"
        << "    particle; FloatTrack text preserved verbatim) as\n"
        << "    particles/<PascalName>.particle.dsl plus a root file whose\n"
        << "    forwards follow gLawnParticleArray order.\n"
        << "reanimc compile-particle --input particles.root.dsl --output DIR\n"
        << "    [--verify GOLD_DIR]\n"
        << "    Compile a particle DSL bundle (forward closure) to byte-\n"
        << "    identical pak64 records: <source-name>.xml.compiled per\n"
        << "    module. --verify compares each record against\n"
        << "    GOLD_DIR/<source-name>.xml.compiled, reporting the first\n"
        << "    differing byte with its field attribution plus the pass rate.\n"
        << "reanimc inspect-particle --input FILE.xml.compiled\n"
        << "    Parse a compiled particle record and print its structure\n"
        << "    summary (emitters, tracks, fields).\n"
        << "reanimc compile-particle-v2 --input particles.root.dsl --output DIR\n"
        << "    Or --input-list FILE: explicit particle source selection, without a root DSL.\n"
        << "    --gold64 GOLD_DIR [--verify GOLD64_DIR]\n"
        << "    Compile runtime-native v2 particle records (cookie DEADFED7):\n"
        << "    payload = 'IDP3' magic + the x64 runtime v1 payload (byte-identical\n"
        << "    to the main.pak64 compiled/particles record taken from --gold64)\n"
        << "    + an IDT3 section pre-linking each emitter's OnDuration to its\n"
        << "    in-definition emitter def index (stricmp fold, first declaration\n"
        << "    wins, 0xFFFFFFFF = none). The DSL bundle supplies the module list\n"
        << "    and a per-emitter Name/OnDuration consistency check. Writes\n"
        << "    <name>.xml.v2.compiled as offline gold per module, plus\n"
        << "    <name>.source-record and sorted image_sources.txt (line 1 =\n"
        << "    source-local image handle 1; 0 = no image).\n"
        << "    --verify re-compares the\n"
        << "    embedded v1 segment against GOLD64_DIR.\n";

}

fs::path ResolveOutput(const fs::path& output, const RawReanim& raw,
                       const fs::path& input)
{
    const std::string stem = raw.name.empty() ? input.stem().string() : raw.name;
    if (output.empty())
        return input.parent_path() / (stem + ".reanim.compiled");
    std::error_code ec;
    if (fs::is_directory(output, ec))
        return output / (stem + ".reanim.compiled");
    return output;
}

bool ReadFileText(const fs::path& path, std::string& text)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "reanimc: cannot open " << path.string() << '\n';
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

bool OutputMissing(const fs::path& path)
{
    if (path.empty() || fs::exists(path)) {
        std::cerr << "reanimc: output exists or is empty; choose a new path: "
                  << path.string() << '\n';
        return true;
    }
    return false;
}

bool WriteText(const fs::path& path, const std::string& text)
{
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        std::cerr << "reanimc: cannot write " << path.string() << '\n';
        return false;
    }
    return true;
}

int RunImport(const std::vector<std::string>& args)
{
    fs::path input, output;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (i + 1 >= args.size()) break;
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if (input.empty() || OutputMissing(output)) return 2;
    RawReanim raw;
    std::string error;
    if (!pak64::ReadRawXml(input, raw, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    if (!WriteText(output, pak64::ToDsl(raw))) return 1;
    std::cout << input.string() << " -> " << output.string() << " (tracks="
              << raw.tracks.size() << ")\n";
    return 0;
}

int RunCompile(const std::vector<std::string>& args)
{
    fs::path input, output;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (i + 1 >= args.size()) break;
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if (input.empty()) return 2;
    std::vector<RawReanim> modules;
    std::string error;
    if (input.extension() == ".reanim") {
        modules.emplace_back();
        if (!pak64::ReadRawXml(input, modules.back(), &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
    } else {
        std::string text;
        if (!ReadFileText(input, text)) return 1;
        if (!pak64::ParseModules(text, modules, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
    }
    std::error_code ec;
    const bool dirOutput = !output.empty() && fs::is_directory(output, ec);
    if (modules.size() > 1 && !output.empty() && !dirOutput) {
        std::cerr << "reanimc: explicit file output requires a single module (got "
                  << modules.size() << "); pass a directory instead\n";
        return 2;
    }
    for (const RawReanim& raw : modules) {
        const fs::path resolved = dirOutput
            ? output / (raw.name + ".reanim.compiled")
            : ResolveOutput(output, raw, input);
        if (OutputMissing(resolved)) return 2;
        if (!pak64::WriteCompiled(raw, resolved, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        std::cout << input.string() << " -> " << resolved.string()
                  << " (module=" << raw.name << ", tracks=" << raw.tracks.size()
                  << ", bytes=" << fs::file_size(resolved) << ")\n";
    }
    return 0;
}

int RunInspect(const std::vector<std::string>& args)
{
    fs::path input, tablePath;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--table") tablePath = args[i + 1];
        else { std::cerr << "reanimc: unknown option " << args[i] << '\n'; return 2; }
    }
    if (input.empty()) return 2;
    RawReanim raw;
    std::string error;
    std::vector<std::uint8_t> bytes;
    {
        std::ifstream in(input, std::ios::binary);
        if (!in) { std::cerr << "reanimc: cannot open " << input.string() << '\n'; return 1; }
        bytes.assign(std::istreambuf_iterator<char>(in), {});
    }
    bool parsed = false;
    std::uint32_t cookie = 0;
    if (bytes.size() >= 4) std::memcpy(&cookie, bytes.data(), 4);
    if (cookie == pak64::kV2Cookie) {
        if (tablePath.empty()) {
            std::cerr << "reanimc: v2 record requires --table reanim_track_ids.bin\n";
            return 2;
        }
        std::vector<pak64::TrackIdEntry> table;
        if (!pak64::ReadTrackIdTableFile(tablePath, table, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        parsed = pak64::ReadCompiledV2FromBytes(bytes, table, raw, &error);
    } else {
        parsed = pak64::ReadCompiledFromBytes(bytes, raw, &error);
    }
    if (!parsed) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    std::uint64_t frames = 0;
    for (const auto& track : raw.tracks) frames += track.frames.size();
    std::cout << input.filename().string() << ": fps=" << raw.fps
              << " tracks=" << raw.tracks.size() << " frames=" << frames << '\n';
    return 0;
}

int RunCompileV2(const std::vector<std::string>& args)
{
    fs::path input, inputList, output, header, tablePath, familyFile, typeMapPath;
    fs::path fontRegistryPath;
    if ((args.size()-1)%2) { std::cerr << "reanimc: option requires a value: " << args.back() << '\n'; return 2; }
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--input-list") inputList = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else if (args[i] == "--header") header = args[i + 1];
        else if (args[i] == "--table") tablePath = args[i + 1];
        else if (args[i] == "--family-file") familyFile = args[i + 1];
        else if (args[i] == "--type-map") typeMapPath = args[i + 1];
        else if (args[i] == "--font-registry") fontRegistryPath = args[i + 1];
        else { std::cerr << "reanimc: unknown option " << args[i] << '\n'; return 2; }
    }
    if ((input.empty() == inputList.empty()) || output.empty()) return 2;

    // 类型映射表（attacher 目标模块的折叠名 -> ReanimationType 数值，
    // 由嫁接仓库 tools/extract_reanim_type_map.py 从游戏源码生成）。
    std::vector<pak64::ReanimTypeMapEntry> typeMap;
    std::string typeMapError;
    if (!typeMapPath.empty() &&
        !pak64::LoadReanimTypeMap(typeMapPath, typeMap, &typeMapError)) {
        std::cerr << "reanimc: " << typeMapError << '\n';
        return 1;
    }

    // families：--family-file（每行一个前缀）与 DSL prefixStrs 的并集（首见序）。
    // 生成头为每个前缀产出编译期 id 区间常量（ReanimTrackRanges::<prefix>），
    // 运行期前缀匹配退化为两次整数比较。
    std::vector<std::string> families;
    if (!familyFile.empty()) {
        std::ifstream in(familyFile);
        if (!in) { std::cerr << "reanimc: cannot open " << familyFile.string() << '\n'; return 1; }
        std::string line;
        while (std::getline(in, line)) {
            while (!line.empty() && (line.back() == '\r' || line.back() == '\n')) line.pop_back();
            if (!line.empty() && line[0] != '#') families.push_back(line);
        }
    }

    // 收集语料：目录（排序遍历 *.reanim / *.dsl）或单文件。
    std::vector<RawReanim> corpus;
    std::string error;
    const auto addXml = [&](const fs::path& path) {
        corpus.emplace_back();
        if (!pak64::ReadRawXml(path, corpus.back(), &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return false;
        }
        return true;
    };
    const auto addDsl = [&](const fs::path& path) {
        std::string text;
        if (!ReadFileText(path, text)) return false;
        std::vector<RawReanim> modules;
        if (!pak64::ParseModules(text, modules, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return false;
        }
        for (RawReanim& module : modules) corpus.push_back(std::move(module));
        return true;
    };
    std::error_code ec;
    if (!inputList.empty()) {
        std::ifstream list(inputList);
        if (!list) { std::cerr << "reanimc: cannot open " << inputList.string() << '\n'; return 1; }
        std::string path;
        while (std::getline(list, path)) {
            if (!path.empty() && path.back() == '\r') path.pop_back();
            if (path.empty()) continue;
            const fs::path source(path);
            if (source.extension() != ".dsl" || !addDsl(source)) return 1;
        }
    } else if (fs::is_directory(input, ec)) {
        std::vector<fs::path> entries;
        for (const auto& entry : fs::directory_iterator(input))
            if (entry.path().extension() == ".reanim" || entry.path().extension() == ".dsl")
                entries.push_back(entry.path());
        std::sort(entries.begin(), entries.end());
        for (const fs::path& path : entries) {
            if (path.extension() == ".reanim" ? !addXml(path) : !addDsl(path)) return 1;
        }
    } else if (input.extension() == ".reanim") {
        if (!addXml(input)) return 1;
    } else {
        // 根文件模式：import/forward 闭包递归加载，prefixStrs 从 DSL 声明合并。
        pak64::DslBundle bundle;
        if (!pak64::LoadDslBundle(input, bundle, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        for (const RawReanim& module : bundle.modules) corpus.push_back(module);
        for (const std::string& prefix : bundle.prefixStrs)
            families.push_back(prefix);  // 与 --family-file 并集（首见序）
        std::cout << "bundle: files=" << bundle.files.size()
                  << " public=" << bundle.publicNames.size() << '\n';
    }
    if (corpus.empty()) {
        std::cerr << "reanimc: no modules found under " << input.string() << '\n';
        return 1;
    }

    try { pak64::ResolveReanimVariants(corpus); }
    catch (const std::exception& failure) { std::cerr << "reanimc: " << failure.what() << '\n'; return 1; }
    std::vector<RawReanim> roots;
    for (const auto& item : corpus) if (item.variantOf.empty()) roots.push_back(item);
    const std::vector<pak64::TrackIdEntry> table = pak64::BuildTrackIdTable(roots);
    fs::create_directories(output);
    if (!WriteText(output / "reanim_variants.json", pak64::MakeReanimVariantMetadata(corpus)) ||
        !WriteText(output / "ReanimVariantId.h", pak64::MakeReanimVariantHeader(corpus))) return 1;
    if (header.empty()) header = output / "ReanimTrackId.h";
    if (tablePath.empty()) tablePath = output / "reanim_track_ids.bin";
    if (!WriteText(header, pak64::MakeTrackIdHeader(table, families))) return 1;
    if (!pak64::WriteTrackIdTableFile(table, tablePath, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    // 全局单表 MPHF 头（唯一 MPHF 层，每类型 ReanimTrackMphf.h 已退役不再
    // 生成）：全语料 (type,id) 联合键 -> 轨道下标一张总表（求解含穷举
    // (type,id) 自校验，不一致报错退出），尾部附语料成员位图
    // kGlobalTypeBits + MphfGlobalTypeInCorpus。
    if (typeMap.empty()) {
        std::cerr << "reanimc: compile-v2 requires --type-map to emit ReanimTrackMphfGlobal.h\n";
        return 1;
    }
    const fs::path mphfGlobalHeader = output / "ReanimTrackMphfGlobal.h";
    if (!WriteText(mphfGlobalHeader,
                   pak64::MakeTrackMphfGlobalHeader(roots, table, typeMap)))
        return 1;

    // Images are source-local names, independent of every game image registry.
    pak64::SourceImageHandles sourceImageHandles;
    for (const auto& definition:corpus)
    {
        for (const auto& track:definition.tracks)
            for (const auto& frame:track.frames)
                if (!frame.image.empty()) sourceImageHandles.emplace(frame.image,0);
        for (const auto& images : definition.variantImages)
            for (const auto& image : images)
                if (!image.empty()) sourceImageHandles.emplace(image, 0);
    }
    std::string imageSources;
    std::uint32_t nextImageHandle=1;
    for (auto& [name,handle]:sourceImageHandles) {
        if (name.find_first_of("\r\n")!=std::string::npos || name.find('\0')!=std::string::npos) {
            std::cerr << "reanimc: image source names cannot contain line breaks or NUL\n"; return 1;
        }
        handle=nextImageHandle++;
        imageSources+=name+'\n';
    }
    if (!WriteText(output / "image_sources.txt",imageSources)) return 1;
    pak64::FontIdRegistry fontSymbols;
    std::uint32_t fontRegistryHash = 0;
    const bool emitSourceRecords = !fontRegistryPath.empty();
    for (const RawReanim& raw : roots)
        for (const auto& track : raw.tracks)
            if (track.storage != pak64::TrackStorage::both && !emitSourceRecords) {
                std::cerr << "reanimc: track storage requires compile-v2 with --font-registry\n";
                return 1;
            }
    if (!fontRegistryPath.empty() &&
        !pak64::LoadFontIdRegistry(fontRegistryPath, fontSymbols, &fontRegistryHash, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }

    std::uint64_t v2Bytes = 0, v4Bytes = 0, v1Bytes = 0, trackSlots = 0;
    for (const RawReanim& raw : roots) {
        const bool legacyStorage = std::all_of(raw.tracks.begin(), raw.tracks.end(),
            [](const auto& track) { return track.storage == pak64::TrackStorage::both; });
        const fs::path record = output / (raw.name + ".reanim.v2.compiled");
        if (legacyStorage) {
            if (OutputMissing(record)) return 2;
            if (!pak64::WriteCompiledV2(raw, table, typeMap, record, &error)) {
                std::cerr << "reanimc: " << error << '\n';
                return 1;
            }
            v2Bytes += fs::file_size(record);
            v1Bytes += pak64::Serialize(raw).size();
        }
        if (emitSourceRecords) {
            const fs::path v4record = output / (raw.name + ".source-record");
            if (OutputMissing(v4record)) return 2;
            if (!pak64::WriteCompiledV4(raw, table, typeMap, sourceImageHandles, fontSymbols,
                                        fontRegistryHash, v4record, &error)) {
                std::cerr << "reanimc: " << error << '\n';
                return 1;
            }
            v4Bytes += fs::file_size(v4record);
            std::cout << raw.name << " -> " << v4record.string()
                      << " (offline source handles, bytes=" << fs::file_size(v4record) << ")\n";
        }
        trackSlots += raw.tracks.size();
        if (legacyStorage)
            std::cout << raw.name << " -> " << record.string() << " (tracks="
                      << raw.tracks.size() << ", bytes=" << fs::file_size(record) << ")\n";
    }
    std::cout << "corpus: modules=" << corpus.size() << " trackSlots=" << trackSlots
              << " trackIds=" << table.size() << " (ids 1.." << table.size() << ", 0=not-found)"
              << " v2=" << v2Bytes << "B vs v1=" << v1Bytes << "B\n";
    std::cout << "generated: " << header.string() << ", " << tablePath.string()
              << ", " << mphfGlobalHeader.string()
              << " (" << fs::file_size(mphfGlobalHeader) << "B)";
    std::cout << '\n';
    return 0;
}

int RunImportTrail(const std::vector<std::string>& args)
{
    fs::path input, output;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if (input.empty() || OutputMissing(output)) return 2;
    trail::RawTrail raw;
    std::string error;
    if (!trail::ReadTrailXml(input, raw, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    if (!WriteText(output, trail::TrailToDsl(raw))) return 1;
    std::cout << input.string() << " -> " << output.string() << " (image=" << raw.image
              << ", maxPoints=" << raw.maxPoints
              << ", loops=" << (raw.loops ? 1 : 0) << ")\n";
    return 0;
}

int RunCompileTrail(const std::vector<std::string>& args)
{
    fs::path input, output, verify;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else if (args[i] == "--verify") verify = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if (input.empty() || output.empty()) return 2;

    trail::TrailDslBundle bundle;
    std::string error;
    if (!trail::LoadTrailDslBundle(input, bundle, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    fs::create_directories(output);

    const auto readBytes = [](const fs::path& path, std::vector<unsigned char>& bytes) {
        std::ifstream in(path, std::ios::binary);
        if (!in) return false;
        bytes.assign(std::istreambuf_iterator<char>(in), {});
        return true;
    };
    const auto findGold = [&](const std::string& name, fs::path& gold) {
        std::string lower = name;
        std::transform(lower.begin(), lower.end(), lower.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        for (const std::string& stem : {name, lower}) {
            for (const char* suffix : {".trail.compiled", ".compiled"}) {
                const fs::path candidate = verify / (stem + suffix);
                if (fs::exists(candidate)) {
                    gold = candidate;
                    return true;
                }
            }
        }
        return false;
    };

    std::size_t verified = 0, mismatches = 0;
    for (const trail::RawTrail& raw : bundle.modules) {
        const fs::path record = output / (raw.name + ".trail.compiled");
        if (OutputMissing(record)) return 2;
        std::vector<std::string> labels;
        std::string writeError;
        if (!trail::WriteTrailCompiled(raw, record, &writeError,
                                        verify.empty() ? nullptr : &labels)) {
            std::cerr << "reanimc: " << writeError << '\n';
            return 1;
        }
        std::cout << raw.name << " -> " << record.string()
                  << " (bytes=" << fs::file_size(record) << ")\n";

        if (verify.empty()) continue;
        fs::path gold;
        if (fs::is_directory(verify)) {
            if (!findGold(raw.name, gold)) {
                std::cerr << "reanimc: verify: no gold record for <" << raw.name
                          << "> under " << verify.string() << '\n';
                return 1;
            }
        } else {
            gold = verify;
        }
        std::vector<unsigned char> goldBytes;
        if (!readBytes(gold, goldBytes)) {
            std::cerr << "reanimc: verify: cannot open " << gold.string() << '\n';
            return 1;
        }
        std::vector<unsigned char> mine;
        if (!readBytes(record, mine)) return 1;
        if (mine == goldBytes) {
            ++verified;
            std::cout << "verify: " << record.filename().string() << " == "
                      << gold.string() << " (" << mine.size() << " bytes, identical)\n";
            continue;
        }
        ++mismatches;
        const std::size_t common = std::min(mine.size(), goldBytes.size());
        std::size_t diff = common;
        for (std::size_t i = 0; i < common; ++i)
            if (mine[i] != goldBytes[i]) {
                diff = i;
                break;
            }
        std::cerr << "verify: MISMATCH " << record.filename().string() << " vs "
                  << gold.string() << " (mine " << mine.size() << "B, gold "
                  << goldBytes.size() << "B)\n";
        if (diff < common) {
            std::cerr << "verify: first difference at byte " << diff << " (0x"
                      << std::hex << diff << std::dec << "): mine=0x" << std::hex
                      << static_cast<unsigned>(mine[diff]) << std::dec
                      << " gold=0x" << std::hex << static_cast<unsigned>(goldBytes[diff])
                      << std::dec << '\n';
            if (diff < labels.size())
                std::cerr << "verify: byte " << diff << " belongs to: "
                          << labels[diff] << '\n';
            else if (diff >= mine.size())
                std::cerr << "verify: byte " << diff << " is beyond my record\n";
        } else {
            std::cerr << "verify: records agree on the common prefix; sizes differ (mine "
                      << mine.size() << " vs gold " << goldBytes.size() << ")\n";
        }
    }
    if (!verify.empty()) {
        std::cout << "verify: " << verified << "/" << bundle.modules.size()
                  << " records byte-identical\n";
        return mismatches == 0 ? 0 : 1;
    }
    return 0;
}

int RunBakeStrings(const std::vector<std::string>& args)
{
    fs::path input, outputDir;
    std::vector<fs::path> sources;
    bool rewrite = false, dryRun = false, entityTables = false;
    for (std::size_t i = 1; i < args.size(); i++) {
        const std::string& arg = args[i];
        if (arg == "--input" && i + 1 < args.size()) input = args[++i];
        else if (arg == "--output-dir" && i + 1 < args.size()) outputDir = args[++i];
        else if (arg == "--sources" && i + 1 < args.size()) sources.push_back(args[++i]);
        else if (arg == "--rewrite") rewrite = true;
        else if (arg == "--dry-run") dryRun = true;
        else if (arg == "--entity-tables") entityTables = true;
        else {
            std::cerr << "reanimc: unknown option " << arg << '\n';
            return 2;
        }
    }
    if (input.empty()) return 2;

    std::vector<stringid::BakedString> strings = stringid::ParseLawnStringsFile(input);
    if (strings.empty()) {
        std::cerr << "reanimc: no entries parsed from " << input.string() << '\n';
        return 1;
    }
    std::cout << "corpus: " << input.string() << " keys=" << strings.size() << '\n';

    if (!outputDir.empty() && !dryRun) {
        const fs::path header = outputDir / "LawnStrings.h";
        const fs::path source = outputDir / "LawnStrings.cpp";
        if (!WriteText(header, stringid::MakeBakedHeader(strings))) return 1;
        if (!WriteText(source, stringid::MakeBakedSource(strings))) return 1;
        std::cout << "generated: " << header.string() << ", " << source.string() << '\n';
    }

    if (entityTables) {
        const std::vector<stringid::EntityFamily> families = stringid::CollectNumericFamilies(strings, true);
        for (const stringid::EntityFamily& family : families) {
            std::cout << "entity-family: " << family.prefix << " count=" << family.count
                      << " array=" << family.arrayName << '\n';
        }
        if (!outputDir.empty() && !dryRun) {
            const fs::path header = outputDir / "EntityStrings.h";
            const fs::path source = outputDir / "EntityStrings.cpp";
            if (!WriteText(header, stringid::MakeEntityHeader(strings))) return 1;
            if (!WriteText(source, stringid::MakeEntitySource(strings))) return 1;
            std::cout << "generated: " << header.string() << ", " << source.string() << '\n';
        }
    }

    if (!sources.empty()) {
        std::set<std::string> keys;
        for (const stringid::BakedString& s : strings) keys.insert(s.key);
        stringid::RewriteReport report;
        const bool apply = rewrite && !dryRun;
        stringid::RewriteTranslateCallsites(sources, keys, apply, report);

        std::cout << "scan: files=" << report.filesScanned
                  << " bracketLiterals=" << report.bracketLiteralUses
                  << " dynamicKeys=" << report.dynamicKeyUses << '\n';
        std::cout << "rewrite: " << (apply ? "applied" : (rewrite ? "preview" : "report-only"))
                  << " edits=" << report.edits
                  << " (TodStringTranslate direct args=" << report.translatedCallArgs << ")\n";
        for (const auto& kv : report.editsByCallee)
            std::cout << "  " << kv.first << ": " << kv.second << '\n';
        if (!report.residualWholeLiteralByCallee.empty()) {
            std::cout << "residual whole-literal args (whitelisted callee, not rewritten):\n";
            for (const auto& kv : report.residualWholeLiteralByCallee)
                if (kv.second > 0)
                    std::cout << "  " << kv.first << ": " << kv.second << '\n';
        }
        if (!report.unknownReceiverLiterals.empty()) {
            std::cout << "bracket literals on non-whitelisted receivers (left as-is):\n";
            for (const auto& kv : report.unknownReceiverLiterals)
                std::cout << "  " << kv.first << ": " << kv.second << '\n';
        }
        if (!report.sourceKeysMissingFromTxt.empty()) {
            std::cout << "keys referenced in sources but missing from txt (left as-is):\n";
            for (const std::string& k : report.sourceKeysMissingFromTxt)
                std::cout << "  [" << k << "]\n";
        }
        if (!report.residualLocations.empty()) {
            std::cout << "residual locations (file:line: [key] -> receiver):\n";
            for (const std::string& loc : report.residualLocations)
                std::cout << "  " << loc << '\n';
        }
    }
    return 0;
}

} // namespace {

int main(int argc, char** argv)
{
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.size() < 2) {
        PrintUsage();
        return 2;
    }
    // 未捕获异常诊断：构建期 Fail 抛 std::runtime_error，带消息退出而非静默 abort。
    try {
        if (args[0] == "pack-atlas-layout") return openpvz::reanim::atlas::RunPackAtlasLayout(args);
        if (args[0] == "compile-projectile") return openpvz::reanim::projectile::RunCompileProjectile(args);
        if (args[0] == "import") return RunImport(args);
        if (args[0] == "compile") return RunCompile(args);
        if (args[0] == "compile-v2") return RunCompileV2(args);
        if (args[0] == "inspect") return RunInspect(args);
        if (args[0] == "import-trail") return RunImportTrail(args);
        if (args[0] == "compile-trail") return RunCompileTrail(args);
        if (args[0] == "bake-strings") return RunBakeStrings(args);
        if (args[0] == "import-particle") return particle::RunImportParticle(args);
        if (args[0] == "compile-particle") return particle::RunCompileParticle(args);
        if (args[0] == "compile-particle-v2") return particle::RunCompileParticleV2(args);
        if (args[0] == "inspect-particle") return particle::RunInspectParticle(args);
    } catch (const std::exception& e) {
        std::cerr << "reanimc: " << e.what() << '\n';
        return 1;
    }
    PrintUsage();
    return 2;
}
