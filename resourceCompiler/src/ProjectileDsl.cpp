#include "reanim/ProjectileDsl.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <locale>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string_view>

namespace openpvz::reanim::projectile {
namespace {

constexpr std::array<std::string_view, 14> kNames = {
    "PROJECTILE_PEA", "PROJECTILE_SNOWPEA", "PROJECTILE_CABBAGE", "PROJECTILE_MELON",
    "PROJECTILE_PUFF", "PROJECTILE_WINTERMELON", "PROJECTILE_FIREBALL", "PROJECTILE_STAR",
    "PROJECTILE_SPIKE", "PROJECTILE_BASKETBALL", "PROJECTILE_KERNEL", "PROJECTILE_COBBIG",
    "PROJECTILE_BUTTER", "PROJECTILE_ZOMBIE_PEA"
};

struct Token {
    std::string text;
    bool quoted = false;
    std::size_t line = 1;
};

struct Projectile {
    std::optional<std::string> image;
    double scale = 1;
};
struct Shadow {
    std::string image;
    std::array<int, 4> rect{};
};

// The other definition lexers are private to their translation units. Keep this
// small grammar independent, with source locations on every diagnostic.
class Parser {
public:
    Parser(std::string_view text, std::string filename) : text_(text), filename_(std::move(filename)) { Next(); }

    std::string Run()
    {
        Expect("export"); Expect("component"); Expect("Projectiles"); Expect("{");
        bool kind = false;
        while (!At("}")) {
            if (At("kind")) {
                if (kind) Fail("duplicate kind");
                Next(); Expect(":"); Expect("projectile"); Expect(";"); kind = true;
            } else if (At("projectile")) {
                Next();
                const auto it = std::find(kNames.begin(), kNames.end(), token_.text);
                if (token_.quoted || it == kNames.end()) Fail("unknown projectile slot '" + token_.text + "'");
                const auto slot = static_cast<std::size_t>(it - kNames.begin());
                if (projectiles_[slot]) Fail("duplicate projectile slot '" + token_.text + "'");
                Next(); Expect("{");
                Projectile value;
                bool image = false, scale = false;
                while (!At("}")) {
                    if (At("image")) {
                        if (image) Fail("duplicate image");
                        Next(); Expect(":");
                        if (At("none")) Next();
                        else value.image = Path();
                        Expect(";"); image = true;
                    } else if (At("scale")) {
                        if (scale) Fail("duplicate scale");
                        Next(); Expect(":"); value.scale = Number(); Expect(";"); scale = true;
                    } else Fail("expected projectile image or scale");
                }
                if (!image || !scale) Fail("projectile requires image and scale");
                if (slot == 6 ? value.image.has_value() : !value.image.has_value())
                    Fail(slot == 6 ? "PROJECTILE_FIREBALL image must be none" : "projectile image must be a relative PNG path");
                if (!std::isfinite(value.scale) || value.scale <= 0 ||
                    value.scale > std::numeric_limits<float>::max() ||
                    value.scale < std::numeric_limits<float>::denorm_min())
                    Fail("scale must be finite and positive in runtime float precision");
                Next(); projectiles_[slot] = value;
            } else if (At("shadow")) {
                Next();
                if (!At("day") && !At("night")) Fail("unknown shadow slot '" + token_.text + "'");
                const std::size_t slot = At("day") ? 0 : 1;
                if (shadows_[slot]) Fail("duplicate shadow slot '" + token_.text + "'");
                Next(); Expect("{");
                Shadow value;
                bool image = false, rect = false;
                while (!At("}")) {
                    if (At("image")) {
                        if (image) Fail("duplicate image");
                        Next(); Expect(":"); value.image = Path(); Expect(";"); image = true;
                    } else if (At("rect")) {
                        if (rect) Fail("duplicate rect");
                        Next(); Expect(":"); Expect("[");
                        for (std::size_t i = 0; i < 4; ++i) {
                            if (token_.quoted) Fail("rect requires four nonnegative integers");
                            int number = 0;
                            const auto [end, error] = std::from_chars(token_.text.data(), token_.text.data() + token_.text.size(), number);
                            if (error != std::errc{} || end != token_.text.data() + token_.text.size() || number < 0)
                                Fail("rect requires four nonnegative integers");
                            value.rect[i] = number; Next();
                            if (i != 3) Expect(",");
                        }
                        Expect("]"); Expect(";"); rect = true;
                    } else Fail("expected shadow image or rect");
                }
                if (!image || !rect) Fail("shadow requires image and rect");
                if (value.rect[2] <= 0 || value.rect[3] <= 0 ||
                    value.rect[0] > std::numeric_limits<int>::max() - value.rect[2] ||
                    value.rect[1] > std::numeric_limits<int>::max() - value.rect[3])
                    Fail("shadow rect must have positive dimensions and fit runtime integer coordinates");
                Next(); shadows_[slot] = value;
            } else Fail("expected kind, projectile, shadow, or '}'");
        }
        if (!kind) Fail("missing kind: projectile");
        for (std::size_t i = 0; i < projectiles_.size(); ++i)
            if (!projectiles_[i]) Fail("missing projectile slot '" + std::string(kNames[i]) + "'");
        for (std::size_t i = 0; i < shadows_.size(); ++i)
            if (!shadows_[i]) Fail(std::string("missing shadow slot '") + (i == 0 ? "day'" : "night'"));
        Next();
        if (!token_.text.empty()) Fail("unexpected trailing input");
        std::ostringstream out;
        out.imbue(std::locale::classic());
        out << std::setprecision(std::numeric_limits<double>::max_digits10);
        out << "{\n  \"format\": \"pvz-projectile-sources-v1\",\n  \"name\": \"Projectiles\",\n  \"projectiles\": [\n";
        for (std::size_t i = 0; i < projectiles_.size(); ++i) {
            const auto& value = *projectiles_[i];
            out << "    {\"name\": " << Quote(std::string(kNames[i])) << ", \"image\": "
                << (value.image ? Quote(*value.image) : "null") << ", \"scale\": " << value.scale
                << "}" << (i + 1 == projectiles_.size() ? "\n" : ",\n");
        }
        out << "  ],\n  \"shadows\": [\n";
        for (std::size_t i = 0; i < shadows_.size(); ++i) {
            const auto& value = *shadows_[i];
            out << "    {\"name\": \"" << (i == 0 ? "day" : "night") << "\", \"image\": " << Quote(value.image)
                << ", \"rect\": [" << value.rect[0] << ", " << value.rect[1] << ", " << value.rect[2] << ", " << value.rect[3]
                << "]}" << (i == 0 ? ",\n" : "\n");
        }
        out << "  ]\n}\n";
        return out.str();
    }

private:
    [[noreturn]] void Fail(const std::string& message) const
    {
        throw std::runtime_error(filename_ + ":" + std::to_string(token_.line) + ": " + message);
    }
    bool At(std::string_view value) const { return !token_.quoted && token_.text == value; }
    void Expect(std::string_view value)
    {
        if (!At(value)) Fail("expected '" + std::string(value) + "', got '" + token_.text + "'");
        Next();
    }
    void Next()
    {
        for (;;) {
            while (position_ < text_.size() && static_cast<unsigned char>(text_[position_]) <= ' ') {
                if (text_[position_++] == '\n') ++line_;
            }
            if (text_.substr(position_, 2) == "//") {
                while (position_ < text_.size() && text_[position_] != '\n') ++position_;
            } else if (text_.substr(position_, 2) == "/*") {
                const auto startLine = line_;
                position_ += 2;
                while (position_ < text_.size() && text_.substr(position_, 2) != "*/") {
                    if (text_[position_++] == '\n') ++line_;
                }
                if (position_ == text_.size()) { token_.line = startLine; Fail("unterminated block comment"); }
                position_ += 2;
            } else break;
        }
        token_ = {"", false, line_};
        if (position_ == text_.size()) return;
        const char c = text_[position_++];
        if (c == '"') {
            token_.quoted = true;
            while (position_ < text_.size()) {
                char value = text_[position_++];
                if (value == '"') return;
                if (static_cast<unsigned char>(value) < 32 || value == 127) Fail("control character in string");
                if (value == '\\') {
                    if (position_ == text_.size()) Fail("unterminated string escape");
                    value = text_[position_++];
                    if (value != '\\' && value != '"') Fail("invalid string escape");
                }
                token_.text += value;
            }
            Fail("unterminated string");
        }
        if (std::string_view("{}[]:;,").find(c) != std::string_view::npos) { token_.text = c; return; }
        token_.text = c;
        while (position_ < text_.size()) {
            const char next = text_[position_];
            if (static_cast<unsigned char>(next) <= ' ' || std::string_view("{}[]:;,\"").find(next) != std::string_view::npos ||
                text_.substr(position_, 2) == "//" || text_.substr(position_, 2) == "/*") break;
            token_.text += next; ++position_;
        }
    }
    double Number()
    {
        double result = 0;
        const auto [end, error] = std::from_chars(token_.text.data(), token_.text.data() + token_.text.size(), result);
        if (token_.quoted || error != std::errc{} || end != token_.text.data() + token_.text.size() ||
            !std::isfinite(result) || result <= 0) Fail("scale must be a finite positive number");
        Next(); return result;
    }
    std::string Path()
    {
        const std::string result = token_.text;
        if (!token_.quoted || result.empty() || result.front() == '/' ||
            result.find_first_of("\\:") != std::string::npos || result.find('/') == std::string::npos ||
            !result.ends_with(".png"))
            Fail("image must be a relative runtime PNG path using forward slashes");
        std::size_t start = 0;
        while (start <= result.size()) {
            const auto end = result.find('/', start);
            const auto part = result.substr(start, end == std::string::npos ? end : end - start);
            if (part.empty() || part == "." || part == "..") Fail("image path contains an empty or traversal component");
            for (const unsigned char c : part)
                if (c < 32 || c == 127 || c == '"' || c == '<' || c == '>' || c == '|' || c == '?' || c == '*')
                    Fail("invalid character in image path");
            if (end == std::string::npos) break;
            start = end + 1;
        }
        Next(); return result;
    }
    static std::string Quote(const std::string& text) { return "\"" + text + "\""; }

    std::string_view text_;
    std::string filename_;
    std::size_t position_ = 0, line_ = 1;
    Token token_;
    std::array<std::optional<Projectile>, 14> projectiles_;
    std::array<std::optional<Shadow>, 2> shadows_;
};
}

int RunCompileProjectile(const std::vector<std::string>& args)
{
    std::filesystem::path input, output;
    if ((args.size() - 1) % 2) { std::cerr << "reanimc: option requires a value: " << args.back() << '\n'; return 2; }
    for (std::size_t i = 1; i + 1 < args.size(); i += 2) {
        if (args[i] == "--input" && input.empty()) input = args[i + 1];
        else if (args[i] == "--output" && output.empty()) output = args[i + 1];
        else { std::cerr << "reanimc: unknown or duplicate option " << args[i] << '\n'; return 2; }
    }
    if (input.empty() || output.empty()) { std::cerr << "reanimc: compile-projectile requires --input and --output\n"; return 2; }
    std::ifstream in(input, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open " + input.string());
    std::ostringstream buffer; buffer << in.rdbuf();
    const std::string source = buffer.str();
    const std::string json = Parser(source, input.string()).Run();
    if (std::filesystem::exists(output)) throw std::runtime_error("output exists; choose a new path: " + output.string());
    if (!output.parent_path().empty()) std::filesystem::create_directories(output.parent_path());
    std::ofstream out(output, std::ios::binary);
    out << json;
    out.close();
    if (!out) throw std::runtime_error("cannot write " + output.string());
    std::cout << input.string() << " -> " << output.string() << " (projectiles=14, shadows=2)\n";
    return 0;
}
}
