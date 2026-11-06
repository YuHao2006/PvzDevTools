#pragma once

// Byte-identical compiler for the compiled/reanim/*.reanim.compiled records
// shipped in the official pak64. The on-disk format (verified against all 146
// installed files) is:
//
//   uint32 LE cookie          = 0xDEADFED4
//   uint32 LE payloadSize     = size of the inflated payload
//   byte   0x78, 0x01         (zlib header bytes)
//   deflate STORED blocks     (<=65535 bytes each; last block has BFINAL=1)
//   uint32 LE adler32(payload)  -- little-endian, unlike the zlib big-endian
//                                  trailer; standard zlib readers reject this
//                                  stream on the checksum, by design of the
//                                  original writer.
//
// payload:
//   uint32 LE schema hash     = 166247922 (Reanim DefMap CRC)
//   ReanimatorDefinition dump, 32 bytes x64 layout, pointer slots zero:
//     [0:8) tracks.ptr = 0, [8:12) trackCount, [12:16) 0,
//     [16:20) fps, [20:24) 0, [24:32) atlas.ptr = 0
//   uint32 24                 (ReanimatorTrack native size)
//   trackCount * 24-byte track dumps: name.ptr=0(8), transforms.ptr=0(8),
//     frameCount(4), pad(4)
//   per track, in order:
//     name:    uint32 len + raw bytes (no entity decoding)
//     uint32 frameCount
//     uint32 56               (ReanimatorTransform native size)
//     frameCount * 56-byte transform dumps: x,y,kx,ky,sx,sy,f,a floats with
//       the XML writer placeholder -10000 for absent fields, then
//       image.ptr=0(8), font.ptr=0(8), text.ptr=0(8)
//     per frame, in order:
//       image: uint32 len + bytes (whitespace-trimmed; uppercased when the
//              trimmed name starts with IMAGE_ or FONT_, case-insensitive)
//       font:  uint32 len + bytes (whitespace-trimmed)
//       text:  uint32 len + raw bytes
//
// XML-to-value semantics copied from the original writer (validated 146/146):
//   - numeric fields parse like sscanf %f (longest numeric prefix) and keep
//     the placeholder when the element is absent or has no numeric prefix;
//   - fps defaults to 12.

#include <cstdint>
#include <filesystem>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace openpvz::reanim::pak64 {

inline constexpr std::uint32_t kCookie = 0xDEADFED4u;
inline constexpr std::uint32_t kReanimSchemaHash = 166247922u;
inline constexpr float kPlaceholder = -10000.0f;

struct RawTransform final {
    std::int32_t cel = -10000;
    float values[8] = {kPlaceholder, kPlaceholder, kPlaceholder, kPlaceholder,
                       kPlaceholder, kPlaceholder, kPlaceholder, kPlaceholder};
    std::string image;  // canonicalized: trimmed, prefix-uppercased
    std::string font;   // trimmed
    std::string text;   // raw between tags, entities untouched
    // Compiler-only identity of an explicit nonempty DSL text field. Expanded
    // frames retain this source frame; unannotated XML/raw frames use their own
    // frame index when nonempty. Never serialized as text or a name.
    std::uint32_t textSourceFrame = 0xFFFFFFFFu;
};

// attacher 段（编译期链接产物）：text 编码 "attacher__REANIM__TRACK[rate][loop]"
// 在编译期解析为整数四元组。loop：0=Loop 1=PlayOnceAndHold 2=PlayOnce。
struct TrackAttacherSegment final {
    std::uint32_t startFrame = 0;        // 含
    std::uint32_t endFrame = 0;          // 不含
    std::uint32_t targetType = 0xFFFFFFFFu;  // ReanimationType 数值；REANIM_NONE=(unsigned)-1 = 目标不存在（运行时 AttachmentDie）。0 是 REANIM_LOADBAR_SPROUT，绝不能当 NONE 用
    std::uint32_t actionTrackId = 0;     // ReanimTrackId（0 = 无动作切换）
    float rate = 12.0f;
    std::uint32_t loop = 0;
    // Contiguous inherited source-text identity run, inclusive/exclusive.
    // IDT5 carries these bounds independently of hidden-frame decode segments.
    std::uint32_t runStart = 0;
    std::uint32_t runEnd = 0;
};

// 类型映射表：attacher 目标的折叠模块名 -> ReanimationType 数值。
// 行格式："folded REANIM_NAME numeric"（'#' 开头为注释）。
struct ReanimTypeMapEntry final {
    std::string folded;
    std::uint32_t value = 0;
};
[[nodiscard]] bool LoadReanimTypeMap(const std::filesystem::path& input,
                                     std::vector<ReanimTypeMapEntry>& map,
                                     std::string* error = nullptr);

// --------------------------------------------- MPHF：id -> trackIndex（全局单表）
//
// 全局单表 MPHF 是唯一 MPHF 层（每类型 ReanimTrackMphf.h / kTrackMphf 函数
// 指针数组体系已退役）：全语料 (ReanimationType, ReanimTrackId) 联合键 ->
// 轨道下标，一张总表一次查找。键 = 每类型首见该 id 的轨道下标（与运行时
// 旧直接寻址表的首条优先语义一致）；id=0（未映射轨道）不进 MPHF。
// 记录格式（v1/v2）零变化。
//
// 算法（两级 hash-displace）——生成函数、求解器 MphfGlobalLookup、运行时
// 调用点逐运算一致（联合键打包、乘法常数、移位、掩码顺序），改动必须三方同步：
//   h1(key)   = (x ^ (x >> S1)) & (buckets-1), x = key * C1      分桶
//   h2(key,r) = (y ^ (y >> S2)) & (slotCount-1), y = key * (C2 + r)  桶内探槽
//   全 uint64 乘法（溢出回绕是设计的一部分）；桶数/槽数均 2 的幂，取模为掩码。
//   查找：slot = h2(key, disp[h1(key)])；槽内 key 相等 -> trackIndex，否则 -1。
//
// 构造：桶按大小降序（并列按桶号升序）逐桶暴力搜 r（u16 种子 0..65535 递增），
// 要求桶内全部键探到互不冲突的全局空闲槽；失败翻倍 bucketCount 重试，
// 仍失败 Fail（不许静默降级）。求解后穷举 type 0..maxType × id 1..词表 回验
// 直接寻址表语义（该类型首见轨道下标 / 其余 -1），不一致 Fail。
inline constexpr std::uint64_t kMphfGlobalC1 = 0x9E3779B97F4A7C15ull;  // 64 位黄金比例
inline constexpr int kMphfGlobalS1 = 31;
inline constexpr std::uint64_t kMphfGlobalC2 = 0xC2B2AE3D27D4EB4Full;
inline constexpr int kMphfGlobalS2 = 32;

struct TrackMphfGlobalSlot final {
    std::uint64_t key = 0;          // (type << 32) | id；0 = padding（不命中）
    std::uint16_t trackIndex = 0;   // 该类型定义内的轨道下标
};

struct TrackMphfGlobal final {
    std::uint32_t count = 0;       // 有效键数（Σ 每类型 distinct id）
    std::uint32_t buckets = 0;     // 分桶数（2 的幂）
    std::uint32_t slotCount = 0;   // 槽数（2 的幂，>= count；差额为 padding）
    std::uint32_t maxType = 0;     // 语料内最大 ReanimationType 数值
    std::vector<std::uint16_t> displacement;        // 每桶位移种子
    std::vector<TrackMphfGlobalSlot> slots;         // 槽序表（槽位顺序）
    std::vector<std::uint64_t> typeBits;             // 语料成员位图（bit t = 类型 t 编入键集）

    [[nodiscard]] bool operator==(const TrackMphfGlobal&) const = default;
};

// IDTF track flags reserve bit 0 for UI text and bits 1-2 for storage.
// Both is zero so existing sources retain their exact compiled bytes.
enum class TrackStorage : std::uint32_t { both = 0, cpu = 1, gpu = 2 };
inline constexpr std::uint32_t kTrackUiTextMask = 1u;
inline constexpr unsigned kTrackStorageShift = 1;
inline constexpr std::uint32_t kTrackStorageMask = 6u;

struct RawTrack final {
    std::string name;  // raw between tags
    std::vector<RawTransform> frames;
    std::vector<TrackAttacherSegment> attacherSegments;  // 仅 attacher 轨道非空
    TrackStorage storage = TrackStorage::both;
};

struct RawReanim final {
    // Module name from `export component Name { ... }`; it names the compiled
    // record (Tallnut -> Tallnut.reanim.compiled) but is not serialized
    // into the record itself.
    std::string name;
    float fps = 12.0f;
    std::vector<RawTrack> tracks;
    std::string variantOf;
    std::string variantRoot;
    std::vector<std::vector<std::string>> variantImages;
};

// Resolve image-only variants after collecting the complete source corpus.
void ResolveReanimVariants(std::vector<RawReanim>& corpus);
[[nodiscard]] std::string MakeReanimVariantMetadata(const std::vector<RawReanim>& corpus);
[[nodiscard]] std::string MakeReanimVariantHeader(const std::vector<RawReanim>& corpus);

// Field order matches the ReanimatorTransform DefMap: x y kx ky sx sy f a.
[[nodiscard]] bool ReadRawXml(const std::filesystem::path& input,
                              RawReanim& output, std::string* error = nullptr);

[[nodiscard]] bool ParseDsl(std::string_view source, RawReanim& output,
                            std::string* error = nullptr);

// One file may contain many `export component Name { ... }` modules; each
// module names its own compiled record. 纯字符串源：文件级指令
// (import/forward/prefixStrs) 不允许出现在这里，请使用 LoadDslBundle。
[[nodiscard]] bool ParseModules(std::string_view source,
                                std::vector<RawReanim>& outputs,
                                std::string* error = nullptr);

// ------------------------------------------------ 模块系统：import/forward
//
// 文件级语法（可在模块之间任意位置出现）：
//   import "relative/path.dsl";     // 引入参与编译，但不对外开放
//   forward "relative/path.dsl";    // 引入参与编译，且对外转发其公开部件
//   prefixStrs: [ "attacher__", "anim_head", ... ];   // 全局属性：前缀清单，
//                                    // 编译进 hpp 的 constexpr id 区间
//   export component A { ... }      // 对外开放
//   component B { ... }             // 私有：仅本文件可见语义（仍参与编译）
// 相对路径相对“包含该指令的文件”的目录解析。一个文件对外开放的部件 =
// 自己的 export + 它 forward 的文件的公开部件（传递）。
// import/forward 均相当于 #include（闭包内全部模块都编译产物）；
// 编译整个语料只需编译一个全是 forward 的根文件。

struct DslFileDirectives final {
    std::vector<std::string> imports;      // 按声明顺序
    std::vector<std::string> forwards;
    std::vector<std::string> prefixStrs;   // 本文件声明（跨文件在 bundle 层合并）
    std::vector<RawReanim> modules;        // 本文件全部模块（export + 私有）
    std::vector<std::string> publicNames;  // 本文件 export 的模块名
};

// 解析单文件文本（含文件级指令）。
[[nodiscard]] bool ParseDslFileText(std::string_view source,
                                    DslFileDirectives& file,
                                    std::string* error = nullptr);

struct DslBundle final {
    std::vector<RawReanim> modules;            // 闭包内全部模块，按模块名排序
    std::vector<std::string> publicNames;      // 根文件对外集（export + forward 传递）
    std::vector<std::string> prefixStrs;       // 跨文件合并（首见序、去重）
    std::vector<std::filesystem::path> files;  // 实际加载的文件（含根）
};

// 从根文件递归加载整个编译单元（记忆化去重、环检测、暴露集传递）。
[[nodiscard]] bool LoadDslBundle(const std::filesystem::path& rootFile,
                                 DslBundle& bundle, std::string* error = nullptr);

[[nodiscard]] std::string ToDsl(const RawReanim& source);

// Writes the exact installed-pak64 byte sequence for this animation.
[[nodiscard]] bool WriteCompiled(const RawReanim& source,
                                 const std::filesystem::path& output,
                                 std::string* error = nullptr);

// Produces the same bytes in memory (used by the equivalence tests).
[[nodiscard]] std::vector<std::uint8_t> Serialize(const RawReanim& source);

// Parses an installed .reanim.compiled file back into the raw model; the
// zlib wrapper is read with the stored-block framing above.
[[nodiscard]] bool ReadCompiled(const std::filesystem::path& input,
                                RawReanim& output, std::string* error = nullptr);

// Same parse from an in-memory record.
[[nodiscard]] bool ReadCompiledFromBytes(const std::vector<std::uint8_t>& bytes,
                                         RawReanim& output,
                                         std::string* error = nullptr);

// --------------------------------------------------------- v2: 编译期句柄内联
//
// v2 把轨道名的解析从运行时搬进编译期：编译器对整个语料做大小写折叠
// （stricmp 语义）去重，相同的名字映射为同一个 int id，id 从 1 开始连续
// 分配（按折叠名排序），0 保留为哨兵——运行时查不到该轨道（id 为 0、
// id 超界、或该定义没有此轨道）时走原版语义：TodTrace 后回退轨道 0。
// 编译器同时生成 hpp（enum = 字符串与 int 的映射）与 sidecar 二进制
// id 表；hpp、sidecar 与全部 v2 产物必须来自同一次编译（同一张表），
// 产物内嵌 tableHash，加载时不一致直接报错。
//
// v2 record:
//   uint32 LE cookie      = 0xDEADFED6
//   uint32 LE payloadSize
//   uint32 LE crc32(payload)   -- 标准 crc32，固定初值 0
//   payload:
//     uint32 LE magic     = 0x32544449 ("IDT2")
//     uint32 LE tableHash = crc32(serialized track-id table)
//     uint32 LE fpsBits
//     uint32 LE trackCount
//     per track:
//       uint32 LE trackId     // 1..N；0 = 编译期未映射（运行时 not-found）
//       uint32 LE frameCount
//       uint32 LE 56          // 与 v1 相同的 transform 元素大小
//       frameCount * 56-byte transform dumps（与 v1 完全一致的内存布局）
//       per frame: image/font/text len+bytes
//         // 资源名仍是字符串：运行时按名绑定 Image*/_Font* 的机制不变，
//         // 内联只针对轨道名（查找协议），不针对资源绑定。
//
// sidecar reanim_track_ids.bin:
//   uint32 LE magic  = 0x324C4254 ("TBL2")
//   uint32 LE count
//   count * { uint32 id（升序 1..N）, folded len+bytes, display len+bytes,
//             uint32 出现次数 }

inline constexpr std::uint32_t kV2Cookie = 0xDEADFED6u;
inline constexpr std::uint32_t kV2PayloadMagic = 0x33544449u;  // 'IDT3'：含编译期 attacher 段表
inline constexpr std::uint32_t kV2TableMagic = 0x324C4254u;

struct TrackIdEntry final {
    std::string folded;    // 大小写折叠键（stricmp 语义的查找键）
    std::string display;   // 代表拼写（语料中出现频次最高的形式，并列取首见）
    std::uint32_t id = 0;  // 1..N，按 folded 升序连续分配
    std::uint32_t count = 0;  // 语料内出现次数（诊断用）

    [[nodiscard]] bool operator==(const TrackIdEntry&) const = default;
};

// 对整个语料构建 id 表：折叠去重、代表拼写取最高频、id 按 folded 排序
// 从 1 连续分配。输入顺序决定代表拼写的并列裁决，必须可复现（排序遍历）。
[[nodiscard]] std::vector<TrackIdEntry> BuildTrackIdTable(
    const std::vector<RawReanim>& corpus);

// 折叠名 -> id；未知名字返回 0（not-found 哨兵）。
[[nodiscard]] std::uint32_t TrackIdOf(const std::vector<TrackIdEntry>& table,
                                      std::string_view name);

// 求解器查找（非键落 padding/他人槽位 → -1）。与生成函数体、运行时调用点
// 逐运算一致；仅用于构建期自校验。
[[nodiscard]] int MphfGlobalLookup(const TrackMphfGlobal& mphf,
                                   std::uint32_t type, std::uint32_t id);

// 对全语料构建全局单表 MPHF：键 = 每类型首见该 id 的轨道下标（与旧直接
// 寻址表同语义），并记录语料成员位图；求解后穷举 (type,id) 回验直接表语义，
// 不一致 Fail（抛异常，经 error 通道上报）。
[[nodiscard]] TrackMphfGlobal BuildTrackMphfGlobal(
    const std::vector<RawReanim>& corpus, const std::vector<TrackIdEntry>& table,
    const std::vector<ReanimTypeMapEntry>& typeMap);

// 生成 ReanimTrackMphfGlobal.h 全文：单个 constexpr 函数 MphfLookupGlobal
//（type,id 双参；联合键打包在函数体内），烘焙掩码取 2 的幂的桶/槽数、
// 位移表与槽序表；尾部附语料成员位图 kGlobalTypeBits + 内联判据
// MphfGlobalTypeInCorpus(type)。typeMap 提供折叠模块名 -> ReanimationType 数值。
[[nodiscard]] std::string MakeTrackMphfGlobalHeader(
    const std::vector<RawReanim>& corpus, const std::vector<TrackIdEntry>& table,
    const std::vector<ReanimTypeMapEntry>& typeMap);

[[nodiscard]] std::vector<std::uint8_t> SerializeV2(
    const RawReanim& source, const std::vector<TrackIdEntry>& table,
    const std::vector<ReanimTypeMapEntry>& typeMap);
[[nodiscard]] bool WriteCompiledV2(const RawReanim& source,
                                   const std::vector<TrackIdEntry>& table,
                                   const std::vector<ReanimTypeMapEntry>& typeMap,
                                   const std::filesystem::path& output,
                                   std::string* error = nullptr);

// Offline IDT5 source records use <stem>.source-record and the DEADFED7 envelope.
// Outer header: u32 cookie, payloadSize, crc32(payload), then payload.
// Payload (all u32/f32 little-endian):
//   magic=0x35544449, trackTableHash, imageHash=0, fontRegistryHash,
//   fpsBits, trackCount;
//   per track: trackId, frameCount, elementSize=32, flags
//              (bit 0: UI text, bits 1-2: storage: 0=both, 1=cpu, 2=gpu),
//              frameCount * {x,y,kx,ky,sx,sy,cel,alpha} f32;
//   per track, per frame: sourceImageHandle, fontId;
//              ONLY flags bit 0: UI text byte length + bytes (sparse/inherited).
//   segmentTrackCount; per segment track: trackIndex, segmentCount;
//              per segment: start,end,targetType,actionTrackId,rateBits,loop,
//                           runStart,runEnd (both bounds pairs end-exclusive).
// Mode 0 has no text/name length slots. Attacher routing and clear markers
// compile to numeric segments, never text. Image handles index image_sources.txt
// and are replaced with inline rectangles by the final offline atlas compiler.
using SourceImageHandles = std::map<std::string, std::uint32_t>;
using FontIdRegistry = std::map<std::string, std::uint32_t>;
// sidecar（'MYSI' 格式：magic+count+{id,len,bytes}*）；fileHash=文件字节
// crc32，写进每条源记录防font注册表/记录错配
[[nodiscard]] bool LoadFontIdRegistry(const std::filesystem::path& sidecar,
                                        FontIdRegistry& out,
                                        std::uint32_t* fileHash = nullptr,
                                        std::string* error = nullptr);
[[nodiscard]] std::vector<std::uint8_t> SerializeV4(
    const RawReanim& source, const std::vector<TrackIdEntry>& table,
    const std::vector<ReanimTypeMapEntry>& typeMap,
    const SourceImageHandles& sourceImageHandles, const FontIdRegistry& fontSymbols,
    std::uint32_t fontRegistryHash);
[[nodiscard]] bool WriteCompiledV4(const RawReanim& source,
                                   const std::vector<TrackIdEntry>& table,
                                   const std::vector<ReanimTypeMapEntry>& typeMap,
                                   const SourceImageHandles& sourceImageHandles,
                                   const FontIdRegistry& fontSymbols,
                                   std::uint32_t fontRegistryHash,
                                   const std::filesystem::path& output,
                                   std::string* error = nullptr);

[[nodiscard]] std::vector<std::uint8_t> SerializeTrackIdTable(
    const std::vector<TrackIdEntry>& table);
[[nodiscard]] std::uint32_t TrackIdTableHash(const std::vector<TrackIdEntry>& table);

// sidecar 表的内存解析与文件读写（校验 magic/结构/一致性）。
[[nodiscard]] bool ReadTrackIdTableBytes(const std::vector<std::uint8_t>& bytes,
                                         std::vector<TrackIdEntry>& table,
                                         std::string* error = nullptr);
[[nodiscard]] bool ReadTrackIdTableFile(const std::filesystem::path& input,
                                        std::vector<TrackIdEntry>& table,
                                        std::string* error = nullptr);
[[nodiscard]] bool WriteTrackIdTableFile(const std::vector<TrackIdEntry>& table,
                                         const std::filesystem::path& output,
                                         std::string* error = nullptr);

[[nodiscard]] bool ReadCompiledV2FromBytes(const std::vector<std::uint8_t>& bytes,
                                           const std::vector<TrackIdEntry>& table,
                                           RawReanim& output,
                                           std::string* error = nullptr);

// Runtime header: stable enum IDs, count/end sentinel, numeric table checksum
// and prefix ranges only. Source names stay in the offline compiler sidecar.
// sanitize: invalid characters -> '_', leading digit -> '_', collisions _2/_3.
[[nodiscard]] std::string MakeTrackIdHeader(
    const std::vector<TrackIdEntry>& table,
    const std::vector<std::string>& families = {});

// 枚举按折叠名字典序分配 id，因此 id 本身字典序单调：任何字符串前缀
// 恰好对应一段连续 id 区间 [首匹配, 末匹配]。前缀查询在编译期折叠为
// 一对枚举值，运行期是两次整数比较——与原版 strnicmp 前缀精确等价。
struct TrackIdRange final {
    std::uint32_t lo = 0;  // 0 = 空区间（无任何名字命中该前缀）
    std::uint32_t hi = 0;
};

// 对每个查询前缀计算区间（folded.startswith 语义 = strnicmp 前缀）。
[[nodiscard]] TrackIdRange RangeOfPrefix(const std::vector<TrackIdEntry>& table,
                                         std::string_view prefix);

// 生成 hpp 的区间块：struct ReanimTrackIdRange + ReanimTrackRanges（每个
// 前缀一个 static constexpr 区间常量，成员名 = sanitize 后的前缀）。
// Repeated exact prefixes are emitted once, preserving first-seen order.
// Distinct spellings retain their names even when they match the same range.
[[nodiscard]] std::string MakeTrackIdRangesBlock(const std::vector<TrackIdEntry>& table,
                                                 const std::vector<std::string>& prefixes);

} // namespace openpvz::reanim::pak64
