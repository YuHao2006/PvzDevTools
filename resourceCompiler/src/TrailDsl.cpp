#include "reanim/TrailDsl.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <stdexcept>

namespace openpvz::reanim::trail {
namespace {

[[noreturn]] void Fail(std::string* error, const std::string& message)
{
    if (error) *error = message;
    throw std::runtime_error(message);
}

std::string Trim(const std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

bool IEquals(std::string_view text, std::string_view lower)
{
    if (text.size() != lower.size()) return false;
    for (std::size_t i = 0; i < text.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(text[i])) != lower[i]) return false;
    return true;
}

// 与 Pak64.cpp 的资源名规范化一致：image_/font_ 前缀整体大写。
std::string CanonicalResource(std::string name)
{
    name = Trim(name);
    const bool image = name.size() >= 6 && IEquals(name.substr(0, 6), "image_");
    const bool font = name.size() >= 5 && IEquals(name.substr(0, 5), "font_");
    if (image || font)
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

// sscanf %f 语义：跳过前导空白，取最长数字前缀。
bool ParseFloatPrefix(const std::string_view text, float& value)
{
    const char* first = text.data();
    const char* last = text.data() + text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    if (first == last) return false;
    char* end = nullptr;
    value = std::strtof(first, &end);
    return end != first;
}

// sscanf %d 语义：跳过前导空白，取最长整数前缀。
bool ParseIntPrefix(const std::string_view text, int& value)
{
    const char* first = text.data();
    const char* last = text.data() + text.size();
    while (first < last && std::isspace(static_cast<unsigned char>(*first))) ++first;
    if (first == last) return false;
    char* end = nullptr;
    const long parsed = std::strtol(first, &end, 10);
    if (end == first) return false;
    value = static_cast<int>(parsed);
    return true;
}

// 最短可往返浮点字面量（先 %.6g，回读位级不符再 %.9g）。
std::string FloatText(const float value)
{
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "%g", value);
    float back = 0.0f;
    if (ParseFloatPrefix(buffer, back) &&
        std::memcmp(&back, &value, sizeof(float)) == 0)
        return buffer;
    std::snprintf(buffer, sizeof buffer, "%.9g", value);
    return buffer;
}

// 曲线名表（gDefTrackEaseSymbols 顺序，大小写敏感前缀匹配，
// 表序保证 "EaseInOutWeak" 先于 "EaseInOut" 被命中）。
struct CurveName final {
    std::int32_t value;
    const char* name;
};
constexpr CurveName kCurveNames[] = {
    {5, "EaseInOutWeak"},   {7, "FastInOutWeak"},   {4, "EaseInOut"},
    {6, "FastInOut"},       {2, "EaseIn"},          {3, "EaseOut"},
    {13, "EaseSinWave"},    {10, "BounceFastMiddle"}, {11, "BounceSlowMiddle"},
    {9, "Bounce"},          {12, "SinWave"},        {1, "Linear"},
};

// oracle 轨道槽位顺序（安装包记录的实际写入序，见 TrailDsl.h 文件头）。
constexpr const char* kTrackSlots[5] = {
    "TrailDuration", "WidthOverLength", "WidthOverTime", "AlphaOverLength", "AlphaOverTime",
};

const std::string& TrackTextOf(const RawTrail& source, std::string_view slot)
{
    if (slot == "TrailDuration") return source.trailDuration;
    if (slot == "WidthOverLength") return source.widthOverLength;
    if (slot == "WidthOverTime") return source.widthOverTime;
    if (slot == "AlphaOverLength") return source.alphaOverLength;
    return source.alphaOverTime;
}

std::uint32_t Adler32(const std::vector<std::uint8_t>& bytes)
{
    std::uint32_t a = 1, b = 0;
    for (const std::uint8_t byte : bytes) {
        a = (a + byte) % 65521;
        b = (b + a) % 65521;
    }
    return (b << 16) | a;
}

void WriteEscaped(std::ostream& out, const std::string_view text)
{
    out << '"';
    for (const char c : text) {
        if (c == '"' || c == '\\') out << '\\';
        if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else out << c;
    }
    out << '"';
}

std::string ReadFileText(const std::filesystem::path& path, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) Fail(error, "cannot open " + path.string());
    std::ostringstream buffer;
    buffer << in.rdbuf();
    return buffer.str();
}

} // namespace

// ------------------------------------------------------------ XML -> DSL 模型

bool ReadTrailXml(const std::filesystem::path& input, RawTrail& output,
                  std::string* error)
{
    try {
        const std::string text = ReadFileText(input, error);
        const std::string_view xml(text);

        output = RawTrail{};
        output.name = input.stem().string();
        output.sourceName = output.name;

        // 已知标签白名单（gTrailDefFields；TrailFlags 只接受 Loops symbol，
        // <TrailFlags> 本体不是合法标签，与官方读取端一致）。
        static constexpr const char* known[] = {"Image",       "MaxPoints",      "MinPointDistance", "Loops",
                               "WidthOverLength", "WidthOverTime", "AlphaOverLength",
                               "AlphaOverTime",   "TrailDuration"};
        std::size_t cursor = 0;
        while (true) {
            const std::size_t openAt = xml.find('<', cursor);
            if (openAt == std::string_view::npos) break;
            const std::size_t closeAt = xml.find('>', openAt);
            if (closeAt == std::string_view::npos) Fail(error, "unterminated tag in " + input.string());
            const std::string tag(xml.substr(openAt + 1, closeAt - openAt - 1));
            cursor = closeAt + 1;
            if (tag.empty() || tag[0] == '?' || tag[0] == '!') continue;  // 声明/注释
            if (tag[0] == '/') continue;  // 结束标签（值在配对时已取）
            const std::size_t endAt = xml.find("</" + tag + ">", cursor);
            if (endAt == std::string_view::npos)
                Fail(error, "unterminated <" + tag + "> in " + input.string());
            const std::string value = Trim(xml.substr(cursor, endAt - cursor));
            cursor = endAt + tag.size() + 3;

            const bool isKnown = std::any_of(std::begin(known), std::end(known),
                                             [&](const char* k) { return tag == k; });
            if (!isKnown) Fail(error, "unknown trail element <" + tag + "> in " + input.string());

            if (tag == "Image") output.image = value;
            else if (tag == "MaxPoints") {
                int parsed = output.maxPoints;
                if (!ParseIntPrefix(value, parsed))
                    Fail(error, "MaxPoints is not an integer: '" + value + "'");
                output.maxPoints = parsed;
            } else if (tag == "MinPointDistance") {
                float parsed = output.minPointDistance;
                if (!ParseFloatPrefix(value, parsed))
                    Fail(error, "MinPointDistance is not a float: '" + value + "'");
                output.minPointDistance = parsed;
            } else if (tag == "Loops") {
                float parsed = 0.0f;
                if (!ParseFloatPrefix(value, parsed))
                    Fail(error, "Loops is not numeric: '" + value + "'");
                output.loops = parsed != 0.0f;  // 官方 %f 再转 int 判零
            } else if (tag == "WidthOverLength") output.widthOverLength = value;
            else if (tag == "WidthOverTime") output.widthOverTime = value;
            else if (tag == "AlphaOverLength") output.alphaOverLength = value;
            else if (tag == "AlphaOverTime") output.alphaOverTime = value;
            else if (tag == "TrailDuration") output.trailDuration = value;
        }
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

std::string TrailToDsl(const RawTrail& source)
{
    std::ostringstream out;
    out << "export component "
        << (source.name.empty() ? std::string("Trail") : source.name) << " {\n";
    out << "    kind: trail;\n";
    out << "    source-name: ";
    WriteEscaped(out, source.sourceName.empty() ? source.name : source.sourceName);
    out << ";\n";
    out << "    image: ";
    if (source.image.empty()) out << "none";
    else WriteEscaped(out, source.image);
    out << ";\n";
    out << "    maxPoints: " << source.maxPoints << ";\n";
    out << "    loops: " << (source.loops ? 1 : 0) << ";\n";
    out << "    minPointDistance: " << FloatText(source.minPointDistance) << ";\n";
    struct TrackField final {
        const char* field;
        const std::string* text;
    };
    const TrackField fields[] = {
        {"trailDuration", &source.trailDuration},
        {"widthOverLength", &source.widthOverLength},
        {"widthOverTime", &source.widthOverTime},
        {"alphaOverLength", &source.alphaOverLength},
        {"alphaOverTime", &source.alphaOverTime},
    };
    for (const TrackField& field : fields) {
        if (field.text->empty()) continue;  // 未提供：轨道计数写 0
        out << "    " << field.field << ": track { " << *field.text << "; }\n";
    }
    out << "}\n";
    return out.str();
}

// ------------------------------------------------------------- FloatTrack 解析

namespace {

// 从 from 开始跳过空白；返回是否仍在串内。
std::size_t SkipSpaces(const std::string_view text, std::size_t from)
{
    while (from < text.size() && std::isspace(static_cast<unsigned char>(text[from]))) ++from;
    return from;
}

// 读一段非空白 run（曲线名候选），返回串尾；无 run 返回 from。
std::size_t ReadRun(const std::string_view text, std::size_t from, std::string_view& run)
{
    const std::size_t begin = SkipSpaces(text, from);
    if (begin >= text.size()) {
        run = {};
        return from;
    }
    std::size_t end = begin;
    while (end < text.size() && !std::isspace(static_cast<unsigned char>(text[end]))) ++end;
    run = text.substr(begin, end - begin);
    return end;
}

// oracle 的曲线名消费：run 以某个曲线名开头（大小写敏感前缀）则赋值；
// 无论是否命中，run 整体被消费掉（数字前瞻被丢弃的历史行为）。
std::size_t ConsumeCurveCandidate(const std::string_view text, std::size_t from,
                                  std::int32_t& curve)
{
    std::string_view run;
    const std::size_t end = ReadRun(text, from, run);
    if (run.empty()) return from;
    for (const CurveName& candidate : kCurveNames)
        if (run.size() >= std::strlen(candidate.name) &&
            run.compare(0, std::strlen(candidate.name), candidate.name) == 0) {
            curve = candidate.value;
            break;
        }
    return end;
}

} // namespace

bool ParseLegacyFloatTrack(const std::string_view text,
                           std::vector<TrackNode>& nodes, std::string* error)
{
    try {
        nodes.clear();
        std::size_t pos = SkipSpaces(text, 0);
        while (pos < text.size()) {
            TrackNode node;  // time=-1 = 未指定，curve/dist = LINEAR
            node.time = -1.0f;

            if (text[pos] == '[') {
                // 范围节点：[lo] | [lo hi] | [lo Dist hi]，可带 ",t"
                ++pos;
                float low = 0.0f;
                if (!ParseFloatPrefix(text.substr(pos), low))
                    Fail(error, "invalid range lower bound in track text");
                pos = SkipSpaces(text, pos);
                {   // 推进过已解析的数字前缀
                    char* end = nullptr;
                    std::strtof(text.data() + pos, &end);
                    pos = static_cast<std::size_t>(end - text.data());
                }
                node.low = node.high = low;
                pos = SkipSpaces(text, pos);
                if (pos < text.size() && text[pos] != ']') {
                    std::string_view run;
                    const std::size_t end = ReadRun(text, pos, run);
                    bool matchedCurve = false;
                    for (const CurveName& candidate : kCurveNames)
                        if (run.size() >= std::strlen(candidate.name) &&
                            run.compare(0, std::strlen(candidate.name), candidate.name) == 0) {
                            node.distribution = candidate.value;
                            pos = SkipSpaces(text, end);
                            matchedCurve = true;
                            break;
                        }
                    if (!matchedCurve) {
                        float high = 0.0f;
                        if (!ParseFloatPrefix(text.substr(pos), high))
                            Fail(error, "invalid range upper bound in track text");
                        node.high = high;
                        char* end = nullptr;
                        std::strtof(text.data() + pos, &end);
                        pos = static_cast<std::size_t>(end - text.data());
                        pos = SkipSpaces(text, pos);
                    }
                }
                if (pos >= text.size() || text[pos] != ']')
                    Fail(error, "range is missing ']' in track text");
                ++pos;
                if (pos < text.size() && text[pos] == ',') {
                    float stamp = 0.0f;
                    if (!ParseFloatPrefix(text.substr(pos + 1), stamp))
                        Fail(error, "invalid range timestamp in track text");
                    node.time = static_cast<float>(static_cast<double>(stamp) * 0.01);
                    char* end = nullptr;
                    std::strtof(text.data() + pos + 1, &end);
                    pos = static_cast<std::size_t>(end - text.data());
                }
            } else {
                // 数值节点：v | v,t
                float value = 0.0f;
                if (!ParseFloatPrefix(text.substr(pos), value))
                    Fail(error, "invalid node value in track text");
                node.low = node.high = value;
                char* end = nullptr;
                std::strtof(text.data() + pos, &end);
                pos = static_cast<std::size_t>(end - text.data());
                if (pos < text.size() && text[pos] == ',') {
                    float stamp = 0.0f;
                    if (!ParseFloatPrefix(text.substr(pos + 1), stamp))
                        Fail(error, "invalid node timestamp in track text");
                    node.time = static_cast<float>(static_cast<double>(stamp) * 0.01);
                    std::strtof(text.data() + pos + 1, &end);
                    pos = static_cast<std::size_t>(end - text.data());
                }
                // oracle 语义：数值节点（含带时间后缀）之后的下一个 run 一律
                // 当曲线名消费；未命中曲线表则整个 run 被丢弃。安装包
                // icetrail 记录的 AlphaOverLength "0,0 .3,20" 因此只剩 1 个
                // 节点 (0,0,0)——复刻该行为以保持字节等价。
                pos = ConsumeCurveCandidate(text, pos, node.curve);
            }
            nodes.push_back(node);
            pos = SkipSpaces(text, pos);
        }

        // 官方时间补齐（DefinitionReadFloatTrackField 尾段）。
        if (!nodes.empty()) {
            std::size_t base = 0;
            float low = 0.0f;
            for (;;) {
                std::size_t index = nodes.size();
                float high = 1.0f;
                for (std::size_t i = base; i < nodes.size(); ++i)
                    if (nodes[i].time >= 0.0f) {
                        index = i;
                        high = nodes[i].time;
                        break;
                    }
                for (std::size_t i = base; i < index; ++i) {
                    float interp;
                    if ((index - 1) - base != 0)
                        interp = static_cast<float>(i - base) /
                                 static_cast<float>((index - 1) - base);
                    else if (base == 0)
                        interp = 0.0f;
                    else
                        interp = 1.0f;
                    nodes[i].time = high * interp + low * (1.0f - interp);
                }
                base = index + 1;
                low = high;
                if (base >= nodes.size()) break;
            }
        }
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

// ------------------------------------------------------------------ DSL 解析

namespace {

class TrailDslParser final {
public:
    TrailDslParser(const std::string_view source, TrailDslFileDirectives* directives,
                   std::vector<RawTrail>& outputs, std::string* error)
        : source_(source), directives_(directives), outputs_(outputs), error_(error) {}

    void Run()
    {
        for (;;) {
            Skip();
            if (position_ >= source_.size()) break;
            const std::string word = ReadWord();
            if (word == "forward" || word == "import") {
                if (!directives_)
                    Fail(error_, "'" + word + "' is a file-level directive; "
                                  "use LoadTrailDslBundle with a root file");
                const std::string reference = ReadString();
                Expect(';');
                if (word == "forward") directives_->forwards.push_back(reference);
                else directives_->imports.push_back(reference);
            } else if (word == "export" || word == "component") {
                const bool isPublic = word == "export";
                if (word == "export") {
                    const std::string next = PeekWord();
                    if (next != "component")
                        Fail(error_, "expected 'component' after 'export', got '" + next + "'");
                    (void)ReadWord();
                }
                RawTrail module;
                module.name = ReadWord();
                if (module.name.empty()) Fail(error_, "missing component name");
                ParseModule(module);
                if (isPublic && directives_)
                    directives_->publicNames.push_back(module.name);
                outputs_.push_back(std::move(module));
            } else {
                Fail(error_, "expected 'forward'/'import'/'component', got '" + word + "'");
            }
        }
    }

private:
    void ParseModule(RawTrail& module)
    {
        Expect('{');
        bool hasKind = false;
        for (;;) {
            Skip();
            if (position_ >= source_.size()) Fail(error_, "unterminated component <" + module.name + ">");
            if (source_[position_] == '}') {
                ++position_;
                break;
            }
            const std::string field = ReadWord();
            Expect(':');
            if (field == "kind") {
                std::string kind = PeekChar() == '"' ? ReadString() : ReadWord();
                if (kind != "trail")
                    Fail(error_, "component <" + module.name + "> has kind '" + kind +
                                     "'; compile-trail only accepts kind: trail");
                hasKind = true;
                Expect(';');
            } else if (field == "source-name") {
                module.sourceName = PeekChar() == '"' ? ReadString() : ReadWord();
                Expect(';');
            } else if (field == "image") {
                if (PeekChar() == '"') module.image = ReadString();
                else {
                    const std::string word = ReadWord();
                    module.image = word == "none" ? std::string() : word;
                }
                Expect(';');
            } else if (field == "maxPoints") {
                const std::string word = ReadWord();
                int parsed = module.maxPoints;
                if (!ParseIntPrefix(word, parsed))
                    Fail(error_, "maxPoints is not an integer: '" + word + "'");
                module.maxPoints = parsed;
                Expect(';');
            } else if (field == "loops") {
                const std::string word = ReadWord();
                float parsed = 0.0f;
                if (!ParseFloatPrefix(word, parsed))
                    Fail(error_, "loops is not numeric: '" + word + "'");
                module.loops = parsed != 0.0f;
                Expect(';');
            } else if (field == "minPointDistance") {
                const std::string word = ReadWord();
                float parsed = module.minPointDistance;
                if (!ParseFloatPrefix(word, parsed))
                    Fail(error_, "minPointDistance is not a float: '" + word + "'");
                module.minPointDistance = parsed;
                Expect(';');
            } else if (field == "trailDuration" || field == "widthOverLength" ||
                       field == "widthOverTime" || field == "alphaOverLength" ||
                       field == "alphaOverTime") {
                const std::string keyword = ReadWord();
                if (keyword != "track")
                    Fail(error_, "field '" + field + "' expects 'track { ... }', got '" + keyword + "'");
                Expect('{');
                const std::size_t end = source_.find('}', position_);
                if (end == std::string_view::npos)
                    Fail(error_, "unterminated track block in <" + module.name + ">");
                std::string raw = Trim(source_.substr(position_, end - position_));
                position_ = end + 1;
                if (!raw.empty() && raw.back() == ';') raw = Trim(raw.substr(0, raw.size() - 1));
                std::string* target = nullptr;
                if (field == "trailDuration") target = &module.trailDuration;
                else if (field == "widthOverLength") target = &module.widthOverLength;
                else if (field == "widthOverTime") target = &module.widthOverTime;
                else if (field == "alphaOverLength") target = &module.alphaOverLength;
                else target = &module.alphaOverTime;
                *target = raw;
                Skip();
                if (position_ < source_.size() && source_[position_] == ';') ++position_;
            } else {
                Fail(error_, "unknown trail field '" + field + "' in <" + module.name + ">");
            }
        }
        if (!hasKind)
            Fail(error_, "component <" + module.name + "> is missing required field 'kind: trail'");
    }

    void Skip()
    {
        for (;;) {
            while (position_ < source_.size() &&
                   std::isspace(static_cast<unsigned char>(source_[position_])))
                ++position_;
            if (source_.substr(position_, 2) == "//") {
                while (position_ < source_.size() && source_[position_] != '\n') ++position_;
                continue;
            }
            if (source_.substr(position_, 2) == "/*") {
                position_ += 2;
                while (position_ < source_.size() && source_.substr(position_, 2) != "*/") ++position_;
                if (position_ >= source_.size()) Fail(error_, "unterminated block comment");
                position_ += 2;
                continue;
            }
            return;
        }
    }

    static bool WordChar(const char c)
    {
        return !std::isspace(static_cast<unsigned char>(c)) && c != '{' && c != '}' &&
               c != ':' && c != ';' && c != '"';
    }

    std::string ReadWord()
    {
        Skip();
        std::string word;
        while (position_ < source_.size() && WordChar(source_[position_]))
            word += source_[position_++];
        return word;
    }

    std::string PeekWord()
    {
        const std::size_t saved = position_;
        const std::string word = ReadWord();
        position_ = saved;
        return word;
    }

    char PeekChar()
    {
        Skip();
        return position_ < source_.size() ? source_[position_] : '\0';
    }

    std::string ReadString()
    {
        Skip();
        if (position_ >= source_.size() || source_[position_] != '"')
            Fail(error_, "expected string literal");
        ++position_;
        std::string text;
        while (position_ < source_.size() && source_[position_] != '"') {
            char value = source_[position_++];
            if (value == '\\' && position_ < source_.size()) {
                const char escape = source_[position_++];
                if (escape == 'n') value = '\n';
                else if (escape == 'r') value = '\r';
                else if (escape == 't') value = '\t';
                else if (escape != '\\' && escape != '"')
                    Fail(error_, "invalid string escape");
            }
            text += value;
        }
        if (position_ >= source_.size()) Fail(error_, "unterminated string");
        ++position_;
        return text;
    }

    void Expect(const char c)
    {
        Skip();
        if (position_ >= source_.size() || source_[position_] != c) {
            std::ostringstream message;
            message << "expected '" << c << "' at offset " << position_;
            Fail(error_, message.str());
        }
        ++position_;
    }

    std::string_view source_;
    TrailDslFileDirectives* directives_;
    std::vector<RawTrail>& outputs_;
    std::string* error_;
    std::size_t position_ = 0;
};

} // namespace

bool ParseTrailDslFileText(const std::string_view source,
                           TrailDslFileDirectives& file, std::string* error)
{
    try {
        file = TrailDslFileDirectives{};
        TrailDslParser parser(source, &file, file.modules, error);
        parser.Run();
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

namespace {

class TrailBundleLoader final {
public:
    explicit TrailBundleLoader(TrailDslBundle& bundle) : bundle_(bundle) {}

    std::size_t Load(const std::filesystem::path& path, std::string* error)
    {
        std::error_code ec;
        const std::filesystem::path canonical =
            std::filesystem::weakly_canonical(path, ec);
        const std::string key = ec ? path.string() : canonical.string();
        for (const std::string& active : stack_) {
            if (active == key) {
                std::string chain;
                for (const std::string& link : stack_) chain += link + " -> ";
                Fail(error, "circular include: " + chain + key);
            }
        }
        const auto memo = byPath_.find(key);
        if (memo != byPath_.end()) return memo->second;
        const std::size_t index = bundle_.files.size();
        bundle_.files.push_back(path);
        stack_.push_back(key);
        TrailDslFileDirectives directives;
        if (!ParseTrailDslFileText(ReadFileText(path, error), directives, error))
            Fail(error, (error && !error->empty()
                             ? *error + " (in " + path.string() + ")"
                             : std::string("while parsing ") + path.string()));
        byPath_.emplace(key, index);
        files_.push_back(std::move(directives));
        for (const std::string& reference : files_[index].forwards)
            Resolve(index, reference, error);
        for (const std::string& reference : files_[index].imports)
            Resolve(index, reference, error);
        stack_.pop_back();
        return index;
    }

    std::vector<std::string> PublicNamesOf(std::size_t index, std::string* error)
    {
        std::vector<std::string> names;
        CollectPublicNames(index, names, error);
        return names;
    }

    void CollectPublicNames(std::size_t index, std::vector<std::string>& names,
                            std::string* error)
    {
        for (const std::string& name : files_[index].publicNames) names.push_back(name);
        for (const std::string& forwardPath : files_[index].forwards) {
            const std::size_t child = Resolve(index, forwardPath, error);
            if (visitedPublic_.insert(child).second)
                CollectPublicNames(child, names, error);
        }
    }

    std::size_t Resolve(std::size_t index, const std::string& reference,
                        std::string* error)
    {
        const std::filesystem::path base = bundle_.files[index].parent_path();
        return Load((base / reference).lexically_normal(), error);
    }

    TrailDslBundle& bundle_;
    std::vector<TrailDslFileDirectives> files_;
    std::map<std::string, std::size_t> byPath_;
    std::vector<std::string> stack_;
    std::set<std::size_t> visitedPublic_;
};

} // namespace

bool LoadTrailDslBundle(const std::filesystem::path& rootFile, TrailDslBundle& bundle,
                        std::string* error)
{
    try {
        bundle = TrailDslBundle{};
        TrailBundleLoader loader(bundle);
        const std::size_t root = loader.Load(rootFile, error);

        std::map<std::string, bool> seen;
        for (const TrailDslFileDirectives& file : loader.files_)
            for (const RawTrail& module : file.modules) {
                if (seen.emplace(module.name, false).second)
                    bundle.modules.push_back(module);
                else
                    Fail(error, "duplicate module name <" + module.name + "> across bundle");
            }
        std::sort(bundle.modules.begin(), bundle.modules.end(),
                  [](const RawTrail& a, const RawTrail& b) { return a.name < b.name; });
        bundle.publicNames = loader.PublicNamesOf(root, error);
        std::sort(bundle.publicNames.begin(), bundle.publicNames.end());
        bundle.publicNames.erase(
            std::unique(bundle.publicNames.begin(), bundle.publicNames.end()),
            bundle.publicNames.end());
        if (bundle.modules.empty()) Fail(error, "no module found in bundle");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

// -------------------------------------------------------------------- 序列化

std::vector<std::uint8_t> SerializeTrail(const RawTrail& source,
                                         std::vector<std::string>* labels)
{
    // ---- 载荷：hash + 104 字节转储 + 尾区（image + 5 条轨道） ----
    std::vector<std::uint8_t> payload;
    std::vector<std::string> payloadLabels;
    const bool wantLabels = labels != nullptr;
    auto putByte = [&](const std::uint8_t byte, const std::string& label) {
        payload.push_back(byte);
        if (wantLabels) payloadLabels.push_back(label);
    };
    auto putU32 = [&](const std::uint32_t value, const std::string& label) {
        for (unsigned i = 0; i < 4; ++i)
            putByte(static_cast<std::uint8_t>(value >> (i * 8)), label);
    };
    auto putF32 = [&](const float value, const std::string& label) {
        std::uint32_t bits = 0;
        std::memcpy(&bits, &value, 4);
        putU32(bits, label);
    };

    putU32(kTrailSchemaHash, "schema hash 0x8488BC08 (DefinitionCalcHash gTrailDefMap x64)");

    // 转储：三标量之外全零（指针槽、计数槽、填充）。
    struct DumpSpan final {
        std::size_t offset;
        std::size_t size;
        const char* label;
    };
    static constexpr DumpSpan spans[] = {
        {0, 8, "dump mImage ptr slot (zero)"},
        {8, 4, "dump mMaxPoints (MaxPoints)"},
        {12, 4, "dump mMinPointDistance (MinPointDistance)"},
        {16, 4, "dump mTrailFlags (Loops bit0)"},
        {20, 4, "dump padding"},
        {24, 16, "dump mTrailDuration slot (zero)"},
        {40, 16, "dump mWidthOverLength slot (zero)"},
        {56, 16, "dump mWidthOverTime slot (zero)"},
        {72, 16, "dump mAlphaOverLength slot (zero)"},
        {88, 16, "dump mAlphaOverTime slot (zero)"},
    };
    std::vector<std::uint8_t> dump(104, 0);
    std::memcpy(dump.data() + 8, &source.maxPoints, 4);
    std::memcpy(dump.data() + 12, &source.minPointDistance, 4);
    const std::int32_t flags = source.loops ? 1 : 0;
    std::memcpy(dump.data() + 16, &flags, 4);
    for (const DumpSpan& span : spans)
        for (std::size_t i = 0; i < span.size; ++i)
            putByte(dump[span.offset + i], span.label);

    // 尾区：image 字符串 + 5 条轨道（oracle 槽位序）。
    const std::string image = CanonicalResource(source.image);
    putU32(static_cast<std::uint32_t>(image.size()), "tail image length");
    for (const char c : image) putByte(static_cast<std::uint8_t>(c), "tail image bytes");
    for (const char* slot : kTrackSlots) {
        std::vector<TrackNode> nodes;
        std::string parseError;
        if (!ParseLegacyFloatTrack(TrackTextOf(source, slot), nodes, &parseError))
            Fail(nullptr, std::string(slot) + ": " + parseError);
        putU32(static_cast<std::uint32_t>(nodes.size()),
               std::string("tail ") + slot + " node count (oracle slot)");
        for (std::size_t i = 0; i < nodes.size(); ++i) {
            const TrackNode& node = nodes[i];
            const std::string prefix = std::string("tail ") + slot + " node " + std::to_string(i);
            putF32(node.time, prefix + " mTime");
            putF32(node.low, prefix + " mLowValue");
            putF32(node.high, prefix + " mHighValue");
            putU32(static_cast<std::uint32_t>(node.curve), prefix + " mCurveType");
            putU32(static_cast<std::uint32_t>(node.distribution), prefix + " mDistribution");
        }
    }

    // ---- 包装：cookie + 长度 + zlib 头 + stored 块 + adler32 小端 ----
    std::vector<std::uint8_t> record;
    auto outByte = [&](const std::uint8_t byte, const std::string& label) {
        record.push_back(byte);
        if (wantLabels) labels->push_back(label);
    };
    auto outU32 = [&](const std::uint32_t value, const std::string& label) {
        for (unsigned i = 0; i < 4; ++i)
            outByte(static_cast<std::uint8_t>(value >> (i * 8)), label);
    };
    outU32(kTrailCookie, "cookie DEADFED4 (LE)");
    outU32(static_cast<std::uint32_t>(payload.size()), "payload size (LE)");
    outByte(0x78, "zlib header 0x78");
    outByte(0x01, "zlib header 0x01");
    std::size_t position = 0;
    do {
        const std::size_t chunk = std::min<std::size_t>(payload.size() - position, 65535);
        const bool last = position + chunk >= payload.size();
        outByte(last ? 0x01 : 0x00, "stored block BFINAL/BTYPE");
        const std::uint16_t length = static_cast<std::uint16_t>(chunk);
        const std::uint16_t negated = static_cast<std::uint16_t>(~length);
        outByte(static_cast<std::uint8_t>(length & 0xFF), "stored block LEN lo");
        outByte(static_cast<std::uint8_t>((length >> 8) & 0xFF), "stored block LEN hi");
        outByte(static_cast<std::uint8_t>(negated & 0xFF), "stored block NLEN lo");
        outByte(static_cast<std::uint8_t>((negated >> 8) & 0xFF), "stored block NLEN hi");
        for (std::size_t i = 0; i < chunk; ++i) {
            const std::size_t payloadIndex = position + i;
            outByte(payload[payloadIndex],
                    wantLabels
                        ? "payload[" + std::to_string(payloadIndex) + "] " +
                              payloadLabels[payloadIndex]
                        : std::string());
        }
        position += chunk;
    } while (position < payload.size());
    outU32(Adler32(payload), "adler32 payload (LE)");
    return record;
}

bool WriteTrailCompiled(const RawTrail& source, const std::filesystem::path& output,
                        std::string* error, std::vector<std::string>* labels)
{
    try {
        const std::vector<std::uint8_t> bytes = SerializeTrail(source, labels);
        if (!output.parent_path().empty())
            std::filesystem::create_directories(output.parent_path());
        std::ofstream out(output, std::ios::binary | std::ios::trunc);
        if (!out) Fail(error, "cannot write " + output.string());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) Fail(error, "write failed for " + output.string());
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

} // namespace openpvz::reanim::trail
