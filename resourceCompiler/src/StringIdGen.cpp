#include "reanim/StringIdGen.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <sstream>

namespace openpvz {
namespace reanim {
namespace stringid {

namespace {

// ---------------------------------------------------------------------------
// 接收函数白名单：这些函数的对应形参在实现内部会（直接或间接）
// TodStringTranslate。把 _S("[X]") 实参换成 gStr.X 后，接收端再 translate
// 一次是恒等变换（烘焙值不含 '['），语义逐位对齐。
// cStr = 形参是 const SexyChar*，改写后需要补 .c_str()。
// ---------------------------------------------------------------------------
struct CalleeRule {
    const char* name;
    int args[5];
    int argCount;
    bool cStr;
};

const CalleeRule kCalleeRules[] = {
    // Sexy.TodLib/TodStringFile.cpp / TodCommon.cpp
    {"TodStringTranslate",          {0},         1, false},
    {"TodDrawString",               {1},         1, false},
    {"TodDrawStringWrapped",        {1},         1, false},
    {"TodDrawStringMatrix",         {3},         1, false},
    {"TodReplaceString",            {0, 2},      2, false},
    {"TodReplaceNumberString",      {0},         1, false},
    // LawnApp.cpp
    {"Pluralize",                   {1, 2},      2, true },
    {"LawnMessageBox",              {1, 2, 3, 4}, 4, true},
    {"DoDialog",                    {2, 3, 4},   3, false},
    // Lawn/Board.cpp -> MessageWidget::SetLabel -> translate
    {"DisplayAdvice",               {0},         1, false},
    {"DisplayAdviceAgain",          {0},         1, false},
    // Lawn/Widget/SeedChooserScreen.cpp -> LawnMessageBox -> translate
    {"DisplayRepickWarningDialog",  {0},         1, true },
    // MessageWidget / ToolTipWidget / GameButton 家族：SetLabel 实现内 translate
    {"SetLabel",                    {0},         1, false},
    {"SetWarningText",              {0},         1, false},
    // GameButton.cpp 工厂：内部 SetLabel -> translate
    {"MakeButton",                  {2},         1, false},
    {"MakeNewButton",               {2},         1, false},
    // GameSelector.cpp：DoDialog + TodStringTranslate -> translate
    {"SelectorMessageBox",          {1, 2, 3, 4}, 4, true},
    // AwardScreen.cpp：参数直通 TodDrawString/Wrapped -> translate
    {"DrawBottom",                  {1, 2, 3},   3, false},
    // LawnDialog.cpp 构造：header/lines 内部 TodStringTranslate；
    // StoreScreen.cpp ShowPurchaseDialog -> StorePurchaseDialog -> LawnDialog
    {"LawnDialog",                  {3, 4},      2, false},
    {"ShowPurchaseDialog",          {1, 2},      2, false},
};

bool IsIdentChar(char c)
{
    return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
}

bool IsSpaceChar(char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n' || c == '\f' || c == '\v';
}

std::string TrimWs(const std::string& s)
{
    std::size_t b = 0, e = s.size();
    while (b < e && IsSpaceChar(s[b])) b++;
    while (e > b && IsSpaceChar(s[e - 1])) e--;
    return s.substr(b, e - b);
}

// 装载端 Sexy::Trim 的等价实现（空白集合一致即可，本语料只有空格/换行）。
std::string TrimLoadWs(const std::string& s)
{
    return TrimWs(s);
}

std::string ToUpperAscii(const std::string& s)
{
    std::string out = s;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c) {
        return static_cast<char>(std::toupper(c));
    });
    return out;
}

bool ReadFileBytes(const fs::path& path, std::string& out)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        std::cerr << "bake-strings: cannot open " << path.string() << '\n';
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    out = buffer.str();
    return true;
}

bool WriteFileBytes(const fs::path& path, const std::string& text)
{
    if (!path.parent_path().empty()) fs::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    out << text;
    if (!out) {
        std::cerr << "bake-strings: cannot write " << path.string() << '\n';
        return false;
    }
    return true;
}

// ---------------------------------------------------------------------------
// 词法 mask：区分 code / string / char / comment，保证只在代码区做匹配、
// 只把真正的字符串字面量当键候选。UTF-8 多字节注释安全（续字节 >= 0x80，
// 不会与 ASCII 引号/斜杠混淆）。不支持原始字符串字面量（语料中不存在）。
// ---------------------------------------------------------------------------
enum class Region : unsigned char { Code, String, Char, Comment };

std::vector<Region> LexRegions(const std::string& text)
{
    const std::size_t n = text.size();
    std::vector<Region> regions(n, Region::Code);
    std::size_t i = 0;
    while (i < n) {
        const char c = text[i];
        if (c == '/' && i + 1 < n && text[i + 1] == '/') {
            const std::size_t start = i;
            i += 2;
            while (i < n && text[i] != '\n') i++;
            for (std::size_t j = start; j < i; j++) regions[j] = Region::Comment;
        } else if (c == '/' && i + 1 < n && text[i + 1] == '*') {
            const std::size_t start = i;
            i += 2;
            while (i + 1 < n && !(text[i] == '*' && text[i + 1] == '/')) i++;
            i = std::min(n, i + 2);
            for (std::size_t j = start; j < i; j++) regions[j] = Region::Comment;
        } else if (c == '"') {
            const std::size_t start = i;
            i++;
            while (i < n && text[i] != '"') {
                if (text[i] == '\\') i++;
                i++;
            }
            i = std::min(n, i + 1);
            for (std::size_t j = start; j < i; j++) regions[j] = Region::String;
        } else if (c == '\'') {
            const std::size_t start = i;
            i++;
            while (i < n && text[i] != '\'') {
                if (text[i] == '\\') i++;
                i++;
            }
            i = std::min(n, i + 1);
            for (std::size_t j = start; j < i; j++) regions[j] = Region::Char;
        } else {
            i++;
        }
    }
    return regions;
}

// 提取一个字符串字面量（含引号）的双引号内原文；非字符串区返回 false。
bool StringLiteralContent(const std::string& text, const std::vector<Region>& regions,
                          std::size_t quotePos, std::string& content, std::size_t* endPos)
{
    if (quotePos >= text.size() || regions[quotePos] != Region::String || text[quotePos] != '"')
        return false;
    std::size_t i = quotePos + 1;
    content.clear();
    while (i < text.size()) {
        const char c = text[i];
        if (c == '\\' && i + 1 < text.size()) {
            content += c;
            content += text[i + 1];
            i += 2;
            continue;
        }
        if (c == '"') {
            if (endPos) *endPos = i + 1;
            return true;
        }
        content += c;
        i++;
    }
    return false;
}

std::size_t LineOf(const std::string& text, std::size_t pos)
{
    std::size_t line = 1;
    for (std::size_t i = 0; i < pos && i < text.size(); i++)
        if (text[i] == '\n') line++;
    return line;
}

// 方括号键字面量判定："[" + [A-Za-z0-9_]+ + "]"，内容不含 %。
bool ParseBracketKey(const std::string& content, std::string& key)
{
    if (content.size() < 3 || content.front() != '[' || content.back() != ']') return false;
    const std::string inner = content.substr(1, content.size() - 2);
    if (inner.empty()) return false;
    for (const char c : inner) {
        if (!(std::isalnum(static_cast<unsigned char>(c)) || c == '_')) return false;
    }
    key = inner;
    return true;
}

bool IsFullyUpperKey(const std::string& key)
{
    for (const char c : key) {
        if (std::islower(static_cast<unsigned char>(c))) return false;
    }
    return true;
}

} // namespace

// ---------------------------------------------------------------------------
// 语料解析（复刻 TodStringListReadItems：名字取 [] 之间并 trim，值取到下一
// 个 '[' 或 EOF、去 \r、trim；名字大写化）。重复键跟随 SetString 的覆盖语义。
// ---------------------------------------------------------------------------
std::vector<BakedString> ParseLawnStringsFile(const fs::path& input)
{
    std::string text;
    if (!ReadFileBytes(input, text)) return {};

    std::map<std::string, std::string> byKey;  // upper key -> value（覆盖语义）
    std::vector<std::string> order;            // 首见序
    std::size_t pos = 0;
    while (true) {
        const std::size_t nameStart = text.find('[', pos);
        if (nameStart == std::string::npos) break;
        const std::size_t nameEnd = text.find(']', nameStart + 1);
        if (nameEnd == std::string::npos) break;
        const std::string name = TrimLoadWs(text.substr(nameStart + 1, nameEnd - nameStart - 1));
        if (name.empty()) break;  // 装载端：读到空名即认为读取完成
        const std::size_t valueEnd = text.find('[', nameEnd + 1);
        const std::size_t valueLen =
            (valueEnd == std::string::npos ? text.size() : valueEnd) - (nameEnd + 1);
        std::string value = text.substr(nameEnd + 1, valueLen);
        value.erase(std::remove(value.begin(), value.end(), '\r'), value.end());
        value = TrimLoadWs(value);

        const std::string key = ToUpperAscii(name);
        if (byKey.find(key) == byKey.end()) order.push_back(key);
        byKey[key] = value;
        pos = nameEnd + 1;
    }

    std::vector<BakedString> out;
    out.reserve(order.size());
    for (const std::string& key : order) {
        BakedString s;
        s.key = key;
        s.ident = MakeStringMemberIdent(key);
        s.value = byKey[key];
        out.push_back(std::move(s));
    }
    return out;
}

std::string MakeStringMemberIdent(const std::string& key)
{
    // 与系统/工具链头文件宏冲突的键名在此登记（成员名会被宏展开打爆）。
    // TRY_AGAIN: WinSock2.h `#define TRY_AGAIN WSATRY_AGAIN`（本语料唯一冲突，
    // 已对 427k 个系统宏 + 全部游戏头宏扫描确认）。
    static const std::set<std::string> kReservedIdents = {"TRY_AGAIN"};

    std::string ident;
    for (const char c : key) {
        if (std::isalnum(static_cast<unsigned char>(c)) || c == '_') ident += c;
        else ident += '_';
    }
    if (ident.empty()) ident = "KEY";
    if (std::isdigit(static_cast<unsigned char>(ident[0]))) ident = "K" + ident;
    if (kReservedIdents.find(ident) != kReservedIdents.end()) ident += "_";
    // 防撞名：清洗后与其它键冲突时追加 _2/_3（当前语料不会触发，防御性保留）。
    return ident;
}

namespace {

// Compiler-only token names. The game receives NUL-free two-byte records and
// resolves neither style names nor substitution names at runtime.
std::string BakeUiTextValue(const std::string& key, const std::string& value)
{
    static const char* const formats[] = {
        "NORMAL", "FLAVOR", "KEYWORD", "NOCTURNAL", "AQUATIC", "STAT", "METAL",
        "KEYMETAL", "SHORTLINE", "EXTRASHORTLINE", "CREDITS1", "CREDITS2"
    };
    static const char* const placeholders[] = {
        "PLAYER_NAME", "MONEY", "UPGRADE_COST", "COUNT", "FLAGS", "SCORE", "HEIGHT",
        "STREAK", "SEED", "PLAYER", "SLOTS", "SELL_PRICE", "PLANT_TYPE", "PAGE", "COST",
        "WAIT_TIME_LENGTH", "WAIT_TIME", "NUM_PAGES", "UPGRADE_TO", "UPGRADE_FROM"
    };
    static const std::set<std::string> daveCommands = {
        "HANDING", "NO_SOUND", "SHORT_SOUND", "SCREAM", "SCREAM2", "SHAKE", "NO_CLICK",
        "SHOW_WALLNUT", "SHOW_HAMMER", "SHOW_FERTILIZER", "SHOW_TREE_FOOD", "SHOW_MONEYBAG",
        "MOUTH_BIG_SMILE", "MOUTH_SMALL_SMILE", "MOUTH_BIG_OH", "MOUTH_SMALL_OH"
    };
    const std::string davePrefix = "CRAZY_DAVE_";
    const bool isDave = key.compare(0, davePrefix.size(), davePrefix) == 0 &&
        key.size() > davePrefix.size() &&
        std::all_of(key.begin() + davePrefix.size(), key.end(),
                    [](unsigned char c) { return c >= '0' && c <= '9'; });
    std::string result;
    bool removedShake = false, removedNoClick = false;
    for (std::size_t i = 0; i < value.size();) {
        if (value[i] != '{') { result += value[i++]; continue; }
        const std::size_t end = value.find('}', i + 1);
        if (end == std::string::npos) { result += value.substr(i); break; }
        const std::string tag = value.substr(i + 1, end - i - 1);
        i = end + 1;
        const auto isTimer = [&](const char* prefix) {
            const std::string name(prefix);
            if (tag.compare(0, name.size(), name) != 0) return false;
            std::size_t start = name.size();
            if (start < tag.size() && (tag[start] == '+' || tag[start] == '-')) ++start;
            return start < tag.size() && std::all_of(tag.begin() + start, tag.end(),
                [](unsigned char c) { return c >= '0' && c <= '9'; });
        };
        if (isDave && tag == "SHAKE" && !removedShake) { removedShake = true; continue; }
        if (isDave && tag == "NO_CLICK" && !removedNoClick) { removedNoClick = true; continue; }
        const bool hiddenDave = isDave &&
            (daveCommands.count(tag) != 0 || isTimer("DELAY_") || isTimer("TIME_"));
        unsigned char id = 0x7F;
        char marker = '\x1B';
        for (std::size_t p = 0; p < sizeof(placeholders) / sizeof(placeholders[0]); ++p) {
            if (tag == placeholders[p]) { id = static_cast<unsigned char>(p + 1); marker = '\x1C'; break; }
        }
        if (marker == '\x1B' && !hiddenDave) {
            // Preserve the original ordered starts_with semantics, including
            // suffixes on a recognized format name and no-op unknown tags.
            for (std::size_t f = 0; f < sizeof(formats) / sizeof(formats[0]); ++f) {
                if (tag.compare(0, std::char_traits<char>::length(formats[f]), formats[f]) == 0) {
                    id = static_cast<unsigned char>(f + 1); break;
                }
            }
        }
        result += marker;
        result += static_cast<char>(id);
    }
    return result;
}

// 值 -> C++ 窄字符串字面量（纯 ASCII 直出；引号/反斜杠/换行转义；
// 非打印字节用 \xNN 并在后面跟十六进制字符时用断接续避免吞并）。
std::string EscapeValue(const std::string& v)
{
    std::string out = "\"";
    for (std::size_t i = 0; i < v.size(); i++) {
        const unsigned char c = static_cast<unsigned char>(v[i]);
        switch (c) {
        case '"': out += "\\\""; break;
        case '\\': out += "\\\\"; break;
        case '\n': out += "\\n"; break;
        case '\r': out += "\\r"; break;
        case '\t': out += "\\t"; break;
        default:
            if (c >= 0x20 && c <= 0x7E) {
                out += static_cast<char>(c);
            } else {
                char buf[8];
                std::snprintf(buf, sizeof(buf), "\\x%02X", c);
                out += buf;
                if (i + 1 < v.size()) {
                    const unsigned char next = static_cast<unsigned char>(v[i + 1]);
                    if (std::isxdigit(next)) out += "\" \"";
                }
            }
            break;
        }
    }
    out += '"';
    return out;
}

} // namespace

std::string MakeBakedHeader(const std::vector<BakedString>& strings)
{
    std::ostringstream out;
    out << "//LawnStrings.h\n";
    out << "// 由 reanimc bake-strings 从 properties\\LawnStrings.txt 生成。DO NOT EDIT.\n";
    out << "//\n";
    out << "// UI text values are preserved; style/placeholder names and Dave commands\n";
    out << "// are compiled offline into numeric controls and dialogue metadata.\n";
    out << "// 调用点用 gStr.<键> 直取：零 map 查找、零装载期转换分配。\n";
    out << "// 代码未引用的键也生成，改版加键/本地化改文只需重跑烘焙重编译。\n";
    out << "// 下一阶段愿景：properties 编译成顺序二进制后，键名与键表完全退役，\n";
    out << "// gStr 成为唯一事实源；本文件即为此预留的落点。\n";
    out << "\n";
    out << "#ifndef __GENERATED_LAWNSTRINGS_H__\n";
    out << "#define __GENERATED_LAWNSTRINGS_H__\n";
    out << "\n";
    out << "// 本头文件只依赖 <string>：SexyString 在本工程即 std::string\n";
    out << "//（SexyAppFramework/Common.h），用 std::string 声明成员可与任何\n";
    out << "// 包含顺序组合，不会把 windows.h 拖进早期包含链。\n";
    out << "#include <string>\n";
    out << "\n";
    out << "struct LawnStrings\n";
    out << "{\n";
    out << "\tstatic const std::string OPTIONS_DISPLAY_MODE;\n";
    bool renamed = false;
    for (const BakedString& s : strings) {
        out << "\tstd::string " << s.ident << ";";
        if (s.ident != s.key) {
            out << "  // 原键名: [" << s.key << "]";
            renamed = true;
        }
        out << "\n";
    }
    out << "};\n";
    out << "\n";
    out << "extern const LawnStrings gStr;\n";
    out << "\n";
    out << "#endif  //__GENERATED_LAWNSTRINGS_H__\n";
    return out.str();
}

std::string MakeBakedSource(const std::vector<BakedString>& strings)
{
    std::ostringstream out;
    out << "//LawnStrings.cpp\n";
    out << "// Generated by reanimc bake-strings or tools/gen_numeric_text_controls.py. DO NOT EDIT.\n";
    out << "// UI text is unchanged; style/placeholder names and Dave commands are compiled offline.\n";
    out << "\n";
    out << "#include \"LawnStrings.h\"\n";
    out << "\n";
    out << "#if defined(__ANDROID__)\n";
    out << "const std::string LawnStrings::OPTIONS_DISPLAY_MODE = \"HDR (disabled)\";\n";
    out << "#else\n";
    out << "const std::string LawnStrings::OPTIONS_DISPLAY_MODE = \"Full Screen\";\n";
    out << "#endif\n\n";
    out << "const LawnStrings gStr =\n";
    out << "{\n";
    for (std::size_t i = 0; i < strings.size(); i++) {
        out << "\t" << EscapeValue(BakeUiTextValue(strings[i].key, strings[i].value)) << ",";
        out << "  // " << strings[i].key;
        if (strings[i].ident != strings[i].key) out << " (" << strings[i].ident << ")";
        out << "\n";
    }
    out << "};\n";
    return out.str();
}

// ---------------------------------------------------------------------------
// 实体表（--entity-tables）
// ---------------------------------------------------------------------------
namespace {

// 数值键族前缀的可读数组名。未列出的前缀走通用 CamelCase 规则。
std::string PrettyFamilyIdent(const std::string& prefix)
{
    static const std::map<std::string, std::string> kPretty = {
        {"TREE_OF_WISDOM", "TreeWisdom"},
        {"CRAZY_DAVE", "CrazyDave"},
        {"CREDITS_NAMES", "CreditsNames"},
        {"CREDITS_ROLES", "CreditsRoles"},
        {"I_ZOMBIE", "IZombie"},
        {"SCARY_POTTER", "ScaryPotter"},
        {"WIN_MESSAGE", "WinMessage"},
        {"BEAT_GAME_MESSAGE", "BeatGameMessage"},
        {"WAR_AND_PEAS", "WarAndPeas"},
    };
    const auto it = kPretty.find(prefix);
    if (it != kPretty.end()) return it->second;

    std::string camel;
    bool upperNext = true;
    for (const char c : prefix) {
        if (c == '_') { upperNext = true; continue; }
        camel += upperNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c)))
                           : static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        upperNext = false;
    }
    return camel;
}

std::string FamilyArrayName(const std::string& prefix)
{
    const std::string ident = PrettyFamilyIdent(prefix);
    const bool pluralTail = prefix == "CREDITS_NAMES" || prefix == "CREDITS_ROLES";
    return "g" + ident + (pluralTail ? "" : "Lines");
}

std::string FamilyCountName(const std::string& prefix)
{
    // 数组名去尾部 s + "Count"：gCrazyDaveLines -> gCrazyDaveLineCount，
    // gCreditsNames -> gCreditsNameCount。
    std::string name = FamilyArrayName(prefix);
    if (!name.empty() && name.back() == 's') name.pop_back();
    return name + "Count";
}

} // namespace

std::vector<EntityFamily> CollectNumericFamilies(const std::vector<BakedString>& strings,
                                                 bool verboseNotes)
{
    // 前缀 -> (键号 -> strings 下标)。前缀 = 键剥离尾部数字，再剥离至多一个
    // 尾部下划线（兼容 TREE_OF_WISDOM_1 与 CREDITS_NAMES1 两种形态）。
    std::map<std::string, std::map<long long, std::size_t>> byPrefix;
    std::vector<std::string> order;
    for (std::size_t i = 0; i < strings.size(); i++) {
        const std::string& key = strings[i].key;
        std::size_t digits = 0;
        while (digits < key.size() &&
               std::isdigit(static_cast<unsigned char>(key[key.size() - 1 - digits])))
            digits++;
        if (digits == 0 || digits == key.size()) continue;  // 无数字或纯数字键
        std::size_t prefixLen = key.size() - digits;
        if (prefixLen > 0 && key[prefixLen - 1] == '_') prefixLen--;
        if (prefixLen == 0) continue;
        const std::string prefix = key.substr(0, prefixLen);
        // 前缀必须以字母/下划线收尾（避免把 WORD2WORD 之类误切）。
        const char last = prefix.back();
        if (!(std::isalpha(static_cast<unsigned char>(last)) || last == '_')) continue;

        const long long index = std::stoll(key.substr(key.size() - digits));
        if (byPrefix.find(prefix) == byPrefix.end()) order.push_back(prefix);
        byPrefix[prefix][index] = i;
    }

    std::vector<EntityFamily> families;
    for (const std::string& prefix : order) {
        const auto& members = byPrefix[prefix];
        const auto first = members.find(1);
        if (first == members.end()) continue;          // 族必须从 1 开始
        long long expect = 1;
        for (const auto& kv : members) {
            if (kv.first != expect) break;             // 有空洞：止步于连续前缀
            expect++;
        }
        const std::size_t count = static_cast<std::size_t>(expect - 1);
        if (count < 2) continue;                       // 单元素族多为巧合命名
        if (members.size() != count) {
            // 稀疏族（如 TREE_OF_WISDOM：1..49 连续 + 50/60/.../1100）不能只
            // 烘焙连续前缀——运行期会取到洞外的键。整族改走键表查找。
            if (verboseNotes) {
                std::cout << "bake-strings: note: numeric family [" << prefix
                          << "_N] is sparse (contiguous 1.." << count << " of "
                          << members.size() << " keys); skipping array, use the key table\n";
            }
            continue;
        }
        EntityFamily family;
        family.prefix = prefix;
        family.arrayName = FamilyArrayName(prefix);
        family.countName = FamilyCountName(prefix);
        family.count = count;
        for (long long idx = 1; idx <= static_cast<long long>(count); idx++)
            family.entryIndex.push_back(members.at(idx));
        families.push_back(std::move(family));
    }
    return families;
}

namespace {

const char* const kPlantTextKeys[] = {
    "PEASHOOTER", "SUNFLOWER", "CHERRY_BOMB", "WALL_NUT", "POTATO_MINE", "SNOW_PEA", "CHOMPER", "REPEATER",
    "PUFF_SHROOM", "SUN_SHROOM", "FUME_SHROOM", "GRAVE_BUSTER", "HYPNO_SHROOM", "SCAREDY_SHROOM", "ICE_SHROOM", "DOOM_SHROOM",
    "LILY_PAD", "SQUASH", "THREEPEATER", "TANGLE_KELP", "JALAPENO", "SPIKEWEED", "TORCHWOOD", "TALL_NUT", "SEA_SHROOM",
    "PLANTERN", "CACTUS", "BLOVER", "SPLIT_PEA", "STARFRUIT", "PUMPKIN", "MAGNET_SHROOM", "CABBAGE_PULT", "FLOWER_POT",
    "KERNEL_PULT", "COFFEE_BEAN", "GARLIC", "UMBRELLA_LEAF", "MARIGOLD", "MELON_PULT", "GATLING_PEA", "TWIN_SUNFLOWER",
    "GLOOM_SHROOM", "CATTAIL", "WINTER_MELON", "GOLD_MAGNET", "SPIKEROCK", "COB_CANNON", "IMITATER", "EXPLODE_O_NUT",
    "GIANT_WALLNUT", "SPROUT", "REPEATER"
};
const char* const kZombieTextKeys[] = {
    "ZOMBIE", "FLAG_ZOMBIE", "CONEHEAD_ZOMBIE", "POLE_VAULTING_ZOMBIE", "BUCKETHEAD_ZOMBIE", "NEWSPAPER_ZOMBIE",
    "SCREEN_DOOR_ZOMBIE", "FOOTBALL_ZOMBIE", "DANCING_ZOMBIE", "BACKUP_DANCER", "DUCKY_TUBE_ZOMBIE", "SNORKEL_ZOMBIE",
    "ZOMBONI", "ZOMBIE_BOBSLED_TEAM", "DOLPHIN_RIDER_ZOMBIE", "JACK_IN_THE_BOX_ZOMBIE", "BALLOON_ZOMBIE", "DIGGER_ZOMBIE",
    "POGO_ZOMBIE", "ZOMBIE_YETI", "BUNGEE_ZOMBIE", "LADDER_ZOMBIE", "CATAPULT_ZOMBIE", "GARGANTUAR", "IMP", "BOSS",
    "ZOMBIE", "ZOMBIE", "ZOMBIE", "ZOMBIE", "ZOMBIE", "ZOMBIE", "REDEYED_GARGANTUAR", "ZOMBATAR_FLAG_ZOMBIE"
};

std::map<int, const BakedString*> EntityMessageSlots(const std::vector<BakedString>& strings, const std::string& prefix)
{
    std::map<int, const BakedString*> slots;
    for (const BakedString& s : strings) {
        if (s.key.compare(0, prefix.size() + 1, prefix + "_") != 0) continue;
        const std::string index = s.key.substr(prefix.size() + 1);
        if (index.empty() || !std::all_of(index.begin(), index.end(), [](unsigned char c) { return std::isdigit(c); })) continue;
        slots.emplace(std::stoi(index), &s);
    }
    return slots;
}

void WriteEntityValueTable(std::ostringstream& out, const char* name, const char* const* keys,
                           std::size_t count, const char* suffix, const char* bound,
                           const std::map<std::string, const BakedString*>& strings)
{
    out << "static_assert(" << bound << " == " << count << ");\n";
    out << "constexpr const std::string* k" << name << "[" << bound << "] =\n{\n";
    for (std::size_t i = 0; i < count; ++i) {
        const auto found = strings.find(std::string(keys[i]) + suffix);
        if (found == strings.end()) out << "\tnullptr,\n";
        else out << "\t&gStr." << found->second->ident << ",\n";
    }
    out << "};\n\n";
}

void WriteEntityMessageTable(std::ostringstream& out, const char* name, const std::map<int, const BakedString*>& slots)
{
    const int limit = slots.empty() ? 0 : slots.rbegin()->first + 1;
    out << "constexpr const std::string* k" << name << "Messages[g" << name << "MessageLimit] =\n{\n";
    for (int i = 0; i < limit; ++i) {
        if (i % 8 == 0) out << '\t';
        const auto found = slots.find(i);
        if (found == slots.end()) out << "nullptr";
        else out << "&gStr." << found->second->ident;
        out << ',';
        if (i % 8 == 7 || i + 1 == limit) out << '\n';
        else out << ' ';
    }
    out << "};\nconstexpr std::uint64_t k" << name << "Presence[" << (limit + 63) / 64 << "] =\n{\n";
    for (int word = 0; word < (limit + 63) / 64; ++word) {
        unsigned long long bits = 0;
        for (const auto& slot : slots)
            if (slot.first / 64 == word) bits |= 1ULL << (slot.first % 64);
        char value[32];
        std::snprintf(value, sizeof(value), "\t0x%016llXULL,\n", bits);
        out << value;
    }
    out << "};\n\n";
}

} // namespace

std::string MakeEntityHeader(const std::vector<BakedString>& strings)
{
    const auto families = CollectNumericFamilies(strings, false);
    const auto dave = EntityMessageSlots(strings, "CRAZY_DAVE");
    const auto tree = EntityMessageSlots(strings, "TREE_OF_WISDOM");
    std::ostringstream out;
    out << "// Generated by reanimc bake-strings --entity-tables or tools/gen_entity_strings.py.\n"
           "// Runtime tables contain UI values and numeric indices only.\n"
           "#ifndef __GENERATED_ENTITYSTRINGS_H__\n#define __GENERATED_ENTITYSTRINGS_H__\n\n"
           "#include \"../../ConstEnums.h\"\n#include <string>\n\n";
    for (const EntityFamily& family : families) {
        out << "inline constexpr int " << family.countName << " = " << family.count << ";\n";
        out << "extern const std::string " << family.arrayName << "[" << family.count + 1 << "];\n";
    }
    out << "\ninline constexpr int gCrazyDaveMessageLimit = " << (dave.empty() ? 0 : dave.rbegin()->first + 1) << ";\n";
    out << "inline constexpr int gTreeOfWisdomMessageLimit = " << (tree.empty() ? 0 : tree.rbegin()->first + 1) << ";\n\n";
    out << "const std::string& EntityPlantName(SeedType theSeedType);\n"
           "const std::string& EntityPlantToolTip(SeedType theSeedType);\n"
           "const std::string& EntityPlantDescription(SeedType theSeedType);\n"
           "const std::string& EntityZombieName(ZombieType theZombieType);\n"
           "const std::string& EntityZombieDescription(ZombieType theZombieType);\n"
           "const std::string& EntityCrazyDaveText(int theMessageIndex);\n"
           "bool EntityCrazyDaveHasMessage(int theMessageIndex);\n"
           "const std::string& EntityTreeOfWisdomText(int theMessageIndex);\n"
           "bool EntityTreeOfWisdomHasMessage(int theMessageIndex);\n\n"
           "// Compatibility for callers that already supply rendered UI values.\n"
           "inline const std::string& EntityTranslate(const std::string& theText) { return theText; }\n"
           "inline std::string EntityTranslate(const char* theText) { return theText ? theText : \"\"; }\n\n"
           "#endif\n";
    return out.str();
}

std::string MakeEntitySource(const std::vector<BakedString>& strings)
{
    const auto families = CollectNumericFamilies(strings, false);
    std::map<std::string, const BakedString*> byKey;
    for (const BakedString& s : strings) byKey.emplace(s.key, &s);
    std::ostringstream out;
    out << "// Generated by reanimc bake-strings --entity-tables or tools/gen_entity_strings.py.\n"
           "#include \"EntityStrings.h\"\n#include <cstddef>\n#include <cstdint>\n\n";
    for (const EntityFamily& family : families) {
        out << "const std::string " << family.arrayName << "[" << family.count + 1 << "] =\n{\n\t\"\",\n";
        for (const auto index : family.entryIndex)
            out << '\t' << EscapeValue(BakeUiTextValue(strings[index].key, strings[index].value)) << ",\n";
        out << "};\n\n";
    }
    out << "namespace\n{\nconst std::string kMissingEntityText = \"<Missing entity text>\";\n"
           "const std::string kMissingMessage = \"<Missing message>\";\n\n";
    const auto plantCount = sizeof(kPlantTextKeys) / sizeof(kPlantTextKeys[0]);
    const auto zombieCount = sizeof(kZombieTextKeys) / sizeof(kZombieTextKeys[0]);
    WriteEntityValueTable(out, "PlantNames", kPlantTextKeys, plantCount, "", "NUM_SEED_TYPES", byKey);
    WriteEntityValueTable(out, "PlantToolTips", kPlantTextKeys, plantCount, "_TOOLTIP", "NUM_SEED_TYPES", byKey);
    WriteEntityValueTable(out, "PlantDescriptions", kPlantTextKeys, plantCount, "_DESCRIPTION", "NUM_SEED_TYPES", byKey);
    WriteEntityValueTable(out, "ZombieNames", kZombieTextKeys, zombieCount, "", "NUM_ZOMBIE_TYPES", byKey);
    WriteEntityValueTable(out, "ZombieDescriptions", kZombieTextKeys, zombieCount, "_DESCRIPTION", "NUM_ZOMBIE_TYPES", byKey);
    WriteEntityMessageTable(out, "CrazyDave", EntityMessageSlots(strings, "CRAZY_DAVE"));
    WriteEntityMessageTable(out, "TreeOfWisdom", EntityMessageSlots(strings, "TREE_OF_WISDOM"));
    out << "template <std::size_t N>\n"
           "const std::string& TextAt(const std::string* const (&theValues)[N], int theIndex, const std::string& theMissing)\n"
           "{\n\tif (static_cast<unsigned>(theIndex) < N && theValues[theIndex])\n"
           "\t\treturn *theValues[theIndex];\n\treturn theMissing;\n}\n\n"
           "template <std::size_t N>\n"
           "bool MessagePresent(const std::uint64_t (&thePresence)[N], int theLimit, int theIndex)\n"
           "{\n\treturn static_cast<unsigned>(theIndex) < static_cast<unsigned>(theLimit) &&\n"
           "\t\t(thePresence[theIndex / 64] & (std::uint64_t{1} << (theIndex % 64))) != 0;\n"
           "}\n} // namespace\n\n";
    static constexpr const char* helpers[][3] = {
        {"PlantName", "PlantNames", "SeedType theSeedType"},
        {"PlantToolTip", "PlantToolTips", "SeedType theSeedType"},
        {"PlantDescription", "PlantDescriptions", "SeedType theSeedType"},
        {"ZombieName", "ZombieNames", "ZombieType theZombieType"},
        {"ZombieDescription", "ZombieDescriptions", "ZombieType theZombieType"}
    };
    for (const auto& helper : helpers) {
        const std::string arg = helper[2];
        out << "const std::string& Entity" << helper[0] << '(' << arg << ")\n{\n"
            << "\treturn TextAt(k" << helper[1] << ", " << arg.substr(arg.find(' ') + 1) << ", kMissingEntityText);\n}\n\n";
    }
    for (const char* name : {"CrazyDave", "TreeOfWisdom"}) {
        out << "const std::string& Entity" << name << "Text(int theMessageIndex)\n{\n"
            << "\treturn TextAt(k" << name << "Messages, theMessageIndex, kMissingMessage);\n}\n\n"
            << "bool Entity" << name << "HasMessage(int theMessageIndex)\n{\n"
            << "\treturn MessagePresent(k" << name << "Presence, g" << name << "MessageLimit, theMessageIndex);\n}\n\n";
    }
    std::string result = out.str();
    result.pop_back(); // One final newline, matching the source-only generator.
    return result;
}

// ---------------------------------------------------------------------------
// 调用点改写引擎
// ---------------------------------------------------------------------------
std::vector<RewriteEdit> RewriteTranslateCallsites(const std::vector<fs::path>& sources,
                                                   const std::set<std::string>& keys,
                                                   bool apply, RewriteReport& report)
{
    std::vector<RewriteEdit> edits;

    // 展开文件集合（目录递归 *.cpp/*.h，排序保证确定性）。
    std::vector<fs::path> files;
    for (const fs::path& src : sources) {
        std::error_code ec;
        if (fs::is_directory(src, ec)) {
            for (const auto& entry : fs::recursive_directory_iterator(src)) {
                const fs::path& p = entry.path();
                if (p.extension() == ".cpp" || p.extension() == ".h")
                    files.push_back(p);
            }
        } else if (fs::exists(src, ec)) {
            files.push_back(src);
        } else {
            std::cerr << "bake-strings: source not found: " << src.string() << '\n';
        }
    }
    std::sort(files.begin(), files.end());
    files.erase(std::unique(files.begin(), files.end()), files.end());

    for (const fs::path& file : files) {
        // 防御：跳过生成物自身。
        if (file.filename() == "LawnStrings.h" || file.filename() == "LawnStrings.cpp") continue;

        std::string text;
        if (!ReadFileBytes(file, text)) continue;
        const std::vector<Region> regions = LexRegions(text);
        report.filesScanned++;

        std::vector<RewriteEdit> fileEdits;
        // 已被本次改写占用的字符串字面量位置，避免同一实参被两条规则重复改。
        std::vector<std::pair<std::size_t, std::size_t>> consumed;

        const auto isConsumed = [&](std::size_t b, std::size_t e) {
            for (const auto& c : consumed)
                if (c.first == b && c.second == e) return true;
            return false;
        };

        // --- 扫描白名单调用 ---
        for (const CalleeRule& rule : kCalleeRules) {
            const std::string name(rule.name);
            for (std::size_t i = 0; i < text.size(); i++) {
                if (regions[i] != Region::Code) continue;
                if (text.compare(i, name.size(), name) != 0) continue;
                if (i > 0 && IsIdentChar(text[i - 1])) continue;
                if (i + name.size() < text.size() && IsIdentChar(text[i + name.size()])) continue;

                // 跳过空白找 '('
                std::size_t p = i + name.size();
                while (p < text.size() && regions[p] == Region::Code && IsSpaceChar(text[p])) p++;
                if (p >= text.size() || text[p] != '(') continue;

                // 解析实参列表：按顶层逗号分割（只统计代码区括号深度）。
                std::size_t depth = 1;
                std::size_t argStart = p + 1;
                std::vector<std::pair<std::size_t, std::size_t>> argSpans;  // [begin,end)
                for (std::size_t j = p + 1; j < text.size(); j++) {
                    if (regions[j] != Region::Code) continue;
                    const char c = text[j];
                    if (c == '(') depth++;
                    else if (c == ')') {
                        depth--;
                        if (depth == 0) {
                            argSpans.emplace_back(argStart, j);
                            break;
                        }
                    } else if (c == ',' && depth == 1) {
                        argSpans.emplace_back(argStart, j);
                        argStart = j + 1;
                    }
                }
                if (depth != 0) continue;  // 没找到匹配的 ')'，放弃这个候选

                // 检查每个白名单实参位。
                for (int ai = 0; ai < rule.argCount; ai++) {
                    const int argIdx = rule.args[ai];
                    if (argIdx < 0 || argIdx >= (int)argSpans.size()) continue;
                    const std::string span = TrimWs(
                        text.substr(argSpans[argIdx].first, argSpans[argIdx].second - argSpans[argIdx].first));
                    if (span.size() < 3) continue;

                    // 语法： [_S (] string [)] 且字面量是唯一内容。
                    std::size_t q = 0;
                    bool hasWrap = false;
                    if (span.compare(q, 3, "_S(") == 0) { q += 3; hasWrap = true; }
                    else if (span.size() >= 4 && span.compare(q, 3, "_S ") == 0 &&
                             span.find('(', 3) != std::string::npos) {
                        // 宽松形态 "_S ("：跳过空白到 '('
                        while (q < span.size() && IsSpaceChar(span[q])) q++;
                        if (q < span.size() && span[q] == '_') {
                            std::size_t w = q;
                            while (w < span.size() && IsIdentChar(span[w])) w++;
                            if (span.substr(q, w - q) == "_S") {
                                while (w < span.size() && IsSpaceChar(span[w])) w++;
                                if (w < span.size() && span[w] == '(') { q = w + 1; hasWrap = true; }
                            }
                        }
                    }
                    while (q < span.size() && IsSpaceChar(span[q])) q++;
                    if (q >= span.size() || span[q] != '"') continue;
                    // 字面量必须一直到 span 结尾（只剩收尾 ')'）
                    std::size_t litEnd = q + 1;
                    while (litEnd < span.size()) {
                        if (span[litEnd] == '\\') { litEnd += 2; continue; }
                        if (span[litEnd] == '"') break;
                        litEnd++;
                    }
                    if (litEnd >= span.size()) continue;  // 字面量没闭合
                    std::size_t after = litEnd + 1;
                    while (after < span.size() && IsSpaceChar(span[after])) after++;
                    if (hasWrap) {
                        if (after >= span.size() || span[after] != ')') continue;
                        after++;
                        while (after < span.size() && IsSpaceChar(span[after])) after++;
                    }
                    if (after != span.size()) continue;  // 实参不是"单个键字面量"，不动

                    const std::string content = span.substr(q + 1, litEnd - q - 1);
                    std::string key;
                    if (!ParseBracketKey(content, key)) continue;
                    if (!IsFullyUpperKey(key)) continue;  // 小写键查找本来就会 Miss，保持原样
                    if (keys.find(key) == keys.end()) continue;  // txt 没有的键：留给人工裁决

                    // 计算该实参在原文中的绝对区间（trim 后）。
                    const auto& sp = argSpans[argIdx];
                    std::size_t absBegin = sp.first, absEnd = sp.second;
                    while (absBegin < absEnd && IsSpaceChar(text[absBegin])) absBegin++;
                    while (absEnd > absBegin && IsSpaceChar(text[absEnd - 1])) absEnd--;
                    if (isConsumed(absBegin, absEnd)) continue;
                    consumed.emplace_back(absBegin, absEnd);

                    RewriteEdit edit;
                    edit.file = file;
                    edit.line = LineOf(text, absBegin);
                    edit.begin = absBegin;
                    edit.end = absEnd;
                    edit.before = text.substr(absBegin, absEnd - absBegin);
                    edit.after = "gStr." + MakeStringMemberIdent(key) + (rule.cStr ? ".c_str()" : "");
                    edit.callee = rule.name;
                    edit.key = key;
                    fileEdits.push_back(std::move(edit));
                }

                // 跳过本次调用名继续外层扫描。
                i = p;
            }
        }

        // --- 统计：全部方括号键字面量（含未改写的），动态键，未知接收者 ---
        for (std::size_t i = 0; i < text.size(); i++) {
            if (regions[i] != Region::String || text[i] != '"') continue;
            // 只看字面量起点（前一个字节不是字符串区内），避免重复统计。
            std::string content;
            std::size_t endPos = 0;
            if (!StringLiteralContent(text, regions, i, content, &endPos)) continue;
            if (i > 0 && regions[i - 1] == Region::String) continue;
            std::string key;
            if (!ParseBracketKey(content, key)) {
                if (!content.empty() && content[0] == '[' && content.find('%') != std::string::npos)
                    report.dynamicKeyUses++;
                continue;
            }
            report.bracketLiteralUses++;
            if (IsFullyUpperKey(key) && keys.find(key) == keys.end())
                report.sourceKeysMissingFromTxt.insert(key);
            const bool rewritten = std::any_of(
                fileEdits.begin(), fileEdits.end(),
                [&](const RewriteEdit& e) { return e.begin <= i && i < e.end; });
            // 同行回溯找接收函数名（报告用途）。`_S(` 形态会先撞到包装宏，
            // 需要继续向前回溯真正的调用名。
            std::string callee = "<none>";
            {
                const std::size_t lineStart = text.rfind('\n', i);
                const std::size_t ls = lineStart == std::string::npos ? 0 : lineStart + 1;
                std::size_t searchEnd = i;  // 在 [ls, searchEnd) 内从右往左找 ident(
                for (int round = 0; round < 4; round++) {
                    std::size_t best = std::string::npos;
                    for (std::size_t j = ls; j + 1 < searchEnd; j++) {
                        if (regions[j] != Region::Code) continue;
                        if (IsIdentChar(text[j]) && !IsIdentChar(text[j + 1])) {
                            std::size_t k = j + 1;
                            while (k < searchEnd && IsSpaceChar(text[k])) k++;
                            if (k < searchEnd && text[k] == '(') best = j;
                        }
                    }
                    if (best == std::string::npos) break;
                    while (best > ls && IsIdentChar(text[best - 1])) best--;  // 回到标识符开头
                    std::size_t e = best;
                    while (e < searchEnd && IsIdentChar(text[e])) e++;
                    callee = text.substr(best, e - best);
                    if (callee != "_S") break;
                    searchEnd = best;  // 跳过 _S( 宏继续回溯
                    callee = "<none>";
                }
            }
            if (rewritten) {
                report.residualWholeLiteralByCallee[callee];  // 计入已改写（值留空）
            } else {
                bool known = false;
                for (const CalleeRule& rule : kCalleeRules)
                    if (callee == rule.name) { known = true; break; }
                if (known)
                    report.residualWholeLiteralByCallee[callee]++;
                else
                    report.unknownReceiverLiterals[callee]++;
                std::ostringstream loc;
                loc << file.string() << ":" << LineOf(text, i) << ": [" << key << "] -> "
                    << callee;
                report.residualLocations.push_back(loc.str());
            }
        }

        if (!fileEdits.empty()) {
            if (apply) {
                std::sort(fileEdits.begin(), fileEdits.end(),
                          [](const RewriteEdit& a, const RewriteEdit& b) { return a.begin > b.begin; });
                for (const RewriteEdit& e : fileEdits)
                    text.replace(e.begin, e.end - e.begin, e.after);
                if (!WriteFileBytes(file, text)) continue;
            }
            for (const RewriteEdit& e : fileEdits) {
                report.edits++;
                if (e.callee == std::string("TodStringTranslate")) report.translatedCallArgs++;
                report.editsByCallee[e.callee]++;
            }
            edits.insert(edits.end(), fileEdits.begin(), fileEdits.end());
            if (!apply) {
                // dry-run：打印预览。
                for (const RewriteEdit& e : fileEdits) {
                    std::cout << file.string() << ":" << e.line << ": " << e.callee << " arg "
                              << e.key << "\n    - " << e.before << "\n    + " << e.after << '\n';
                }
            }
        }
    }
    return edits;
}

} // namespace stringid
} // namespace reanim
} // namespace openpvz
