#include "reanim/ParticleDsl.h"
#include "reanim/SourceNumber.h"

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <numeric>
#include <ostream>
#include <set>
#include <sstream>
#include <stdexcept>

namespace openpvz::reanim::particle {
namespace {

// ---------------------------------------------------------------- 小工具

[[noreturn]] void Fail(std::string* error, const std::string& message)
{
    if (error) *error = message;
    throw std::runtime_error(message);
}

std::string Trimmed(std::string_view text)
{
    std::size_t begin = 0;
    std::size_t end = text.size();
    while (begin < end && std::isspace(static_cast<unsigned char>(text[begin]))) ++begin;
    while (end > begin && std::isspace(static_cast<unsigned char>(text[end - 1]))) --end;
    return std::string(text.substr(begin, end - begin));
}

bool IEquals(std::string_view a, std::string_view b)
{
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i)
        if (std::tolower(static_cast<unsigned char>(a[i])) !=
            std::tolower(static_cast<unsigned char>(b[i])))
            return false;
    return true;
}

std::string Lowered(std::string text)
{
    for (char& c : text) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return text;
}

bool IsSpace(const char c)
{
    return c == ' ' || c == '\t' || c == '\r' || c == '\n';
}

// sscanf %f 语义：跳过前导空白，取最长数字前缀（float32 直接舍入）。
bool ScanFloat(std::string_view text, std::size_t& at, float& value)
{
    const std::size_t n = text.size();
    std::size_t i = at;
    while (i < n && IsSpace(text[i])) ++i;
    std::size_t j = i;
    if (j < n && (text[j] == '+' || text[j] == '-')) ++j;
    std::size_t digits = 0;
    while (j < n && std::isdigit(static_cast<unsigned char>(text[j]))) { ++j; ++digits; }
    if (j < n && text[j] == '.') {
        ++j;
        while (j < n && std::isdigit(static_cast<unsigned char>(text[j]))) { ++j; ++digits; }
    }
    if (digits == 0) return false;
    if (j < n && (text[j] == 'e' || text[j] == 'E')) {
        std::size_t k = j + 1;
        if (k < n && (text[k] == '+' || text[k] == '-')) ++k;
        if (k < n && std::isdigit(static_cast<unsigned char>(text[k]))) {
            while (k < n && std::isdigit(static_cast<unsigned char>(text[k]))) ++k;
            j = k;
        }
    }
    const std::string token(text.substr(i, j - i));
    char* end = nullptr;
    value = std::strtof(token.c_str(), &end);
    if (end != token.c_str() + token.size()) return false;
    at = j;
    return true;
}

// sscanf %d 语义（int）。
bool ScanInt(std::string_view text, std::size_t& at, long& value)
{
    const std::size_t n = text.size();
    std::size_t i = at;
    while (i < n && IsSpace(text[i])) ++i;
    std::size_t j = i;
    if (j < n && (text[j] == '+' || text[j] == '-')) ++j;
    const std::size_t digitsBegin = j;
    while (j < n && std::isdigit(static_cast<unsigned char>(text[j]))) ++j;
    if (j == digitsBegin) return false;
    value = std::strtol(std::string(text.substr(i, j - i)).c_str(), nullptr, 10);
    at = j;
    return true;
}

// TodCurves 匹配表（前缀比较序，与 gDefTrackEaseSymbols 一致）。
struct CurveSymbol final {
    const char* name;
    std::int32_t value;
};
constexpr CurveSymbol kCurveSymbols[12] = {
    {"EaseInOutWeak", 5}, {"FastInOutWeak", 7}, {"EaseInOut", 4}, {"FastInOut", 6},
    {"EaseIn", 2},        {"EaseOut", 3},       {"EaseSinWave", 13},
    {"BounceFastMiddle", 10}, {"BounceSlowMiddle", 11}, {"Bounce", 9},
    {"SinWave", 12},      {"Linear", 1},
};

bool CurveAt(std::string_view text, std::size_t at, std::int32_t& value,
             std::size_t& length)
{
    for (const CurveSymbol& symbol : kCurveSymbols) {
        const std::size_t len = std::strlen(symbol.name);
        if (text.size() - at >= len && text.compare(at, len, symbol.name) == 0) {
            value = symbol.value;
            length = len;
            return true;
        }
    }
    return false;
}

struct SymbolEntry final {
    const char* name;
    std::int32_t value;
};

constexpr SymbolEntry kEmitterTypeSymbols[5] = {
    {"Circle", 0}, {"Box", 1}, {"BoxPath", 2}, {"CirclePath", 3}, {"CircleEvenSpacing", 4}};
constexpr SymbolEntry kFieldTypeSymbols[11] = {
    {"Friction", 1}, {"Acceleration", 2}, {"Attractor", 3}, {"MaxVelocity", 4},
    {"Velocity", 5}, {"Position", 6},     {"SystemPosition", 7}, {"GroundConstraint", 8},
    {"Shake", 9},    {"Circle", 10},      {"Away", 11}};
constexpr SymbolEntry kParticleFlagSymbols[kFlagCount] = {
    {"RandomLaunchSpin", 0}, {"AlignLaunchSpin", 1}, {"AlignToPixel", 2},
    {"SystemLoops", 3}, {"ParticleLoops", 4}, {"ParticlesDontFollow", 5},
    {"RandomStartTime", 6}, {"DieIfOverloaded", 7}, {"Additive", 8},
    {"FullScreen", 9}, {"SoftwareOnly", 10}, {"HardwareOnly", 11}};

// gLawnParticleArray（TodParticle.cpp）的原始大小写文件主名，按枚举序
// （root 的 forward 顺序 = 此序；106 条）。
const char* const kLawnParticleFiles[106] = {
    "MelonImpact", "WinterMelonImpact", "FumeCloud", "PopcornSplash", "Powie",
    "JackExplode", "ZombieHead", "ZombieArm", "ZombieTrafficCone", "ZombiePail",
    "ZombieHelmet", "ZombieFlag", "ZombieDoor", "ZombieNewspaper", "ZombieHeadLight",
    "Pow", "ZombiePogo", "ZombieNewspaperHead", "ZombieBalloonHead", "SodRoll",
    "GraveStoneRise", "Planting", "PlantingPool", "ZombieRise", "GraveBuster",
    "GraveBusterDie", "PoolSplash", "IceSparkle", "SeedPacket", "TallNutBlock",
    "Doom", "DiggerRise", "DiggerTunnel", "DancerRise", "PoolSparkly",
    "WallnutEatSmall", "WallnutEatLarge", "PeaSplat", "ButterSplat", "CabbageSplat",
    "PuffSplat", "StarSplat", "IceTrap", "SnowPeaSplat", "SnowPeaPuff",
    "SnowPeaTrail", "LanternShine", "Award", "PotatoMine", "PotatoMineRise",
    "PuffShroomTrail", "PuffShroomMuzzle", "SeedPacketFlash", "WhackAZombieRise",
    "ZombieLadder", "UmbrellaReflect", "SeedPacketPick", "IceTrapZombie",
    "IceTrapRelease", "ZamboniSmoke", "GloomCloud", "ZombiePogoHead", "ZamboniTire",
    "ZamboniExplosion", "ZamboniExplosion2", "CatapultExplosion", "MowerCloud",
    "BossIceBallTrail", "BlastMark", "CoinPickupArrow", "PresentPickup",
    "ImitaterMorph", "MoweredZombieHead", "MoweredZombieArm", "ZombieHeadPool",
    "Zombie_boss_fireball", "FireballDeath", "IceBallDeath", "Iceball_Trail",
    "Fireball_Trail", "BossExplosion", "ScreenFlash", "TrophySparkle",
    "PortalCircle", "PortalSquare", "PottedPlantGlow", "PottedWaterPlantGlow",
    "PottedZenGlow", "MindControl", "VaseShatter", "VaseShatterLeaf",
    "VaseShatterZombie", "AwardPickupArrow", "Zombie_seaweed", "ZombieMustache",
    "ZombieFutureGlasses", "Pinata", "Dust_Squash", "Dust_Foot", "Daisy",
    "Credits_Strobe", "Credits_RaysWipe", "Credits_ZombieHeadWipe", "Starburst",
    "Credits_fog", "UpsellArrow"};

std::string PascalFromStem(const std::string& stem)
{
    // 按 '_' 分段、逐段首字母大写，其余保留原样。
    std::string out;
    out.reserve(stem.size());
    bool upperNext = true;
    for (const char c : stem) {
        if (c == '_') {
            upperNext = true;
            continue;
        }
        out += upperNext ? static_cast<char>(std::toupper(static_cast<unsigned char>(c))) : c;
        upperNext = false;
    }
    return out;
}

std::string LowerFirst(std::string name)
{
    if (!name.empty())
        name[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(name[0])));
    return name;
}

int TrackIndex(std::string_view key, const char* const* names, int count)
{
    for (int i = 0; i < count; ++i)
        if (IEquals(key, Lowered(names[i]))) return i;
    return -1;
}

int SymbolValue(std::string_view text, const SymbolEntry* table, std::size_t count)
{
    for (std::size_t i = 0; i < count; ++i)
        if (IEquals(text, Lowered(table[i].name))) return table[i].value;
    return -1;
}

std::string FlagList(std::uint32_t flags)
{
    std::string out;
    for (int i = 0; i < kFlagCount; ++i)
        if (flags & (1u << i)) {
            if (!out.empty()) out += "|";
            out += kParticleFlagSymbols[i].name;
        }
    return out;
}

void PutU32(std::vector<std::uint8_t>& out, const std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void PutI32(std::vector<std::uint8_t>& out, const std::int32_t value)
{
    PutU32(out, static_cast<std::uint32_t>(value));
}

void PutF32(std::vector<std::uint8_t>& out, const float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    PutU32(out, bits);
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

// 带逐字节标注的字节汇集器（verify 差异定位）。
class ByteSink final {
public:
    explicit ByteSink(std::vector<std::string>* labels) : labels_(labels) {}

    void Add(const std::vector<std::uint8_t>& bytes, const std::string& label)
    {
        out_.insert(out_.end(), bytes.begin(), bytes.end());
        if (labels_)
            for (std::size_t i = 0; i < bytes.size(); ++i) labels_->push_back(label);
    }
    void AddU32(const std::uint32_t value, const std::string& label)
    {
        std::vector<std::uint8_t> bytes;
        PutU32(bytes, value);
        Add(bytes, label);
    }
    void AddF32(const float value, const std::string& label)
    {
        std::vector<std::uint8_t> bytes;
        PutF32(bytes, value);
        Add(bytes, label);
    }
    void AddString(const std::string& text, const std::string& label)
    {
        std::vector<std::uint8_t> bytes;
        PutU32(bytes, static_cast<std::uint32_t>(text.size()));
        bytes.insert(bytes.end(), text.begin(), text.end());
        Add(bytes, label);
    }
    void AddZeros(const std::size_t count, const std::string& label)
    {
        const std::size_t base = out_.size();
        out_.resize(base + count, 0);
        if (labels_)
            for (std::size_t i = 0; i < count; ++i) labels_->push_back(label);
    }
    [[nodiscard]] const std::vector<std::uint8_t>& Out() const noexcept { return out_; }
    [[nodiscard]] std::vector<std::uint8_t> Take() noexcept { return std::move(out_); }

private:
    std::vector<std::uint8_t> out_;
    std::vector<std::string>* labels_;
};

bool ReadFileText(const std::filesystem::path& path, std::string& text, std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    std::ostringstream buffer;
    buffer << in.rdbuf();
    text = buffer.str();
    return true;
}

bool WriteTextFile(const std::filesystem::path& path, const std::string& text,
                   std::string* error)
{
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        if (error) *error = "cannot write " + path.string();
        return false;
    }
    out << text;
    if (!out) {
        if (error) *error = "write failed for " + path.string();
        return false;
    }
    return true;
}

bool ReadFileBytes(const std::filesystem::path& path, std::vector<std::uint8_t>& bytes,
                   std::string* error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        if (error) *error = "cannot open " + path.string();
        return false;
    }
    bytes.assign(std::istreambuf_iterator<char>(in), {});
    return true;
}

bool OutputMissing(const std::filesystem::path& path)
{
    if (path.empty() || std::filesystem::exists(path)) return true;
    return false;
}

} // namespace

// ------------------------------------------------------- FloatTrack 解析

bool ParseLegacyFloatTrack(const std::string_view text,
                           std::vector<TrackNode>& nodes, std::string* error)
{
    try {
        nodes.clear();
        const std::size_t n = text.size();
        std::size_t idx = 0;
        const auto atEnd = [&](std::size_t i) { return i >= n; };
        while (true) {
            while (idx < n && IsSpace(text[idx])) ++idx;
            if (atEnd(idx)) return true;
            TrackNode node;
            node.time = -1.0f;
            node.curve = 1;
            node.distribution = 1;
            if (text[idx] == '[') {
                // <range>
                ++idx;
                float value = 0.0f;
                if (!ScanFloat(text, idx, value))
                    Fail(error, "float track: missing range lower bound");
                node.low = node.high = value;
                if (idx < n && text[idx] == ' ') {
                    ++idx;
                    std::int32_t curve = 0;
                    std::size_t length = 0;
                    if (CurveAt(text, idx, curve, length)) {
                        node.distribution = curve;
                        idx += length + 1;
                    }
                    if (ScanFloat(text, idx, value)) node.high = value;
                }
                while (idx < n && text[idx] != ']') ++idx;
                if (atEnd(idx)) Fail(error, "float track: range missing ']'");
                ++idx;
                if (atEnd(idx)) {
                    nodes.push_back(node);
                    return true;
                }
                if (text[idx] == ',') {
                    ++idx;
                    if (!ScanFloat(text, idx, value))
                        Fail(error, "float track: invalid node time");
                    node.time = value * 0.01f;
                }
                if (atEnd(idx)) {
                    nodes.push_back(node);
                    return true;
                }
                ++idx;
                nodes.push_back(node);
            } else {
                // <norange>
                float value = 0.0f;
                if (!ScanFloat(text, idx, value))
                    Fail(error, "float track: invalid node value");
                node.low = node.high = value;
                if (idx < n && text[idx] == ']') ++idx;  // 被吞 "[lo" 的残留括号
                if (atEnd(idx)) {
                    nodes.push_back(node);
                    return true;
                }
                if (text[idx] == ',') {
                    ++idx;
                    if (!ScanFloat(text, idx, value))
                        Fail(error, "float track: invalid node time");
                    node.time = value * 0.01f;
                }
                if (atEnd(idx)) {
                    nodes.push_back(node);
                    return true;
                }
                ++idx;
                bool curved = false;
                std::int32_t curve = 0;
                std::size_t length = 0;
                if (CurveAt(text, idx, curve, length)) {
                    node.curve = curve;
                    idx += length;
                    if (atEnd(idx)) {
                        nodes.push_back(node);
                        return true;
                    }
                    ++idx;
                    curved = true;
                }
                nodes.push_back(node);
                if (!curved) {
                    // 历史行为：数值节点后的下一个 token 被当作曲线名消费，
                    // 命中则赋曲线，否则整 token 丢弃。
                    while (idx < n && IsSpace(text[idx])) ++idx;
                    while (idx < n && !IsSpace(text[idx])) ++idx;
                }
            }
        }
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

namespace {

// 区间时间补齐（oracle 语义：除数为零时 interp=0，尾段节点时间保持为
// 最后一个锚点时间）。
void BackfillTimes(std::vector<TrackNode>& nodes)
{
    std::size_t base = 0;
    float low = 0.0f;
    while (true) {
        float high = 1.0f;
        std::size_t anidx = nodes.size();
        for (std::size_t i = base; i < nodes.size(); ++i) {
            if (nodes[i].time >= 0.0f) {
                anidx = i;
                high = nodes[i].time;
                break;
            }
        }
        for (std::size_t i = base; i < anidx; ++i) {
            const float interp =
                (anidx - 1 - base) != 0
                    ? static_cast<float>(i - base) /
                          static_cast<float>(anidx - 1 - base)
                    : 0.0f;
            nodes[i].time = high * interp + low * (1.0f - interp);
        }
        base = anidx + 1;
        low = high;
        if (base >= nodes.size()) break;
    }
}

} // namespace

bool ParseFloatTrackOracle(std::string_view text, std::vector<TrackNode>& nodes,
                           std::string* error)
{
    if (!ParseLegacyFloatTrack(text, nodes, error)) return false;
    BackfillTimes(nodes);
    return true;
}

namespace {
struct AuthoredNode {
    std::int32_t numerator = -1, denominator = 1;
    float low = 0, high = 0;
    std::int32_t curve = 1, distribution = 1;
};
struct AuthoredLiteralError : std::runtime_error {
    std::size_t lineOffset;
    AuthoredLiteralError(std::size_t offset, const std::string& message)
        : std::runtime_error(message), lineOffset(offset) {}
};

std::vector<AuthoredNode> ParseExplicitNodes(std::string_view text)
{
    std::size_t at = 6; // "nodes{"
    std::vector<AuthoredNode> nodes;
    const auto whitespace = [&] { while (at < text.size() && IsSpace(text[at])) ++at; };
    const auto expect = [&](char c) {
        whitespace();
        if (at >= text.size() || text[at++] != c)
            throw std::runtime_error(std::string("particle nodes expected '") + c + "'");
    };
    while (true) {
        whitespace();
        if (at < text.size() && text[at] == '}') { ++at; break; }
        expect('{');
        AuthoredNode node;
        bool low = false, high = false;
        while (true) {
            whitespace();
            if (at < text.size() && text[at] == '}') { ++at; break; }
            auto start = at;
            while (at < text.size() && text[at] != ':' && !IsSpace(text[at])) ++at;
            const std::string key(text.substr(start, at - start));
            expect(':'); whitespace(); start = at;
            while (at < text.size() && text[at] != ';' && !IsSpace(text[at])) ++at;
            const auto value = text.substr(start, at - start);
            expect(';');
            if (key == "time") {
                const auto ratio = ParseTimeRatio(value);
                node.numerator = ratio.first;
                node.denominator = ratio.second;
                if (node.numerator < 0 || node.numerator > node.denominator)
                    throw std::runtime_error("particle node time must be in [0, 1]");
            } else if (key == "low") { node.low = ParseFiniteFloat(value); low = true; }
            else if (key == "high") { node.high = ParseFiniteFloat(value); high = true; }
            else if (key == "curve" || key == "distribution") {
                std::int32_t curve = 0;
                std::size_t length = 0;
                if (value != "Constant" &&
                    (!CurveAt(value, 0, curve, length) || length != value.size()))
                    throw std::runtime_error("unknown particle curve '" + std::string(value) + "'");
                (key == "curve" ? node.curve : node.distribution) = curve;
            } else throw std::runtime_error("unknown particle node field '" + key + "'");
        }
        if (!low || !high) throw std::runtime_error("particle node requires low and high");
        nodes.push_back(node);
        if (nodes.size() > 100000) throw std::runtime_error("too many particle nodes");
    }
    whitespace();
    if (at != text.size()) throw std::runtime_error("trailing particle nodes text");
    // Only omitted times are inferred; explicit decimal anchors stay exact.
    std::size_t base = 0;
    std::int64_t previousNum = 0, previousDen = 1;
    while (base < nodes.size()) {
        std::size_t anchor = base;
        while (anchor < nodes.size() && nodes[anchor].numerator < 0) ++anchor;
        const auto nextNum = anchor < nodes.size() ? nodes[anchor].numerator : 1;
        const auto nextDen = anchor < nodes.size() ? nodes[anchor].denominator : 1;
        const auto span = base == 0 ? (anchor == nodes.size() ? nodes.size() - 1 : anchor)
                                   : (anchor == nodes.size() ? nodes.size() - base : anchor - base + 1);
        const auto common = std::lcm(previousDen, static_cast<std::int64_t>(nextDen));
        for (auto i = base; i < anchor; ++i) {
            const std::int64_t step = i - base + (base == 0 ? 0 : 1);
            const std::int64_t intervals = span == 0 ? 1 : span;
            std::int64_t num = previousNum * (common / previousDen) * (intervals - step) +
                               nextNum * (common / nextDen) * step;
            std::int64_t den = common * intervals;
            const auto divisor = std::gcd(num, den);
            num /= divisor; den /= divisor;
            if (num < INT32_MIN || num > INT32_MAX || den > INT32_MAX)
                throw std::runtime_error("particle node rational exceeds int32");
            nodes[i].numerator = static_cast<std::int32_t>(num);
            nodes[i].denominator = static_cast<std::int32_t>(den);
        }
        if (anchor < nodes.size()) {
            const auto divisor = std::gcd(nodes[anchor].numerator, nodes[anchor].denominator);
            nodes[anchor].numerator /= divisor;
            nodes[anchor].denominator /= divisor;
        }
        previousNum = nextNum; previousDen = nextDen;
        base = anchor + 1;
    }
    return nodes;
}

std::vector<AuthoredNode> ParseAuthoredTrack(std::string_view text)
{
    // Validate every literal, including tokens consumed by the historical
    // track grammar without becoming a node. No overprecision can hide in a
    // discarded curve token.
    for (std::size_t i = 0; i < text.size();) {
        if (IsSpace(text[i]) || std::string_view("[]{}:,;").find(text[i]) != std::string_view::npos) {
            ++i; continue;
        }
        const auto start = i;
        while (i < text.size() && !IsSpace(text[i]) &&
               std::string_view("[]{}:,;").find(text[i]) == std::string_view::npos) ++i;
        const auto token = text.substr(start, i - start);
        try {
        if ((token[0] >= '0' && token[0] <= '9') || token[0] == '+' ||
            token[0] == '-' || token[0] == '.') {
            (void)ParseFiniteFloat(token);
        } else {
            std::int32_t curve = 0;
            std::size_t length = 0;
            if (token != "nodes" && token != "time" && token != "low" && token != "high" &&
                token != "curve" && token != "distribution" && token != "Constant" &&
                (!CurveAt(token, 0, curve, length) || length != token.size()))
                throw std::runtime_error("invalid particle numeric/curve token '" + std::string(token) + "'");
        }
        } catch (const std::exception& e) {
            throw AuthoredLiteralError(static_cast<std::size_t>(
                std::count(text.begin(), text.begin() + start, '\n')), e.what());
        }
    }
    if (text.starts_with("nodes{")) return ParseExplicitNodes(text);
    std::vector<AuthoredNode> nodes;
    std::size_t at = 0;
    const auto numberText = [&]() {
        while (at < text.size() && IsSpace(text[at])) ++at;
        const auto start = at;
        while (at < text.size() && !IsSpace(text[at]) &&
               text[at] != ',' && text[at] != ']') ++at;
        return text.substr(start, at - start);
    };
    const auto number = [&]() { return ParseFiniteFloat(numberText()); };
    const auto ratio = [](AuthoredNode& node, std::int64_t num, std::int64_t den) {
        const auto gcd = std::gcd(num, den);
        num /= gcd; den /= gcd;
        if (den <= 0 || den > INT32_MAX || num < INT32_MIN || num > INT32_MAX)
            throw std::runtime_error("particle node time rational exceeds int32");
        node.numerator = static_cast<std::int32_t>(num);
        node.denominator = static_cast<std::int32_t>(den);
    };
    while (true) {
        while (at < text.size() && IsSpace(text[at])) ++at;
        if (at == text.size()) break;
        AuthoredNode node;
        const bool range = text[at] == '[';
        if (range) ++at;
        node.low = node.high = number();
        if (range) {
            if (at < text.size() && text[at] == ' ') {
                ++at;
                std::size_t length = 0;
                if (CurveAt(text, at, node.distribution, length)) at += length + 1;
                if (at < text.size() && text[at] != ']') node.high = number();
            }
            if (at >= text.size() || text[at] != ']')
                throw std::runtime_error("particle range missing ']'");
            ++at;
        } else if (at < text.size() && text[at] == ']') ++at;
        if (at < text.size() && text[at] == ',') {
            ++at;
            const auto stamp = ParseTimeRatio(numberText(), 100);
            ratio(node, stamp.first, stamp.second);
        }
        if (at < text.size()) {
            ++at;
            if (!range) {
                std::size_t length = 0;
                if (CurveAt(text, at, node.curve, length)) {
                    at += length;
                    if (at < text.size()) ++at;
                } else {
                    // Preserve the original track grammar's consumed curve token.
                    while (at < text.size() && IsSpace(text[at])) ++at;
                    while (at < text.size() && !IsSpace(text[at])) ++at;
                }
            }
        }
        nodes.push_back(node);
    }
    std::size_t base = 0;
    std::int64_t lowNum = 0, lowDen = 1;
    while (base < nodes.size()) {
        std::size_t anchor = base;
        while (anchor < nodes.size() && nodes[anchor].numerator < 0) ++anchor;
        const std::int64_t highNum = anchor < nodes.size() ? nodes[anchor].numerator : 1;
        const std::int64_t highDen = anchor < nodes.size() ? nodes[anchor].denominator : 1;
        // Match the established compiler's omitted-time expansion exactly,
        // retaining its rational value instead of an intermediate float.
        const std::int64_t intervals = anchor > base + 1 ? anchor - 1 - base : 1;
        for (auto i = base; i < anchor; ++i) {
            const std::int64_t step = anchor > base + 1 ? i - base : 0;
            const auto common = std::lcm(lowDen, highDen);
            ratio(nodes[i], lowNum * (common / lowDen) * (intervals - step) +
                           highNum * (common / highDen) * step, common * intervals);
        }
        base = anchor + 1;
        lowNum = highNum; lowDen = highDen;
    }
    return nodes;
}
}

// ------------------------------------------------------------ XML 读取

namespace {

struct XmlTag final {
    std::string name;
    std::string content;
};

bool NextTag(std::string_view body, std::size_t& at, XmlTag& tag)
{
    const std::size_t open = body.find('<', at);
    if (open == std::string_view::npos) return false;
    const std::size_t nameBegin = open + 1;
    const std::size_t nameEnd = body.find('>', nameBegin);
    if (nameEnd == std::string_view::npos) return false;
    tag.name = std::string(body.substr(nameBegin, nameEnd - nameBegin));
    at = nameEnd + 1;
    if (!tag.name.empty() && tag.name.back() == '/') {  // 自闭合（语料未用）
        tag.name.pop_back();
        tag.content.clear();
        return true;
    }
    const std::string close = "</" + tag.name + ">";
    const std::size_t closeAt = body.find(close, at);
    if (closeAt == std::string_view::npos) return false;
    tag.content = std::string(body.substr(at, closeAt - at));
    at = closeAt + close.size();
    return true;
}

bool ParseFieldBlock(std::string_view content, ParticleField& field, std::string* error)
{
    std::size_t at = 0;
    XmlTag tag;
    while (NextTag(content, at, tag)) {
        if (IEquals(tag.name, "FieldType")) {
            field.fieldType = SymbolValue(Trimmed(tag.content), kFieldTypeSymbols, 11);
            if (field.fieldType < 0)
                Fail(error, "unknown particle field type <" + Trimmed(tag.content) + ">");
        } else if (IEquals(tag.name, "X")) {
            field.x = Trimmed(tag.content);
        } else if (IEquals(tag.name, "Y")) {
            field.y = Trimmed(tag.content);
        } else {
            Fail(error, "unknown element <" + tag.name + "> inside <Field>");
        }
    }
    return true;
}

bool ParseEmitterBlock(std::string_view content, ParticleEmitter& emitter,
                       std::string* error)
{
    std::size_t at = 0;
    XmlTag tag;
    while (NextTag(content, at, tag)) {
        const std::string& name = tag.name;
        if (IEquals(name, "Field") || IEquals(name, "SystemField")) {
            ParticleField field;
            if (!ParseFieldBlock(tag.content, field, error)) return false;
            (IEquals(name, "Field") ? emitter.particleFields : emitter.systemFields)
                .push_back(std::move(field));
            continue;
        }
        const int flag = SymbolValue(name, kParticleFlagSymbols, kFlagCount);
        if (flag >= 0) {
            // 历史行为：元素出现即置位，值被忽略。
            emitter.flags |= 1u << flag;
            continue;
        }
        if (IEquals(name, "EmitterType")) {
            const int type = SymbolValue(Trimmed(tag.content), kEmitterTypeSymbols, 5);
            if (type < 0) Fail(error, "unknown emitter type <" + Trimmed(tag.content) + ">");
            emitter.emitterType = type;
            continue;
        }
        if (IEquals(name, "Image")) {
            emitter.image = Trimmed(tag.content);
            continue;
        }
        if (IEquals(name, "Name")) {
            emitter.name = tag.content;
            continue;
        }
        if (IEquals(name, "OnDuration")) {
            emitter.onDuration = tag.content;
            continue;
        }
        if (IEquals(name, "ImageRow") || IEquals(name, "ImageCol") ||
            IEquals(name, "ImageFrames") || IEquals(name, "Animated")) {
            long value = 0;
            std::size_t pos = 0;
            if (!ScanInt(tag.content, pos, value))
                Fail(error, "invalid int for <" + name + ">: " + Trimmed(tag.content));
            if (IEquals(name, "ImageRow"))
                emitter.imageRow = static_cast<std::int32_t>(value);
            else if (IEquals(name, "ImageCol"))
                emitter.imageCol = static_cast<std::int32_t>(value);
            else if (IEquals(name, "ImageFrames"))
                emitter.imageFrames = static_cast<std::int32_t>(value);
            else
                emitter.animated = static_cast<std::int32_t>(value);
            continue;
        }
        const int sysTrack = TrackIndex(name, kSystemTrackNames, kSystemTrackCount);
        if (sysTrack >= 0) {
            emitter.systemTracks[sysTrack] = Trimmed(tag.content);
            continue;
        }
        const int partTrack = TrackIndex(name, kParticleTrackNames, kParticleTrackCount);
        if (partTrack >= 0) {
            emitter.particleTracks[partTrack] = Trimmed(tag.content);
            continue;
        }
        Fail(error, "unknown element <" + name + "> inside <Emitter>");
    }
    return true;
}

} // namespace

std::string ParticleModuleName(std::string_view sourceStem)
{
    return PascalFromStem(std::string(sourceStem));
}

bool ReadParticleXml(const std::filesystem::path& input, ParticleDefinition& output,
                     std::string* error)
{
    try {
        std::string text;
        if (!ReadFileText(input, text, error)) return false;
        output = ParticleDefinition{};
        output.sourceName = Lowered(input.stem().string());
        output.name = PascalFromStem(input.stem().string());
        // 磁盘文件可能全小写（melonimpact.xml）；模块名优先取 gLawnParticleArray
        // 的规范大小写（MelonImpact），保证 DSL 文件名与 root 一致。
        for (const char* canonical : kLawnParticleFiles) {
            if (Lowered(canonical) == output.sourceName) {
                output.name = PascalFromStem(canonical);
                break;
            }
        }
        std::size_t at = 0;
        XmlTag tag;
        while (NextTag(text, at, tag)) {
            if (IEquals(tag.name, "Emitter")) {
                ParticleEmitter emitter;
                if (!ParseEmitterBlock(tag.content, emitter, error)) return false;
                output.emitters.push_back(std::move(emitter));
            } else {
                Fail(error, "unknown element <" + tag.name + "> (expected <Emitter>)");
            }
        }
        if (output.emitters.empty())
            Fail(error, "no <Emitter> found in " + input.string());
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

// ------------------------------------------------------------- DSL 输出

namespace {

// 单数值 token 可内联；单区间转 range{}；其余 track{}（原文保留）。
bool IsPlainNumber(std::string_view text)
{
    if (text.empty()) return false;
    for (const char c : text) {
        if (!(std::isdigit(static_cast<unsigned char>(c)) || c == '.' || c == '-' ||
              c == '+'))
            return false;
    }
    float value = 0.0f;
    std::size_t at = 0;
    return ScanFloat(text, at, value) && at == text.size();
}

bool IsSingleRange(std::string_view text)
{
    if (text.size() < 2 || text.front() != '[' || text.back() != ']') return false;
    return text.find('[', 1) == std::string_view::npos &&
           text.find(']', 1) == text.size() - 1;
}

void WriteTrackValue(std::ostream& out, std::string_view raw)
{
    if (IsPlainNumber(raw)) {
        out << raw << ";\n";
    } else if (IsSingleRange(raw)) {
        out << "range { " << raw.substr(1, raw.size() - 2) << "; }\n";
    } else {
        out << "track { " << raw << "; }\n";
    }
}

void WriteEscaped(std::ostream& out, std::string_view text)
{
    out << '"';
    for (const char c : text) {
        if (c == '\n') out << "\\n";
        else if (c == '\r') out << "\\r";
        else if (c == '\t') out << "\\t";
        else if (c == '\\' || c == '"') out << '\\' << c;
        else out << c;
    }
    out << '"';
}

void WriteFieldBlocks(std::ostream& out, const std::vector<ParticleField>& fields,
                      const char* key)
{
    for (const ParticleField& field : fields) {
        out << "        " << key << " {\n";
        out << "            fieldType: ";
        const char* typeName = "?";
        for (const SymbolEntry& entry : kFieldTypeSymbols)
            if (entry.value == field.fieldType) typeName = entry.name;
        out << typeName << ";\n";
        if (!field.x.empty()) {
            out << "            x: ";
            WriteTrackValue(out, field.x);
        }
        if (!field.y.empty()) {
            out << "            y: ";
            WriteTrackValue(out, field.y);
        }
        out << "        }\n";
    }
}

const char* EmitterTypeName(std::int32_t value)
{
    for (const SymbolEntry& entry : kEmitterTypeSymbols)
        if (entry.value == value) return entry.name;
    return "?";
}

} // namespace

std::string ParticleToDsl(const ParticleDefinition& source)
{
    std::ostringstream out;
    out << "export component " << (source.name.empty() ? "Particle" : source.name)
        << " {\n";
    out << "    kind: particle;\n";
    out << "    source-name: ";
    WriteEscaped(out, source.sourceName);
    out << ";\n";
    for (const ParticleEmitter& emitter : source.emitters) {
        out << "    Emitter {\n";
        out << "        image: ";
        if (emitter.image.empty()) out << "none";
        else WriteEscaped(out, emitter.image);
        out << ";\n";
        if (!emitter.name.empty()) {
            out << "        name: ";
            WriteEscaped(out, emitter.name);
            out << ";\n";
        }
        if (!emitter.onDuration.empty()) {
            out << "        onDuration: ";
            WriteEscaped(out, emitter.onDuration);
            out << ";\n";
        }
        if (emitter.imageRow != 0)
            out << "        imageRow: " << emitter.imageRow << ";\n";
        if (emitter.imageCol != 0)
            out << "        imageCol: " << emitter.imageCol << ";\n";
        if (emitter.imageFrames != 1)
            out << "        imageFrames: " << emitter.imageFrames << ";\n";
        if (emitter.animated != 0)
            out << "        animated: " << emitter.animated << ";\n";
        if (emitter.emitterType != 1)
            out << "        emitterType: " << EmitterTypeName(emitter.emitterType)
                << ";\n";
        for (int i = 0; i < kFlagCount; ++i)
            if (emitter.flags & (1u << i))
                out << "        " << LowerFirst(kParticleFlagSymbols[i].name)
                    << ": 1;\n";
        for (int i = 0; i < kSystemTrackCount; ++i) {
            if (emitter.systemTracks[i].empty()) continue;
            out << "        " << LowerFirst(kSystemTrackNames[i]) << ": ";
            WriteTrackValue(out, emitter.systemTracks[i]);
        }
        for (int i = 0; i < kParticleTrackCount; ++i) {
            if (emitter.particleTracks[i].empty()) continue;
            out << "        " << LowerFirst(kParticleTrackNames[i]) << ": ";
            WriteTrackValue(out, emitter.particleTracks[i]);
        }
        WriteFieldBlocks(out, emitter.particleFields, "field");
        WriteFieldBlocks(out, emitter.systemFields, "systemField");
        out << "    }\n";
    }
    out << "}\n";
    return out.str();
}

// ------------------------------------------------------------- DSL 解析

namespace {

enum class TokKind {
    identifier, string, number, brace, colon, semicolon, bracket, comma, end
};

struct Token final {
    TokKind kind = TokKind::end;
    std::string text;
    std::size_t line = 1;
};

class Lexer final {
public:
    explicit Lexer(std::string_view source) : source_(source) { Advance(); }
    [[nodiscard]] const Token& current() const noexcept { return token_; }
    void Advance()
    {
        for (;;) {
            while (position_ < source_.size() &&
                   std::isspace(static_cast<unsigned char>(source_[position_])))
                ++position_;
            if (source_.substr(position_, 2) == "//") {
                while (position_ < source_.size() && source_[position_] != '\n')
                    ++position_;
                continue;
            }
            if (source_.substr(position_, 2) == "/*") {
                position_ += 2;
                while (position_ < source_.size() &&
                       source_.substr(position_, 2) != "*/")
                    ++position_;
                if (position_ >= source_.size()) Fail(nullptr, "unterminated block comment");
                position_ += 2;
                continue;
            }
            break;
        }
        token_ = {};
        line_ += static_cast<std::size_t>(std::count(source_.begin() + countedAt_,
                                                   source_.begin() + position_, '\n'));
        countedAt_ = position_;
        token_.line = line_;
        if (position_ >= source_.size()) return;
        const char c = source_[position_];
        if (c == '"') {
            ++position_;
            token_.kind = TokKind::string;
            while (position_ < source_.size() && source_[position_] != '"') {
                char value = source_[position_++];
                if (value == '\\' && position_ < source_.size()) {
                    const char escape = source_[position_++];
                    if (escape == 'n') value = '\n';
                    else if (escape == 'r') value = '\r';
                    else if (escape == 't') value = '\t';
                    else if (escape != '\\' && escape != '"')
                        Fail(nullptr, "invalid string escape");
                }
                token_.text += value;
            }
            if (position_ >= source_.size()) Fail(nullptr, "unterminated string");
            ++position_;
            return;
        }
        if (c == '{' || c == '}') {
            token_.kind = TokKind::brace;
            token_.text = c;
            ++position_;
            return;
        }
        if (c == '[' || c == ']') {
            token_.kind = TokKind::bracket;
            token_.text = c;
            ++position_;
            return;
        }
        if (c == ',') {
            token_.kind = TokKind::comma;
            token_.text = ",";
            ++position_;
            return;
        }
        if (c == ':') {
            token_.kind = TokKind::colon;
            token_.text = ":";
            ++position_;
            return;
        }
        if (c == ';') {
            token_.kind = TokKind::semicolon;
            token_.text = ";";
            ++position_;
            return;
        }
        const auto digitFollows = [&]() {
            if (position_ + 1 >= source_.size()) return false;
            const char next = source_[position_ + 1];
            if (std::isdigit(static_cast<unsigned char>(next))) return true;
            return next == '.' && position_ + 2 < source_.size() &&
                   std::isdigit(static_cast<unsigned char>(source_[position_ + 2]));
        };
        token_.kind =
            std::isdigit(static_cast<unsigned char>(c)) ||
                    ((c == '-' || c == '+') && digitFollows()) ||
                    (c == '.' && position_ + 1 < source_.size() &&
                     std::isdigit(static_cast<unsigned char>(source_[position_ + 1])))
                ? TokKind::number
                : TokKind::identifier;
        while (position_ < source_.size()) {
            const char k = source_[position_];
            if (std::isspace(static_cast<unsigned char>(k)) || k == '{' || k == '}' ||
                k == '[' || k == ']' || k == ',' || k == ':' || k == ';' || k == '"')
                break;
            token_.text += k;
            ++position_;
        }
    }
    // track { 原文 } / range { 原文 }：捕获花括号内原文区间（不分词，保真
    // 逗号/括号/多空格），允许末尾一个 ';'。
    [[nodiscard]] std::string TakeRawBlock(bool preserveWhitespace = false)
    {
        while (position_ < source_.size() &&
               std::isspace(static_cast<unsigned char>(source_[position_])))
            ++position_;
        if (position_ >= source_.size() || source_[position_] != '{')
            Fail(nullptr, "expected '{' after track/range");
        ++position_;
        const std::size_t begin = position_;
        int depth = 1;
        while (position_ < source_.size()) {
            if (source_[position_] == '{') ++depth;
            if (source_[position_] == '}' && --depth == 0) break;
            ++position_;
        }
        if (position_ >= source_.size()) Fail(nullptr, "unterminated raw block");
        std::string raw = preserveWhitespace
            ? std::string(source_.substr(begin, position_ - begin))
            : Trimmed(source_.substr(begin, position_ - begin));
        ++position_;  // '}'
        if (!raw.empty() && raw.back() == ';') raw = Trimmed(raw.substr(0, raw.size() - 1));
        Advance();
        return raw;
    }

private:
    std::size_t line_ = 1, countedAt_ = 0;
    std::string_view source_;
    std::size_t position_ = 0;
    Token token_;
};

class DslParser final {
public:
    DslParser(std::string_view source, ParticleDslFileDirectives* directives,
              std::vector<ParticleDefinition>& outputs, std::string* error, bool legacy)
        : lexer_(source), directives_(directives), outputs_(outputs), error_(error),
          legacy_(legacy) {}

    void Run()
    {
        for (;;) {
            const Token& token = lexer_.current();
            if (token.kind == TokKind::end) break;
            if (token.kind == TokKind::identifier && token.text == "import") {
                if (directives_ == nullptr)
                    Fail(error_, "file-level 'import' requires a file-based load");
                directives_->imports.push_back(ReadFileReference("import"));
            } else if (token.kind == TokKind::identifier && token.text == "forward") {
                if (directives_ == nullptr)
                    Fail(error_, "file-level 'forward' requires a file-based load");
                directives_->forwards.push_back(ReadFileReference("forward"));
            } else {
                ParseModule();
            }
        }
    }

private:
    std::string ReadFileReference(const char* keyword)
    {
        lexer_.Advance();
        const std::string path = TakeString();
        ExpectSemicolon();
        if (path.empty())
            Fail(error_, std::string(keyword) + " requires a non-empty file path");
        return path;
    }
    void ParseModule()
    {
        bool exported = false;
        if (IsIdent("export")) {
            exported = true;
            lexer_.Advance();
        }
        ExpectIdent("component");
        if (lexer_.current().kind != TokKind::identifier)
            Fail(error_, "expected component name");
        ParticleDefinition module;
        module.name = lexer_.current().text;
        lexer_.Advance();
        ExpectBrace("{");
        bool sawKind = false;
        bool sawVariant = false, sawImage = false, sawImages = false;
        while (!(lexer_.current().kind == TokKind::brace &&
                 lexer_.current().text == "}")) {
            if (lexer_.current().kind != TokKind::identifier)
                Fail(error_, "expected component property");
            const std::string property = lexer_.current().text;
            lexer_.Advance();
            if (property == "kind") {
                ExpectColon();
                if (!IsIdent("particle"))
                    Fail(error_, "kind must be 'particle' for particle components");
                lexer_.Advance();
                ExpectSemicolon();
                sawKind = true;
            } else if (property == "source-name") {
                ExpectColon();
                module.sourceName = TakeString();
                ExpectSemicolon();
            } else if (property == "variant-of") {
                if (sawVariant) Fail(error_, "duplicate variant-of in <" + module.name + ">");
                sawVariant = true;
                ExpectColon();
                if (lexer_.current().kind != TokKind::identifier)
                    Fail(error_, "variant-of requires a component name");
                module.variantOf = lexer_.current().text;
                lexer_.Advance();
                ExpectSemicolon();
            } else if (property == "image") {
                if (!legacy_)
                    Fail(error_, "particle variants require per-emitter images lists");
                if (sawImage) Fail(error_, "duplicate variant image in <" + module.name + ">");
                sawImage = true;
                ExpectColon();
                module.variantImage = TakeString();
                ExpectSemicolon();
            } else if (property == "images") {
                if (sawImages) Fail(error_, "duplicate variant images");
                sawImages = true;
                ExpectColon();
                ExpectBracket("[");
                if (lexer_.current().text != "]") {
                    for (;;) {
                        if (module.variantImages.size() >= 65536)
                            Fail(error_, "too many variant image lists");
                        module.variantImages.push_back(ReadImages());
                        if (lexer_.current().kind != TokKind::comma) break;
                        lexer_.Advance();
                    }
                }
                ExpectBracket("]");
                ExpectSemicolon();
            } else if (property == "Emitter" || property == "emitter") {
                ParseEmitter(module);
            } else {
                Fail(error_, "unknown component property <" + property +
                                 "> (expected kind/source-name/variant-of/images/Emitter)");
            }
        }
        ExpectBrace("}");
        if (!sawKind) Fail(error_, "particle component <" + module.name + "> requires kind: particle");
        if (module.sourceName.empty())
            Fail(error_, "particle component <" + module.name +
                             "> requires source-name (names the compiled record)");
        if (sawVariant && legacy_ && !sawImages && (!sawImage || module.variantImage.empty()))
            Fail(error_, "particle variant <" + module.name + "> requires a non-empty image");
        if (sawImage && !sawVariant)
            Fail(error_, "component image requires variant-of in <" + module.name + ">");
        if (sawImages && !sawVariant)
            Fail(error_, "component images requires variant-of in <" + module.name + ">");
        if (sawVariant && !legacy_ && !sawImages)
            Fail(error_, "particle variant <" + module.name + "> requires images");
        if (sawImage && sawImages)
            Fail(error_, "variant image and images cannot be combined");
        if (sawVariant && !module.emitters.empty())
            Fail(error_, "particle variant <" + module.name + "> cannot declare Emitter blocks");
        for (const ParticleDefinition& existing : outputs_)
            if (existing.name == module.name)
                Fail(error_, "duplicate module name <" + module.name + ">");
        if (exported && directives_ != nullptr)
            directives_->publicNames.push_back(module.name);
        outputs_.push_back(std::move(module));
    }
    void ParseEmitter(ParticleDefinition& module)
    {
        ExpectBrace("{");
        ParticleEmitter emitter;
        bool sawImages = false;
        while (!(lexer_.current().kind == TokKind::brace &&
                 lexer_.current().text == "}")) {
            if (lexer_.current().kind != TokKind::identifier)
                Fail(error_, "expected emitter property");
            const std::string property = lexer_.current().text;
            lexer_.Advance();
            if (property == "field" || property == "systemField") {
                ParticleField field;
                ParseField(property, field);
                (property == "field" ? emitter.particleFields : emitter.systemFields)
                    .push_back(std::move(field));
                continue;
            }
            ExpectColon();
            if (property == "image") {
                if (sawImages) Fail(error_, "duplicate emitter image/images");
                sawImages = true;
                emitter.image = TakeStringOrIdent();
                if (IEquals(emitter.image, "none")) emitter.image.clear();
                if (!emitter.image.empty()) emitter.images.push_back(emitter.image);
                ExpectSemicolon();
            } else if (property == "images") {
                if (sawImages) Fail(error_, "duplicate emitter image/images");
                sawImages = true;
                emitter.images = ReadImages();
                emitter.image = emitter.images.empty() ? "" : emitter.images.front();
                ExpectSemicolon();
            } else if (property == "name") {
                emitter.name = TakeString();
                ExpectSemicolon();
            } else if (property == "onDuration") {
                emitter.onDuration = TakeString();
                ExpectSemicolon();
            } else if (property == "imageRow" || property == "imageCol" ||
                       property == "imageFrames" || property == "animated") {
                if (!legacy_ && (property == "imageRow" || property == "imageCol"))
                    Fail(error_, "imageRow/imageCol are legacy-only; use images");
                const long value = TakeInt();
                if (property == "imageRow")
                    emitter.imageRow = static_cast<std::int32_t>(value);
                else if (property == "imageCol")
                    emitter.imageCol = static_cast<std::int32_t>(value);
                else if (property == "imageFrames")
                    emitter.imageFrames = static_cast<std::int32_t>(value);
                else
                    emitter.animated = static_cast<std::int32_t>(value);
            } else if (property == "emitterType") {
                const int value = SymbolValue(TakeIdent(), kEmitterTypeSymbols, 5);
                if (value < 0) Fail(error_, "unknown emitterType value");
                emitter.emitterType = value;
                ExpectSemicolon();
            } else {
                const int flag =
                    SymbolValue(property, kParticleFlagSymbols, kFlagCount);
                if (flag >= 0) {
                    (void)TakeInt();  // 值被忽略：出现即置位（历史行为）
                    emitter.flags |= 1u << flag;
                } else {
                    const int sysTrack =
                        TrackIndex(property, kSystemTrackNames, kSystemTrackCount);
                    const int partTrack = TrackIndex(property, kParticleTrackNames,
                                                    kParticleTrackCount);
                    if (sysTrack < 0 && partTrack < 0)
                        Fail(error_, "unknown emitter property <" + property + ">");
                    std::string raw;
                    ReadTrackValue(raw, property);
                    if (sysTrack >= 0) emitter.systemTracks[sysTrack] = raw;
                    else emitter.particleTracks[partTrack] = raw;
                }
            }
        }
        ExpectBrace("}");
        if (!legacy_) {
            if (emitter.imageFrames <= 0 || emitter.imageFrames > 65536)
                Fail(error_, "imageFrames must be in [1, 65536]");
        }
        module.emitters.push_back(std::move(emitter));
    }
    void ExpectBracket(std::string_view bracket)
    {
        if (lexer_.current().kind != TokKind::bracket || lexer_.current().text != bracket)
            Fail(error_, "expected '" + std::string(bracket) + "'");
        lexer_.Advance();
    }
    std::int32_t ReadInteger()
    {
        if (lexer_.current().kind != TokKind::number)
            Fail(error_, "expected integer");
        const std::string text = lexer_.current().text;
        std::size_t consumed = 0;
        long long value = 0;
        try { value = std::stoll(text, &consumed, 10); }
        catch (...) { Fail(error_, "integer out of range"); }
        if (consumed != text.size() || value < std::numeric_limits<std::int32_t>::min() ||
            value > std::numeric_limits<std::int32_t>::max())
            Fail(error_, "expected 32-bit integer");
        lexer_.Advance();
        return static_cast<std::int32_t>(value);
    }
    std::vector<std::string> ReadImages()
    {
        std::vector<std::string> images;
        ExpectBracket("[");
        if (lexer_.current().text != "]") {
            for (;;) {
                if (images.size() >= 65536) Fail(error_, "too many particle images");
                std::string image = TakeString();
                if (image.empty() || image.find_first_of("\r\n") != std::string::npos ||
                    image.find('\0') != std::string::npos)
                    Fail(error_, "particle images require non-empty source names without line breaks or NUL");
                images.push_back(std::move(image));
                if (lexer_.current().kind != TokKind::comma) break;
                lexer_.Advance();
            }
        }
        ExpectBracket("]");
        return images;
    }
    void ParseField(const std::string& key, ParticleField& field)
    {
        ExpectBrace("{");
        while (!(lexer_.current().kind == TokKind::brace &&
                 lexer_.current().text == "}")) {
            if (lexer_.current().kind != TokKind::identifier)
                Fail(error_, "expected field property");
            const std::string property = lexer_.current().text;
            lexer_.Advance();
            ExpectColon();
            if (property == "fieldType") {
                const int value = SymbolValue(TakeIdent(), kFieldTypeSymbols, 11);
                if (value < 0) Fail(error_, "unknown fieldType value");
                field.fieldType = value;
                ExpectSemicolon();
            } else if (property == "x" || property == "y") {
                std::string raw;
                ReadTrackValue(raw, key + "." + property);
                (property == "x" ? field.x : field.y) = raw;
            } else {
                Fail(error_, "unknown property <" + property + "> inside " + key);
            }
        }
        ExpectBrace("}");
        if (field.fieldType < 0) Fail(error_, key + " requires fieldType");
    }
    // 值形式：number; | track { raw } | range { raw }
    void ReadTrackValue(std::string& raw, const std::string& field)
    {
        const auto line = lexer_.current().line;
        const Token& token = lexer_.current();
        if (token.kind == TokKind::number) {
            raw = token.text;
            lexer_.Advance();
            ExpectSemicolon();
        } else if (token.kind == TokKind::identifier &&
                   (token.text == "track" || token.text == "range" || token.text == "nodes")) {
            const bool range = token.text == "range";
            const bool explicitNodes = token.text == "nodes";
            // 不 Advance：TakeRawBlock 直接从 position_（紧跟 track/range
            // 标识符之后、'{' 之前）捕获原文，避免把 '{' 分词吞掉。
            raw = lexer_.TakeRawBlock(explicitNodes);
            if (range) raw = "[" + Trimmed(raw) + "]";
            if (explicitNodes) raw = "nodes{" + raw + "}";
            OptionalSemicolon();
        } else {
            Fail(error_, "line " + std::to_string(line) + ", " + field +
                 ": track value must be decimal or track {}/range {}: '" + token.text + "'");
        }
        try { (void)ParseAuthoredTrack(raw); }
        catch (const AuthoredLiteralError& e) {
            Fail(error_, "line " + std::to_string(line + e.lineOffset) + ", " + field + ": " + e.what());
        }
        catch (const std::exception& e) {
            Fail(error_, "line " + std::to_string(line) + ", " + field + ": " + e.what());
        }
    }
    [[nodiscard]] bool IsIdent(const std::string_view keyword) const
    {
        return lexer_.current().kind == TokKind::identifier &&
               lexer_.current().text == keyword;
    }
    void ExpectIdent(const std::string_view keyword)
    {
        if (!IsIdent(keyword))
            Fail(error_, "expected '" + std::string(keyword) + "'");
        lexer_.Advance();
    }
    void ExpectBrace(const std::string_view brace)
    {
        if (lexer_.current().kind != TokKind::brace || lexer_.current().text != brace)
            Fail(error_, std::string("expected '") + std::string(brace) + "'");
        lexer_.Advance();
    }
    void ExpectColon()
    {
        if (lexer_.current().kind != TokKind::colon) Fail(error_, "expected ':'");
        lexer_.Advance();
    }
    void ExpectSemicolon()
    {
        if (lexer_.current().kind != TokKind::semicolon) Fail(error_, "expected ';'");
        lexer_.Advance();
    }
    void OptionalSemicolon()
    {
        if (lexer_.current().kind == TokKind::semicolon) lexer_.Advance();
    }
    std::string TakeString()
    {
        if (lexer_.current().kind != TokKind::string) Fail(error_, "expected string");
        const std::string value = lexer_.current().text;
        lexer_.Advance();
        return value;
    }
    std::string TakeIdent()
    {
        if (lexer_.current().kind != TokKind::identifier)
            Fail(error_, "expected identifier");
        const std::string value = lexer_.current().text;
        lexer_.Advance();
        return value;
    }
    std::string TakeStringOrIdent()
    {
        if (lexer_.current().kind == TokKind::identifier) return TakeIdent();
        return TakeString();
    }
    long TakeInt()
    {
        const long value = ReadInteger();
        ExpectSemicolon();
        return value;
    }

    Lexer lexer_;
    ParticleDslFileDirectives* directives_;
    std::vector<ParticleDefinition>& outputs_;
    std::string* error_;
    bool legacy_;
};

} // namespace

bool ParseParticleDslFileText(const std::string_view source,
                              ParticleDslFileDirectives& file, std::string* error,
                              const bool legacy)
{
    try {
        file = ParticleDslFileDirectives{};
        DslParser parser(source, &file, file.modules, error, legacy);
        parser.Run();
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

namespace {

// Resolve only after all files are loaded: neither declaration order nor input-list
// order determines visibility. Metadata stays compiler-side and is never serialized.
void ResolveParticleVariants(ParticleDslBundle& bundle, std::string* error, bool legacy = false)
{
    std::map<std::string, std::size_t> byName;
    std::set<std::string> sourceNames;
    bool hasVariants = false;
    for (std::size_t i = 0; i < bundle.modules.size(); ++i) {
        const auto& module = bundle.modules[i];
        if (!byName.emplace(module.name, i).second)
            Fail(error, "duplicate particle module <" + module.name + ">");
        hasVariants |= !module.variantOf.empty();
    }
    // Output records must not overwrite another component, including case-insensitive
    // filesystems and gold files whose filename casing differs from source-name.
    for (const auto& module : bundle.modules)
        if (!sourceNames.insert(Lowered(module.sourceName)).second)
            Fail(error, "duplicate particle source-name <" + module.sourceName + ">");
    for (const auto& module : bundle.modules)
        if (module.sourceName.empty() || module.sourceName.find_first_of("/\\:\r\n\t") != std::string::npos ||
            std::any_of(module.sourceName.begin(), module.sourceName.end(),
                        [](unsigned char c) { return c < 32; }))
            Fail(error, "invalid particle source-name <" + module.sourceName + ">");
    if (!hasVariants) return;
    std::vector<unsigned char> state(bundle.modules.size(), 0);
    std::function<void(std::size_t)> resolve = [&](const std::size_t index) {
        auto& module = bundle.modules[index];
        if (state[index] == 2) return;
        if (state[index] == 1)
            Fail(error, "particle variant cycle at <" + module.name + ">");
        state[index] = 1;
        if (!module.variantOf.empty()) {
            const auto base = byName.find(module.variantOf);
            if (base == byName.end())
                Fail(error, "particle variant <" + module.name + "> has missing base <" +
                                module.variantOf + ">");
            resolve(base->second);
            module.emitters = bundle.modules[base->second].emitters;
            if ((!legacy || !module.variantImages.empty()) &&
                module.variantImages.size() != module.emitters.size())
                Fail(error, "particle variant <" + module.name +
                                "> requires one images list per inherited emitter");
            for (std::size_t e = 0; e < module.emitters.size(); ++e) {
                auto& emitter = module.emitters[e];
                if (!legacy && module.variantImages[e].size() != emitter.images.size())
                    Fail(error, "particle variant <" + module.name +
                                    "> images count differs from inherited emitter " + std::to_string(e));
                emitter.images = module.variantImages.empty()
                    ? std::vector<std::string>{module.variantImage} : module.variantImages[e];
                emitter.image = emitter.images.empty() ? "" : emitter.images.front();
            }
        }
        state[index] = 2;
    };
    for (std::size_t i = 0; i < bundle.modules.size(); ++i) resolve(i);
}

class BundleLoader final {
public:
    explicit BundleLoader(ParticleDslBundle& bundle, bool legacy)
        : bundle_(bundle), legacy_(legacy) {}

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
        std::string text;
        if (!ReadFileText(path, text, error)) Fail(error, *error);
        const std::size_t index = bundle_.files.size();
        bundle_.files.push_back(path);
        stack_.push_back(key);
        ParticleDslFileDirectives directives;
        if (!ParseParticleDslFileText(text, directives, error, legacy_))
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

    std::set<std::size_t> visitedPublic_;

    std::size_t Resolve(std::size_t index, const std::string& reference,
                        std::string* error)
    {
        const std::filesystem::path base = bundle_.files[index].parent_path();
        return Load((base / reference).lexically_normal(), error);
    }

    ParticleDslBundle& bundle_;
    std::vector<ParticleDslFileDirectives> files_;
    std::map<std::string, std::size_t> byPath_;
    std::vector<std::string> stack_;
    bool legacy_;
};

} // namespace

bool LoadParticleDslBundle(const std::filesystem::path& rootFile,
                           ParticleDslBundle& bundle, std::string* error, const bool legacy)
{
    try {
        bundle = ParticleDslBundle{};
        BundleLoader loader(bundle, legacy);
        const std::size_t root = loader.Load(rootFile, error);
        std::map<std::string, bool> seen;
        for (const ParticleDslFileDirectives& file : loader.files_) {
            for (const ParticleDefinition& module : file.modules) {
                if (seen.emplace(module.name, false).second)
                    bundle.modules.push_back(module);
                else
                    Fail(error,
                         "duplicate module name <" + module.name + "> across bundle");
            }
        }
        std::sort(bundle.modules.begin(), bundle.modules.end(),
                  [](const ParticleDefinition& a, const ParticleDefinition& b) {
                      return a.name < b.name;
                  });
        bundle.publicNames = loader.PublicNamesOf(root, error);
        std::sort(bundle.publicNames.begin(), bundle.publicNames.end());
        bundle.publicNames.erase(
            std::unique(bundle.publicNames.begin(), bundle.publicNames.end()),
            bundle.publicNames.end());
        if (bundle.modules.empty()) Fail(error, "no module found in bundle");
        ResolveParticleVariants(bundle, error, legacy);
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

// --------------------------------------------------------------- 序列化

namespace {

void SerializeTrackNodes(ByteSink& sink, const std::vector<TrackNode>& nodes,
                         const std::string& label)
{
    sink.AddU32(static_cast<std::uint32_t>(nodes.size()), label + " node count");
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::string nodeLabel = label + " node " + std::to_string(i);
        sink.AddF32(nodes[i].time, nodeLabel + " time");
        sink.AddF32(nodes[i].low, nodeLabel + " low");
        sink.AddF32(nodes[i].high, nodeLabel + " high");
        sink.Add({static_cast<std::uint8_t>(nodes[i].curve),
                  static_cast<std::uint8_t>(nodes[i].curve >> 8),
                  static_cast<std::uint8_t>(nodes[i].curve >> 16),
                  static_cast<std::uint8_t>(nodes[i].curve >> 24)},
                 nodeLabel + " curve");
        sink.Add({static_cast<std::uint8_t>(nodes[i].distribution),
                  static_cast<std::uint8_t>(nodes[i].distribution >> 8),
                  static_cast<std::uint8_t>(nodes[i].distribution >> 16),
                  static_cast<std::uint8_t>(nodes[i].distribution >> 24)},
                 nodeLabel + " distribution");
    }
}

void SerializeTrack(ByteSink& sink, const std::string& raw, const std::string& label,
                    std::string* error)
{
    std::vector<TrackNode> nodes;
    if (raw.starts_with("nodes{")) {
        for (const auto& authored : ParseAuthoredTrack(raw)) {
            TrackNode node;
            node.time = static_cast<float>(static_cast<double>(authored.numerator) / authored.denominator);
            node.low = authored.low;
            node.high = authored.high;
            node.curve = authored.curve; node.distribution = authored.distribution;
            nodes.push_back(node);
        }
    } else if (!raw.empty() && !ParseFloatTrackOracle(raw, nodes, error))
        Fail(error, (error && !error->empty() ? *error : std::string("track parse failed")) +
                        " in " + label);
    SerializeTrackNodes(sink, nodes, label);
}

void SerializeFieldArray(ByteSink& sink, const std::vector<ParticleField>& fields,
                         const std::string& label, std::string* error)
{
    sink.AddU32(kParticleFieldDumpSize, label + " element size");
    for (std::size_t i = 0; i < fields.size(); ++i) {
        std::vector<std::uint8_t> dump(kParticleFieldDumpSize, 0);
        const std::uint32_t type = static_cast<std::uint32_t>(fields[i].fieldType);
        dump[0] = static_cast<std::uint8_t>(type & 0xFF);
        dump[1] = static_cast<std::uint8_t>((type >> 8) & 0xFF);
        dump[2] = static_cast<std::uint8_t>((type >> 16) & 0xFF);
        dump[3] = static_cast<std::uint8_t>((type >> 24) & 0xFF);
        sink.Add(dump, label + "[" + std::to_string(i) + "] type dump");
    }
    for (std::size_t i = 0; i < fields.size(); ++i) {
        SerializeTrack(sink, fields[i].x, label + "[" + std::to_string(i) + "].x",
                       error);
        SerializeTrack(sink, fields[i].y, label + "[" + std::to_string(i) + "].y",
                       error);
    }
}

} // namespace

std::vector<std::uint8_t> SerializeParticle(const ParticleDefinition& source,
                                            std::vector<std::string>* labels)
{
    std::string error;
    std::vector<std::string> payloadLabels;
    ByteSink payloadSink(labels ? &payloadLabels : nullptr);
    payloadSink.AddU32(kParticleSchemaHash, "schema hash");
    {
        std::vector<std::uint8_t> top(16, 0);
        const std::uint32_t count =
            static_cast<std::uint32_t>(source.emitters.size());
        top[8] = static_cast<std::uint8_t>(count & 0xFF);
        top[9] = static_cast<std::uint8_t>((count >> 8) & 0xFF);
        top[10] = static_cast<std::uint8_t>((count >> 16) & 0xFF);
        top[11] = static_cast<std::uint8_t>((count >> 24) & 0xFF);
        payloadSink.Add(top, "definition dump (emitterCount)");
    }
    payloadSink.AddU32(kParticleEmitterDumpSize, "emitter element size");
    for (std::size_t e = 0; e < source.emitters.size(); ++e) {
        const ParticleEmitter& emitter = source.emitters[e];
        std::vector<std::uint8_t> dump(kParticleEmitterDumpSize, 0);
        const auto putInt = [&dump](std::size_t offset, std::int32_t value) {
            const std::uint32_t bits = static_cast<std::uint32_t>(value);
            dump[offset] = static_cast<std::uint8_t>(bits & 0xFF);
            dump[offset + 1] = static_cast<std::uint8_t>((bits >> 8) & 0xFF);
            dump[offset + 2] = static_cast<std::uint8_t>((bits >> 16) & 0xFF);
            dump[offset + 3] = static_cast<std::uint8_t>((bits >> 24) & 0xFF);
        };
        putInt(4, emitter.imageCol);
        putInt(8, emitter.imageRow);
        putInt(12, emitter.imageFrames);
        putInt(16, emitter.animated);
        putInt(20, static_cast<std::int32_t>(emitter.flags));
        putInt(24, emitter.emitterType);
        putInt(404, static_cast<std::int32_t>(emitter.particleFields.size()));
        putInt(420, static_cast<std::int32_t>(emitter.systemFields.size()));
        payloadSink.Add(dump, "emitter[" + std::to_string(e) + "] struct dump");
    }
    for (std::size_t e = 0; e < source.emitters.size(); ++e) {
        const ParticleEmitter& emitter = source.emitters[e];
        const std::string prefix = "emitter[" + std::to_string(e) + "]";
        payloadSink.AddString(emitter.image, prefix + " image string");
        payloadSink.AddString(emitter.name, prefix + " name string");
        payloadSink.AddString(emitter.onDuration, prefix + " onDuration string");
        for (int i = 0; i < kSystemTrackCount; ++i)
            SerializeTrack(payloadSink, emitter.systemTracks[i],
                           prefix + " " + kSystemTrackNames[i], &error);
        SerializeFieldArray(payloadSink, emitter.particleFields,
                            prefix + " Field array", &error);
        SerializeFieldArray(payloadSink, emitter.systemFields,
                            prefix + " SystemField array", &error);
        for (int i = 0; i < kParticleTrackCount; ++i)
            SerializeTrack(payloadSink, emitter.particleTracks[i],
                           prefix + " " + kParticleTrackNames[i], &error);
    }
    const std::vector<std::uint8_t> payload = payloadSink.Take();
    if (!error.empty()) throw std::runtime_error(error);

    // 容器包装：cookie + 长度 + zlib 头 + stored 块 + 小端 adler32。
    // 标注按记录字节展开：10 字节头 +（每块 5 字节块头 + 载荷切片标注）+ 4 字节尾。
    std::vector<std::uint8_t> record;
    std::vector<std::string> recordLabels;
    const bool wantLabels = labels != nullptr;
    const auto pushLabel = [&](const std::string& label) {
        if (wantLabels) recordLabels.push_back(label);
    };
    {
        std::vector<std::uint8_t> head;
        PutU32(head, kParticleCookie);
        PutU32(head, static_cast<std::uint32_t>(payload.size()));
        head.push_back(0x78);
        head.push_back(0x01);
        record.insert(record.end(), head.begin(), head.end());
        for (std::size_t i = 0; i < head.size(); ++i) pushLabel("container header");
    }
    std::size_t position = 0;
    do {
        const std::size_t chunk =
            std::min<std::size_t>(payload.size() - position, 65535);
        const bool last = position + chunk >= payload.size();
        std::vector<std::uint8_t> blockHead;
        blockHead.push_back(last ? 0x01 : 0x00);
        const std::uint16_t length = static_cast<std::uint16_t>(chunk);
        const std::uint16_t negated = static_cast<std::uint16_t>(~length);
        blockHead.push_back(static_cast<std::uint8_t>(length & 0xFF));
        blockHead.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
        blockHead.push_back(static_cast<std::uint8_t>(negated & 0xFF));
        blockHead.push_back(static_cast<std::uint8_t>((negated >> 8) & 0xFF));
        record.insert(record.end(), blockHead.begin(), blockHead.end());
        for (std::size_t i = 0; i < blockHead.size(); ++i)
            pushLabel("stored deflate block header");
        record.insert(record.end(),
                      payload.begin() + static_cast<std::ptrdiff_t>(position),
                      payload.begin() +
                          static_cast<std::ptrdiff_t>(position + chunk));
        if (wantLabels)
            recordLabels.insert(recordLabels.end(),
                                payloadLabels.begin() +
                                    static_cast<std::ptrdiff_t>(position),
                                payloadLabels.begin() +
                                    static_cast<std::ptrdiff_t>(position + chunk));
        position += chunk;
    } while (position < payload.size());
    {
        std::vector<std::uint8_t> tail;
        PutU32(tail, Adler32(payload));
        record.insert(record.end(), tail.begin(), tail.end());
        for (std::size_t i = 0; i < tail.size(); ++i)
            pushLabel("adler32 (little-endian)");
    }
    if (labels) *labels = std::move(recordLabels);
    return record;
}

bool WriteParticleCompiled(const ParticleDefinition& source,
                           const std::filesystem::path& output, std::string* error,
                           std::vector<std::string>* labels)
{
    try {
        std::vector<std::uint8_t> bytes;
        try {
            bytes = SerializeParticle(source, labels);
        } catch (const std::exception& e) {
            if (error) *error = e.what();
            return false;
        }
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

// ------------------------------------------------------------- 读取端

namespace {

class RecordReader final {
public:
    RecordReader(const std::vector<std::uint8_t>& bytes, std::string* error)
        : bytes_(bytes), error_(error) {}
    std::uint32_t U32(const char* what)
    {
        if (at_ + 4 > bytes_.size())
            Fail(error_, std::string("truncated: ") + what);
        std::uint32_t value = 0;
        std::memcpy(&value, bytes_.data() + at_, 4);
        at_ += 4;
        return value;
    }
    std::string String(const char* what)
    {
        const std::uint32_t length = U32(what);
        if (length > 100000 || at_ + length > bytes_.size())
            Fail(error_, std::string("truncated string: ") + what);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + at_), length);
        at_ += length;
        return value;
    }
    std::vector<TrackNode> Track(const char* what)
    {
        const std::uint32_t count = U32(what);
        if (count > 100000 ||
            at_ + static_cast<std::size_t>(count) * 20 > bytes_.size())
            Fail(error_, std::string("truncated track: ") + what);
        std::vector<TrackNode> nodes;
        nodes.reserve(count);
        for (std::uint32_t i = 0; i < count; ++i) {
            TrackNode node;
            std::memcpy(&node.time, bytes_.data() + at_, 4);
            std::memcpy(&node.low, bytes_.data() + at_ + 4, 4);
            std::memcpy(&node.high, bytes_.data() + at_ + 8, 4);
            std::memcpy(&node.curve, bytes_.data() + at_ + 12, 4);
            std::memcpy(&node.distribution, bytes_.data() + at_ + 16, 4);
            nodes.push_back(node);
            at_ += 20;
        }
        return nodes;
    }
    void Skip(const std::size_t count, const char* what)
    {
        if (at_ + count > bytes_.size())
            Fail(error_, std::string("truncated: ") + what);
        at_ += count;
    }
    [[nodiscard]] std::size_t At() const noexcept { return at_; }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::string* error_;
    std::size_t at_ = 0;
};

bool InflateRecord(const std::vector<std::uint8_t>& file,
                   std::vector<std::uint8_t>& payload, std::string* error)
{
    if (file.size() < 15) {
        if (error) *error = "compiled record too small";
        return false;
    }
    std::uint32_t cookie = 0, declared = 0;
    std::memcpy(&cookie, file.data(), 4);
    std::memcpy(&declared, file.data() + 4, 4);
    if (cookie != kParticleCookie) {
        if (error) *error = "unexpected cookie (expected 0xDEADFED4)";
        return false;
    }
    if (file[8] != 0x78 || file[9] != 0x01) {
        if (error) *error = "missing zlib header bytes 78 01";
        return false;
    }
    // 安装包记录为 stored-deflate 块 + 小端 adler 尾（小端 adler 不满足
    // zlib 校验，故手写 stored 块展开）。
    std::size_t at = 10;
    payload.clear();
    while (true) {
        if (at + 5 > file.size()) {
            if (error) *error = "truncated deflate block";
            return false;
        }
        const std::uint8_t header = file[at++];
        std::uint16_t length = 0, negated = 0;
        std::memcpy(&length, file.data() + at, 2);
        std::memcpy(&negated, file.data() + at + 2, 2);
        at += 4;
        if (length != static_cast<std::uint16_t>(~negated)) {
            if (error) *error = "invalid stored block length";
            return false;
        }
        if (at + length > file.size()) {
            if (error) *error = "truncated stored block";
            return false;
        }
        payload.insert(payload.end(),
                       file.begin() + static_cast<std::ptrdiff_t>(at),
                       file.begin() + static_cast<std::ptrdiff_t>(at + length));
        at += length;
        if (header & 1) break;
    }
    if (payload.size() != declared) {
        if (error) *error = "payload size mismatch";
        return false;
    }
    return true;
}

} // namespace

bool ReadParticleCompiled(const std::filesystem::path& input, ParticleRecord& record,
                          std::string* error)
{
    try {
        std::ifstream in(input, std::ios::binary);
        if (!in) Fail(error, "cannot open " + input.string());
        const std::vector<std::uint8_t> file(std::istreambuf_iterator<char>(in), {});
        std::vector<std::uint8_t> payload;
        if (!InflateRecord(file, payload, error)) return false;
        record = ParticleRecord{};
        RecordReader reader(payload, error);
        record.schemaHash = reader.U32("schema hash");
        if (record.schemaHash != kParticleSchemaHash)
            Fail(error, "schema hash mismatch (expected 3767703017)");
        reader.Skip(8, "top dump pointer");
        const std::uint32_t emitterCount = reader.U32("top dump emitterCount");
        reader.Skip(4, "top dump pad");
        if (emitterCount > 1000) Fail(error, "emitter count out of range");
        record.emitterCount = static_cast<int>(emitterCount);
        if (reader.U32("emitter element size") != kParticleEmitterDumpSize)
            Fail(error, "unexpected emitter element size");
        const std::size_t dumpsBase = reader.At();
        struct DumpInfo final {
            std::int32_t pf;
            std::int32_t sf;
            std::int32_t col, row, frames, animated, flags, type;
        };
        std::vector<DumpInfo> infos;
        for (std::uint32_t e = 0; e < emitterCount; ++e) {
            const std::size_t base = dumpsBase +
                static_cast<std::size_t>(e) * kParticleEmitterDumpSize;
            if (base + kParticleEmitterDumpSize > payload.size())
                Fail(error, "truncated emitter dump");
            DumpInfo info{};
            std::memcpy(&info.col, payload.data() + base + 4, 4);
            std::memcpy(&info.row, payload.data() + base + 8, 4);
            std::memcpy(&info.frames, payload.data() + base + 12, 4);
            std::memcpy(&info.animated, payload.data() + base + 16, 4);
            std::memcpy(&info.flags, payload.data() + base + 20, 4);
            std::memcpy(&info.type, payload.data() + base + 24, 4);
            std::memcpy(&info.pf, payload.data() + base + 404, 4);
            std::memcpy(&info.sf, payload.data() + base + 420, 4);
            infos.push_back(info);
        }
        reader.Skip(static_cast<std::size_t>(emitterCount) *
                        kParticleEmitterDumpSize,
                    "emitter dumps");
        for (std::uint32_t e = 0; e < emitterCount; ++e) {
            EmitterRecord emitter;
            const std::string prefix = "emitter[" + std::to_string(e) + "]";
            emitter.image = reader.String((prefix + " image").c_str());
            emitter.name = reader.String((prefix + " name").c_str());
            emitter.onDuration = reader.String((prefix + " onDuration").c_str());
            for (int i = 0; i < kSystemTrackCount; ++i)
                emitter.systemTracks.push_back(
                    reader.Track((prefix + " " + kSystemTrackNames[i]).c_str()));
            const auto readFieldArray = [&](std::int32_t count,
                                            std::vector<ParticleField>& fields) {
                if (reader.U32("field element size") != kParticleFieldDumpSize)
                    Fail(error, "unexpected field element size");
                std::vector<std::int32_t> types;
                for (std::int32_t i = 0; i < count; ++i) {
                    if (reader.At() + kParticleFieldDumpSize > payload.size())
                        Fail(error, "truncated field dump");
                    std::int32_t type = 0;
                    std::memcpy(&type, payload.data() + reader.At(), 4);
                    types.push_back(type);
                    reader.Skip(kParticleFieldDumpSize, "field dump");
                }
                for (std::int32_t i = 0; i < count; ++i) {
                    ParticleField field;
                    field.fieldType = types[i];
                    fields.push_back(std::move(field));
                }
            };
            readFieldArray(infos[e].pf, emitter.particleFields);
            for (std::size_t i = 0; i < emitter.particleFields.size(); ++i) {
                const std::vector<TrackNode> x = reader.Track(
                    (prefix + " Field[" + std::to_string(i) + "].x").c_str());
                const std::vector<TrackNode> y = reader.Track(
                    (prefix + " Field[" + std::to_string(i) + "].y").c_str());
                emitter.particleFieldNodes.push_back({x, y});
            }
            readFieldArray(infos[e].sf, emitter.systemFields);
            for (std::size_t i = 0; i < emitter.systemFields.size(); ++i) {
                const std::vector<TrackNode> x = reader.Track(
                    (prefix + " SystemField[" + std::to_string(i) + "].x").c_str());
                const std::vector<TrackNode> y = reader.Track(
                    (prefix + " SystemField[" + std::to_string(i) + "].y").c_str());
                emitter.systemFieldNodes.push_back({x, y});
            }
            for (int i = 0; i < kParticleTrackCount; ++i)
                emitter.particleTracks.push_back(
                    reader.Track((prefix + " " + kParticleTrackNames[i]).c_str()));
            emitter.imageCol = infos[e].col;
            emitter.imageRow = infos[e].row;
            emitter.imageFrames = infos[e].frames;
            emitter.animated = infos[e].animated;
            emitter.flags = static_cast<std::uint32_t>(infos[e].flags);
            emitter.emitterType = infos[e].type;
            record.emitters.push_back(std::move(emitter));
        }
        if (reader.At() != payload.size())
            Fail(error, "trailing payload bytes");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

std::string InspectParticleText(const ParticleRecord& record, const std::string& fileName)
{
    std::ostringstream out;
    out << fileName << ": schema=" << record.schemaHash
        << " emitters=" << record.emitterCount << '\n';
    for (std::size_t e = 0; e < record.emitters.size(); ++e) {
        const EmitterRecord& emitter = record.emitters[e];
        const char* typeName = "?";
        for (const SymbolEntry& entry : kEmitterTypeSymbols)
            if (entry.value == emitter.emitterType) typeName = entry.name;
        out << "  emitter " << e << ": name=\"" << emitter.name << "\" image=\""
            << emitter.image << "\" type=" << typeName
            << " frames=" << emitter.imageFrames << " col=" << emitter.imageCol
            << " row=" << emitter.imageRow << " animated=" << emitter.animated
            << " flags=0x" << std::hex << emitter.flags << std::dec << " ("
            << FlagList(emitter.flags) << ")\n";
        std::size_t sysNodes = 0, partNodes = 0;
        for (const auto& nodes : emitter.systemTracks) sysNodes += nodes.size();
        for (const auto& nodes : emitter.particleTracks) partNodes += nodes.size();
        for (int i = 0; i < kSystemTrackCount; ++i) {
            if (emitter.systemTracks[i].empty()) continue;
            out << "    " << kSystemTrackNames[i] << ": "
                << emitter.systemTracks[i].size() << " node(s)\n";
        }
        for (int i = 0; i < kParticleTrackCount; ++i) {
            if (emitter.particleTracks[i].empty()) continue;
            out << "    " << kParticleTrackNames[i] << ": "
                << emitter.particleTracks[i].size() << " node(s)\n";
        }
        out << "    tracks total: system=" << sysNodes << " nodes particle="
            << partNodes << " nodes; fields: particle="
            << emitter.particleFields.size()
            << " system=" << emitter.systemFields.size() << '\n';
        const auto printFields = [&](const std::vector<ParticleField>& fields,
                                     const auto& nodes, const char* key) {
            for (std::size_t i = 0; i < fields.size(); ++i) {
                const char* fieldTypeName = "?";
                for (const SymbolEntry& entry : kFieldTypeSymbols)
                    if (entry.value == fields[i].fieldType) fieldTypeName = entry.name;
                out << "    " << key << "[" << i << "] " << fieldTypeName
                    << " x=" << nodes[i].first.size()
                    << " y=" << nodes[i].second.size() << " node(s)\n";
            }
        };
        printFields(emitter.particleFields, emitter.particleFieldNodes, "field");
        printFields(emitter.systemFields, emitter.systemFieldNodes, "systemField");
    }
    return out.str();
}

// --------------------------------------------------- v2：运行时记录 + IDT3

namespace {

// 标准 crc32（多项式 0xEDB88320，初值/终值翻转；与 reanim v2 / pak64 同源）。
std::uint32_t ParticleV2Crc32(const std::uint8_t* data, const std::size_t size)
{
    static std::uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (std::uint32_t i = 0; i < 256; ++i) {
            std::uint32_t value = i;
            for (int bit = 0; bit < 8; ++bit)
                value = (value & 1) ? (0xEDB88320u ^ (value >> 1)) : (value >> 1);
            table[i] = value;
        }
        ready = true;
    }
    std::uint32_t crc = 0xFFFFFFFFu;
    for (std::size_t i = 0; i < size; ++i)
        crc = table[(crc ^ data[i]) & 0xFF] ^ (crc >> 8);
    return crc ^ 0xFFFFFFFFu;
}

// 运行时载荷的带界游标（提取 Name/OnDuration 用；任何越界/越样报错）。
class RuntimePayloadWalker final {
public:
    RuntimePayloadWalker(const std::vector<std::uint8_t>& bytes, std::string* error)
        : bytes_(bytes), error_(error) {}

    std::uint32_t U32(const char* what)
    {
        if (at_ + 4 > bytes_.size())
            Fail(error_, std::string("truncated: ") + what);
        std::uint32_t value = 0;
        std::memcpy(&value, bytes_.data() + at_, 4);
        at_ += 4;
        return value;
    }
    std::string String(const char* what)
    {
        const std::uint32_t length = U32(what);
        if (length > 100000 || at_ + length > bytes_.size())
            Fail(error_, std::string("truncated string: ") + what);
        std::string value(reinterpret_cast<const char*>(bytes_.data() + at_), length);
        at_ += length;
        return value;
    }
    void Track(const char* what)
    {
        const std::uint32_t count = U32(what);
        if (count > 100000 ||
            at_ + static_cast<std::size_t>(count) * 20 > bytes_.size())
            Fail(error_, std::string("truncated track: ") + what);
        at_ += static_cast<std::size_t>(count) * 20;
    }
    void Skip(const std::size_t count, const char* what)
    {
        if (at_ + count > bytes_.size())
            Fail(error_, std::string("truncated: ") + what);
        at_ += count;
    }
    [[nodiscard]] std::size_t At() const noexcept { return at_; }

private:
    const std::vector<std::uint8_t>& bytes_;
    std::string* error_;
    std::size_t at_ = 0;
};

std::string Folded(std::string_view text)
{
    std::string out(text);
    for (char& c : out) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return out;
}

} // namespace

bool ReadParticleRuntimePayload(const std::filesystem::path& input,
                                std::vector<std::uint8_t>& payload,
                                std::string* error)
{
    try {
        std::vector<std::uint8_t> file;
        if (!ReadFileBytes(input, file, error)) return false;
        if (file.size() < 16) Fail(error, "runtime record too small: " + input.string());
        std::uint32_t cookie = 0, declared = 0;
        std::memcpy(&cookie, file.data(), 4);
        std::memcpy(&declared, file.data() + 4, 4);
        if (cookie == 0xDEADFED5u) {
            // main.pak64 内的原始记录：header(cookie+size) + 载荷原样。
            if (static_cast<std::size_t>(declared) != file.size() - 8)
                Fail(error, "runtime payload size mismatch: " + input.string());
            payload.assign(file.begin() + 8, file.end());
            return true;
        }
        if (cookie == kParticleCookie) {
            // 安装包样式容器（stored 块 + 小端 adler）：解出的载荷即运行时等价物
            // ——前提是其 emitter 元素大小为 688（x64 语料），32 位安装记录
            // （704）不属于运行时记录，拒绝以免误当 v1 段。
            if (!InflateRecord(file, payload, error)) return false;
            if (payload.size() < 24) Fail(error, "payload too small: " + input.string());
            std::uint32_t elementSize = 0;
            std::memcpy(&elementSize, payload.data() + 20, 4);
            if (elementSize != kParticleEmitterNativeDumpSize)
                Fail(error, "record is not x64-native (element size " +
                                std::to_string(elementSize) + "): " + input.string());
            return true;
        }
        Fail(error, "unsupported runtime record cookie (expected DEADFED5 or "
                    "DEADFED4 stored): " + input.string());
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

bool ExtractParticleEmitterStrings(
    const std::vector<std::uint8_t>& payload,
    std::vector<std::pair<std::string, std::string>>& emitterStrings,
    std::string* error,
    std::vector<std::pair<std::size_t, std::string>>* imageFields)
{
    try {
        emitterStrings.clear();
        if (imageFields) imageFields->clear();
        if (payload.size() < 24) Fail(error, "runtime payload too small");
        std::uint32_t schema = 0;
        std::memcpy(&schema, payload.data(), 4);
        if (schema != kParticleSchemaHash)
            Fail(error, "schema hash mismatch (expected 3767703017)");
        RuntimePayloadWalker reader(payload, error);
        reader.Skip(4, "schema hash");
        reader.Skip(16, "top dump");
        if (reader.U32("emitter element size") != kParticleEmitterNativeDumpSize)
            Fail(error, "unexpected emitter element size");
        const std::uint32_t emitterCount = [&] {
            std::uint32_t count = 0;
            std::memcpy(&count, payload.data() + 12, 4);
            return count;
        }();
        if (emitterCount > 1000) Fail(error, "emitter count out of range");
        std::vector<std::pair<std::int32_t, std::int32_t>> fieldCounts;
        for (std::uint32_t e = 0; e < emitterCount; ++e) {
            std::int32_t pf = 0, sf = 0;
            std::memcpy(&pf, payload.data() + reader.At() +
                                 static_cast<std::size_t>(e) *
                                     kParticleEmitterNativeDumpSize + 408, 4);
            std::memcpy(&sf, payload.data() + reader.At() +
                                 static_cast<std::size_t>(e) *
                                     kParticleEmitterNativeDumpSize + 424, 4);
            if (pf < 0 || sf < 0) Fail(error, "negative field count");
            fieldCounts.emplace_back(pf, sf);
        }
        reader.Skip(static_cast<std::size_t>(emitterCount) *
                        kParticleEmitterNativeDumpSize,
                    "emitter dumps");
        for (std::uint32_t e = 0; e < emitterCount; ++e) {
            const std::string prefix = "emitter[" + std::to_string(e) + "]";
            const std::size_t imageAt = reader.At();
            const std::string image = reader.String((prefix + " image").c_str());
            if (imageFields) imageFields->emplace_back(imageAt, image);
            const std::size_t nameAt = reader.At();
            std::string name = reader.String((prefix + " name").c_str());
            if (imageFields) imageFields->emplace_back(nameAt, name);
            reader.Track((prefix + " SystemDuration").c_str());
            const std::size_t onDurationAt = reader.At();
            std::string onDuration = reader.String((prefix + " onDuration").c_str());
            if (imageFields) imageFields->emplace_back(onDurationAt, onDuration);
            for (int i = 1; i < kSystemTrackCount; ++i)
                reader.Track((prefix + " " + kSystemTrackNames[i]).c_str());
            for (const std::int32_t count : {fieldCounts[e].first, fieldCounts[e].second}) {
                if (reader.U32("field element size") != kParticleFieldDumpSize)
                    Fail(error, "unexpected field element size");
                reader.Skip(static_cast<std::size_t>(count) * kParticleFieldDumpSize,
                            "field dumps");
                for (std::int32_t i = 0; i < count; ++i) {
                    reader.Track("field x");
                    reader.Track("field y");
                }
            }
            for (int i = 0; i < kParticleTrackCount; ++i)
                reader.Track((prefix + " " + kParticleTrackNames[i]).c_str());
            emitterStrings.emplace_back(std::move(name), std::move(onDuration));
        }
        if (reader.At() != payload.size())
            Fail(error, "trailing payload bytes");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

std::vector<std::uint32_t> ResolveParticleOnDurationTargets(
    const std::vector<std::pair<std::string, std::string>>& emitterStrings)
{
    std::vector<std::string> foldedNames;
    foldedNames.reserve(emitterStrings.size());
    for (const auto& entry : emitterStrings)
        foldedNames.push_back(Folded(entry.first));
    std::vector<std::uint32_t> targets;
    targets.reserve(emitterStrings.size());
    for (const auto& entry : emitterStrings) {
        if (entry.second.empty()) {
            targets.push_back(kParticleOnDurationNone);
            continue;
        }
        // 与 FindEmitterDefByName 对齐：stricmp 折叠、按 def 声明序取第一。
        const std::string wanted = Folded(entry.second);
        std::uint32_t target = kParticleOnDurationNone;
        for (std::size_t j = 0; j < foldedNames.size(); ++j) {
            if (foldedNames[j] == wanted) {
                target = static_cast<std::uint32_t>(j);
                break;
            }
        }
        targets.push_back(target);
    }
    return targets;
}

std::vector<std::uint8_t> SerializeParticleV2(
    const std::vector<std::uint8_t>& runtimePayload,
    const std::vector<std::uint32_t>& onDurationTargets)
{
    std::vector<std::uint8_t> payload;
    PutU32(payload, kParticleV2PayloadMagic);
    payload.insert(payload.end(), runtimePayload.begin(), runtimePayload.end());
    PutU32(payload, static_cast<std::uint32_t>(onDurationTargets.size()));
    for (const std::uint32_t target : onDurationTargets) PutU32(payload, target);

    std::vector<std::uint8_t> out;
    PutU32(out, kParticleV2Cookie);
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    PutU32(out, ParticleV2Crc32(payload.data(), payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

std::vector<std::uint8_t> SerializeParticleV4(
    const std::vector<std::uint8_t>& runtimePayload,
    const ParticleDefinition& source,
    const std::map<std::string, std::uint32_t>& sourceImageHandles,
    const std::vector<std::uint32_t>& onDurationTargets,
    const std::vector<std::uint32_t>& nameIds,
    const std::uint32_t nameTableHash)
{
    if (source.emitters.size() != nameIds.size() ||
        nameIds.size() != onDurationTargets.size())
        throw std::runtime_error("particle v4: emitter tables disagree");

    RuntimePayloadWalker reader(runtimePayload, nullptr);
    if (reader.U32("schema") != kParticleSchemaHash)
        throw std::runtime_error("particle v4: unexpected gold schema");
    reader.Skip(8, "top pointer");
    const std::uint32_t count = reader.U32("emitter count");
    reader.Skip(4, "top padding");
    if (reader.U32("emitter size") != kParticleEmitterNativeDumpSize ||
        count != nameIds.size())
        throw std::runtime_error("particle v4: unexpected gold emitter layout");
    const std::size_t dumpsAt = reader.At();
    reader.Skip(static_cast<std::size_t>(count) * kParticleEmitterNativeDumpSize,
                "emitter dumps");

    const auto dump32 = [&](std::size_t offset) {
        std::uint32_t value = 0;
        if (offset + 4 > runtimePayload.size())
            throw std::runtime_error("particle v4: truncated gold dump");
        std::memcpy(&value, runtimePayload.data() + offset, 4);
        return value;
    };
    std::vector<std::uint8_t> body;
    // Native runtime track order differs from the historical import/oracle
    // order stored in ParticleEmitter::systemTracks.
    static constexpr int authoredSystemIndex[kSystemTrackCount] = {
        0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 12, 13, 11, 14, 20, 21, 15, 16, 17, 18, 19};
    const auto writeAuthoredTrack = [&](const std::string& raw) {
        const auto nodes = ParseAuthoredTrack(raw);
        PutU32(body, static_cast<std::uint32_t>(nodes.size()));
        for (const auto& node : nodes) {
            PutU32(body, static_cast<std::uint32_t>(node.numerator));
            PutU32(body, static_cast<std::uint32_t>(node.denominator));
            PutF32(body, node.low);
            PutF32(body, node.high);
            PutU32(body, static_cast<std::uint32_t>(node.curve));
            PutU32(body, static_cast<std::uint32_t>(node.distribution));
        }
    };
    const auto copyTrack = [&](const std::string& raw) {
        reader.Track("legacy comparison track");
        writeAuthoredTrack(raw);
    };
    for (std::uint32_t e = 0; e < count; ++e) {
        const std::size_t dumpAt = dumpsAt + e * kParticleEmitterNativeDumpSize;
        reader.String("legacy gold image");
        const auto& images = source.emitters[e].images;
        if (images.size() > 65536)
            throw std::runtime_error("particle source has too many images");
        PutU32(body, static_cast<std::uint32_t>(images.size()));
        for (const auto& image : images) {
            const auto found = sourceImageHandles.find(image);
            if (image.empty() || found == sourceImageHandles.end())
                throw std::runtime_error("particle image absent from source handles: " + image);
            PutU32(body, found->second);
        }
        for (auto value : {source.emitters[e].imageFrames, source.emitters[e].animated,
                           static_cast<std::int32_t>(source.emitters[e].flags),
                           source.emitters[e].emitterType})
            PutU32(body, static_cast<std::uint32_t>(value));
        // Gold strings are consumed only by the compiler. No length or text is emitted.
        reader.String("name");
        copyTrack(source.emitters[e].systemTracks[authoredSystemIndex[0]]);
        reader.String("onDuration");
        for (int t = 1; t < kSystemTrackCount; ++t)
            copyTrack(source.emitters[e].systemTracks[authoredSystemIndex[t]]);

        for (const std::size_t countOffset : {408u, 424u}) {
            const auto& authoredFields = countOffset == 408u
                ? source.emitters[e].particleFields : source.emitters[e].systemFields;
            const std::uint32_t fieldCount = dump32(dumpAt + countOffset);
            if (fieldCount > 4 || reader.U32("field size") != kParticleFieldDumpSize)
                throw std::runtime_error("particle v4: invalid gold field array");
            reader.Skip(fieldCount * kParticleFieldDumpSize, "field dumps");
            for (std::uint32_t f = 0; f < fieldCount; ++f) {
                reader.Track("legacy field x");
                reader.Track("legacy field y");
            }
            PutU32(body, static_cast<std::uint32_t>(authoredFields.size()));
            for (const auto& field : authoredFields) {
                PutU32(body, static_cast<std::uint32_t>(field.fieldType));
                writeAuthoredTrack(field.x);
                writeAuthoredTrack(field.y);
            }
        }
        for (int t = 0; t < kParticleTrackCount; ++t)
            copyTrack(source.emitters[e].particleTracks[t]);
    }
    if (reader.At() != runtimePayload.size())
        throw std::runtime_error("particle v4: unconsumed gold payload");

    std::vector<std::uint8_t> payload;
    PutU32(payload, 0x46524449u); // IDRF: finite float bounds, rational times.
    PutU32(payload, 0);  // No game image registry participates in source records.
    PutU32(payload, nameTableHash);
    PutU32(payload, count);
    PutU32(payload, static_cast<std::uint32_t>(body.size()));
    payload.insert(payload.end(), body.begin(), body.end());
    PutU32(payload, 0x33544449u); // IDT3: definition indices, declaration order.
    PutU32(payload, count);
    for (const auto target : onDurationTargets)
        PutU32(payload, target);
    PutU32(payload, 0x35544449u); // IDT5: corpus name IDs, declaration order.
    PutU32(payload, count);
    for (const auto id : nameIds)
        PutU32(payload, id);

    std::vector<std::uint8_t> out;
    PutU32(out, kParticleV4Cookie);
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    PutU32(out, ParticleV2Crc32(payload.data(), payload.size()));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

// ----------------------------------------------------------- 发射器名字语料表

std::vector<ParticleEmitterNameEntry> BuildParticleEmitterNameTable(
    const ParticleDslBundle& bundle)
{
    struct Spelling final {
        std::uint32_t count = 0;
        std::uint32_t firstSeen = 0;
    };
    // folded -> (拼写 -> {次数, 首见序})。代表拼写：最高频，并列取首见。
    std::map<std::string, std::map<std::string, Spelling>> byFolded;
    std::uint32_t seen = 0;
    for (const ParticleDefinition& definition : bundle.modules)
        for (const ParticleEmitter& emitter : definition.emitters) {
            if (emitter.name.empty()) continue;
            const std::string folded = Folded(emitter.name);
            Spelling& spelling = byFolded[folded][emitter.name];
            if (spelling.count == 0) spelling.firstSeen = seen;
            ++spelling.count;
            ++seen;
        }

    std::vector<ParticleEmitterNameEntry> table;
    table.reserve(byFolded.size());
    for (const auto& [key, perName] : byFolded) {
        ParticleEmitterNameEntry entry;
        entry.folded = key;
        std::uint32_t bestCount = 0, bestFirst = 0xFFFFFFFFu;
        for (const auto& [name, spelling] : perName) {
            if (spelling.count > bestCount ||
                (spelling.count == bestCount && spelling.firstSeen < bestFirst)) {
                bestCount = spelling.count;
                bestFirst = spelling.firstSeen;
                entry.display = name;
            }
        }
        table.push_back(std::move(entry));
    }
    std::sort(table.begin(), table.end(),
              [](const ParticleEmitterNameEntry& a, const ParticleEmitterNameEntry& b) {
                  return a.folded < b.folded;
              });
    // 枚举标识符：折叠名清洗（非 [a-z0-9] → '_'，数字开头前缀 '_'），
    // 清洗后碰撞直接硬错（人工改名是唯一出路）。
    std::map<std::string, std::string> idents;
    for (std::size_t i = 0; i < table.size(); ++i) {
        table[i].id = static_cast<std::uint32_t>(i + 1);
        std::string ident;
        for (char c : table[i].folded)
            ident += (std::isalnum(static_cast<unsigned char>(c))) ? c : '_';
        if (!ident.empty() && std::isdigit(static_cast<unsigned char>(ident[0])))
            ident.insert(ident.begin(), '_');
        if (idents.count(ident))
            throw std::runtime_error("particle emitter name enum collision: '" +
                                     idents[ident] + "' vs '" + table[i].display + "'");
        idents[ident] = table[i].display;
        table[i].ident = ident;
    }
    return table;
}

std::uint32_t ParticleEmitterNameTableHash(
    const std::vector<ParticleEmitterNameEntry>& table)
{
    std::vector<std::uint8_t> stream;
    const auto put8 = [&stream](std::uint8_t b) { stream.push_back(b); };
    for (const ParticleEmitterNameEntry& entry : table) {
        for (int k = 0; k < 4; ++k)
            put8(static_cast<std::uint8_t>(entry.id >> (8 * k)));
        for (char c : entry.folded) put8(static_cast<std::uint8_t>(c));
        put8(0);
    }
    return ParticleV2Crc32(stream.data(), stream.size());
}

std::string MakeParticleEmitterNameHeader(
    const std::vector<ParticleEmitterNameEntry>& table)
{
    std::ostringstream out;
    out << "#pragma once\n// Generated by reanimc compile-particle-v2.\n#include <cstdint>\n\n";
    out << "enum class ParticleEmitterName : std::uint32_t\n{\n    unnamed = 0,\n";
    for (const auto& entry : table)
        out << "    " << entry.ident << " = " << entry.id << ",\n";
    out << "    top = " << table.size() + 1 << "\n};\n\n";
    out << "inline constexpr std::uint32_t kParticleEmitterNameCount = " << table.size() << "u;\n";
    out << "inline constexpr std::uint32_t kParticleEmitterNameTableHash = "
        << ParticleEmitterNameTableHash(table) << "u;\n\n";
    out << R"(struct ParticleEmitterNameRange final
{
    ParticleEmitterName mBegin;
    ParticleEmitterName mEnd;

    constexpr bool Contains(ParticleEmitterName id) const noexcept
    {
        return id >= mBegin && id < mEnd;
    }
};

struct ParticleEmitterRanges final
{
    static constexpr ParticleEmitterNameRange All{ParticleEmitterName::unnamed, ParticleEmitterName::top};
)";
    // Prefix bounds are resolved by the compiler. Empty prefix is represented only by All.
    std::set<std::string> prefixes;
    for (std::size_t i = 1; i < table.size(); ++i) {
        const auto& a = table[i - 1].folded;
        const auto& b = table[i].folded;
        std::size_t n = 0;
        while (n < a.size() && n < b.size() && a[n] == b[n]) ++n;
        if (n >= 3) prefixes.insert(a.substr(0, n));
    }
    std::set<std::string> used;
    const auto enumAt = [&](std::size_t i) {
        return i == table.size() ? std::string("top") : table[i].ident;
    };
    for (const auto& prefix : prefixes) {
        const auto first = std::lower_bound(table.begin(), table.end(), prefix,
            [](const auto& entry, const auto& key) { return entry.folded < key; });
        auto last = first;
        while (last != table.end() && last->folded.starts_with(prefix)) ++last;
        if (last - first < 2) continue;
        std::string ident;
        for (const auto c : prefix)
            ident += std::isalnum(static_cast<unsigned char>(c)) ? c : '_';
        if (std::isdigit(static_cast<unsigned char>(ident.front()))) ident.insert(ident.begin(), '_');
        if (!used.insert(ident).second)
            throw std::runtime_error("particle emitter range collision: " + prefix);
        out << "    static constexpr ParticleEmitterNameRange " << ident
            << "{ParticleEmitterName::" << enumAt(first - table.begin())
            << ", ParticleEmitterName::" << enumAt(last - table.begin()) << "};\n";
    }
    out << "};\n";
    return out.str();
}

// --------------------------------------------------------------- CLI 子命令

namespace fs = std::filesystem;

int RunImportParticle(const std::vector<std::string>& args)
{
    fs::path input, output;
    std::string rootName = "particles.root.dsl";
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else if (args[i] == "--root") rootName = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if (input.empty() || output.empty()) {
        std::cerr << "reanimc: import-particle requires --input and --output\n";
        return 2;
    }
    std::error_code ec;
    std::vector<fs::path> sources;
    if (fs::is_directory(input, ec)) {
        for (const auto& entry : fs::directory_iterator(input))
            if (entry.path().extension() == ".xml") sources.push_back(entry.path());
        std::sort(sources.begin(), sources.end());
    } else {
        sources.push_back(input);
    }
    if (sources.empty()) {
        std::cerr << "reanimc: no particle XML under " << input.string() << '\n';
        return 1;
    }
    std::map<std::string, std::string> bySourceName;  // lower stem -> module name
    for (const fs::path& source : sources) {
        ParticleDefinition definition;
        std::string error;
        if (!ReadParticleXml(source, definition, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        const fs::path target =
            output / "particles" / (definition.name + ".particle.dsl");
        if (OutputMissing(target)) {
            std::cerr << "reanimc: output exists or is empty; choose a new path: "
                      << target.string() << '\n';
            return 2;
        }
        if (!WriteTextFile(target, ParticleToDsl(definition), &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        bySourceName[definition.sourceName] = definition.name;
        std::cout << source.string() << " -> " << target.string()
                  << " (emitters=" << definition.emitters.size() << ")\n";
    }
    // root：按 gLawnParticleArray 顺序 forward（仅收录已导入的源）。
    std::ostringstream root;
    root << "// particle DSL root: forward closure in gLawnParticleArray order.\n";
    int forwards = 0;
    for (const char* file : kLawnParticleFiles) {
        const std::string lower = Lowered(file);
        const auto found = bySourceName.find(lower);
        if (found == bySourceName.end()) continue;
        root << "forward \"particles/" << found->second << ".particle.dsl\";\n";
        ++forwards;
    }
    const fs::path rootPath = output / rootName;
    if (OutputMissing(rootPath)) {
        std::cerr << "reanimc: output exists or is empty; choose a new path: "
                  << rootPath.string() << '\n';
        return 2;
    }
    std::string error;
    if (!WriteTextFile(rootPath, root.str(), &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    std::cout << "root: " << rootPath.string() << " (" << forwards << " forwards, "
              << bySourceName.size() << " modules imported)\n";
    return 0;
}

int RunCompileParticle(const std::vector<std::string>& args)
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
    if (input.empty() || output.empty()) {
        std::cerr << "reanimc: compile-particle requires --input and --output\n";
        return 2;
    }
    ParticleDslBundle bundle;
    std::string error;
    if (!LoadParticleDslBundle(input, bundle, &error, true)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    fs::create_directories(output);
    std::size_t verified = 0, mismatches = 0;
    for (const ParticleDefinition& definition : bundle.modules) {
        const fs::path record =
            output / (definition.sourceName + ".xml.compiled");
        if (OutputMissing(record)) {
            std::cerr << "reanimc: output exists or is empty; choose a new path: "
                      << record.string() << '\n';
            return 2;
        }
        std::vector<std::string> labels;
        if (!WriteParticleCompiled(definition, record, &error,
                                   verify.empty() ? nullptr : &labels)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        std::cout << definition.name << " -> " << record.string()
                  << " (emitters=" << definition.emitters.size()
                  << ", bytes=" << fs::file_size(record) << ")\n";
        if (verify.empty()) continue;
        const fs::path gold =
            fs::is_directory(verify)
                ? verify / (definition.sourceName + ".xml.compiled")
                : verify;
        std::vector<std::uint8_t> goldBytes;
        if (!ReadFileBytes(gold, goldBytes, &error)) {
            std::cerr << "reanimc: verify: " << error << '\n';
            return 1;
        }
        std::vector<std::uint8_t> mine;
        if (!ReadFileBytes(record, mine, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        if (mine == goldBytes) {
            ++verified;
            std::cout << "verify: " << record.filename().string() << " == "
                      << gold.string() << " (" << mine.size()
                      << " bytes, identical)\n";
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
                      << " gold=0x" << std::hex
                      << static_cast<unsigned>(goldBytes[diff]) << std::dec << '\n';
            if (diff < labels.size())
                std::cerr << "verify: byte " << diff
                          << " belongs to: " << labels[diff] << '\n';
            else if (diff >= mine.size())
                std::cerr << "verify: byte " << diff
                          << " is beyond my record\n";
        } else {
            std::cerr << "verify: records agree on the common prefix; sizes differ"
                      << " (mine " << mine.size() << " vs gold "
                      << goldBytes.size() << ")\n";
        }
    }
    if (!verify.empty()) {
        std::cout << "verify: " << verified << "/" << bundle.modules.size()
                  << " records byte-identical\n";
        return mismatches == 0 ? 0 : 1;
    }
    return 0;
}

int RunInspectParticle(const std::vector<std::string>& args)
{
    fs::path input;
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if (input.empty()) {
        std::cerr << "reanimc: inspect-particle requires --input\n";
        return 2;
    }
    ParticleRecord record;
    std::string error;
    if (!ReadParticleCompiled(input, record, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    std::cout << InspectParticleText(record, input.filename().string());
    return 0;
}

int RunCompileParticleV2(const std::vector<std::string>& args)
{
    fs::path input, inputList, output, gold64, verify;
    if ((args.size()-1)%2) { std::cerr << "reanimc: option requires a value: " << args.back() << '\n'; return 2; }
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input") input = args[i + 1];
        else if (args[i] == "--input-list") inputList = args[i + 1];
        else if (args[i] == "--output") output = args[i + 1];
        else if (args[i] == "--gold64") gold64 = args[i + 1];
        else if (args[i] == "--verify") verify = args[i + 1];
        else {
            std::cerr << "reanimc: unknown option " << args[i] << '\n';
            return 2;
        }
    }
    if ((input.empty() == inputList.empty()) || output.empty() || gold64.empty()) {
        std::cerr << "reanimc: compile-particle-v2 requires --input, --output and "
                     "--gold64 (runtime-native gold records, e.g. extracted from "
                     "main.pak64 compiled/particles)\n";
        return 2;
    }
    ParticleDslBundle bundle;
    std::string error;
    if (!inputList.empty()) {
        std::ifstream list(inputList);
        if (!list) { std::cerr << "reanimc: cannot open " << inputList.string() << '\n'; return 1; }
        std::set<std::string> names;
        std::string path;
        while (std::getline(list, path)) {
            if (!path.empty() && path.back() == '\r') path.pop_back();
            if (path.empty()) continue;
            std::string text;
            ParticleDslFileDirectives file;
            if (!ReadFileText(path, text, &error) || !ParseParticleDslFileText(text, file, &error)) {
                std::cerr << "reanimc: " << error << '\n'; return 1;
            }
            if (!file.imports.empty() || !file.forwards.empty()) {
                std::cerr << "reanimc: compilation sources belong in the build configuration: " << path << '\n';
                return 1;
            }
            bundle.files.emplace_back(path);
            for (auto& module : file.modules) {
                if (!names.insert(module.name).second) {
                    std::cerr << "reanimc: duplicate particle module: " << module.name << '\n'; return 1;
                }
                bundle.publicNames.push_back(module.name);
                bundle.modules.push_back(std::move(module));
            }
        }
        std::sort(bundle.modules.begin(), bundle.modules.end(),
                  [](const auto& a, const auto& b) { return a.name < b.name; });
        if (bundle.modules.empty()) { std::cerr << "reanimc: empty particle input list\n"; return 1; }
        try {
            ResolveParticleVariants(bundle, &error);
        } catch (const std::exception& e) {
            std::cerr << "reanimc: " << e.what() << '\n'; return 1;
        }
    } else if (!LoadParticleDslBundle(input, bundle, &error)) {
        std::cerr << "reanimc: " << error << '\n';
        return 1;
    }
    if (!fs::is_directory(gold64)) {
        std::cerr << "reanimc: --gold64 is not a directory: " << gold64.string() << '\n';
        return 2;
    }
    // Each selected source corpus owns its sorted offline image handle list.
    std::map<std::string, std::uint32_t> sourceImageHandles;
    for (const auto& definition:bundle.modules)
        for (const auto& emitter:definition.emitters)
            for (const auto& image : emitter.images)
                sourceImageHandles.emplace(image, 0);
    fs::create_directories(output);
    {
        std::ofstream sources(output / "image_sources.txt",std::ios::binary | std::ios::trunc);
        std::uint32_t nextHandle=1;
        for (auto& [name,handle]:sourceImageHandles) {
            if (name.find_first_of("\r\n")!=std::string::npos || name.find('\0')!=std::string::npos) {
                std::cerr << "reanimc: image source names cannot contain line breaks or NUL\n"; return 1;
            }
            handle=nextHandle++;
            sources << name << '\n';
        }
        if (!sources) {
            std::cerr << "reanimc: cannot write image_sources.txt\n"; return 1;
        }
    }
    // 发射器名字语料表（2026-10-09 用户设计，镜像 reanim 轨道名模式）：
    // 全语料 emitter 名折叠去重、字典序 1 基，0=unnamed；头文件随产物发射，
    // 每 v4 记录尾部 IDT5 段携带逐 emitter 名 id——运行时整数比对零字符串。
    fs::create_directories(output);
    const std::vector<ParticleEmitterNameEntry> emitterNameTable =
        BuildParticleEmitterNameTable(bundle);
    const std::uint32_t emitterNameTableHash = ParticleEmitterNameTableHash(emitterNameTable);
    std::map<std::string, std::uint32_t> emitterNameIdByFolded;
    for (const ParticleEmitterNameEntry& entry : emitterNameTable)
        emitterNameIdByFolded[entry.folded] = entry.id;
    {
        const fs::path headerPath = output / "ParticleEmitterName.h";
        const std::string headerText = MakeParticleEmitterNameHeader(emitterNameTable);
        std::ofstream headerOut(headerPath, std::ios::binary | std::ios::trunc);
        if (!headerOut) {
            std::cerr << "reanimc: cannot write " << headerPath.string() << '\n';
            return 1;
        }
        headerOut.write(headerText.data(), static_cast<std::streamsize>(headerText.size()));
        if (!headerOut) {
            std::cerr << "reanimc: write failed for " << headerPath.string() << '\n';
            return 1;
        }
        std::cout << "emitter name table: entries=" << emitterNameTable.size()
                  << " hash=0x" << std::hex << emitterNameTableHash << std::dec << '\n';
    }
    // 金标准按折叠 stem 索引（语料 source-name 全小写，pak 内文件名大小写不一）。
    std::map<std::string, fs::path> goldByStem;
    for (const auto& entry : fs::directory_iterator(gold64)) {
        const std::string name = entry.path().filename().string();
        if (!name.ends_with(".xml.compiled")) continue;
        goldByStem[Lowered(name.substr(0, name.size() - 13))] = entry.path();
    }
    fs::create_directories(output);
    std::size_t linked = 0, verified = 0, mismatches = 0;
    std::map<std::string, const ParticleDefinition*> definitionsByName;
    for (const auto& definition : bundle.modules)
        definitionsByName.emplace(definition.name, &definition);
    {
        std::map<std::string, std::string> inheritance;
        for (const auto& definition : bundle.modules)
            if (!definition.variantOf.empty())
                inheritance.emplace("compiled/particles/" + Lowered(definition.sourceName) + ".compiled",
                    "compiled/particles/" + Lowered(definitionsByName.at(definition.variantOf)->sourceName) + ".compiled");
        std::ofstream sidecar(output / "particle_inheritance.json", std::ios::binary | std::ios::trunc);
        sidecar << "{\n";
        bool first = true;
        for (const auto& [child, parent] : inheritance) {
            if (!first) sidecar << ",\n";
            first = false;
            sidecar << "  "; WriteEscaped(sidecar, child);
            sidecar << ": "; WriteEscaped(sidecar, parent);
        }
        sidecar << "\n}\n";
        if (!sidecar) { std::cerr << "reanimc: cannot write particle_inheritance.json\n"; return 1; }
        std::map<std::string, std::vector<const ParticleDefinition*>> roots;
        for (const auto& definition : bundle.modules) {
            const auto* root = &definition;
            while (!root->variantOf.empty()) root = definitionsByName.at(root->variantOf);
            roots[root->name].push_back(&definition);
        }
        std::ofstream metadata(output / "particle_variants.json", std::ios::binary | std::ios::trunc);
        std::ofstream enums(output / "ParticleVariantId.h", std::ios::binary | std::ios::trunc);
        metadata << "{\"roots\":{";
        enums << "// Generated particle appearance indices.\n#pragma once\n#include <cstdint>\nnamespace ParticleVariants {\n";
        bool firstRoot = true;
        for (auto& [rootName, variants] : roots) {
            const auto* root = definitionsByName.at(rootName);
            std::sort(variants.begin(), variants.end(), [root](auto* a, auto* b) {
                if (a == root || b == root) return a == root && b != root;
                return a->name < b->name;
            });
            if (!firstRoot) metadata << ',';
            firstRoot = false;
            WriteEscaped(metadata, "compiled/particles/" + Lowered(root->sourceName) + ".compiled");
            metadata << ":{\"name\":"; WriteEscaped(metadata, rootName);
            metadata << ",\"variants\":[";
            enums << "enum class " << rootName << " : std::uint32_t { Default = 0";
            for (std::size_t i = 0; i < variants.size(); ++i) {
                const auto* item = variants[i];
                if (i) { metadata << ','; enums << ", " << item->name << " = " << i; }
                metadata << "{\"name\":"; WriteEscaped(metadata, item->name);
                metadata << ",\"source\":"; WriteEscaped(metadata, item->sourceName);
                metadata << ",\"path\":";
                WriteEscaped(metadata, "compiled/particles/" + Lowered(item->sourceName) + ".compiled");
                metadata << ",\"index\":" << i << '}';
            }
            metadata << "]}";
            enums << " };\n";
        }
        metadata << "}}\n";
        enums << "}\n";
        if (!metadata || !enums) { std::cerr << "reanimc: cannot write particle variant metadata\n"; return 1; }
    }
    for (const ParticleDefinition& definition : bundle.modules) {
        const ParticleDefinition* original = &definition;
        while (!original->variantOf.empty())
            original = definitionsByName.at(original->variantOf);
        const auto gold = goldByStem.find(original->sourceName);
        if (gold == goldByStem.end()) {
            std::cerr << "reanimc: no runtime gold record for <" << definition.sourceName
                      << "> under " << gold64.string() << '\n';
            return 1;
        }
        std::vector<std::uint8_t> payload;
        if (!ReadParticleRuntimePayload(gold->second, payload, &error)) {
            std::cerr << "reanimc: " << error << '\n';
            return 1;
        }
        std::vector<std::pair<std::string, std::string>> emitterStrings;
        if (!ExtractParticleEmitterStrings(payload, emitterStrings, &error,
                                           nullptr)) {
            std::cerr << "reanimc: " << gold->second.string() << ": " << error << '\n';
            return 1;
        }
        // DSL 模型与运行时金标准一致性：emitter 数与逐 emitter 的
        // Name/OnDuration 必须一致（两者同源 XML；分歧即语料漂移，拒绝编译）。
        if (emitterStrings.size() != definition.emitters.size()) {
            std::cerr << "reanimc: " << definition.name << ": emitter count mismatch (dsl "
                      << definition.emitters.size() << " vs gold "
                      << emitterStrings.size() << ")\n";
            return 1;
        }
        for (std::size_t e = 0; e < emitterStrings.size(); ++e) {
            if (emitterStrings[e].first != definition.emitters[e].name ||
                emitterStrings[e].second != definition.emitters[e].onDuration) {
                std::cerr << "reanimc: " << definition.name << ": emitter " << e
                          << " name/onDuration mismatch (dsl <"
                          << definition.emitters[e].name << ">/<"
                          << definition.emitters[e].onDuration << "> vs gold <"
                          << emitterStrings[e].first << ">/<"
                          << emitterStrings[e].second << ">)\n";
                return 1;
            }
        }
        const std::vector<std::uint32_t> targets =
            ResolveParticleOnDurationTargets(emitterStrings);
        // 逐 emitter 语料名 id（0=未名）；stricmp 折叠与名字表同一语义。
        std::vector<std::uint32_t> nameIds;
        nameIds.reserve(emitterStrings.size());
        for (const auto& [name, onDuration] : emitterStrings) {
            std::uint32_t id = 0;
            if (!name.empty()) {
                const auto found = emitterNameIdByFolded.find(Folded(name));
                if (found == emitterNameIdByFolded.end())
                    throw std::runtime_error("particle emitter name missing from table: '" + name + "'");
                id = found->second;
            }
            nameIds.push_back(id);
        }
        std::size_t recordLinks = 0;
        for (const std::uint32_t target : targets)
            if (target != kParticleOnDurationNone) ++recordLinks;
        linked += recordLinks;
        // 产物命名沿用金标准主名（<原名>.xml.v2.compiled）。
        std::string base = gold->second.filename().string();
        base = base.substr(0, base.size() - 13);  // 去 ".xml.compiled"
        if (!definition.variantOf.empty()) base = definition.sourceName;
        const fs::path record = output / (base + ".xml.v2.compiled");
        if (OutputMissing(record)) {
            std::cerr << "reanimc: output exists or is empty; choose a new path: "
                      << record.string() << '\n';
            return 2;
        }
        const std::vector<std::uint8_t> bytes = SerializeParticleV2(payload, targets);
        std::ofstream out(record, std::ios::binary | std::ios::trunc);
        if (!out) {
            std::cerr << "reanimc: cannot write " << record.string() << '\n';
            return 1;
        }
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
        if (!out) {
            std::cerr << "reanimc: write failed for " << record.string() << '\n';
            return 1;
        }
        std::cout << definition.name << " -> " << record.string()
                  << " (emitters=" << emitterStrings.size()
                  << ", onDuration links=" << recordLinks
                  << ", bytes=" << bytes.size() << ")\n";
        {
            const fs::path v4record = output / (base + ".source-record");
            if (OutputMissing(v4record)) {
                std::cerr << "reanimc: output exists or is empty; choose a new path: "
                          << v4record.string() << '\n';
                return 2;
            }
            std::vector<std::uint8_t> v4bytes;
            try {
                v4bytes = SerializeParticleV4(payload, definition, sourceImageHandles,
                                              targets, nameIds,
                                              emitterNameTableHash);
            } catch (const std::exception& e) {
                std::cerr << "reanimc: " << definition.name << ": " << e.what() << '\n';
                return 1;
            }
            std::ofstream v4out(v4record, std::ios::binary | std::ios::trunc);
            if (!v4out) {
                std::cerr << "reanimc: cannot write " << v4record.string() << '\n';
                return 1;
            }
            v4out.write(reinterpret_cast<const char*>(v4bytes.data()),
                        static_cast<std::streamsize>(v4bytes.size()));
            if (!v4out) {
                std::cerr << "reanimc: write failed for " << v4record.string() << '\n';
                return 1;
            }
            std::cout << definition.name << " -> " << v4record.string()
                      << " (offline source image handles=" << definition.emitters.size()
                      << ", bytes=" << v4bytes.size() << ")\n";
        }
        if (verify.empty()) continue;
        const fs::path verifyGold = fs::is_directory(verify)
            ? (verify / gold->second.filename())
            : verify;
        std::vector<std::uint8_t> goldPayload;
        if (!ReadParticleRuntimePayload(verifyGold, goldPayload, &error)) {
            std::cerr << "reanimc: verify: " << error << '\n';
            return 1;
        }
        // v1 段 = 容器头（12 字节）+ magic（4 字节）之后、IDT3 之前的字节。
        const bool same = bytes.size() >= 16 + goldPayload.size() + 4 &&
            std::equal(goldPayload.begin(), goldPayload.end(), bytes.begin() + 16);
        if (same) {
            ++verified;
            std::cout << "verify: " << record.filename().string()
                      << " v1 segment == " << verifyGold.string() << " ("
                      << goldPayload.size() << " bytes, identical)\n";
        } else {
            ++mismatches;
            std::cerr << "verify: MISMATCH " << record.filename().string()
                      << " v1 segment vs " << verifyGold.string() << '\n';
        }
    }
    std::cout << "compile-particle-v2: " << bundle.modules.size()
              << " records, onDuration links=" << linked << '\n';
    if (!verify.empty()) {
        std::cout << "verify: " << verified << "/" << bundle.modules.size()
                  << " v1 segments byte-identical\n";
        return mismatches == 0 ? 0 : 1;
    }
    return 0;
}

} // namespace openpvz::reanim::particle
