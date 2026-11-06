#ifndef OPENPVZ_REANIM_STRING_ID_GEN_H
#define OPENPVZ_REANIM_STRING_ID_GEN_H

// LawnStrings 烘焙器 + 调用点改写器。
//
// 背景：游戏代码里到处写 _S("[LEVEL]") 方括号字面量，TodStringTranslate 每次
// 都要剥壳 + std::map 查找 + 三次字符串转换（SexyStringToString → map find →
// WStringToSexyString），每次至少 3 次堆分配。LawnStrings.txt 本身就是完整
// 语料：bake-strings 把其中每个 [键] 值 直接烘焙成结构体成员（零装载、零
// 查找、零分配），这是 reanim v3（轨道名 → uint32 id）哲学在字符串表的复刻。
//
// 产物（键集合与取值全部来自 txt，代码未引用的键也生成，天然向前兼容）：
//   LawnStrings.h ：struct LawnStrings { SexyString <键标识符>; ... };
//                   extern const LawnStrings gStr;
//   LawnStrings.cpp：const LawnStrings gStr = { <烘焙值>, ... };
// 值里的占位符（{FLAGS}、{COUNT} 等）原样保留在字面量里，运行时
// TodReplaceString / Pluralize 的替换逻辑不变。
//
// 改写器（--sources）：把「已验证内部会 TodStringTranslate 的接收函数」的
// 直接字面量实参从 _S("[X]") 换成 gStr.X（const SexyChar* 形参加 .c_str()）。
// 语义对齐依据：烘焙值永远不以 '[' 开头（装载端解析决定了值内不含 '['），
// 所以接收端再 translate 一次是恒等变换。TodStringTranslate 原样保留，
// 服务于动态拼键（StrFormat("[CRAZY_DAVE_%d]") 等）与非枚举串。

#include <cstddef>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace openpvz {
namespace reanim {
namespace stringid {

namespace fs = std::filesystem;

// txt 里的一个键值对（已按装载端语义解析：键大写、值去 \r、两端 trim）。
struct BakedString {
    std::string key;    // 如 "LEVEL"
    std::string ident;  // 结构体成员标识符
    std::string value;  // 烘焙值（原字节，占位符保留）
};

// 一次调用点实参改写：Func(..., _S("[X]"), ...) -> Func(..., gStr.X, ...)
struct RewriteEdit {
    fs::path file;
    std::size_t line = 0;  // 1-based
    std::size_t begin = 0; // 实参表达式字节区间（整个 _S("[X]")）
    std::size_t end = 0;
    std::string before;
    std::string after;
    std::string callee;   // 接收函数名
    std::string key;      // 方括号键名
};

// 改写/扫描统计（汇报用）。
struct RewriteReport {
    std::size_t filesScanned = 0;
    std::size_t bracketLiteralUses = 0;      // "[KEY]" 完整实参形式出现总数
    std::size_t dynamicKeyUses = 0;          // StrFormat("[%s]") 等动态键（不可枚举）
    std::size_t edits = 0;                   // 已改写实参数
    std::size_t translatedCallArgs = 0;      // 其中 TodStringTranslate 直接实参
    std::map<std::string, std::size_t> editsByCallee;
    std::map<std::string, std::size_t> residualWholeLiteralByCallee; // 未改写点按接收函数分布
    std::map<std::string, std::size_t> unknownReceiverLiterals;     // 接收函数不在白名单的字面量
    std::set<std::string> sourceKeysMissingFromTxt;                 // 源码引用但 txt 没有的键
    // 残余点定位（file:line:callee），供人工裁决。
    std::vector<std::string> residualLocations;
};

// 解析 LawnStrings.txt（复刻 TodStringListReadItems 语义）。firstWins 控制
// 重复键处理（装载端 SetString 是覆盖语义，这里跟随覆盖，与运行时终态一致）。
std::vector<BakedString> ParseLawnStringsFile(const fs::path& input);

// 键名 -> 合法 C++ 结构体成员标识符。[A-Z0-9_]+ 本身合法；数字开头加前缀
// "K"；其余非法字符最小清洗为 '_'（并防撞名）。改名键在生成文件注释里留原名。
std::string MakeStringMemberIdent(const std::string& key);

// 生成 LawnStrings.h / LawnStrings.cpp 文本（确定性输出）。
std::string MakeBakedHeader(const std::vector<BakedString>& strings);
std::string MakeBakedSource(const std::vector<BakedString>& strings);

// ---- 实体表（--entity-tables）------------------------------------------------
//
// 数值键族（[CRAZY_DAVE_1..180]、[TREE_OF_WISDOM_1..80]、[CREDITS_NAMES1..11]
// 等前缀+连续 1..N 形态）烘焙为定长数组（1 基，[0] 空串）+ 计数常量，
// 调用点按索引直取。任意键（运行期 "[%s]" 拼装的植物/僵尸/挑战名）走
// EntityStringFind 的有序键表二分查找，条目直接指向 gStr 成员（零值拷贝）。
// 查找语义与 TodStringListFind 逐字节对齐：键大小写敏感，未命中返回
// "<Missing 键>"。存在前缀覆盖同名数值族时以最具体（最长前缀）为准。
struct EntityFamily {
    std::string prefix;      // 数值剥离后的族前缀，如 "CRAZY_DAVE"
    std::string arrayName;   // 如 "gCrazyDaveLines"
    std::string countName;   // 如 "gCrazyDaveLineCount"
    std::size_t count = 0;   // 族长度 N（键覆盖 1..N 且无空洞）
    std::vector<std::size_t> entryIndex;  // 对应 strings 下标，size == N（下标 0 = 键 1）
};

// 识别全部完整（1..N 连续、N >= 2）的数值键族；序按首见顺序。
std::vector<EntityFamily> CollectNumericFamilies(const std::vector<BakedString>& strings,
                                                 bool verboseNotes = false);

// 生成 EntityStrings.h / EntityStrings.cpp 文本（确定性输出；.cpp 引用同目录
// LawnStrings.h 的 gStr 成员地址）。
std::string MakeEntityHeader(const std::vector<BakedString>& strings);
std::string MakeEntitySource(const std::vector<BakedString>& strings);

// 扫描（并可选写回）改写点。keys 为 txt 键集合；apply=false 即 --dry-run。
std::vector<RewriteEdit> RewriteTranslateCallsites(const std::vector<fs::path>& sources,
                                                   const std::set<std::string>& keys,
                                                   bool apply, RewriteReport& report);

} // namespace stringid
} // namespace reanim
} // namespace openpvz

#endif // OPENPVZ_REANIM_STRING_ID_GEN_H
