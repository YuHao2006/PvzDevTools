#pragma once

// trail 定义的 DSL 管线：XML -> DSL 导入（import-trail）与
// DSL -> 字节级等价的 pak64 编译记录（compile-trail）。
//
// 记录容器格式（与 reanim v1 相同，见 Pak64.h）：
//   uint32 LE cookie      = 0xDEADFED4
//   uint32 LE payloadSize = 载荷字节数
//   byte   0x78, 0x01     （zlib 头）
//   deflate STORED 块     （<=65535 分块，末块 BFINAL=1）
//   uint32 LE adler32(payload)  -- 小端，非标准 zlib 大端
//
// 载荷（x64 布局，全部字段经 DefinitionReadCacheRecord/DefMapReadFromCache
// 逐字节核对，并与安装包 compiled/particles/icetrail.trail.compiled 比对）：
//   uint32 LE schema hash = 2223553544 (0x8488BC08)
//     = DefinitionCalcHash(gTrailDefMap)，x64 sizeof(TrailDefinition)=104：
//     字段表（类型,偏移）序列 IMAGE@0 INT@8 FLOAT@12 FLAGS@16(Loops)
//     TRACK@40 TRACK@56 TRACK@72 TRACK@88 TRACK@24，字段名不参与 hash。
//   104 字节 TrailDefinition 转储（x64 槽位，指针与计数恒为零）：
//     [0:8)   mImage 指针槽 = 0
//     [8:12)  mMaxPoints（int）
//     [12:16) mMinPointDistance（float）
//     [16:20) mTrailFlags（int，bit0 = Loops）
//     [20:24) 填充 = 0
//     [24:40) mTrailDuration  { mNodes.ptr=0(8), mCountNodes=0(4), 填充(4) }
//     [40:56) mWidthOverLength   同上
//     [56:72) mWidthOverTime     同上
//     [72:88) mAlphaOverLength   同上
//     [88:104) mAlphaOverTime    同上
//     计数只存在于深层尾区；转储里的计数槽写零（读取端忽略转储计数）。
//   尾区（深层数据，按写入槽位顺序）：
//     Image 字符串：uint32 len + 字节（image_/font_ 前缀资源名规范化为大写）
//     5 条轨道，每条：uint32 节点数 + 节点*20 字节
//       节点 { float mTime, mLowValue, mHighValue; int mCurveType, mDistribution; }
//       TodCurves：LINEAR=1 EASE_IN=2 EASE_OUT=3 EASE_IN_OUT=4 EASE_IN_OUT_WEAK=5
//       FAST_IN_OUT=6 FAST_IN_OUT_WEAK=7 BOUNCE=9 BOUNCE_FAST_MIDDLE=10
//       BOUNCE_SLOW_MIDDLE=11 SIN_WAVE=12 EASE_SIN_WAVE=13
//
// 轨道槽位顺序 = 安装包 oracle 的实际写入序（TrailDuration 在最前）：
//   TrailDuration, WidthOverLength, WidthOverTime, AlphaOverLength, AlphaOverTime
// 读取端 gTrailDefFields 的表序是 WidthOverLength..AlphaOverTime, TrailDuration，
// 因此 oracle 记录里的轨道数据实际落在"下一条"成员上（WoL 数据被运行时读成
// WidthOverTime，以此类推；未写轨道由 FloatTrackSetDefault 运行时补默认值）。
// 这是历史转换器的槽位错位；reanimc 以安装包记录为金标准，按 oracle 序复刻，
// hash 不受影响（hash 只覆盖字段表的类型与偏移序列）。
//
// FloatTrack 文本解析（编译期，oracle 语义，与安装记录逐字节一致）：
//   - 数值节点 "v" 或 "v,t"（t 为百分数：float((double)t * 0.01)）；
//   - 范围节点 "[lo]"、"[lo hi]"、"[lo Dist hi]"，可带 ",t" 时间后缀；
//   - 数值节点后的下一个空白分隔 token 一律当作曲线名消费：命中曲线表则
//     赋给当前节点，否则整个 token 被丢弃（oracle 的历史行为：数字前瞻被
//     当曲线名吞掉，例如 "0,0 .3,20" 只留下 1 个节点 (0,0,0)）。这与游戏
//     端修复后的 DefinitionReadFloatTrackField 不同，仅为字节等价保留；
//   - 未指定时间的节点按官方区间规则补齐：high*interp + low*(1-interp)，
//     首段单节点时间为 0，其余区间单节点时间为区间末端。

#include <cstdint>
#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace openpvz::reanim::trail {

inline constexpr std::uint32_t kTrailCookie = 0xDEADFED4u;
inline constexpr std::uint32_t kTrailSchemaHash = 2223553544u;  // 0x8488BC08

struct RawTrail final {
    // 模块名来自 `export component <name>`，命名编译记录
    // （IceTrail -> IceTrail.trail.compiled），不进入记录字节。
    std::string name;
    std::string sourceName;  // 源 XML 文件名（信息性，不进入记录字节）
    std::string image;       // 原文；序列化时做 image_/font_ 大写规范化
    int maxPoints = 2;       // 官方构造默认 MaxPoints = 2
    float minPointDistance = 1.0f;  // 官方默认 1.0
    bool loops = false;      // <Loops> 非零 -> mTrailFlags bit0
    // FloatTrack 原文（空 = XML 未提供，轨道计数写 0，由运行时补默认值）
    std::string widthOverLength;
    std::string widthOverTime;
    std::string alphaOverLength;
    std::string alphaOverTime;
    std::string trailDuration;
};

// 读取 legacy trail XML（<Image>/<MaxPoints>/<Loops>/<MinPointDistance>/
// <WidthOverLength>/<WidthOverTime>/<AlphaOverLength>/<AlphaOverTime>/
// <TrailDuration>，未知标签报错）。name/sourceName 取输入文件主名。
[[nodiscard]] bool ReadTrailXml(const std::filesystem::path& input,
                                RawTrail& output, std::string* error = nullptr);

[[nodiscard]] std::string TrailToDsl(const RawTrail& source);

// ------------------------------------------------------- 模块系统：import/forward
//
// 文件级语法与 reanim DSL 相同：
//   import "relative/path.dsl";     // 引入编译，不对外暴露
//   forward "relative/path.dsl";    // 引入编译，并转发其公开部件
//   export component A { ... }      // 对外开放
//   component B { ... }             // 私有（仍参与编译）
// 模块体字段：
//   kind: trail;                    // 必填，且必须为 trail（喂错 kind 拒绝）
//   source-name: "IceTrail";
//   image: "IMAGE_ICETRAIL";        // 或 none
//   maxPoints: 20;
//   loops: 1;                       // 数值非零即置位
//   minPointDistance: 3;
//   widthOverLength: track { 12 EaseOut 5; }   // 原文保留，编译期解析
//   widthOverTime / alphaOverLength / alphaOverTime / trailDuration 同上

struct TrailDslFileDirectives final {
    std::vector<std::string> imports;   // 按声明顺序
    std::vector<std::string> forwards;
    std::vector<RawTrail> modules;      // 本文件全部模块（export + 私有）
    std::vector<std::string> publicNames;  // 本文件 export 的模块名
};

[[nodiscard]] bool ParseTrailDslFileText(std::string_view source,
                                         TrailDslFileDirectives& file,
                                         std::string* error = nullptr);

struct TrailDslBundle final {
    std::vector<RawTrail> modules;           // 闭包内全部模块，按模块名排序
    std::vector<std::string> publicNames;    // 根文件对外集（export + forward 传递）
    std::vector<std::filesystem::path> files;  // 实际加载的文件（含根）
};

// 从根文件递归加载编译单元（记忆化、环检测、相对路径基于包含文件）。
[[nodiscard]] bool LoadTrailDslBundle(const std::filesystem::path& rootFile,
                                      TrailDslBundle& bundle,
                                      std::string* error = nullptr);

// ---------------------------------------------------------------- 序列化

// legacy FloatTrack 解析（oracle 语义，见文件头注释）。
struct TrackNode final {
    float time = 0.0f;
    float low = 0.0f;
    float high = 0.0f;
    std::int32_t curve = 1;          // TodCurves::CURVE_LINEAR
    std::int32_t distribution = 1;   // TodCurves::CURVE_LINEAR
};

[[nodiscard]] bool ParseLegacyFloatTrack(std::string_view text,
                                         std::vector<TrackNode>& nodes,
                                         std::string* error = nullptr);

// 产出与安装包记录逐字节一致的完整记录（含容器包装）。
// labels 非空时返回逐字节标注（verify 差异定位用），与记录字节等长。
[[nodiscard]] std::vector<std::uint8_t> SerializeTrail(
    const RawTrail& source, std::vector<std::string>* labels = nullptr);

[[nodiscard]] bool WriteTrailCompiled(const RawTrail& source,
                                      const std::filesystem::path& output,
                                      std::string* error = nullptr,
                                      std::vector<std::string>* labels = nullptr);

} // namespace openpvz::reanim::trail
