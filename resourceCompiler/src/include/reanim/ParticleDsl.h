#pragma once

// particle 定义的 DSL 管线：XML -> DSL 导入（import-particle）与
// DSL -> 字节级等价的 pak64 编译记录（compile-particle / inspect-particle）。
//
// 记录容器格式（与 reanim v1 / trail 相同，见 Pak64.h）：
//   uint32 LE cookie      = 0xDEADFED4
//   uint32 LE payloadSize = 载荷字节数
//   byte   0x78, 0x01     （zlib 头）
//   deflate STORED 块     （<=65535 分块，末块 BFINAL=1）
//   uint32 LE adler32(payload)  -- 小端，非标准 zlib 大端
//
// 载荷（对安装包 compiled/particles/*.xml.compiled 共 112 个记录逐字节逆向，
// XML->payload 以 112/112 字节一致收口；结构为历史转换器的实际写入布局，
// 以安装包记录为金标准复刻）：
//   uint32 LE schema hash = 3767703017
//   16 字节 TodParticleDefinition 转储（x64 槽位）：[0:8) 指针槽=0，
//     [8:12) emitterCount，[12:16) 填充=0
//   uint32 704（emitter 元素大小，历史布局；指针/轨道计数槽恒为零）
//   emitterCount * 704 字节 emitter 转储，非零槽位只有（int，LE）：
//     [4]   ImageCol     [8]  ImageRow     [12] ImageFrames（缺省 1）
//     [16]  Animated     [20] ParticleFlags（bit 见下）  [24] EmitterType（缺省 1=Box）
//     [404] particleFieldCount   [420] systemFieldCount
//   每 emitter 尾区（顺序固定，数组计数只存在于转储、不写入流）：
//     Image 字符串、Name 字符串、OnDuration 字符串（各 uint32 len + 字节）
//     22 条系统轨道（SystemDuration CrossFadeDuration SpawnRate SpawnMinActive
//       SpawnMaxActive SpawnMaxLaunched EmitterRadius EmitterOffsetX
//       EmitterOffsetY EmitterBoxX EmitterBoxY EmitterPath EmitterSkewX
//       EmitterSkewY ParticleDuration SystemRed SystemGreen SystemBlue
//       SystemAlpha SystemBrightness LaunchSpeed LaunchAngle）：
//       各 uint32 节点数 + 节点*20 字节
//     Field 数组：uint32 40 + count*40 字节转储（仅 [0]=FieldType 非零）
//       + 每 field 的 x 轨道、y 轨道
//     SystemField 数组：同上
//     16 条粒子轨道（ParticleRed ParticleGreen ParticleBlue ParticleAlpha
//       ParticleBrightness ParticleSpinAngle ParticleSpinSpeed ParticleScale
//       ParticleStretch CollisionReflect CollisionSpin ClipTop ClipBottom
//       ClipLeft ClipRight AnimationRate）
//   节点 { float mTime, mLowValue, mHighValue; int mCurveType, mDistribution; }
//     TodCurves：LINEAR=1 EASE_IN=2 EASE_OUT=3 EASE_IN_OUT=4 EASE_IN_OUT_WEAK=5
//     FAST_IN_OUT=6 FAST_IN_OUT_WEAK=7 BOUNCE=9 BOUNCE_FAST_MIDDLE=10
//     BOUNCE_SLOW_MIDDLE=11 SIN_WAVE=12 EASE_SIN_WAVE=13
//
// 枚举（安装包数值）：EmitterType Circle=0 Box=1 BoxPath=2 CirclePath=3
//   CircleEvenSpacing=4；FieldType Friction=1 Acceleration=2 Attractor=3
//   MaxVelocity=4 Velocity=5 Position=6 SystemPosition=7 GroundConstraint=8
//   Shake=9 Circle=10 Away=11；ParticleFlags 位 = gParticleFlagSymbols
//   （RandomLaunchSpin=0 AlignLaunchSpin=1 AlignToPixel=2 SystemLoops=3
//   ParticleLoops=4 ParticlesDontFollow=5 RandomStartTime=6 DieIfOverloaded=7
//   Additive=8 FullScreen=9 SoftwareOnly=10 HardwareOnly=11）。
//   历史行为：flag 元素只要出现即置位（元素值被忽略，与游戏端修复后不同）。
//
// FloatTrack 文本解析（oracle 语义，与安装记录逐字节一致；语料例：
//   "1,80 0"->1 节点、"2 4 6 2 0 8 2 6 4"->5 节点、"0 [20 300],60 [25 310]"
//   ->[(0,0),(0.6,300),(0.6,310)]、"[.3.8],80 0"->high 保持 .3）：
//   - 数值节点 "v" 或 "v,t"（t 为百分数：float32(v) * float32(0.01)）；
//     数值后若紧跟 ']' 则先跳过（吞掉被丢弃的 "[lo" token 的残留括号）；
//   - 范围节点 "[lo]"、"[lo hi]"、"[lo Dist hi]"（高位值仅在前导空格后
//     读取；无空格时保持 = lo），可带 ",t" 时间后缀；
//   - 数值节点后的下一个空白分隔 token 当作曲线名消费：命中曲线表则赋给
//     当前节点（CURVE_*），否则整个 token 被丢弃（"1 .5 1" -> 节点 1 和 1，
//     ".5" 被吞）。范围节点不消费后续 token；
//   - 未指定时间的节点按区间规则补齐：high*interp + low*(1-interp)，
//     除数为零时 interp=0（尾段节点时间保持为最后一个锚点时间，
//     与游戏端修复后 interp=1 不同）。

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openpvz::reanim::particle {

inline constexpr std::uint32_t kParticleCookie = 0xDEADFED4u;
inline constexpr std::uint32_t kParticleSchemaHash = 3767703017u;
inline constexpr std::uint32_t kParticleEmitterDumpSize = 704;
inline constexpr std::uint32_t kParticleFieldDumpSize = 40;

// 轨道槽位序（= 安装包 oracle 写入序；读取端 gEmitterDefFields 表序
// 的 OnDuration/SystemDuration 位置与之互换，hash 不受影响）。
inline constexpr int kSystemTrackCount = 22;
inline constexpr int kParticleTrackCount = 16;
inline constexpr int kFlagCount = 12;

const char* const kSystemTrackNames[kSystemTrackCount] = {
    "SystemDuration", "CrossFadeDuration", "SpawnRate", "SpawnMinActive",
    "SpawnMaxActive", "SpawnMaxLaunched", "EmitterRadius", "EmitterOffsetX",
    "EmitterOffsetY", "EmitterBoxX", "EmitterBoxY", "EmitterPath",
    "EmitterSkewX", "EmitterSkewY", "ParticleDuration", "SystemRed",
    "SystemGreen", "SystemBlue", "SystemAlpha", "SystemBrightness",
    "LaunchSpeed", "LaunchAngle"};
const char* const kParticleTrackNames[kParticleTrackCount] = {
    "ParticleRed", "ParticleGreen", "ParticleBlue", "ParticleAlpha",
    "ParticleBrightness", "ParticleSpinAngle", "ParticleSpinSpeed",
    "ParticleScale", "ParticleStretch", "CollisionReflect", "CollisionSpin",
    "ClipTop", "ClipBottom", "ClipLeft", "ClipRight", "AnimationRate"};

struct TrackNode final {
    float time = 0.0f;
    float low = 0.0f;
    float high = 0.0f;
    std::int32_t curve = 1;          // TodCurves::CURVE_LINEAR
    std::int32_t distribution = 1;   // TodCurves::CURVE_LINEAR
};

struct ParticleField final {
    std::int32_t fieldType = -1;
    std::string x;  // FloatTrack 原文（空 = 未提供，计数写 0）
    std::string y;
};

struct ParticleEmitter final {
    std::string image;        // 记录里保存 trim 后文本；空 = 无图像
    std::string name;
    std::string onDuration;
    std::int32_t imageCol = 0;
    std::int32_t imageRow = 0;
    // Ordered source images, one full image per drawable frame. Repeats matter.
    std::vector<std::string> images;
    std::int32_t imageFrames = 1;  // 官方构造默认 1
    std::int32_t animated = 0;
    std::uint32_t flags = 0;
    std::int32_t emitterType = 1;  // 官方构造默认 EMITTER_BOX
    // FloatTrack 原文，按 kSystemTrackNames / kParticleTrackNames 槽位序。
    std::string systemTracks[kSystemTrackCount];
    std::string particleTracks[kParticleTrackCount];
    std::vector<ParticleField> particleFields;
    std::vector<ParticleField> systemFields;
};

struct ParticleDefinition final {
    // 模块名来自 `export component <name>`，仅命名 DSL 文件
    // （MelonImpact -> particles/MelonImpact.particle.dsl）。
    std::string name;
    // 源记录名（小写主名，如 "melonimpact"），编译产物命名为
    // <source-name>.xml.compiled。
    std::string sourceName;
    std::vector<ParticleEmitter> emitters;
    // Compile-time image variant. Resolved after collecting the complete bundle;
    // retained only so the compiler can reuse the original runtime gold layout.
    std::string variantOf;
    std::string variantImage;
    std::vector<std::vector<std::string>> variantImages;
};

// 读取 legacy particle XML（<Emitter> 列表；子标签 Image/ImageRow/ImageCol/
// ImageFrames/Animated/EmitterType/Name/OnDuration/flag 标签/38 条轨道标签/
// Field/SystemField，未知标签报错）。
[[nodiscard]] bool ReadParticleXml(const std::filesystem::path& input,
                                   ParticleDefinition& output,
                                   std::string* error = nullptr);

// 模块名：源文件主名按 '_' 分段、逐段首字母大写（melonimpact ->
// MelonImpact；用 gLawnParticleArray 的原始大小写文件名，见 .cpp）。
[[nodiscard]] std::string ParticleModuleName(std::string_view sourceName);

[[nodiscard]] std::string ParticleToDsl(const ParticleDefinition& source);

// ------------------------------------------------------- 模块系统：import/forward
//
// 文件级语法与 reanim/trail DSL 相同：
//   import "relative/path.dsl";     // 引入编译，不对外暴露
//   forward "relative/path.dsl";    // 引入编译，并转发其公开部件
//   export component A { ... }      // 对外开放
// 模块体字段：
//   kind: particle;                 // 必填，且必须为 particle
//   source-name: "melonimpact";
//   Emitter { ... }                 // 每 XML <Emitter> 一条块
// Image-only variants replace Emitter blocks with:
//   variant-of: BaseComponent;      // may appear later or in another input file
//   images: [["frame0.png", "frame1.png"], ["other.png"]]; // one list per emitter
// The base definition remains in the output; variants may inherit variants.
// Emitter 体字段（XML 标签名折叠小驼峰）：
//   image: "IMAGE_MELONPULT_PARTICLES";   // 或 none
//   name: "AwardRay8";
//   onDuration: "...";
//   images: ["frame0.png", "frame1.png"]; // ordered full-image frames
//   imageFrames/animated: <int>; // logical frame count, independent of image count
//   emitterType: Circle|Box|BoxPath|CirclePath|CircleEvenSpacing;
//   <flag>: 1;   // randomLaunchSpin/additive/... 出现即置位（值被忽略）
//   <track>: 200;                     // 单数值原文
//   <track>: range { 5 30; };         // 单区间原文（无括号）
//   <track>: track { 1,80 0; };       // 其余原文（区间/逗号/曲线/多节点）
//   field { fieldType: Friction; x: ...; y: ...; }
//   systemField { fieldType: SystemPosition; ... }

struct ParticleDslFileDirectives final {
    std::vector<std::string> imports;   // 按声明顺序
    std::vector<std::string> forwards;
    std::vector<ParticleDefinition> modules;
    std::vector<std::string> publicNames;  // 本文件 export 的模块名
};

[[nodiscard]] bool ParseParticleDslFileText(std::string_view source,
                                            ParticleDslFileDirectives& file,
                                            std::string* error = nullptr,
                                            bool legacy = false);

struct ParticleDslBundle final {
    std::vector<ParticleDefinition> modules;  // 闭包内全部模块，按模块名排序
    std::vector<std::string> publicNames;     // 根文件对外集
    std::vector<std::filesystem::path> files; // 实际加载的文件（含根）
};

[[nodiscard]] bool LoadParticleDslBundle(const std::filesystem::path& rootFile,
                                         ParticleDslBundle& bundle,
                                         std::string* error = nullptr,
                                         bool legacy = false);

// ---------------------------------------------------------------- 序列化

// legacy FloatTrack 解析（oracle 语义，见文件头注释）。
[[nodiscard]] bool ParseLegacyFloatTrack(std::string_view text,
                                         std::vector<TrackNode>& nodes,
                                         std::string* error = nullptr);

// 产出与安装包记录逐字节一致的完整记录（含容器包装）。
// labels 非空时返回逐字节标注（verify 差异定位用），与记录字节等长。
[[nodiscard]] std::vector<std::uint8_t> SerializeParticle(
    const ParticleDefinition& source, std::vector<std::string>* labels = nullptr);

[[nodiscard]] bool WriteParticleCompiled(const ParticleDefinition& source,
                                         const std::filesystem::path& output,
                                         std::string* error = nullptr,
                                         std::vector<std::string>* labels = nullptr);

// ---------------------------------------------------------------- 读取端

struct EmitterRecord final {
    std::string image, name, onDuration;
    std::int32_t imageCol = 0, imageRow = 0, imageFrames = 0, animated = 0;
    std::uint32_t flags = 0;
    std::int32_t emitterType = 0;
    std::vector<std::vector<TrackNode>> systemTracks;  // 按 kSystemTrackNames
    std::vector<std::vector<TrackNode>> particleTracks;
    std::vector<ParticleField> particleFields;   // fieldType（x/y 为空）
    std::vector<ParticleField> systemFields;
    // 与上面字段一一对应的 x/y 解析轨道。
    std::vector<std::pair<std::vector<TrackNode>, std::vector<TrackNode>>>
        particleFieldNodes;
    std::vector<std::pair<std::vector<TrackNode>, std::vector<TrackNode>>>
        systemFieldNodes;
};

struct ParticleRecord final {
    std::uint32_t schemaHash = 0;
    int emitterCount = 0;
    std::vector<EmitterRecord> emitters;
};

// 解析安装包 .xml.compiled（含容器）；供 inspect 与诊断使用。
[[nodiscard]] bool ReadParticleCompiled(const std::filesystem::path& input,
                                        ParticleRecord& record,
                                        std::string* error = nullptr);

[[nodiscard]] std::string InspectParticleText(const ParticleRecord& record,
                                              const std::string& fileName);

// ------------------------------------------------ v2：运行时记录 + OnDuration 预链接
//
// v2 记录面向游戏运行时直读装载（TodParticleLoadADef 的 v2 路径）：
//   uint32 LE cookie      = 0xDEADFED7
//   uint32 LE payloadSize = 载荷字节数
//   uint32 LE crc32(payload)   -- 标准 crc32，固定初值 0（与 reanim v2 相同）
//   payload:
//     uint32 LE magic     = 0x33504449 ("IDP3")
//     <v1 载荷>  与 pak64 运行时记录（main.pak64 内 compiled\particles\*
//                的 DEADFED5 载荷段）逐字节一致：schema hash 3767703017 +
//                16 字节 TodParticleDefinition 转储（count@8）+ u32 688 +
//                N*688 字节 TodEmitterDefinition 转储（x64 布局：ImageCol@8
//                ImageRow@12 ImageFrames@16 Animated@20 ParticleFlags@24
//                EmitterType@28 particleFieldCount@408 systemFieldCount@424）
//                + 尾区（Image 字符串、Name 字符串、SystemDuration 轨道、
//                  OnDuration 字符串、21 条系统轨道（CrossFadeDuration..
//                  LaunchAngle）、Field/SystemField 数组（u32 40 + count*40
//                  转储 + 逐 field x/y 轨道）、16 条粒子轨道）
//     IDT3 段（OnDuration 编译期预链接）：
//       uint32 LE emitterDefCount（与 v1 顶转储一致，运行时做交叉校验）
//       每 emitter def 一个 uint32 LE onDurationTarget：
//         解析到的同名发射器 def 下标；无 OnDuration 或未命中 = 0xFFFFFFFF
//
// 注意：v1 段使用 x64 运行时布局（688），不是 compile-particle 复刻的
// 32 位安装记录布局（704）。DSL 语料按 32 位安装记录语义复刻（flag 元素
// 出现即置位、旧 FloatTrack 补齐），与 pak64 运行时记录（DefinitionReadFlag
// Field 的值语义、修复后的补齐）存在实际语料分歧（如 Award.xml 的
// RandomLaunchSpin=0），从 DSL 重序列化会引入行为回归；因此 v2 记录以
// 运行时金标准（pak64 提取）的载荷为 v1 段来源，DSL 束负责模块清单与
// Name/OnDuration 一致性校验。IDT3 解析语义与运行时
// TodParticleSystem::FindEmitterDefByName 对齐：stricmp 折叠、按 def 声明
// 序取第一。

inline constexpr std::uint32_t kParticleV2Cookie = 0xDEADFED7u;
inline constexpr std::uint32_t kParticleV2PayloadMagic = 0x33504449u;  // 'IDP3'
// Compact particle records carry scalar integers and raw 20-byte track nodes.
// IDR4 source header: zero registry hash, emitter-name hash, count, body size.
// Each emitter: image count, ordered source handles (including repeats),
// logical imageFrames, animated, flags, type, then unchanged gold tracks/fields.
// IDT3 carries declaration-order OnDuration indices; IDT5 carries numeric name IDs.
inline constexpr std::uint32_t kParticleV4Cookie = 0xDEADFED8u;
inline constexpr std::uint32_t kParticleV4PayloadMagic = 0x34524449u;  // 'IDR4'
inline constexpr std::uint32_t kParticleOnDurationNone = 0xFFFFFFFFu;
inline constexpr std::uint32_t kParticleEmitterNativeDumpSize = 688;

// 读取运行时金标准记录并返回其 v1 载荷：支持 main.pak64 内的
// DEADFED5 原始记录，以及安装包样式的 DEADFED4 + stored 块 + 小端 adler
// 记录（解出的载荷即运行时等价物）。zlib 真压缩外壳（compiled64-v1 的
// 78 9c 流）不被支持：本工具不链接 zlib。
[[nodiscard]] bool ReadParticleRuntimePayload(const std::filesystem::path& input,
                                              std::vector<std::uint8_t>& payload,
                                              std::string* error = nullptr);

// 从运行时载荷尾区提取每 emitter 的 (Name, OnDuration)（严格走完整个
// 载荷，任何偏差报错）。imageFields 非空时按装载序记录每 emitter 三元组
// {image, name, onDuration} 的 (载荷偏移, 文本)——v4 外科手术的替换点
//（用于旧金标准的编译期解码，字符串不进入新记录）。
[[nodiscard]] bool ExtractParticleEmitterStrings(
    const std::vector<std::uint8_t>& payload,
    std::vector<std::pair<std::string, std::string>>& emitterStrings,
    std::string* error = nullptr,
    std::vector<std::pair<std::size_t, std::string>>* imageFields = nullptr);

// OnDuration -> def 下标预链接（返回每 emitter 一个 u32）。
[[nodiscard]] std::vector<std::uint32_t> ResolveParticleOnDurationTargets(
    const std::vector<std::pair<std::string, std::string>>& emitterStrings);

[[nodiscard]] std::vector<std::uint8_t> SerializeParticleV2(
    const std::vector<std::uint8_t>& runtimePayload,
    const std::vector<std::uint32_t>& onDurationTargets);

// Emit offline source-record intermediates using gold track bytes and parsed
// DSL image names. sourceImageHandles indexes sorted image_sources.txt only.
[[nodiscard]] std::vector<std::uint8_t> SerializeParticleV4(
    const std::vector<std::uint8_t>& runtimePayload,
    const ParticleDefinition& source,
    const std::map<std::string, std::uint32_t>& sourceImageHandles,
    const std::vector<std::uint32_t>& onDurationTargets,
    const std::vector<std::uint32_t>& nameIds,
    std::uint32_t nameTableHash);

// ---------------------------------------------------------------------------
// 发射器名字语料表（2026-10-09 用户设计，镜像 reanim 轨道名模式）：
// 全语料 emitter 名折叠（stricmp 语义）去重后按字典序赋 id（1 基），0=未名。
// 编译期发射 ParticleEmitterName.h（enum + 表 + 表哈希）；v4 记录尾部追加
// IDT5 段：[u32 count][u32 表哈希][u32 nameId × count]。运行时按整数比对，
// 零字符串。
struct ParticleEmitterNameEntry
{
    std::uint32_t id = 0;        // 1 基；0 保留给未名
    std::string folded;          // 折叠拼写（小写，stricmp 语义）
    std::string display;         // 代表拼写（最高频，并列取首见）
    std::string ident;           // 枚举标识符（折叠 + 非法字符替换 '_'）
};

// 从 DSL 模块集收集全语料 emitter 名并建表（字典序、1 基）。
[[nodiscard]] std::vector<ParticleEmitterNameEntry> BuildParticleEmitterNameTable(
    const ParticleDslBundle& bundle);

// 发射 ParticleEmitterName.h（enum ParticleEmitterName + 表 + 哈希常量）。
[[nodiscard]] std::string MakeParticleEmitterNameHeader(
    const std::vector<ParticleEmitterNameEntry>& table);

// 表哈希：逐项 [u32 LE id][folded 字节][0x00] 流的 crc32（与
// ParticleV2Crc32 同多项式）。运行时仅比对生成的数值常量。
[[nodiscard]] std::uint32_t ParticleEmitterNameTableHash(
    const std::vector<ParticleEmitterNameEntry>& table);

// ---------------------------------------------------------------- CLI 子命令
// 实现位于 ParticleDsl.cpp；main.cpp 仅分发。

int RunImportParticle(const std::vector<std::string>& args);
int RunCompileParticle(const std::vector<std::string>& args);
int RunInspectParticle(const std::vector<std::string>& args);
int RunCompileParticleV2(const std::vector<std::string>& args);

} // namespace openpvz::reanim::particle
