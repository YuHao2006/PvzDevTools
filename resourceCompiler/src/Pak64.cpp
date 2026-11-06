#include "reanim/Pak64.h"
#include "reanim/SourceNumber.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <functional>
#include <iomanip>
#include <limits>
#include <map>
#include <set>
#include <tuple>
#include <sstream>
#include <stdexcept>
#include <zlib.h>

namespace openpvz::reanim::pak64 {
namespace {

constexpr std::size_t kMaxTracks = 100000;
constexpr std::size_t kMaxFramesPerTrack = 1000000;
constexpr std::size_t kMaxStringLength = 100000;
constexpr const char* kFieldTags[8] = {"x", "y", "kx", "ky", "sx", "sy", "f", "a"};

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

std::string CanonicalResource(std::string name)
{
    name = Trim(name);
    const bool image = name.size() >= 6 && IEquals(name.substr(0, 6), "image_");
    const bool font = name.size() >= 5 && IEquals(name.substr(0, 5), "font_");
    if (image || font)
        for (char& c : name) c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
    return name;
}

// sscanf %f semantics: leading whitespace skipped, longest numeric prefix.
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

void PutU32(std::vector<std::uint8_t>& out, const std::uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i)
        out.push_back(static_cast<std::uint8_t>(value >> (i * 8)));
}

void PutF32(std::vector<std::uint8_t>& out, const float value)
{
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, 4);
    PutU32(out, bits);
}

void PutString(std::vector<std::uint8_t>& out, const std::string& text)
{
    if (text.size() > kMaxStringLength) Fail(nullptr, "string too long");
    PutU32(out, static_cast<std::uint32_t>(text.size()));
    out.insert(out.end(), text.begin(), text.end());
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

std::string_view Between(const std::string_view text, const std::string_view open,
                         const std::string_view close, const std::size_t from,
                         std::size_t& contentBegin, bool& found)
{
    const std::size_t at = text.find(open, from);
    if (at == std::string_view::npos) {
        found = false;
        return {};
    }
    const std::size_t begin = at + open.size();
    const std::size_t end = text.find(close, begin);
    if (end == std::string_view::npos) {
        found = false;
        return {};
    }
    found = true;
    contentBegin = begin;
    return text.substr(begin, end - begin);
}

std::string ChildText(const std::string_view block, const std::string_view tag)
{
    const std::string open = "<" + std::string(tag) + ">";
    const std::string close = "</" + std::string(tag) + ">";
    std::size_t begin = 0;
    bool found = false;
    return std::string(Between(block, open, close, 0, begin, found));
}

bool HasChild(const std::string_view block, const std::string_view tag)
{
    return block.find("<" + std::string(tag) + ">") != std::string_view::npos;
}

} // namespace

bool ReadRawXml(const std::filesystem::path& input, RawReanim& output,
                std::string* error)
{
    try {
        std::ifstream in(input, std::ios::binary);
        if (!in) Fail(error, "cannot open " + input.string());
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::string text = buffer.str();
        const std::string_view xml(text);

        output = RawReanim{};
        output.name = input.stem().string();
        float fps = 12.0f;
        if (xml.find("<fps>") != std::string_view::npos) {
            float parsed = 0.0f;
            if (ParseFloatPrefix(ChildText(xml, "fps"), parsed) && parsed != kPlaceholder)
                fps = parsed;
        }
        output.fps = fps;

        std::size_t cursor = 0;
        while (true) {
            const std::size_t openAt = xml.find("<track>", cursor);
            if (openAt == std::string_view::npos) break;
            const std::size_t closeAt = xml.find("</track>", openAt);
            if (closeAt == std::string_view::npos)
                Fail(error, "unterminated <track>");
            const std::string_view block = xml.substr(openAt + 7, closeAt - openAt - 7);
            cursor = closeAt + 8;

            RawTrack track;
            track.name = ChildText(block, "name");
            std::size_t frameCursor = 0;
            while (true) {
                const std::size_t frameOpen = block.find("<t>", frameCursor);
                if (frameOpen == std::string_view::npos) break;
                const std::size_t frameClose = block.find("</t>", frameOpen);
                if (frameClose == std::string_view::npos)
                    Fail(error, "unterminated <t> in track <" + track.name + ">");
                const std::string_view frame = block.substr(frameOpen + 3, frameClose - frameOpen - 3);
                frameCursor = frameClose + 4;

                RawTransform transform;
                for (int i = 0; i < 8; ++i) {
                    if (!HasChild(frame, kFieldTags[i])) continue;
                    float parsed = 0.0f;
                    if (ParseFloatPrefix(ChildText(frame, kFieldTags[i]), parsed) &&
                        parsed != kPlaceholder)
                        transform.values[i] = parsed;
                }
                if (HasChild(frame, "i"))
                    transform.image = CanonicalResource(ChildText(frame, "i"));
                if (HasChild(frame, "font"))
                    transform.font = Trim(ChildText(frame, "font"));
                if (HasChild(frame, "text"))
                    transform.text = ChildText(frame, "text");
                track.frames.push_back(std::move(transform));
            }
            if (track.frames.size() > kMaxFramesPerTrack)
                Fail(error, "track exceeds frame limit");
            output.tracks.push_back(std::move(track));
        }
        if (output.tracks.size() > kMaxTracks) Fail(error, "track limit exceeded");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

// ---------------------------------------------------------------- DSL syntax

namespace {

enum class TokenKind { identifier, string, number, brace, colon, semicolon, assign, bracket, comma, end };

struct Token final {
    TokenKind kind = TokenKind::end;
    std::string text;
    std::size_t line = 1;
};

class Lexer final {
public:
    explicit Lexer(const std::string_view source) : source_(source) { Advance(); }
    [[nodiscard]] const Token& current() const noexcept { return token_; }
    void Advance()
    {
        for (;;) {
            while (position_ < source_.size() && std::isspace(static_cast<unsigned char>(source_[position_])))
                ++position_;
            if (source_.substr(position_, 2) == "//") {
                while (position_ < source_.size() && source_[position_] != '\n') ++position_;
                continue;
            }
            if (source_.substr(position_, 2) == "/*") {
                position_ += 2;
                while (position_ < source_.size() && source_.substr(position_, 2) != "*/") ++position_;
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
            token_.kind = TokenKind::string;
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
            token_.kind = TokenKind::brace;
            token_.text = c;
            ++position_;
            return;
        }
        if (c == '[' || c == ']') {
            token_.kind = TokenKind::bracket;
            token_.text = c;
            ++position_;
            return;
        }
        if (c == ',') {
            token_.kind = TokenKind::comma;
            token_.text = ",";
            ++position_;
            return;
        }
        if (c == ':') {
            if (position_ + 1 < source_.size() && source_[position_ + 1] == '=') {
                token_.kind = TokenKind::assign;
                token_.text = ":=";
                position_ += 2;
            } else {
                token_.kind = TokenKind::colon;
                token_.text = ":";
                ++position_;
            }
            return;
        }
        if (c == ';') {
            token_.kind = TokenKind::semicolon;
            token_.text = ";";
            ++position_;
            return;
        }
        token_.kind = std::isdigit(static_cast<unsigned char>(c)) ||
                              ((c == '-' || c == '+') && position_ + 1 < source_.size() &&
                               std::isdigit(static_cast<unsigned char>(source_[position_ + 1])))
                          ? TokenKind::number
                          : TokenKind::identifier;
        while (position_ < source_.size()) {
            const char k = source_[position_];
            if (std::isspace(static_cast<unsigned char>(k)) || k == '{' || k == '}' ||
                k == '[' || k == ']' || k == ',' || k == ':' || k == ';' || k == '"')
                break;
            token_.text += k;
            ++position_;
        }
    }

private:
    std::size_t line_ = 1, countedAt_ = 0;
    std::string_view source_;
    std::size_t position_ = 0;
    Token token_;
};

class DslParser final {
public:
    DslParser(const std::string_view source, std::vector<RawReanim>& outputs,
              std::string* error)
        : lexer_(source), outputs_(outputs), error_(error) {}

    // theDirectives 为空 = 纯字符串源（文件级指令报错）；
    // 非空 = 文件模式，收集 import/forward/prefixStrs 与公开模块名。
    void Run(DslFileDirectives* theDirectives)
    {
        for (;;) {
            const Token& token = lexer_.current();
            if (token.kind == TokenKind::end)
                break;
            if (token.kind == TokenKind::identifier && token.text == "import") {
                if (theDirectives == nullptr)
                    Fail(error_, "file-level 'import' requires a file-based load (LoadDslBundle)");
                theDirectives->imports.push_back(ReadFileReference("import"));
            } else if (token.kind == TokenKind::identifier && token.text == "forward") {
                if (theDirectives == nullptr)
                    Fail(error_, "file-level 'forward' requires a file-based load (LoadDslBundle)");
                theDirectives->forwards.push_back(ReadFileReference("forward"));
            } else if (token.kind == TokenKind::identifier && token.text == "prefixStrs") {
                if (theDirectives == nullptr)
                    Fail(error_, "file-level 'prefixStrs' requires a file-based load (LoadDslBundle)");
                ReadPrefixStrs(*theDirectives);
            } else {
                ParseModule(theDirectives);
            }
        }
        // 注意：文件可以只含指令（例如全是 forward 的根文件），模块为空合法；
        // “无模块”的检查由 ParseModules（字符串源）与 LoadDslBundle（整体）负责。
    }

private:
    std::string ReadFileReference(const char* theKeyword)
    {
        lexer_.Advance();  // keyword
        const std::string path = TakeString();
        ExpectSemicolon();
        if (path.empty())
            Fail(error_, std::string(theKeyword) + " requires a non-empty file path");
        return path;
    }

    void ReadPrefixStrs(DslFileDirectives& theDirectives)
    {
        lexer_.Advance();  // prefixStrs
        if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':'");
        lexer_.Advance();
        if (!(lexer_.current().kind == TokenKind::bracket && lexer_.current().text == "["))
            Fail(error_, "expected '[' after prefixStrs:");
        lexer_.Advance();
        if (!(lexer_.current().kind == TokenKind::bracket && lexer_.current().text == "]")) {
            for (;;) {
                theDirectives.prefixStrs.push_back(TakeString());
                if (lexer_.current().kind == TokenKind::bracket && lexer_.current().text == "]")
                    break;
                if (lexer_.current().kind != TokenKind::comma)
                    Fail(error_, "expected ',' or ']' in prefixStrs array");
                lexer_.Advance();
            }
        }
        lexer_.Advance();  // ']'
        ExpectSemicolon();
    }

    void ParseModule(DslFileDirectives* theDirectives)
    {
        bool exported = false;
        if (lexer_.current().kind == TokenKind::identifier && lexer_.current().text == "export") {
            exported = true;
            lexer_.Advance();
        }
        Expect("component");
        if (lexer_.current().kind != TokenKind::identifier)
            Fail(error_, "expected component name");
        RawReanim module;
        module.name = lexer_.current().text;
        lexer_.Advance();
        if (lexer_.current().kind == TokenKind::identifier &&
            lexer_.current().text == "inherits") {
            lexer_.Advance();
            ConsumeIdentifier();
        }
        ExpectBrace("{");
        bool hasImages = false, hasFps = false, hasStorageRegion = false;
        TrackStorage storage = TrackStorage::both;
        while (lexer_.current().kind == TokenKind::identifier) {
            if (lexer_.current().text == "cpu" || lexer_.current().text == "gpu" ||
                lexer_.current().text == "both") {
                const std::string region = lexer_.current().text;
                lexer_.Advance();
                const TrackStorage selected = region == "cpu" ? TrackStorage::cpu :
                    region == "gpu" ? TrackStorage::gpu : TrackStorage::both;
                if (lexer_.current().kind == TokenKind::colon) {
                    lexer_.Advance();
                    storage = selected;
                } else {
                    // A declaration prefix applies only to this track. Keep
                    // the enclosing region's default for the next declaration.
                    if (lexer_.current().kind != TokenKind::identifier)
                        Fail(error_, "expected ':' or track declaration after storage <" + region + ">");
                    ReadTrack(module, selected);
                }
                hasStorageRegion = true;
            } else if (lexer_.current().text == "variant-of") {
                if (!module.variantOf.empty()) Fail(error_, "duplicate variant-of");
                lexer_.Advance();
                if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':'");
                lexer_.Advance();
                if (lexer_.current().kind != TokenKind::identifier) Fail(error_, "expected variant parent");
                module.variantOf = lexer_.current().text;
                lexer_.Advance();
                ExpectSemicolon();
            } else if (lexer_.current().text == "images") {
                if (hasImages) Fail(error_, "duplicate variant images");
                hasImages = true;
                lexer_.Advance();
                if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':'");
                lexer_.Advance();
                const auto bracket = [&](std::string_view value) {
                    if (lexer_.current().kind != TokenKind::bracket || lexer_.current().text != value)
                        Fail(error_, "expected '" + std::string(value) + "'");
                    lexer_.Advance();
                };
                bracket("[");
                while (lexer_.current().text != "]") {
                    bracket("[");
                    std::vector<std::string> images;
                    while (lexer_.current().text != "]") {
                        std::string image;
                        if (lexer_.current().kind == TokenKind::identifier && lexer_.current().text == "none")
                            lexer_.Advance();
                        else {
                            image = TakeString();
                            if (image.empty() || image.find_first_of("\r\n") != std::string::npos ||
                                image.find('\0') != std::string::npos)
                                Fail(error_, "variant images require non-empty source names or none");
                            image = CanonicalResource(image);
                        }
                        images.push_back(std::move(image));
                        if (lexer_.current().kind != TokenKind::comma) break;
                        lexer_.Advance();
                        if (lexer_.current().text == "]") Fail(error_, "trailing variant image comma");
                    }
                    bracket("]");
                    module.variantImages.push_back(std::move(images));
                    if (lexer_.current().kind != TokenKind::comma) break;
                    lexer_.Advance();
                    if (lexer_.current().text == "]") Fail(error_, "trailing variant track comma");
                }
                bracket("]");
                ExpectSemicolon();
            } else if (lexer_.current().text == "fps") {
                hasFps = true;
                ReadFps(module);
            }
            else if (lexer_.current().text == "texture")
                Fail(error_, "texture declarations were removed; write image: <resource-name> directly");
            else
                ReadTrack(module, storage);
        }
        ExpectBrace("}");
        if (!module.variantOf.empty()) {
            if (!hasImages) Fail(error_, "reanim variant requires images");
            if (hasStorageRegion) Fail(error_, "reanim variant cannot override storage");
            if (hasFps || !module.tracks.empty()) Fail(error_, "reanim variant cannot override behavior");
        } else if (hasImages) Fail(error_, "variant images requires variant-of");
        for (const RawReanim& existing : outputs_)
            if (existing.name == module.name)
                Fail(error_, "duplicate module name <" + module.name + ">");
        if (exported && theDirectives != nullptr)
            theDirectives->publicNames.push_back(module.name);
        outputs_.push_back(std::move(module));
    }
    void Expect(const std::string_view keyword)
    {
        if (lexer_.current().kind != TokenKind::identifier || lexer_.current().text != keyword)
            Fail(error_, "expected '" + std::string(keyword) + "'");
        lexer_.Advance();
    }
    void ExpectBrace(const std::string_view brace)
    {
        if (lexer_.current().kind != TokenKind::brace || lexer_.current().text != brace)
            Fail(error_, std::string("expected '") + std::string(brace) + "'");
        lexer_.Advance();
    }
    void ConsumeIdentifier()
    {
        if (lexer_.current().kind != TokenKind::identifier)
            Fail(error_, "expected identifier");
        lexer_.Advance();
    }
    std::string TakeString()
    {
        if (lexer_.current().kind != TokenKind::string) Fail(error_, "expected string");
        const std::string value = lexer_.current().text;
        lexer_.Advance();
        return value;
    }
    void ReadFps(RawReanim& module)
    {
        lexer_.Advance();
        if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':' after fps");
        lexer_.Advance();
        try {
            module.fps = ParseFiniteFloat(lexer_.current().text);
            if (module.fps <= 0) Fail(error_, "fps must be positive");
        } catch (const std::exception& e) {
            Fail(error_, "line " + std::to_string(lexer_.current().line) + ", fps: " + e.what());
        }
        lexer_.Advance();
        ExpectSemicolon();
    }
    void ExpectSemicolon()
    {
        if (lexer_.current().kind != TokenKind::semicolon) Fail(error_, "expected ';'");
        lexer_.Advance();
    }
    void ReadTrack(RawReanim& module, const TrackStorage storage)
    {
        // `Track { ... }` directly, or `name := Track { ... }`; the instance
        // name is cosmetic because source-name carries the real track name.
        if (lexer_.current().kind != TokenKind::identifier)
            Fail(error_, "expected identifier");
        const std::string declaration = lexer_.current().text;
        lexer_.Advance();
        if (lexer_.current().kind == TokenKind::colon)
            Fail(error_, "unknown storage region <" + declaration + ">");
        if (lexer_.current().kind == TokenKind::assign) {
            lexer_.Advance();
            Expect("Track");
        } else if (declaration != "Track")
            Fail(error_, "expected track declaration");
        ExpectBrace("{");
        RawTrack track;
        track.storage = storage;
        struct Declared final {
            std::uint64_t index;
            RawTransform value;
            bool set[8] = {};
            bool setImage = false, setFont = false, setText = false;
        };
        std::vector<Declared> declared;
        std::uint64_t frameCount = 0;
        bool explicitCount = false;
        while (!(lexer_.current().kind == TokenKind::brace && lexer_.current().text == "}")) {
            if (lexer_.current().kind != TokenKind::identifier)
                Fail(error_, "expected track property");
            const std::string property = lexer_.current().text;
            lexer_.Advance();
            if (property == "source-name") {
                if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':'");
                lexer_.Advance();
                track.name = TakeString();
                ExpectSemicolon();
            } else if (property == "frame-count") {
                if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':'");
                lexer_.Advance();
                if (lexer_.current().kind != TokenKind::number)
                    Fail(error_, "expected frame-count number");
                const auto parsedCount = ParseDecimalInteger(lexer_.current().text);
                if (parsedCount < 0) Fail(error_, "frame-count must be nonnegative");
                frameCount = static_cast<std::uint64_t>(parsedCount);
                if (frameCount > kMaxFramesPerTrack) Fail(error_, "frame-count out of range");
                explicitCount = true;
                lexer_.Advance();
                ExpectSemicolon();
            } else if (property == "frame") {
                if (lexer_.current().kind != TokenKind::number)
                    Fail(error_, "expected nonnegative integer frame index");
                const auto parsedIndex = ParseDecimalInteger(lexer_.current().text);
                if (parsedIndex < 0) Fail(error_, "frame index must be nonnegative");
                const std::uint64_t index = static_cast<std::uint64_t>(parsedIndex);
                if (index >= kMaxFramesPerTrack) Fail(error_, "frame index out of range");
                lexer_.Advance();
                ExpectBrace("{");
                Declared item{index, {}, {}, false, false, false};
                item.value = ReadFrameBody(item.set, item.setImage, item.setFont, item.setText);
                declared.push_back(std::move(item));
                frameCount = std::max<std::uint64_t>(frameCount, index + 1);
            } else {
                Fail(error_, "unknown track property <" + property + ">");
            }
        }
        ExpectBrace("}");
        if (frameCount > kMaxFramesPerTrack) Fail(error_, "frame limit exceeded");
        track.frames.assign(static_cast<std::size_t>(frameCount), RawTransform{});
        std::sort(declared.begin(), declared.end(),
                  [](const Declared& a, const Declared& b) { return a.index < b.index; });
        for (std::size_t i = 1; i < declared.size(); ++i)
            if (declared[i].index == declared[i - 1].index)
                Fail(error_, "duplicate frame index");
        (void)explicitCount;
        // Declared frames inherit every unset field from the previous frame
        // (frame 0 inherits the all-placeholder default), matching the
        // "undeclared frames use the previous frame" authoring rule.
        std::size_t previous = 0;
        for (const Declared& item : declared) {
            const std::size_t at = static_cast<std::size_t>(item.index);
            for (std::size_t fill = previous; fill < at; ++fill)
                track.frames[fill] = track.frames[previous];
            RawTransform merged = track.frames[previous];
            for (int i = 0; i < 8; ++i)
                if (item.set[i]) {
                    merged.values[i] = item.value.values[i];
                    if (i == 6) merged.cel = item.value.cel;
                }
            if (item.setImage) merged.image = item.value.image;
            if (item.setFont) merged.font = item.value.font;
            if (item.setText) {
                merged.text = item.value.text;
                // Empty source text inherits the previous runtime pointer;
                // every explicit nonempty field allocates a distinct pointer,
                // including a repetition of the same spelling.
                merged.textSourceFrame = merged.text.empty() ? 0xFFFFFFFFu
                    : static_cast<std::uint32_t>(at);
            }
            track.frames[at] = std::move(merged);
            previous = at;
        }
        for (std::size_t fill = previous + 1; fill < track.frames.size(); ++fill)
            track.frames[fill] = track.frames[previous];
        module.tracks.push_back(std::move(track));
    }
    RawTransform ReadFrameBody(bool (&set)[8], bool& setImage, bool& setFont,
                               bool& setText)
    {
        static constexpr const char* names[8] = {"x", "y", "skew-x", "skew-y", "scale-x", "scale-y", "cel", "alpha"};
        RawTransform transform;
        while (!(lexer_.current().kind == TokenKind::brace && lexer_.current().text == "}")) {
            if (lexer_.current().kind != TokenKind::identifier)
                Fail(error_, "expected frame property");
            const std::string field = lexer_.current().text;
            lexer_.Advance();
            if (lexer_.current().kind != TokenKind::colon) Fail(error_, "expected ':'");
            lexer_.Advance();
            if (field == "image") {
                // 裸标识符 = 资源名本身（texture 占位层已取消）；
                // 引号字符串用于含空格等非法标识符字符的名字。none = 无图像。
                std::string value;
                if (lexer_.current().kind == TokenKind::identifier) {
                    value = lexer_.current().text;
                    lexer_.Advance();
                } else {
                    value = TakeString();
                }
                transform.image = value == "none" ? std::string{} : CanonicalResource(value);
                setImage = true;
            } else if (field == "font") {
                std::string value;
                if (lexer_.current().kind == TokenKind::identifier) {
                    value = lexer_.current().text;
                    lexer_.Advance();
                } else {
                    value = TakeString();
                }
                transform.font = value == "none" ? std::string{} : Trim(value);
                setFont = true;
            } else if (field == "text") {
                transform.text = TakeString();
                setText = true;
            } else {
                const auto found = std::find(names, names + 8, field);
                if (found == names + 8) Fail(error_, "unknown frame property <" + field + ">");
                const int index = static_cast<int>(found - names);
                try {
                    if (index == 6) {
                        transform.cel = ParseDecimalInteger(lexer_.current().text);
                        transform.values[index] = static_cast<float>(transform.cel);
                    } else transform.values[index] = ParseFiniteFloat(lexer_.current().text);
                } catch (const std::exception& e) {
                    Fail(error_, "line " + std::to_string(lexer_.current().line) +
                         ", " + field + ": " + e.what());
                }
                set[found - names] = true;
                lexer_.Advance();
            }
            ExpectSemicolon();
        }
        ExpectBrace("}");
        return transform;
    }

    Lexer lexer_;
    std::vector<RawReanim>& outputs_;
    std::string* error_;
};

void WriteEscaped(std::ostream& out, const std::string_view text)
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

std::string FloatText(const float value)
{
    if (!std::isfinite(value)) throw std::runtime_error("nonfinite reanim source value");
    std::ostringstream out;
    out << std::setprecision(std::numeric_limits<float>::max_digits10) << value;
    return out.str();
}

} // namespace

bool ParseModules(const std::string_view source, std::vector<RawReanim>& outputs,
                  std::string* error)
{
    try {
        outputs.clear();
        DslParser parser(source, outputs, error);
        parser.Run(nullptr);  // 纯字符串源：文件级指令在此报错
        if (outputs.empty()) Fail(error, "no module found");
        for (const RawReanim& module : outputs)
            if (module.tracks.size() > kMaxTracks)
                Fail(error, "track limit exceeded in <" + module.name + ">");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

bool ParseDslFileText(const std::string_view source, DslFileDirectives& file,
                      std::string* error)
{
    try {
        file = DslFileDirectives{};
        DslParser parser(source, file.modules, error);
        parser.Run(&file);
        for (const RawReanim& module : file.modules)
            if (module.tracks.size() > kMaxTracks)
                Fail(error, "track limit exceeded in <" + module.name + ">");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

void ResolveReanimVariants(std::vector<RawReanim>& corpus)
{
    std::map<std::string, std::size_t> names;
    std::set<std::string> paths;
    for (std::size_t i = 0; i < corpus.size(); ++i) {
        std::string folded = corpus[i].name;
        std::transform(folded.begin(), folded.end(), folded.begin(),
                       [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        if (!names.emplace(corpus[i].name, i).second || !paths.insert(folded).second)
            throw std::runtime_error("duplicate reanim module/source name: " + corpus[i].name);
    }
    std::vector<unsigned char> state(corpus.size());
    std::function<void(std::size_t)> visit = [&](std::size_t i) {
        auto& item = corpus[i];
        if (state[i] == 2) return;
        if (state[i] == 1) throw std::runtime_error("reanim variant cycle: " + item.name);
        state[i] = 1;
        if (item.variantOf.empty()) {
            item.variantRoot = item.name;
            item.variantImages.clear();
            for (const auto& track : item.tracks) {
                std::vector<std::string> images;
                for (const auto& frame : track.frames)
                    if (!frame.image.empty() && std::find(images.begin(), images.end(), frame.image) == images.end())
                        images.push_back(frame.image);
                item.variantImages.push_back(std::move(images));
            }
        } else {
            const auto parent = names.find(item.variantOf);
            if (parent == names.end()) throw std::runtime_error("missing reanim variant parent: " + item.variantOf);
            visit(parent->second);
            item.variantRoot = corpus[parent->second].variantRoot;
            const auto& root = corpus[names.at(item.variantRoot)];
            if (item.variantImages.size() != root.tracks.size())
                throw std::runtime_error("reanim variant requires one images list per root track: " + item.name);
            item.fps = root.fps;
            for (std::size_t t = 0; t < root.tracks.size(); ++t) {
                const auto& original = root.variantImages[t];
                if (item.variantImages[t].size() != original.size())
                    throw std::runtime_error("reanim variant image count differs from root track: " + item.name);
            }
        }
        state[i] = 2;
    };
    for (std::size_t i = 0; i < corpus.size(); ++i) visit(i);
}

std::string MakeReanimVariantMetadata(const std::vector<RawReanim>& corpus)
{
    std::ostringstream out;
    out << "{\"roots\":{";
    bool firstRoot = true;
    for (const auto& root : corpus) {
        if (!root.variantOf.empty()) continue;
        if (!firstRoot) out << ',';
        firstRoot = false;
        WriteEscaped(out, root.name);
        out << ":{\"tracks\":[";
        for (std::size_t t = 0; t < root.tracks.size(); ++t) {
            if (t) out << ',';
            WriteEscaped(out, root.tracks[t].name);
        }
        out << "],\"variants\":[";
        std::vector<const RawReanim*> variants{&root};
        for (const auto& item : corpus)
            if (!item.variantOf.empty() && item.variantRoot == root.name) variants.push_back(&item);
        std::sort(variants.begin() + 1, variants.end(), [](auto* a, auto* b) { return a->name < b->name; });
        for (std::size_t v = 0; v < variants.size(); ++v) {
            if (v) out << ',';
            const auto& item = *variants[v];
            out << "{\"name\":"; WriteEscaped(out, item.name);
            out << ",\"parent\":"; WriteEscaped(out, item.variantOf);
            out << ",\"index\":" << v << ",\"images\":[";
            for (std::size_t t = 0; t < item.variantImages.size(); ++t) {
                if (t) out << ',';
                out << '[';
                for (std::size_t s = 0; s < item.variantImages[t].size(); ++s) {
                    if (s) out << ',';
                    if (item.variantImages[t][s].empty()) out << "null";
                    else WriteEscaped(out, item.variantImages[t][s]);
                }
                out << ']';
            }
            out << "]}";
        }
        out << "]}";
    }
    out << "}}\n";
    return out.str();
}

std::string MakeReanimVariantHeader(const std::vector<RawReanim>& corpus)
{
    std::ostringstream out;
    out << "// Generated from reanim variant declarations.\n#pragma once\n#include <cstdint>\nnamespace ReanimVariants {\n";
    for (const auto& root : corpus) {
        if (!root.variantOf.empty()) continue;
        out << "enum class " << root.name << " : std::uint32_t { Default = 0";
        std::vector<std::string> names;
        for (const auto& item : corpus)
            if (!item.variantOf.empty() && item.variantRoot == root.name) names.push_back(item.name);
        std::sort(names.begin(), names.end());
        for (std::size_t i = 0; i < names.size(); ++i)
            out << ", " << names[i] << " = " << i + 1;
        out << " };\n";
    }
    out << "}\n";
    return out.str();
}

namespace {

class BundleLoader final {
public:
    explicit BundleLoader(DslBundle& theBundle) : bundle_(theBundle) {}

    // 返回文件索引；重复包含（菱形）直接复用已解析结果。
    std::size_t Load(const std::filesystem::path& path, std::string* error)
    {
        std::error_code ec;
        const std::filesystem::path canonical = std::filesystem::weakly_canonical(path, ec);
        const std::string key = ec ? path.string() : canonical.string();
        // 环检测必须在记忆化之前：正在栈上的文件（未完成）被再次包含是环；
        // 已完成（已出栈）的重复包含走记忆化。
        for (const std::string& active : stack_) {
            if (active == key) {
                std::string chain;
                for (const std::string& link : stack_) chain += link + " -> ";
                Fail(error, "circular include: " + chain + key);
            }
        }
        const auto memo = byPath_.find(key);
        if (memo != byPath_.end())
            return memo->second;
        std::ifstream in(path, std::ios::binary);
        if (!in) Fail(error, "cannot open dsl file " + path.string());
        std::ostringstream buffer;
        buffer << in.rdbuf();
        const std::size_t index = bundle_.files.size();
        bundle_.files.push_back(path);
        stack_.push_back(key);
        DslFileDirectives directives;
        if (!ParseDslFileText(buffer.str(), directives, error))
            Fail(error, (error && !error->empty() ? *error + " (in " + path.string() + ")"
                                                  : std::string("while parsing ") + path.string()));
        byPath_.emplace(key, index);
        files_.push_back(std::move(directives));
        // 自身仍在栈上时递归 includes：环 a->b->a 命中栈检测报错；
        // 菱形/重复包含命中 byPath_ 记忆化。forward/import 差别只在暴露集。
        for (const std::string& path : files_[index].forwards)
            Resolve(index, path, error);
        for (const std::string& path : files_[index].imports)
            Resolve(index, path, error);
        stack_.pop_back();
        return index;
    }

    // 暴露集：本文件 export + 其 forward 目标的公开集（传递，不含 import）。
    std::vector<std::string> PublicNamesOf(std::size_t index, std::string* error)
    {
        std::vector<std::string> names;
        CollectPublicNames(index, names, error);
        return names;
    }

    void CollectPublicNames(std::size_t index, std::vector<std::string>& names,
                            std::string* error)
    {
        for (const std::string& name : files_[index].publicNames)
            names.push_back(name);
        for (const std::string& forwardPath : files_[index].forwards) {
            const std::size_t child = Resolve(index, forwardPath, error);
            // Resolve 对已加载文件直接走记忆化；环已在加载期被拒绝，
            // 这里再防一层深层 DAG 的重复展开上限。
            if (visitedPublic_.insert(child).second)
                CollectPublicNames(child, names, error);
        }
    }

    std::set<std::size_t> visitedPublic_;

    // 相对路径基于包含文件所在目录。
    std::size_t Resolve(std::size_t index, const std::string& reference, std::string* error)
    {
        const std::filesystem::path base = bundle_.files[index].parent_path();
        return Load((base / reference).lexically_normal(), error);
    }

    DslBundle& bundle_;
    std::vector<DslFileDirectives> files_;
    std::map<std::string, std::size_t> byPath_;
    std::vector<std::string> stack_;
};

} // namespace

bool LoadDslBundle(const std::filesystem::path& rootFile, DslBundle& bundle,
                   std::string* error)
{
    try {
        bundle = DslBundle{};
        BundleLoader loader(bundle);
        const std::size_t root = loader.Load(rootFile, error);  // 递归加载整个闭包

        std::map<std::string, bool> seen;     // 名字 -> 是否公开
        for (const DslFileDirectives& file : loader.files_) {
            for (const RawReanim& module : file.modules) {
                if (seen.emplace(module.name, false).second)
                    bundle.modules.push_back(module);
                else
                    Fail(error, "duplicate module name <" + module.name + "> across bundle");
            }
            for (const std::string& prefix : file.prefixStrs) {
                if (std::find(bundle.prefixStrs.begin(), bundle.prefixStrs.end(), prefix) ==
                    bundle.prefixStrs.end())
                    bundle.prefixStrs.push_back(prefix);  // 首见序合并去重
            }
        }
        std::sort(bundle.modules.begin(), bundle.modules.end(),
                  [](const RawReanim& a, const RawReanim& b) { return a.name < b.name; });
        bundle.publicNames = loader.PublicNamesOf(root, error);
        std::sort(bundle.publicNames.begin(), bundle.publicNames.end());
        bundle.publicNames.erase(
            std::unique(bundle.publicNames.begin(), bundle.publicNames.end()),
            bundle.publicNames.end());
        if (bundle.modules.empty())
            Fail(error, "no module found in bundle");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

bool ParseDsl(const std::string_view source, RawReanim& output, std::string* error)
{
    std::vector<RawReanim> modules;
    if (!ParseModules(source, modules, error))
        return false;
    if (modules.size() != 1) {
        if (error) *error = "expected exactly one module, found " + std::to_string(modules.size());
        return false;
    }
    output = std::move(modules.front());
    return true;
}

namespace {

// 资源名是否可以裸写在 image: 位置（合法标识符且非保留字 none）。
bool IsBareIdentifierName(const std::string& text)
{
    if (text.empty() || text == "none")
        return false;
    if (!std::isalpha(static_cast<unsigned char>(text[0])) && text[0] != '_')
        return false;
    for (const char c : text)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_')
            return false;
    return true;
}

void RequireLegacyStorage(const RawReanim& source)
{
    for (const RawTrack& track : source.tracks)
        if (track.storage != TrackStorage::both)
            Fail(nullptr, "legacy reanim records cannot represent track storage; use compile-v2 with --font-registry");
}

std::uint32_t StorageBits(const TrackStorage storage)
{
    switch (storage) {
    case TrackStorage::both:
    case TrackStorage::cpu:
    case TrackStorage::gpu:
        return static_cast<std::uint32_t>(storage) << kTrackStorageShift;
    }
    Fail(nullptr, "invalid reanim track storage");
}

} // namespace

std::string ToDsl(const RawReanim& source)
{
    static constexpr const char* names[8] = {"x", "y", "skew-x", "skew-y", "scale-x", "scale-y", "cel", "alpha"};
    std::ostringstream out;
    out << "export component "
        << (source.name.empty() ? std::string("Reanim") : source.name)
        << " {\n";
    out << "    fps: " << FloatText(source.fps) << ";\n";
    TrackStorage storage = TrackStorage::both;
    for (const RawTrack& track : source.tracks) {
        if (track.storage != storage) {
            (void)StorageBits(track.storage);
            out << "    " << (track.storage == TrackStorage::cpu ? "cpu" :
                track.storage == TrackStorage::gpu ? "gpu" : "both") << ":\n";
            storage = track.storage;
        }
        out << "    Track {\n        source-name: ";
        WriteEscaped(out, track.name);
        out << ";\n        frame-count: " << track.frames.size() << ";\n";
        RawTransform previous;
        for (std::size_t index = 0; index < track.frames.size(); ++index) {
            const RawTransform& frame = track.frames[index];
            std::ostringstream body;
            for (int i = 0; i < 8; ++i)
                if (std::memcmp(&frame.values[i], &previous.values[i], sizeof(float)) != 0)
                    body << names[i] << ": " << FloatText(frame.values[i]) << "; ";
            if (frame.image != previous.image) {
                body << "image: ";
                if (frame.image.empty())
                    body << "none";
                else if (IsBareIdentifierName(frame.image))
                    body << frame.image;  // 裸资源名（texture 占位层已取消）
                else
                    WriteEscaped(body, frame.image);
                body << "; ";
            }
            if (frame.font != previous.font) {
                body << "font: ";
                if (frame.font.empty()) body << "none"; else WriteEscaped(body, frame.font);
                body << "; ";
            }
            const bool startsTextSource = !frame.text.empty() &&
                (frame.textSourceFrame == 0xFFFFFFFFu || frame.textSourceFrame == index);
            if (frame.text != previous.text || startsTextSource) {
                body << "text: ";
                WriteEscaped(body, frame.text);
                body << "; ";
            }
            if (!body.str().empty()) {
                out << "        frame " << index << " { " << body.str() << "}\n";
                previous = frame;
            }
        }
        out << "    }\n";
    }
    out << "}\n";
    return out.str();
}

// ------------------------------------------------------------- serialization

std::vector<std::uint8_t> Serialize(const RawReanim& source)
{
    RequireLegacyStorage(source);
    std::vector<std::uint8_t> payload;
    PutU32(payload, kReanimSchemaHash);

    // ReanimatorDefinition dump, 32 bytes, x64 layout, pointer slots zero.
    payload.resize(payload.size() + 32, 0);
    const std::uint32_t trackCount = static_cast<std::uint32_t>(source.tracks.size());
    for (unsigned i = 0; i < 4; ++i)
        payload[4 + 8 + i] = static_cast<std::uint8_t>(trackCount >> (i * 8));
    std::uint32_t fpsBits = 0;
    std::memcpy(&fpsBits, &source.fps, 4);
    for (unsigned i = 0; i < 4; ++i)
        payload[4 + 16 + i] = static_cast<std::uint8_t>(fpsBits >> (i * 8));

    PutU32(payload, 24);  // ReanimatorTrack native element size
    for (const RawTrack& track : source.tracks) {
        const std::size_t base = payload.size();
        payload.resize(base + 24, 0);
        const std::uint32_t count = static_cast<std::uint32_t>(track.frames.size());
        for (unsigned i = 0; i < 4; ++i)
            payload[base + 16 + i] = static_cast<std::uint8_t>(count >> (i * 8));
    }
    for (const RawTrack& track : source.tracks) {
        PutString(payload, track.name);
        PutU32(payload, static_cast<std::uint32_t>(track.frames.size()));
        PutU32(payload, 56);  // ReanimatorTransform native element size
        for (const RawTransform& frame : track.frames) {
            for (int i = 0; i < 8; ++i) PutF32(payload, frame.values[i]);
            payload.resize(payload.size() + 24, 0);  // image/font/text pointer slots
        }
        for (const RawTransform& frame : track.frames) {
            PutString(payload, frame.image);
            PutString(payload, frame.font);
            PutString(payload, frame.text);
        }
    }

    // Wrapper: cookie + inflated size + zlib header + stored deflate blocks +
    // little-endian adler32, byte-identical to the installed pak64 records.
    std::vector<std::uint8_t> out;
    PutU32(out, kCookie);
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    out.push_back(0x78);
    out.push_back(0x01);
    std::size_t position = 0;
    do {
        const std::size_t chunk = std::min<std::size_t>(payload.size() - position, 65535);
        const bool last = position + chunk >= payload.size();
        out.push_back(last ? 0x01 : 0x00);
        const std::uint16_t length = static_cast<std::uint16_t>(chunk);
        const std::uint16_t negated = static_cast<std::uint16_t>(~length);
        out.push_back(static_cast<std::uint8_t>(length & 0xFF));
        out.push_back(static_cast<std::uint8_t>((length >> 8) & 0xFF));
        out.push_back(static_cast<std::uint8_t>(negated & 0xFF));
        out.push_back(static_cast<std::uint8_t>((negated >> 8) & 0xFF));
        out.insert(out.end(), payload.begin() + static_cast<std::ptrdiff_t>(position),
                   payload.begin() + static_cast<std::ptrdiff_t>(position + chunk));
        position += chunk;
    } while (position < payload.size());
    PutU32(out, Adler32(payload));
    return out;
}

bool WriteCompiled(const RawReanim& source, const std::filesystem::path& output,
                   std::string* error)
{
    try {
        const std::vector<std::uint8_t> bytes = Serialize(source);
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

bool ReadCompiled(const std::filesystem::path& input, RawReanim& output,
                  std::string* error)
{
    try {
        std::ifstream in(input, std::ios::binary);
        if (!in) Fail(error, "cannot open " + input.string());
        const std::vector<std::uint8_t> file(std::istreambuf_iterator<char>(in), {});
        return ReadCompiledFromBytes(file, output, error);
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

bool ReadCompiledFromBytes(const std::vector<std::uint8_t>& file, RawReanim& output,
                           std::string* error)
{
    try {
        if (file.size() < 14) Fail(error, "compiled record too small");
        std::uint32_t cookie = 0, declared = 0;
        std::memcpy(&cookie, file.data(), 4);
        std::memcpy(&declared, file.data() + 4, 4);
        if (cookie != kCookie) Fail(error, "unexpected cookie");
        if ((file[8] & 0x0F) != 8)   // CMF 低 4 位 = CM，8 = deflate
            Fail(error, "missing zlib header bytes");

        // 通用 zlib 流（header 78 xx + deflate 任意块型 + adler32 尾）。
        // 自家 WriteCompiled 产出存储块（78 01）；原版 1073 记录是动态
        // Huffman（78 9c）——统一交给 zlib inflate，adler 由 zlib 校验。
        std::vector<std::uint8_t> payload(declared);
        uLongf inflated_length = declared;
        if (const uLong inflate_code = uncompress(
                payload.data(), &inflated_length, file.data() + 8,
                static_cast<uLong>(file.size() - 8));
            inflate_code != Z_OK || inflated_length != declared)
            Fail(error, "zlib inflate failed");

        // Payload walk mirrors Serialize. The definition dump occupies
        // payload[4..35]: trackCount at byte 12, fps at byte 20.
        std::size_t at = 0;
        std::uint32_t hash = 0;
        std::memcpy(&hash, payload.data(), 4);
        if (hash != kReanimSchemaHash) Fail(error, "schema hash mismatch");
        output = RawReanim{};
        std::uint32_t trackCount = 0;
        std::memcpy(&trackCount, payload.data() + 12, 4);
        float fps = 0.0f;
        std::memcpy(&fps, payload.data() + 20, 4);
        output.fps = fps;
        at = 36;
        std::uint32_t trackElement = 0;
        std::memcpy(&trackElement, payload.data() + at, 4); at += 4;
        if (trackElement != 24) Fail(error, "unexpected track element size");
        const auto takeString = [&](std::string& value) {
            std::uint32_t length = 0;
            std::memcpy(&length, payload.data() + at, 4); at += 4;
            value.assign(reinterpret_cast<const char*>(payload.data() + at), length);
            at += length;
        };
        at += static_cast<std::size_t>(trackCount) * 24;
        for (std::uint32_t t = 0; t < trackCount; ++t) {
            RawTrack track;
            takeString(track.name);
            std::uint32_t frameCount = 0, transformElement = 0;
            std::memcpy(&frameCount, payload.data() + at, 4); at += 4;
            std::memcpy(&transformElement, payload.data() + at, 4); at += 4;
            if (transformElement != 56) Fail(error, "unexpected transform element size");
            for (std::uint32_t f = 0; f < frameCount; ++f) {
                RawTransform frame;
                for (int i = 0; i < 8; ++i) {
                    std::memcpy(&frame.values[i], payload.data() + at, 4); at += 4;
                }
                at += 24;
                track.frames.push_back(std::move(frame));
            }
            for (auto& frame : track.frames) {
                takeString(frame.image);
                takeString(frame.font);
                takeString(frame.text);
            }
            output.tracks.push_back(std::move(track));
        }
        if (at != payload.size()) Fail(error, "trailing payload bytes");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

// ------------------------------------------------------- v2: 编译期句柄内联

namespace {

std::string Folded(const std::string_view text)
{
    std::string folded;
    folded.reserve(text.size());
    for (const char c : text)
        folded += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    return folded;
}

std::uint32_t Crc32(const std::uint8_t* data, const std::size_t size)
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

std::uint32_t Crc32(const std::vector<std::uint8_t>& bytes)
{
    return Crc32(bytes.data(), bytes.size());
}

std::string SanitizedIdentifier(std::string name)
{
    for (char& c : name)
        if (!std::isalnum(static_cast<unsigned char>(c)) && c != '_') c = '_';
    const bool needsPrefix = name.empty() ||
        (!std::isalpha(static_cast<unsigned char>(name[0])) && name[0] != '_');
    if (needsPrefix) name.insert(name.begin(), '_');
    return name;
}

void WriteCIdentifier(std::ostream& out, const std::string& text)
{
    for (const char c : text) {
        if (c == '"' || c == '\\') out << '\\' << c;
        else if (c == '\n') out << "\\n";
        else out << c;
    }
}

} // namespace

std::vector<TrackIdEntry> BuildTrackIdTable(const std::vector<RawReanim>& corpus)
{
    struct Spelling final {
        std::uint32_t count = 0;
        std::uint32_t firstSeen = 0;
    };
    // folded -> (拼写 -> {次数, 首见序})。代表拼写：最高频，并列取首见。
    std::map<std::string, std::map<std::string, Spelling>> byFolded;
    std::uint32_t seen = 0;
    for (const RawReanim& module : corpus)
        for (const RawTrack& track : module.tracks) {
            if (track.name.empty()) continue;
            Spelling& spelling = byFolded[Folded(track.name)][track.name];
            if (spelling.count == 0) spelling.firstSeen = seen;
            ++spelling.count;
            ++seen;
        }

    std::vector<TrackIdEntry> table;
    table.reserve(byFolded.size());
    for (const auto& [key, perName] : byFolded) {
        TrackIdEntry entry;
        entry.folded = key;
        std::uint32_t bestCount = 0, bestFirst = 0xFFFFFFFFu;
        for (const auto& [name, spelling] : perName) {
            entry.count += spelling.count;
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
              [](const TrackIdEntry& a, const TrackIdEntry& b) { return a.folded < b.folded; });
    for (std::size_t i = 0; i < table.size(); ++i)
        table[i].id = static_cast<std::uint32_t>(i + 1);
    return table;
}

// ---- 编译期 attacher 链接 ----

// 复刻运行时 ParseAttacherTrack 的解析（strstr 语义逐字符等价）。
struct AttacherDecoded final
{
    bool blank = true;   // 空帧（mFrame == -10000）：无目标
    bool hasTarget = false;
    std::string module;
    std::string action;
    float rate = 12.0f;
    std::uint32_t loop = 0;  // 0=Loop 1=PlayOnceAndHold 2=PlayOnce
};

AttacherDecoded DecodeAttacherText(const std::string_view text, bool blankFrame)
{
    AttacherDecoded d;
    if (blankFrame)  // 空白帧：运行时 ParseAttacherTrack 直接返回默认值
        return d;
    d.blank = false;
    const std::size_t p1 = text.find("__");
    if (p1 == std::string_view::npos)
        return d;
    d.hasTarget = true;
    const std::size_t tagsAt = text.find('[', p1 + 2);
    const std::size_t trackAt = text.find("__", p1 + 2);
    if (tagsAt != std::string_view::npos && trackAt != std::string_view::npos &&
        tagsAt < trackAt)
        return d;  // 运行时同款非法判定
    if (trackAt != std::string_view::npos)
    {
        d.module.assign(text.substr(p1 + 2, trackAt - p1 - 2));
        const std::size_t actionEnd =
            tagsAt != std::string_view::npos ? tagsAt : text.size();
        d.action.assign(text.substr(trackAt + 2, actionEnd - trackAt - 2));
    }
    else if (tagsAt != std::string_view::npos)
    {
        d.module.assign(text.substr(p1 + 2, tagsAt - p1 - 2));
    }
    else
    {
        d.module.assign(text.substr(p1 + 2));
    }
    std::size_t at = tagsAt;
    while (at != std::string_view::npos)
    {
        const std::size_t end = text.find(']', at + 1);
        if (end == std::string_view::npos)
            break;
        const std::string code(text.substr(at + 1, end - at - 1));
        if (code == "hold")
            d.loop = 1;
        else if (code == "once")
            d.loop = 2;
        else {
            d.rate = ParseFiniteFloat(code);
        }
        at = text.find('[', end + 1);
    }
    return d;
}

std::uint32_t TrackIdOf(const std::vector<TrackIdEntry>& table, const std::string_view name)
{
    const std::string folded = Folded(name);
    const auto it = std::lower_bound(
        table.begin(), table.end(), folded,
        [](const TrackIdEntry& entry, const std::string& key) { return entry.folded < key; });
    if (it == table.end() || it->folded != folded) return 0;
    return it->id;
}

std::vector<std::uint8_t> SerializeTrackIdTable(const std::vector<TrackIdEntry>& table)
{
    std::vector<std::uint8_t> out;
    PutU32(out, kV2TableMagic);
    PutU32(out, static_cast<std::uint32_t>(table.size()));
    for (const TrackIdEntry& entry : table) {  // id 升序 = folded 升序
        PutU32(out, entry.id);
        PutString(out, entry.folded);
        PutString(out, entry.display);
        PutU32(out, entry.count);
    }
    return out;
}

std::uint32_t TrackIdTableHash(const std::vector<TrackIdEntry>& table)
{
    // The compiler hashes the offline mapping; generated runtime headers carry
    // only this numeric checksum, never the source folded/display strings.
    std::vector<std::uint8_t> mapping;
    for (const TrackIdEntry& entry : table) {
        PutU32(mapping, entry.id);
        PutString(mapping, entry.folded);
        PutString(mapping, entry.display);
    }
    return Crc32(mapping);
}

bool ReadTrackIdTableBytes(const std::vector<std::uint8_t>& bytes,
                           std::vector<TrackIdEntry>& table, std::string* error)
{
    try {
        table.clear();
        std::size_t at = 0;
        const auto take32 = [&](std::uint32_t& value) {
            if (at + 4 > bytes.size()) Fail(error, "truncated track-id table");
            std::memcpy(&value, bytes.data() + at, 4);
            at += 4;
        };
        std::uint32_t magic = 0, count = 0;
        take32(magic);
        take32(count);
        if (magic != kV2TableMagic) Fail(error, "unexpected track-id table magic");
        if (count > kMaxTracks) Fail(error, "track-id table too large");
        for (std::uint32_t i = 0; i < count; ++i) {
            TrackIdEntry entry;
            take32(entry.id);
            if (entry.id != i + 1) Fail(error, "track-id table ids must ascend from 1");
            const auto takeString = [&](std::string& value) {
                std::uint32_t length = 0;
                take32(length);
                if (at + length > bytes.size()) Fail(error, "truncated track-id table string");
                value.assign(reinterpret_cast<const char*>(bytes.data() + at), length);
                at += length;
            };
            takeString(entry.folded);
            takeString(entry.display);
            take32(entry.count);
            if (entry.folded.empty()) Fail(error, "empty folded key in track-id table");
            table.push_back(std::move(entry));
        }
        if (at != bytes.size()) Fail(error, "trailing track-id table bytes");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

bool ReadTrackIdTableFile(const std::filesystem::path& input,
                          std::vector<TrackIdEntry>& table, std::string* error)
{
    try {
        std::ifstream in(input, std::ios::binary);
        if (!in) Fail(error, "cannot open " + input.string());
        const std::vector<std::uint8_t> bytes(std::istreambuf_iterator<char>(in), {});
        return ReadTrackIdTableBytes(bytes, table, error);
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

bool WriteTrackIdTableFile(const std::vector<TrackIdEntry>& table,
                           const std::filesystem::path& output, std::string* error)
{
    try {
        const std::vector<std::uint8_t> bytes = SerializeTrackIdTable(table);
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

bool LoadReanimTypeMap(const std::filesystem::path& input,
                       std::vector<ReanimTypeMapEntry>& map, std::string* error)
{
    try {
        map.clear();
        std::ifstream in(input);
        if (!in) Fail(error, "cannot open type map " + input.string());
        std::string line;
        while (std::getline(in, line)) {
                        while (!line.empty() && (line.back() == '\r' || line.back() == '\n'))
                line.pop_back();
                        const std::size_t first = line.find(' ');
            if (first == std::string::npos) Fail(error, "bad type map line: " + line);
            ReanimTypeMapEntry entry;
            entry.folded = line.substr(0, first);
            entry.value = static_cast<std::uint32_t>(std::strtoul(line.c_str() + first + 1, nullptr, 10));
map.push_back(std::move(entry));
        }
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

struct SourceTextRunBounds final
{
    std::uint32_t start = 0;
    std::uint32_t end = 0;
};

std::vector<SourceTextRunBounds> SourceTextRuns(const RawTrack& track)
{
    std::vector<SourceTextRunBounds> bounds(track.frames.size());
    std::uint32_t identity = 0xFFFFFFFFu;
    std::uint32_t start = 0;
    for (std::size_t f = 0; f < track.frames.size(); ++f) {
        const RawTransform& frame = track.frames[f];
        const std::uint32_t next = frame.text.empty() ? identity
            : frame.textSourceFrame == 0xFFFFFFFFu ? static_cast<std::uint32_t>(f)
            : frame.textSourceFrame;
        if (next != identity) {
            for (std::uint32_t i = start; i < f; ++i)
                bounds[i] = {start, static_cast<std::uint32_t>(f)};
            start = static_cast<std::uint32_t>(f);
            identity = next;
        }
    }
    for (std::size_t i = start; i < bounds.size(); ++i)
        bounds[i] = {start, static_cast<std::uint32_t>(bounds.size())};
    return bounds;
}

// A text source run is UI only if some inherited frame actually draws it as
// a caption. Keep sparse caption fields unchanged; all routing-only tracks
// have mode 0 and therefore no text-length slots at all.
bool HasUiText(const RawTrack& track)
{
    if (Folded(track.name).rfind("attacher__", 0) == 0) return false;
    const auto runs = SourceTextRuns(track);
    std::vector<bool> captionRun(track.frames.size(), false);
    std::string text, font, image;
    float cel = 0.0f, alpha = 1.0f;
    bool hasCaption = false;
    for (std::size_t f = 0; f < track.frames.size(); ++f) {
        const RawTransform& frame = track.frames[f];
        if (!frame.text.empty()) text = frame.text;
        if (!frame.font.empty()) font = frame.font;
        if (!frame.image.empty()) image = frame.image;
        if (frame.values[6] != kPlaceholder) cel = frame.values[6];
        if (frame.values[7] != kPlaceholder) alpha = frame.values[7];
        if (!text.empty() && !font.empty() && image.empty() && cel != -1.0f && alpha > 0.0f) {
            if (text.rfind("attacher__", 0) == 0)
                Fail(nullptr, "routing text used as a UI caption in track " + track.name);
            captionRun[runs[f].start] = true;
            hasCaption = true;
        }
    }
    if (hasCaption)
        for (std::size_t f = 0; f < track.frames.size(); ++f)
            if (!track.frames[f].text.empty() && !captionRun[runs[f].start])
                Fail(nullptr, "mixed UI and non-UI source text in track " + track.name);
    return hasCaption;
}

void DeriveAttacherSegments(RawReanim& source,
                            const std::vector<TrackIdEntry>& table,
                            const std::vector<ReanimTypeMapEntry>& typeMap,
                            bool preserveSourceRuns = false)
{
        // REANIM_NONE = (unsigned int)-1（ConstEnums.h）。0 号是 REANIM_LOADBAR_SPROUT，
        // 写 0 会让运行时给 die 区间真的挂上读条芽。
        constexpr std::uint32_t kAttacherNone = 0xFFFFFFFFu;
        const auto typeOf = [&](const std::string& mod) -> std::uint32_t {
            const std::string folded = Folded(mod);
            for (const ReanimTypeMapEntry& entry : typeMap)
                if (entry.folded == folded)
                    return entry.value;
            return kAttacherNone;  // 不在映射表（如 zombie9）：REANIM_NONE，运行时 AttachmentDie，与原版一致
        };

    for (RawTrack& track : source.tracks) {
        track.attacherSegments.clear();
        if (Folded(track.name).rfind("attacher__", 0) != 0)
            continue;  // 家族门：与运行时 UpdateAttacherTrack 的 Contains 判断一致

        // 继承复刻（ReanimationFillTrackPlaceholders 逐帧填充语义）：
        //   空 text → 取前一帧 text（首帧前 ""）；f 占位(-10000) → 取前一帧 f（首帧前 0=可见）。
        //   仅显式 f==-1 为隐藏帧 → 原版 ParseAttacherTrack 返回默认 → AttachmentDie。
        // die 区间显式产出 targetType=REANIM_NONE 段：段表全覆盖时间线，运行时不再需要
        // "空窗=继承"的猜测语义（空窗只可能来自 bug）。
        // 2026-10-06 复审回退：原始帧值判定版（占位不传染 -1、单拍 NONE）与运行时
        // 段表整数取帧组合后，在 credits 终场把 125/217/300 眨拍变成"杀旧挂新"，
        // 旧子动画未即逝→歌手/气泡/红字重影（比大军缺失更糟，用户裁决回退）。
        // 注意：不做闪烁合并——隐藏帧上的 kill/respawn 循环是 1073 原生编舞
        // （credits 开场 one two three four 每个数一个僵尸在不同挂点弹出），
        // 合并保活会把离散弹出变成单个僵尸平移。
        const auto sourceRuns = preserveSourceRuns ? SourceTextRuns(track)
            : std::vector<SourceTextRunBounds>{};
        std::string prevText;
        float prevFrame = 0.0f;
        bool runActive = false;
        AttacherDecoded run;
        std::uint32_t runStart = 0;
        const auto flush = [&](std::uint32_t endExclusive) {
            if (!runActive) return;
            TrackAttacherSegment seg;
            seg.startFrame = runStart;
            seg.endFrame = endExclusive;
            if (preserveSourceRuns) {
                seg.runStart = sourceRuns[runStart].start;
                seg.runEnd = sourceRuns[runStart].end;
            }
            seg.rate = run.rate;
            seg.loop = run.loop;
            if (run.blank || run.module.empty()) {
                // die：隐藏帧 / 无 "__" 目标编码 / 模块名缺映射——全部对齐原版"解析默认→die"
                seg.targetType = kAttacherNone;
                seg.actionTrackId = 0;
            } else {
                seg.targetType = typeOf(run.module);
                if (run.action.empty()) {
                    seg.actionTrackId = 0;
                } else {
                    seg.actionTrackId = TrackIdOf(table, run.action);
                    if (seg.actionTrackId == 0)
                        Fail(nullptr, "attacher action name missing from track table: " + run.action);
                }
            }
            track.attacherSegments.push_back(seg);
        };
        for (std::size_t f = 0; f < track.frames.size(); ++f) {
            const float rawFrame = track.frames[f].values[6];
            const float frameVal = rawFrame == kPlaceholder ? prevFrame : rawFrame;
            prevFrame = frameVal;
            const std::string& rawText = track.frames[f].text;
            const std::string effective = rawText.empty() ? prevText : rawText;
            if (!rawText.empty()) prevText = rawText;
            const bool hiddenFrame = frameVal == -1.0f;
            const AttacherDecoded d = DecodeAttacherText(effective, hiddenFrame);
            const bool same = runActive && d.module == run.module && d.action == run.action &&
                              d.rate == run.rate && d.loop == run.loop && d.blank == run.blank &&
                              (!preserveSourceRuns || sourceRuns[f].start == sourceRuns[runStart].start);
            if (!same) {
                flush(static_cast<std::uint32_t>(f));
                runActive = true;
                runStart = static_cast<std::uint32_t>(f);
                run = d;
            }
        }
        flush(static_cast<std::uint32_t>(track.frames.size()));
    }
}

std::vector<std::string> BuildModuleTable(const std::vector<RawReanim>& corpus)
{
    std::vector<std::string> names;
    for (const RawReanim& module : corpus)
        names.push_back(module.name);
    std::sort(names.begin(), names.end());
    names.erase(std::unique(names.begin(), names.end()), names.end());
    return names;
}

// ---------------------------------------------------- 'MPHF'：id -> trackIndex
//
// 每类型 MPHF（ReanimTrackMphf.h / kTrackMphf 函数指针数组）已退役：全局
// 单表 MPHF（下方 TrackMphfGlobal 体系）成为唯一 MPHF 层，运行时不再有
// 每类型函数指针分派与 nullptr 槽位；旧 int16 直接寻址表仅按需兜底构建
//（语料外类型 / PVZ_MPHF_VERIFY 对拍闸 / PVZ_MPHF=0 基线）。

// --------------------------------- 全局单表 MPHF：(type,id) 联合键 -> trackIndex

int MphfGlobalLookup(const TrackMphfGlobal& mphf, std::uint32_t type,
                     std::uint32_t id)
{
    // 与生成函数（MakeTrackMphfGlobalHeader 函数体）、运行时调用点
    //（Reanimator.cpp ReanimatorTrackIndexOf）逐运算一致：同联合键打包、
    // 同乘法常数、同 xorshift（对象是乘积）、同掩码顺序。改这里必须三方同步。
    const std::uint64_t key = (static_cast<std::uint64_t>(type) << 32) | id;
    const std::uint64_t mixed1 = key * kMphfGlobalC1;
    const std::uint32_t bucket = static_cast<std::uint32_t>(
        (mixed1 ^ (mixed1 >> kMphfGlobalS1)) & (mphf.buckets - 1));
    const std::uint16_t seed = mphf.displacement[bucket];
    const std::uint64_t mixed2 = key * (kMphfGlobalC2 + seed);
    const std::uint32_t slot = static_cast<std::uint32_t>(
        (mixed2 ^ (mixed2 >> kMphfGlobalS2)) & (mphf.slotCount - 1));
    const TrackMphfGlobalSlot& entry = mphf.slots[slot];
    return entry.key == key ? static_cast<int>(entry.trackIndex) : -1;
}

TrackMphfGlobal BuildTrackMphfGlobal(const std::vector<RawReanim>& corpus,
                                     const std::vector<TrackIdEntry>& table,
                                     const std::vector<ReanimTypeMapEntry>& typeMap)
{
    // 折叠模块名 -> ReanimationType 数值（与 MakeTrackMphfHeader 同源同表）。
    const auto typeOf = [&](const std::string& module) -> std::uint32_t {
        const std::string folded = Folded(module);
        for (const ReanimTypeMapEntry& entry : typeMap)
            if (entry.folded == folded)
                return entry.value == 0xFFFFFFFFu ? 0xFFFFFFFFu : entry.value;
        return 0xFFFFFFFFu;
    };

    // 键集：全语料每类型 (type,id) -> 首见轨道下标（std::map 幂等插入 = 首见
    // 优先，与每类型 MPHF、运行时直接寻址表同语义）。联合键按数值升序遍历，
    // 求解确定性（重跑字节一致）。
    std::map<std::uint64_t, std::uint16_t> firstIndexOf;
    std::map<std::uint32_t, std::map<std::uint32_t, std::uint16_t>> perType;
    std::uint32_t maxType = 0;
    for (const RawReanim& module : corpus) {
        const std::uint32_t type = typeOf(module.name);
        if (type == 0xFFFFFFFFu) continue;  // 不在类型表：无 ReanimationType 下标
        maxType = std::max(maxType, type);
        for (std::size_t i = 0; i < module.tracks.size(); ++i) {
            const std::uint32_t id = TrackIdOf(table, module.tracks[i].name);
            if (id == 0) continue;
            firstIndexOf.emplace((static_cast<std::uint64_t>(type) << 32) | id,
                                 static_cast<std::uint16_t>(i));
            perType[type].emplace(id, static_cast<std::uint16_t>(i));
        }
    }

    TrackMphfGlobal mphf;
    mphf.count = static_cast<std::uint32_t>(firstIndexOf.size());
    if (mphf.count == 0)
        return mphf;  // 无有效键：调用方 Fail（全局表必须非空）

    // 语料成员位图：bit t = 类型 t 编入全局键集（perType 非空）。随头发射为
    // kGlobalTypeBits + MphfGlobalTypeInCorpus，运行时对拍闸的 -1 一致性判据
    // 与旧直接寻址表的按需构建判据都以它为准（取代已退役的 kTrackMphf 槽位
    // nullptr 判据）。
    mphf.maxType = maxType;
    mphf.typeBits.assign(maxType / 64 + 1, 0);
    for (const auto& [type, ids] : perType) {
        (void)ids;
        mphf.typeBits[type >> 6] |= 1ull << (type & 63);
    }

    // 槽数：最小 2 的幂 >= count；差额为 padding 条目 {key=0,index=0}
    //（id 1 基且 type>=0，真实键恒非 0，padding 天然不命中）。
    mphf.slotCount = 1;
    while (mphf.slotCount < mphf.count) mphf.slotCount *= 2;

    // bucketCount 候选：2 的幂，从 ~count/2 起步翻倍；到 2*slotCount（平均桶
    // 宽 <1）仍失败即 Fail。求解纪律与每类型一致：桶序 = 大小降序（并列桶号
    // 升序），逐桶暴力搜 r（u16 种子 0..65535 递增），要求桶内全部键按掩码
    // 探到互不冲突的全局空闲槽。
    std::uint32_t firstBuckets = 1;
    while (firstBuckets < mphf.count / 2) firstBuckets *= 2;
    bool placed = false;
    for (std::uint32_t bucketCount = firstBuckets;
         bucketCount <= mphf.slotCount * 2 && !placed; bucketCount *= 2) {
        std::vector<std::vector<std::pair<std::uint64_t, std::uint16_t>>> buckets(bucketCount);
        for (const auto& [key, index] : firstIndexOf) {
            const std::uint64_t mixed = key * kMphfGlobalC1;
            buckets[static_cast<std::uint32_t>(
                (mixed ^ (mixed >> kMphfGlobalS1)) & (bucketCount - 1))]
                .emplace_back(key, index);
        }
        std::vector<std::uint32_t> order(bucketCount);
        for (std::uint32_t b = 0; b < bucketCount; ++b) order[b] = b;
        std::sort(order.begin(), order.end(), [&](std::uint32_t a, std::uint32_t b) {
            if (buckets[a].size() != buckets[b].size())
                return buckets[a].size() > buckets[b].size();
            return a < b;
        });

        std::vector<std::int32_t> slotOwner(mphf.slotCount, -1);
        mphf.displacement.assign(bucketCount, 0);
        mphf.slots.assign(mphf.slotCount, TrackMphfGlobalSlot{});
        placed = true;
        for (const std::uint32_t b : order) {
            const auto& keys = buckets[b];
            bool found = false;
            std::vector<std::uint32_t> probed(keys.size());
            for (std::uint32_t r = 0; r <= 0xFFFF; ++r) {
                bool distinct = true;
                for (std::size_t k = 0; k < keys.size(); ++k) {
                    const std::uint64_t key = keys[k].first;
                    const std::uint64_t mixed = key * (kMphfGlobalC2 + r);
                    const std::uint32_t slot = static_cast<std::uint32_t>(
                        (mixed ^ (mixed >> kMphfGlobalS2)) & (mphf.slotCount - 1));
                    if (slotOwner[slot] != -1) { distinct = false; break; }
                    for (std::size_t j = 0; j < k; ++j)
                        if (probed[j] == slot) { distinct = false; break; }
                    if (!distinct) break;
                    probed[k] = slot;
                }
                if (!distinct) continue;
                for (std::size_t k = 0; k < keys.size(); ++k) {
                    slotOwner[probed[k]] = static_cast<std::int32_t>(b);
                    mphf.slots[probed[k]] =
                        TrackMphfGlobalSlot{keys[k].first, keys[k].second};
                }
                mphf.displacement[b] = static_cast<std::uint16_t>(r);
                found = true;
                break;
            }
            if (!found) { placed = false; break; }
        }
        if (!placed) continue;
        mphf.buckets = bucketCount;
    }
    if (!placed)
        Fail(nullptr, "mphf-global: no displacement found (keys=" +
                          std::to_string(mphf.count) + ")");

    // 穷举自校验：type 0..maxType × id 1..词表大小，查找结果必须与直接寻址
    // 表语义（该类型首见轨道下标 / 其余 -1）逐 (type,id) 一致，不一致 Fail。
    // 直接寻址语义即唯一基准（每类型 MPHF 已退役，不再另作平行回验）。
    for (std::uint32_t type = 0; type <= maxType; ++type) {
        static const std::map<std::uint32_t, std::uint16_t> kNoKeys;
        const auto it = perType.find(type);
        const std::map<std::uint32_t, std::uint16_t>& ids =
            it == perType.end() ? kNoKeys : it->second;
        for (std::uint32_t id = 1; id <= table.size(); ++id) {
            const auto key = ids.find(id);
            const int expected = key == ids.end() ? -1 : static_cast<int>(key->second);
            if (MphfGlobalLookup(mphf, type, id) != expected)
                Fail(nullptr, "mphf-global self-check mismatch at type=" +
                                  std::to_string(type) + " id=" + std::to_string(id));
        }
    }
    return mphf;
}

std::string MakeTrackMphfGlobalHeader(const std::vector<RawReanim>& corpus,
                                      const std::vector<TrackIdEntry>& table,
                                      const std::vector<ReanimTypeMapEntry>& typeMap)
{
    // 求解（含穷举 (type,id) 自校验，不一致抛错）。
    const TrackMphfGlobal mphf = BuildTrackMphfGlobal(corpus, table, typeMap);
    if (mphf.count == 0)
        Fail(nullptr, "mphf-global: empty key set (corpus has no typed tracks)");

    std::ostringstream out;
    out << "// Generated by reanimc compile-v2 -- DO NOT EDIT.\n";
    out << "// 全局单表 MPHF（唯一 MPHF 层，每类型体系已退役）：全语料\n";
    out << "// (ReanimationType, ReanimTrackId) 联合键 -> 轨道索引\n";
    out << "//（键值语义：每类型首见该 id 的轨道下标，与旧直接寻址表同语义）。\n";
    out << "// 联合键 key = (uint64(type) << 32) | id；id 1 基且 type >= 0，真实键恒非 0，\n";
    out << "// 槽表 padding 条目 {key=0} 天然不命中，验证恰一次 64 位比较。\n";
    out << "// 桶数/槽数均 2 的幂：运行期取模全部为掩码（零 div）。\n";
    out << "// 函数体与 reanimc 求解器（Pak64.cpp MphfGlobalLookup）、运行时调用点\n";
    out << "//（Reanimator.cpp ReanimatorTrackIndexOf）逐运算一致：联合键打包、乘法常数、\n";
    out << "// 移位、掩码顺序完全相同；全 uint64 乘法回绕是设计的一部分。\n";
    out << "// 本头文件必须与 ReanimTrackId.h 及全部 .reanim.v2.compiled 来自同一次\n";
    out << "// compile-v2（id 表不同则键集不同，哈希即失配）。\n";
    out << "#pragma once\n\n#include <cstdint>\n\n";
    out << "// keys=" << mphf.count << " buckets=" << mphf.buckets
        << " slots=" << mphf.slotCount << " (padding "
        << (mphf.slotCount - mphf.count) << " x {key=0})\n";
    out << "inline constexpr int MphfLookupGlobal(const std::uint32_t theType, const std::uint32_t theTrackId)\n{\n";
    out << "    struct MphfGlobalSlot { std::uint64_t key; std::uint16_t index; };\n";
    out << "    static constexpr std::uint32_t kBuckets = " << mphf.buckets << ";\n";
    out << "    static constexpr std::uint16_t kDisp[kBuckets] = {";
    for (std::size_t i = 0; i < mphf.displacement.size(); ++i)
        out << (i % 24 == 0 ? "\n        " : " ") << mphf.displacement[i] << ",";
    out << "\n    };\n";
    out << "    static constexpr std::uint32_t kSlotCount = " << mphf.slotCount << ";\n";
    out << "    static constexpr MphfGlobalSlot kSlots[kSlotCount] = {";
    for (std::size_t i = 0; i < mphf.slots.size(); ++i) {
        out << (i % 4 == 0 ? "\n        " : " ") << "{0x" << std::hex
            << mphf.slots[i].key << "ull, " << std::dec
            << mphf.slots[i].trackIndex << "},";
    }
    out << "\n    };\n";
    // 与求解器 MphfGlobalLookup / 运行时调用点逐运算一致（改一处必须三方同步）
    out << "    const std::uint64_t aKey = (static_cast<std::uint64_t>(theType) << 32) | theTrackId;\n";
    out << "    const std::uint64_t aMixed1 = aKey * 0x9E3779B97F4A7C15ull;\n";
    out << "    const std::uint32_t aBucket = static_cast<std::uint32_t>((aMixed1 ^ (aMixed1 >> 31)) & (kBuckets - 1));\n";
    out << "    const std::uint16_t aSeed = kDisp[aBucket];\n";
    out << "    const std::uint64_t aMixed2 = aKey * (0xC2B2AE3D27D4EB4Full + aSeed);\n";
    out << "    const std::uint32_t aSlot = static_cast<std::uint32_t>((aMixed2 ^ (aMixed2 >> 32)) & (kSlotCount - 1));\n";
    out << "    const MphfGlobalSlot& aEntry = kSlots[aSlot];\n";
    out << "    return aEntry.key == aKey ? static_cast<int>(aEntry.index) : -1;\n";
    out << "}\n\n";

    // 语料成员位图：bit t = 类型 t 编入全局键集（kGlobalTypeBits[153 位]）。
    // 运行时判据 MphfGlobalTypeInCorpus(type)：对拍闸的 "-1 须与旧表一致"
    // 仅约束语料内类型（语料外类型全局表无键，-1 由旧表兜底）；旧直接寻址
    // 表也仅对语料外类型/对拍闸/PVZ_MPHF=0 基线构建。
    out << "inline constexpr std::uint64_t kGlobalTypeBits[" << mphf.typeBits.size() << "] = {";
    for (std::size_t i = 0; i < mphf.typeBits.size(); ++i)
        out << (i % 3 == 0 ? "\n        " : " ") << "0x" << std::hex
            << mphf.typeBits[i] << "ull,";
    out << std::dec << "\n    };\n";
    out << "inline constexpr bool MphfGlobalTypeInCorpus(const std::uint32_t theType)\n{\n";
    out << "    return theType < " << (mphf.maxType + 1) << "u &&\n";
    out << "        (kGlobalTypeBits[theType >> 6] & (1ull << (theType & 63u))) != 0ull;\n";
    out << "}\n";

    // 完整 attacher 位掩码（2026-10-06 credits 合影缺失案后新增）：每类型
    // **全部** attacher 轨道下标置位，按原始轨道表计算——同 id 重复轨道
    // （credits MAIN3 两条 attacher__ZombieArmy2 id=458）在上方 kSlots 被
    // MPHF 首见折叠，但掩码必须覆盖它们，否则该轨道运行时永不被
    // UpdateAttacherTrack 驱动（段表在也不生成附件）。掩码与段表同源
    // （同一次 compile-v2 的同一轨道表），下游 gen_reanim_attacher_masks.py
    // 转写进游戏侧 ReanimAttacherMasks.h（不再从 kSlots 自行推导）。
    {
        // 类型映射按名线性查（文件无有序保证；构建期一次性成本）
        const auto typeOfModule = [&](const RawReanim& module) -> std::uint32_t {
            const std::string folded = Folded(module.name);
            for (const ReanimTypeMapEntry& entry : typeMap)
                if (entry.folded == folded)
                    return entry.value;
            return 0xFFFFFFFFu;
        };
        std::uint32_t aMaxType = 0, aMaxTracks = 0;
        for (const RawReanim& module : corpus) {
            const std::uint32_t aType = typeOfModule(module);
            if (aType == 0xFFFFFFFFu) continue;  // 无类型号模块（不进运行时类型系）
            aMaxType = std::max(aMaxType, aType);
            aMaxTracks = std::max<std::uint32_t>(aMaxTracks, (std::uint32_t)module.tracks.size());
        }
        const std::uint32_t aWords = (aMaxTracks + 63) / 64;
        std::vector<std::vector<std::uint64_t>> masks(aMaxType + 1, std::vector<std::uint64_t>(aWords, 0));
        for (const RawReanim& module : corpus) {
            const std::uint32_t aType = typeOfModule(module);
            if (aType == 0xFFFFFFFFu) continue;
            auto& bits = masks[aType];
            for (std::size_t i = 0; i < module.tracks.size(); ++i)
                if (Folded(module.tracks[i].name).rfind("attacher__", 0) == 0)
                    bits[i >> 6] |= 1ull << (i & 63);
        }
        out << "\n// 每类型 attacher 轨道全量位掩码（原始轨道表计算，含同 id 重复轨道；\n"
               "// kSlots 首见折叠不含它们——查询语义无损，掩码语义必须无损才是对的）。\n";
        out << "inline constexpr int kAttacherMaskTypeCount = " << (aMaxType + 1) << ";\n";
        out << "inline constexpr int kAttacherMaskWords = " << aWords << ";  // ceil("
            << aMaxTracks << "/64)\n";
        out << "inline constexpr std::uint64_t kAttacherMaskByType[kAttacherMaskTypeCount][kAttacherMaskWords] = {\n";
        for (std::size_t t = 0; t < masks.size(); ++t) {
            out << "    {";
            for (std::size_t w = 0; w < masks[t].size(); ++w)
                out << (w ? ", " : "") << "0x" << std::hex << masks[t][w] << "ull";
            out << std::dec << "},  // type " << t << "\n";
        }
        out << "};\n";
    }
    return out.str();
}


std::vector<std::uint8_t> SerializeV2(const RawReanim& source,
                                      const std::vector<TrackIdEntry>& table,
                                      const std::vector<ReanimTypeMapEntry>& typeMap)
{
    RequireLegacyStorage(source);
    // 编译期 attacher 链接：text 编码 → 整数段表（局部，不改入参模型）
    std::vector<std::vector<TrackAttacherSegment>> localSegs(source.tracks.size());
    {
        RawReanim probe = source;  // Derive 需要可变模型（填段表）
        DeriveAttacherSegments(probe, table, typeMap);
        for (std::size_t i = 0; i < probe.tracks.size(); ++i)
            localSegs[i] = std::move(probe.tracks[i].attacherSegments);
    }
    std::vector<std::uint8_t> payload;
    PutU32(payload, kV2PayloadMagic);
    PutU32(payload, TrackIdTableHash(table));
    std::uint32_t fpsBits = 0;
    std::memcpy(&fpsBits, &source.fps, 4);
    PutU32(payload, fpsBits);
    PutU32(payload, static_cast<std::uint32_t>(source.tracks.size()));
    for (const RawTrack& track : source.tracks) {
        // 编译期解析：轨道名 -> 语料 id。名字不在表内（编译输入与建表输入
        // 不一致）时写 0，运行时即走 not-found（TodTrace 后回退轨道 0）。
        PutU32(payload, TrackIdOf(table, track.name));
        PutU32(payload, static_cast<std::uint32_t>(track.frames.size()));
        PutU32(payload, 56);
        for (const RawTransform& frame : track.frames) {
            for (int i = 0; i < 8; ++i) PutF32(payload, frame.values[i]);
            payload.resize(payload.size() + 24, 0);
        }
        for (const RawTransform& frame : track.frames) {
            PutString(payload, frame.image);
            PutString(payload, frame.font);
            PutString(payload, frame.text);
        }
    }

    // IDT3：编译期 attacher 段表（仅含派生出段落的轨道）。
    std::uint32_t segTrackCount = 0;
    for (const auto& segs : localSegs)
        if (!segs.empty())
            ++segTrackCount;
    PutU32(payload, segTrackCount);
    for (std::size_t ti = 0; ti < localSegs.size(); ++ti) {
        const auto& segs = localSegs[ti];
        if (segs.empty()) continue;
        PutU32(payload, static_cast<std::uint32_t>(ti));
        PutU32(payload, static_cast<std::uint32_t>(segs.size()));
        for (const TrackAttacherSegment& seg : segs) {
            PutU32(payload, seg.startFrame);
            PutU32(payload, seg.endFrame);
            PutU32(payload, seg.targetType);
            PutU32(payload, seg.actionTrackId);
            PutF32(payload, seg.rate);
            PutU32(payload, seg.loop);
        }
    }

    std::vector<std::uint8_t> out;
    PutU32(out, kV2Cookie);
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    PutU32(out, Crc32(payload));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool WriteCompiledV2(const RawReanim& source, const std::vector<TrackIdEntry>& table,
                     const std::vector<ReanimTypeMapEntry>& typeMap,
                     const std::filesystem::path& output, std::string* error)
{
    try {
        const std::vector<std::uint8_t> bytes = SerializeV2(source, table, typeMap);
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

// Independent font identifiers; images never load a game registry.
bool LoadFontIdRegistry(const std::filesystem::path& sidecar,
                          FontIdRegistry& out,
                          std::uint32_t* fileHash,
                          std::string* error)
{
    try {
        std::ifstream in(sidecar, std::ios::binary);
        if (!in) { Fail(error, "cannot open font sidecar " + sidecar.string()); return false; }
        std::vector<std::uint8_t> bytes((std::istreambuf_iterator<char>(in)),
                                        std::istreambuf_iterator<char>());
        if (fileHash) *fileHash = Crc32(bytes);
        std::size_t at = 0;
        const auto take32 = [&]() {
            std::uint32_t v = 0;
            if (at + 4 > bytes.size()) throw std::runtime_error("sidecar truncated");
            std::memcpy(&v, bytes.data() + at, 4);
            at += 4;
            return v;
        };
        const std::uint32_t magic = take32();
        if (magic != 0x4953594D /* 'MYSI' */) { Fail(error, "bad symbol sidecar magic"); return false; }
        const std::uint32_t count = take32();
        out.clear();
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint32_t id = take32();
            const std::uint32_t len = take32();
            if (at + len > bytes.size()) throw std::runtime_error("sidecar truncated name");
            out.emplace(std::string(reinterpret_cast<const char*>(bytes.data() + at), len), id);
            at += len;
        }
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

std::vector<std::uint8_t> SerializeV4(const RawReanim& source,
                                      const std::vector<TrackIdEntry>& table,
                                      const std::vector<ReanimTypeMapEntry>& typeMap,
                                      const SourceImageHandles& sourceImageHandles,
                                      const FontIdRegistry& fontSymbols,
                                      const std::uint32_t fontRegistryHash)
{
    // Preserve source text identity before numeric decode merges routing runs.
    std::vector<std::vector<TrackAttacherSegment>> localSegs(source.tracks.size());
    {
        RawReanim probe = source;
        DeriveAttacherSegments(probe, table, typeMap, true);
        for (std::size_t i = 0; i < probe.tracks.size(); ++i)
            localSegs[i] = std::move(probe.tracks[i].attacherSegments);
    }
    const auto idOf = [](const auto& registry, const std::string& name,
                             const char* what, const std::string& module) -> std::uint32_t {
        const auto it = registry.find(name);
        if (it == registry.end())
            throw std::runtime_error(std::string(what) + " missing from offline table: "
                + name + " (module " + module + ")");
        return it->second;
    };
    std::vector<bool> hasUiText;
    hasUiText.reserve(source.tracks.size());
    for (const RawTrack& track : source.tracks)
        hasUiText.push_back(HasUiText(track));
    std::vector<std::uint8_t> payload;
    PutU32(payload, 0x46544449 /* 'IDTF': finite float sources, integer cel */);
    PutU32(payload, TrackIdTableHash(table));
    PutU32(payload, 0);  // Image handles are local to image_sources.txt.
    PutU32(payload, fontRegistryHash);
    PutF32(payload, source.fps);
    PutU32(payload, static_cast<std::uint32_t>(source.tracks.size()));
    for (std::size_t ti = 0; ti < source.tracks.size(); ++ti) {
        const RawTrack& track = source.tracks[ti];
        PutU32(payload, TrackIdOf(table, track.name));
        PutU32(payload, static_cast<std::uint32_t>(track.frames.size()));
        PutU32(payload, 32);  // 8 x f32; no native pointers or padding
        PutU32(payload, (hasUiText[ti] ? kTrackUiTextMask : 0u) | StorageBits(track.storage));
        for (const RawTransform& frame : track.frames)
            for (int i = 0; i < 8; ++i) {
                if (i == 6) PutU32(payload, static_cast<std::uint32_t>(frame.cel));
                else PutF32(payload, frame.values[i]);
            }
    }
    for (std::size_t ti = 0; ti < source.tracks.size(); ++ti) {
        const RawTrack& track = source.tracks[ti];
        for (const RawTransform& frame : track.frames) {
            PutU32(payload, frame.image.empty() ? 0
                : idOf(sourceImageHandles, frame.image, "source image", source.name));
            PutU32(payload, frame.font.empty() ? 0
                : idOf(fontSymbols, frame.font, "font", source.name));
            if (hasUiText[ti]) PutString(payload, frame.text);
        }
    }
    // IDT5: numeric attachment decode plus inherited source-text run bounds.
    std::uint32_t segTrackCount = 0;
    for (const auto& segs : localSegs)
        if (!segs.empty()) ++segTrackCount;
    PutU32(payload, segTrackCount);
    for (std::size_t ti = 0; ti < localSegs.size(); ++ti) {
        const auto& segs = localSegs[ti];
        if (segs.empty()) continue;
        PutU32(payload, static_cast<std::uint32_t>(ti));
        PutU32(payload, static_cast<std::uint32_t>(segs.size()));
        for (const TrackAttacherSegment& seg : segs) {
            PutU32(payload, seg.startFrame);
            PutU32(payload, seg.endFrame);
            PutU32(payload, seg.targetType);
            PutU32(payload, seg.actionTrackId);
            PutF32(payload, seg.rate);
            PutU32(payload, seg.loop);
            PutU32(payload, seg.runStart);
            PutU32(payload, seg.runEnd);
        }
    }

    std::vector<std::uint8_t> out;
    PutU32(out, 0xDEADFED7u);  // Offline source-record envelope, IDT5 payload.
    PutU32(out, static_cast<std::uint32_t>(payload.size()));
    PutU32(out, Crc32(payload));
    out.insert(out.end(), payload.begin(), payload.end());
    return out;
}

bool WriteCompiledV4(const RawReanim& source,
                     const std::vector<TrackIdEntry>& table,
                     const std::vector<ReanimTypeMapEntry>& typeMap,
                     const SourceImageHandles& sourceImageHandles,
                     const FontIdRegistry& fontSymbols,
                     const std::uint32_t fontRegistryHash,
                     const std::filesystem::path& output,
                     std::string* error)
{
    try {
        const std::vector<std::uint8_t> bytes = SerializeV4(
            source, table, typeMap, sourceImageHandles, fontSymbols,
            fontRegistryHash);
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

bool ReadCompiledV2FromBytes(const std::vector<std::uint8_t>& bytes,
                             const std::vector<TrackIdEntry>& table,
                             RawReanim& output, std::string* error)
{
    try {
        if (bytes.size() < 16) Fail(error, "v2 record too small");
        std::uint32_t cookie = 0, declared = 0, storedCrc = 0;
        std::memcpy(&cookie, bytes.data(), 4);
        std::memcpy(&declared, bytes.data() + 4, 4);
        std::memcpy(&storedCrc, bytes.data() + 8, 4);
        if (cookie != kV2Cookie) Fail(error, "unexpected v2 cookie");
        if (bytes.size() - 12 != declared) Fail(error, "v2 payload size mismatch");
        const std::vector<std::uint8_t> payload(bytes.begin() + 12, bytes.end());
        if (Crc32(payload) != storedCrc) Fail(error, "v2 payload crc mismatch");

        std::size_t at = 0;
        const auto take32 = [&](std::uint32_t& value) {
            if (at + 4 > payload.size()) Fail(error, "truncated v2 payload");
            std::memcpy(&value, payload.data() + at, 4);
            at += 4;
        };
        std::uint32_t magic = 0, tableHash = 0;
        take32(magic);
        take32(tableHash);
        if (magic != kV2PayloadMagic) Fail(error, "unexpected v2 payload magic");
        if (tableHash != TrackIdTableHash(table))
            Fail(error, "track-id table does not match this record (rebuild together)");

        output = RawReanim{};
        std::uint32_t fpsBits = 0, trackCount = 0;
        take32(fpsBits);
        take32(trackCount);
        std::memcpy(&output.fps, &fpsBits, 4);
        for (std::uint32_t t = 0; t < trackCount; ++t) {
            RawTrack track;
            std::uint32_t trackId = 0, frameCount = 0, transformElement = 0;
            take32(trackId);
            take32(frameCount);
            take32(transformElement);
            if (transformElement != 56) Fail(error, "unexpected v2 transform element size");
            if (trackId != 0) {
                if (trackId > table.size()) Fail(error, "track id out of table range");
                track.name = table[trackId - 1].display;  // 诊断/前缀豁免路径用
            }
            for (std::uint32_t f = 0; f < frameCount; ++f) {
                RawTransform frame;
                for (int i = 0; i < 8; ++i) {
                    if (at + 4 > payload.size()) Fail(error, "truncated v2 frame");
                    std::memcpy(&frame.values[i], payload.data() + at, 4);
                    at += 4;
                }
                at += 24;
                track.frames.push_back(std::move(frame));
            }
            const auto takeString = [&](std::string& value) {
                std::uint32_t length = 0;
                take32(length);
                if (at + length > payload.size()) Fail(error, "truncated v2 string");
                value.assign(reinterpret_cast<const char*>(payload.data() + at), length);
                at += length;
            };
            for (auto& frame : track.frames) {
                takeString(frame.image);
                takeString(frame.font);
                takeString(frame.text);
            }
            output.tracks.push_back(std::move(track));
        }
        // IDT3 段表（仅含派生出段落的轨道）
        std::uint32_t segTrackCount = 0;
        take32(segTrackCount);
        for (std::uint32_t s = 0; s < segTrackCount; ++s) {
            std::uint32_t trackIndex = 0, segCount = 0;
            take32(trackIndex);
            take32(segCount);
            if (trackIndex >= output.tracks.size()) Fail(error, "attacher segment track index out of range");
            RawTrack& segTrack = output.tracks[trackIndex];
            for (std::uint32_t g = 0; g < segCount; ++g) {
                TrackAttacherSegment seg;
                std::uint32_t loop = 0;
                take32(seg.startFrame);
                take32(seg.endFrame);
                take32(seg.targetType);
                take32(seg.actionTrackId);
                if (at + 4 > payload.size()) Fail(error, "truncated attacher segment");
                std::memcpy(&seg.rate, payload.data() + at, 4);
                at += 4;
                take32(loop);
                seg.loop = loop;
                segTrack.attacherSegments.push_back(seg);
            }
        }
        if (at != payload.size()) Fail(error, "trailing v2 payload bytes");
        return true;
    } catch (const std::exception& e) {
        if (error && error->empty()) *error = e.what();
        return false;
    }
}

TrackIdRange RangeOfPrefix(const std::vector<TrackIdEntry>& table,
                            const std::string_view prefix)
{
    // 左闭右开：lo = 首个命中 id；hi = 末个命中 id + 1（即字典序里紧跟
    // 该前缀块的下一个枚举值）。Contains 用 id >= lo && id < hi 两次比较。
    const std::string folded = Folded(prefix);
    TrackIdRange range;
    for (const TrackIdEntry& entry : table)  // table 按 folded 升序，命中连续
    {
        if (entry.folded.rfind(folded, 0) == 0)
        {
            if (range.lo == 0) range.lo = entry.id;
            range.hi = entry.id + 1;
        }
        else if (range.lo != 0)
            break;  // 连续段已结束
    }
    return range;
}

std::vector<std::string> HeaderIdentifierOf(const std::vector<TrackIdEntry>& table)
{
    std::vector<std::string> idents;
    idents.reserve(table.size());
    for (const TrackIdEntry& entry : table) {
        std::string ident = SanitizedIdentifier(entry.display);
        if (std::find(idents.begin(), idents.end(), ident) != idents.end()) {
            std::uint32_t suffix = 2;
            while (std::find(idents.begin(), idents.end(),
                             ident + '_' + std::to_string(suffix)) != idents.end())
                ++suffix;
            ident += '_' + std::to_string(suffix);
        }
        idents.push_back(std::move(ident));
    }
    return idents;
}

std::string MakeTrackIdRangesBlock(const std::vector<TrackIdEntry>& table,
                                   const std::vector<std::string>& prefixes)
{
    static_assert(sizeof(unsigned int) == 4);
    const std::vector<std::string> entryIdents = HeaderIdentifierOf(table);
    // CLI family hints and DSL prefixStrs can repeat the same declaration.
    // Deduplicate exact spellings before allocating collision suffixes.
    std::vector<std::string> uniquePrefixes;
    std::set<std::string> seenPrefixes;
    for (const std::string& prefix : prefixes)
        if (seenPrefixes.insert(prefix).second)
            uniquePrefixes.push_back(prefix);

    std::vector<std::string> idents;
    for (const std::string& prefix : uniquePrefixes) {
        std::string ident = SanitizedIdentifier(prefix);
        if (std::find(idents.begin(), idents.end(), ident) != idents.end()) {
            unsigned int suffix = 2;
            while (std::find(idents.begin(), idents.end(),
                             ident + '_' + std::to_string(suffix)) != idents.end())
                ++suffix;
            ident += '_' + std::to_string(suffix);
        }
        idents.push_back(std::move(ident));
    }

    std::ostringstream out;
    out << "struct ReanimTrackIdRange final\n{\n";
    out << "    ReanimTrackId mLo = ReanimTrackId::None;  // 首个命中（含）\n";
    out << "    ReanimTrackId mHi = ReanimTrackId::None;  // 末个命中的下一个（不含）\n";
    out << "    [[nodiscard]] constexpr bool Contains(const ReanimTrackId theId) const\n    {\n";
    out << "        return static_cast<std::uint32_t>(theId) >= static_cast<std::uint32_t>(mLo) &&\n";
    out << "            static_cast<std::uint32_t>(theId) < static_cast<std::uint32_t>(mHi);\n";
    out << "    }\n";
    out << "};\n\n";
    out << "// 枚举从 1 开始（None = 0）；前缀家族的编译期区间为左闭右开 [mLo, mHi)：\n";
    out << "// 枚举按折叠字典序分配 id，任何字符串前缀恰好是一段连续 id，\n";
    out << "// mHi 即字典序里紧跟该块的下一个枚举值。运行期前缀匹配 = Contains\n";
    out << "//（两次整数比较），与原版 strnicmp 前缀精确等价，无字符串比较。\n";
    out << "struct ReanimTrackRanges final\n{\n";
    for (std::size_t i = 0; i < uniquePrefixes.size(); ++i) {
        const TrackIdRange range = RangeOfPrefix(table, uniquePrefixes[i]);
        out << "    static constexpr ReanimTrackIdRange " << idents[i] << "{";
        if (range.lo == 0) {
            out << "ReanimTrackId::None, ReanimTrackId::None};\n";
        } else {
            // hi 左闭右开：指向紧跟家族块的下一个枚举成员（显式出现在 hpp）；
            // 家族延伸到枚举末尾时用 PVZ_REANIM_TRACK_ID_END（COUNT+1）。
            const bool reachesEnd = range.hi > table.size();
            out << "ReanimTrackId::" << entryIdents[range.lo - 1] << ", ";
            if (reachesEnd)
                out << "PVZ_REANIM_TRACK_ID_END";
            else
                out << "ReanimTrackId::" << entryIdents[range.hi - 1];
            out << "};\n";
        }
    }
    out << "};\n";
    return out.str();
}

std::string MakeTrackIdHeader(const std::vector<TrackIdEntry>& table,
                               const std::vector<std::string>& families)
{
    const std::vector<std::string> identifiers = HeaderIdentifierOf(table);

    std::ostringstream out;
    out << "// Generated by reanimc compile-v2 -- DO NOT EDIT.\n";
    out << "// Numeric runtime track IDs, checksum and prefix ranges.\n";
    out << "// Rebuild this header and the compiled records together.\n";
    out << "#pragma once\n\n#include <cstdint>\n\n";
    out << "enum class ReanimTrackId : std::uint32_t\n{\n";
    out << "    None = 0,  // 编译期未映射 / 运行时找不到\n";
    for (std::size_t i = 0; i < table.size(); ++i)
        out << "    " << identifiers[i] << " = " << (i + 1) << ",\n";
    out << "};\n\n";
    out << "constexpr std::uint32_t PVZ_REANIM_TRACK_ID_COUNT = " << table.size() << ";\n";
    out << "// 区间的右端点（不含）：枚举最大值 + 1，用于延伸到末尾的家族。\n";
    out << "constexpr ReanimTrackId PVZ_REANIM_TRACK_ID_END =\n";
    out << "    static_cast<ReanimTrackId>(PVZ_REANIM_TRACK_ID_COUNT + 1);\n\n";
    out << "inline constexpr std::uint32_t PVZ_REANIM_TRACK_TABLE_HASH = "
        << TrackIdTableHash(table) << "u;\n";
    if (!families.empty()) {
        out << "\n";
        out << MakeTrackIdRangesBlock(table, families);
    }
    return out.str();
}

} // namespace openpvz::reanim::pak64
