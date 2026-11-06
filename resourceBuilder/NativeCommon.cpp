#include "NativeBuild.h"
#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iomanip>
#include <regex>
#include <sstream>
#include <stdexcept>
#include <zlib.h>
#ifdef _WIN32
#define NOMINMAX
#include <windows.h>
#else
#include <spawn.h>
#include <sys/wait.h>
extern char **environ;
#endif

namespace pvz::build
{
void Require(bool value, std::string_view message)
{
    if (!value)
        throw std::runtime_error(std::string(message));
}
Bytes Read(const fs::path &path)
{
    std::ifstream f(path, std::ios::binary);
    Require(bool(f), "Cannot read " + path.string());
    return Bytes(std::istreambuf_iterator<char>(f), {});
}
std::string Text(const fs::path &path)
{
    auto bytes = Read(path);
    std::string s(bytes.begin(), bytes.end());
    if (s.starts_with("\xef\xbb\xbf"))
        s.erase(0, 3);
    return s;
}
Json ReadJson(const fs::path &path)
{
    std::vector<std::set<std::string>> members;
    return Json::parse(Text(path), [&](int, Json::parse_event_t event, Json &value) {
        if (event == Json::parse_event_t::object_start)
            members.emplace_back();
        else if (event == Json::parse_event_t::key)
            Require(members.back().insert(value.get<std::string>()).second,
                    "Duplicate JSON member in " + path.string());
        else if (event == Json::parse_event_t::object_end)
            members.pop_back();
        return true;
    });
}
namespace
{
void ReplaceFile(const fs::path &from, const fs::path &to)
{
#ifdef _WIN32
    Require(MoveFileExW(from.c_str(), to.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH),
            "Cannot replace " + to.string());
#else
    fs::rename(from, to);
#endif
}
} // namespace
void Publish(const fs::path &path, std::span<const unsigned char> bytes)
{
    if (fs::exists(path) && Read(path) == Bytes(bytes.begin(), bytes.end()))
        return;
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    auto tmp = path;
    tmp += ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary);
        Require(bool(f), "Cannot write " + tmp.string());
        f.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        Require(bool(f), "Failed writing " + tmp.string());
    }
    ReplaceFile(tmp, path);
}
void Publish(const fs::path &p, std::string_view s)
{
    Publish(p, std::span(reinterpret_cast<const unsigned char *>(s.data()), s.size()));
}
Reader::Reader(std::span<const unsigned char> b) : data(b)
{
}
void Reader::Skip(std::size_t n)
{
    Require(n <= data.size() - at, "Truncated resource record");
    at += n;
}
Bytes Reader::Take(std::size_t n)
{
    const auto p = at;
    Skip(n);
    return Bytes(data.begin() + p, data.begin() + at);
}
unsigned Reader::Word()
{
    auto p = at;
    Skip(4);
    return GetWord(data, p);
}
int Reader::Int()
{
    return std::bit_cast<int>(Word());
}
float Reader::Float()
{
    return std::bit_cast<float>(Word());
}
double Reader::Double()
{
    std::uint64_t x = Word();
    x |= std::uint64_t(Word()) << 32;
    return std::bit_cast<double>(x);
}
std::string Reader::String()
{
    auto b = Take(Word());
    return std::string(b.begin(), b.end());
}
void Reader::Finish() const
{
    Require(at == data.size(), "Trailing resource record bytes");
}
unsigned GetWord(std::span<const unsigned char> b, std::size_t p)
{
    Require(p <= b.size() && b.size() - p >= 4, "Truncated word");
    return unsigned(b[p]) | (unsigned(b[p + 1]) << 8) | (unsigned(b[p + 2]) << 16) |
           (unsigned(b[p + 3]) << 24);
}
void Word(Bytes &b, unsigned v)
{
    for (int i = 0; i < 4; ++i)
        b.push_back(static_cast<unsigned char>(v >> (8 * i)));
}
void Integer(Bytes &b, int v)
{
    Word(b, std::bit_cast<unsigned>(v));
}
void Float(Bytes &b, float v)
{
    Word(b, std::bit_cast<unsigned>(v));
}
void Double(Bytes &b, double v)
{
    auto x = std::bit_cast<std::uint64_t>(v);
    Word(b, unsigned(x));
    Word(b, unsigned(x >> 32));
}
void Append(Bytes &b, std::span<const unsigned char> x)
{
    b.insert(b.end(), x.begin(), x.end());
}
void String(Bytes &b, std::string_view s)
{
    Word(b, unsigned(s.size()));
    b.insert(b.end(), s.begin(), s.end());
}
void SetWord(Bytes &b, std::size_t at, unsigned x)
{
    Require(at + 4 <= b.size(), "Invalid word patch");
    for (int i = 0; i < 4; ++i)
        b[at + i] = static_cast<unsigned char>(x >> (8 * i));
}
unsigned Crc(std::span<const unsigned char> b)
{
    return unsigned(crc32_z(0, b.data(), b.size()));
}
std::string Lower(std::string s)
{
    for (char &c : s)
        if (c >= 'A' && c <= 'Z')
            c += 32;
    return s;
}
std::string Upper(std::string s)
{
    for (char &c : s)
        if (c >= 'a' && c <= 'z')
            c -= 32;
    return s;
}
std::string Key(std::string s)
{
    std::replace(s.begin(), s.end(), '\\', '/');
    Require(!s.empty() && s[0] != '/' && s.back() != '/' && s.find('\0') == s.npos &&
                s.find_first_of(":<>|\"*?") == std::string::npos && s.find("//") == s.npos,
            "Unsafe resource path");
    for (auto &c : fs::path(s))
    {
        auto part = Lower(c.string());
        Require(part != ".." && part != "." && part.back() != ' ' && part.back() != '.',
                "Unsafe resource path");
        auto stem = fs::path(part).stem().string();
        Require(!std::regex_match(stem, std::regex("con|prn|aux|nul|com[1-9]|lpt[1-9]")),
                "Reserved resource path");
    }
    return Lower(s);
}
std::string CleanComments(std::string_view s)
{
    std::string out;
    for (std::size_t i = 0; i < s.size();)
    {
        if (s[i] == '"' || s[i] == '\'')
        {
            char q = s[i];
            out += s[i++];
            while (i < s.size())
            {
                char c = s[i++];
                out += c;
                if (c == '\\' && i < s.size())
                    out += s[i++];
                else if (c == q)
                    break;
            }
        }
        else if (s.substr(i, 2) == "//")
        {
            while (i < s.size() && s[i] != '\n')
                ++i;
        }
        else if (s.substr(i, 2) == "/*")
        {
            auto e = s.find("*/", i + 2);
            Require(e != s.npos, "Unterminated source comment");
            out += ' ';
            i = e + 2;
        }
        else
            out += s[i++];
    }
    return out;
}
std::vector<std::string> StringLiterals(std::string_view input)
{
    std::vector<std::string> out;
    std::string s = CleanComments(input);
    for (std::size_t i = 0; i < s.size(); ++i)
        if (s[i] == '"')
        {
            std::string v;
            for (++i; i < s.size() && s[i] != '"'; ++i)
            {
                char c = s[i];
                if (c == '\\' && i + 1 < s.size())
                {
                    c = s[++i];
                    if (c == 'n')
                        c = '\n';
                    else if (c == 'r')
                        c = '\r';
                    else if (c == 't')
                        c = '\t';
                }
                v += c;
            }
            out.push_back(v);
        }
    return out;
}
std::string CppString(std::string_view s)
{
    bool escaped = false;
    for (unsigned char c : s)
        escaped |= c >= 127 || (c < 32 && c != '\b' && c != '\f' && c != '\n' && c != '\r' && c != '\t');
    if (!escaped)
        return Json(std::string(s)).dump(-1, ' ', true);
    std::ostringstream o;
    o << '"';
    for (unsigned char c : s)
        o << "\\x" << std::hex << std::setw(2) << std::setfill('0') << unsigned(c);
    o << '"';
    return o.str();
}
std::string CppFloat(double v)
{
    float f = float(v);
    Require(std::isfinite(f), "Nonfinite generated float");
    std::ostringstream o;
    o << std::setprecision(9) << f;
    auto s = o.str();
    if (s.find_first_of(".eE") == s.npos)
        s += ".0";
    return s + "f";
}
std::string Region(const Json &r, int w, int h)
{
    int x = r[0], y = r[1], a = r[2], b = r[3];
    return "{" + CppFloat(w ? double(x) / w : 0) + ", " + CppFloat(h ? double(y) / h : 0) + ", " +
           CppFloat(w ? double(x + a) / w : 0) + ", " + CppFloat(h ? double(y + b) / h : 0) + ", " +
           std::to_string(a) + ", " + std::to_string(b) + "}";
}
Bytes Envelope(unsigned cookie, const Bytes &payload)
{
    Bytes b;
    Word(b, cookie);
    Word(b, unsigned(payload.size()));
    Word(b, Crc(payload));
    Append(b, payload);
    return b;
}
void Run(const fs::path &exe, const std::vector<std::string> &args)
{
#ifdef _WIN32
    auto quote = [](std::wstring_view s) {
        std::wstring q = L"\"";
        unsigned slash = 0;
        for (auto c : s)
        {
            if (c == L'\\')
            {
                ++slash;
                continue;
            }
            q.append(slash * (c == L'"' ? 2 : 1), L'\\');
            slash = 0;
            if (c == L'"')
                q += L'\\';
            q += c;
        }
        q.append(slash * 2, L'\\');
        return q + L'"';
    };
    std::wstring command = quote(exe.wstring());
    for (auto &s : args)
        command += L" " + quote(fs::path(s).wstring());
    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESTDHANDLES;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    startup.hStdOutput = GetStdHandle(STD_OUTPUT_HANDLE);
    startup.hStdError = GetStdHandle(STD_ERROR_HANDLE);
    PROCESS_INFORMATION process{};
    Require(CreateProcessW(exe.c_str(), command.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW, nullptr,
                           nullptr, &startup, &process),
            "Cannot start " + exe.string());
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD result = 1;
    GetExitCodeProcess(process.hProcess, &result);
    CloseHandle(process.hThread);
    CloseHandle(process.hProcess);
    Require(result == 0, "Resource compiler subprocess failed");
#else
    std::vector<std::string> strings{exe.string()};
    strings.insert(strings.end(), args.begin(), args.end());
    std::vector<char *> argv;
    for (auto &s : strings)
        argv.push_back(s.data());
    argv.push_back(nullptr);
    pid_t process;
    Require(posix_spawn(&process, exe.c_str(), nullptr, nullptr, argv.data(), environ) == 0,
            "Cannot start compiler");
    int result = 0;
    Require(waitpid(process, &result, 0) == process && WIFEXITED(result) && WEXITSTATUS(result) == 0,
            "Resource compiler subprocess failed");
#endif
}
void Zip(const fs::path &path, const Records &records)
{
    Bytes central;
    auto tmp = path;
    tmp += ".tmp";
    if (!path.parent_path().empty())
        fs::create_directories(path.parent_path());
    std::ofstream out(tmp, std::ios::binary);
    Require(bool(out), "Cannot open package");
    for (auto &[name, bytes] : records)
    {
        const auto offset = unsigned(out.tellp());
        unsigned crc = Crc(bytes), size = unsigned(bytes.size());
        Bytes h;
        Word(h, 0x04034b50);
        for (unsigned short v : {20, 0, 0, 0, 33})
        {
            h.push_back(v & 255);
            h.push_back(v >> 8);
        }
        Word(h, crc);
        Word(h, size);
        Word(h, size);
        h.push_back(name.size() & 255);
        h.push_back(name.size() >> 8);
        h.insert(h.end(), 2, 0);
        h.insert(h.end(), name.begin(), name.end());
        out.write(reinterpret_cast<const char *>(h.data()), h.size());
        out.write(reinterpret_cast<const char *>(bytes.data()), bytes.size());
        Word(central, 0x02014b50);
        for (unsigned short v : {20, 20, 0, 0, 0, 33})
        {
            central.push_back(v & 255);
            central.push_back(v >> 8);
        }
        Word(central, crc);
        Word(central, size);
        Word(central, size);
        central.push_back(name.size() & 255);
        central.push_back(name.size() >> 8);
        central.insert(central.end(), 8, 0);
        Word(central, 0x20);
        Word(central, offset);
        central.insert(central.end(), name.begin(), name.end());
    }
    const auto at = unsigned(out.tellp());
    out.write(reinterpret_cast<const char *>(central.data()), central.size());
    Bytes end;
    Word(end, 0x06054b50);
    Word(end, 0);
    end.push_back(records.size() & 255);
    end.push_back(records.size() >> 8);
    end.push_back(records.size() & 255);
    end.push_back(records.size() >> 8);
    Word(end, unsigned(central.size()));
    Word(end, at);
    end.insert(end.end(), 2, 0);
    out.write(reinterpret_cast<const char *>(end.data()), end.size());
    Require(bool(out), "Package write failed");
    out.close();
    ReplaceFile(tmp, path);
}
// FIPS 180-4 SHA-256. Used for portable build/cache identities, not authentication.
std::string Sha256(std::span<const unsigned char> source)
{
    static constexpr unsigned k[] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    unsigned h[] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a,
                    0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    Bytes b(source.begin(), source.end());
    b.push_back(128);
    while (b.size() % 64 != 56)
        b.push_back(0);
    auto bits = std::uint64_t(source.size()) * 8;
    for (int i = 7; i >= 0; --i)
        b.push_back(static_cast<unsigned char>(bits >> (i * 8)));
    for (std::size_t p = 0; p < b.size(); p += 64)
    {
        unsigned w[64];
        for (unsigned i = 0; i < 16; ++i)
            w[i] = (unsigned(b[p + i * 4]) << 24) | (unsigned(b[p + i * 4 + 1]) << 16) |
                   (unsigned(b[p + i * 4 + 2]) << 8) | b[p + i * 4 + 3];
        for (int i = 16; i < 64; ++i)
        {
            auto x = w[i - 15], y = w[i - 2];
            w[i] = w[i - 16] + (std::rotr(x, 7) ^ std::rotr(x, 18) ^ (x >> 3)) + w[i - 7] +
                   (std::rotr(y, 17) ^ std::rotr(y, 19) ^ (y >> 10));
        }
        auto a = h[0], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], q = h[7], v = h[1];
        for (int i = 0; i < 64; ++i)
        {
            unsigned t = q + (std::rotr(e, 6) ^ std::rotr(e, 11) ^ std::rotr(e, 25)) + ((e & f) ^ (~e & g)) +
                         k[i] + w[i];
            unsigned u =
                (std::rotr(a, 2) ^ std::rotr(a, 13) ^ std::rotr(a, 22)) + ((a & v) ^ (a & c) ^ (v & c));
            q = g;
            g = f;
            f = e;
            e = d + t;
            d = c;
            c = v;
            v = a;
            a = t + u;
        }
        h[0] += a;
        h[1] += v;
        h[2] += c;
        h[3] += d;
        h[4] += e;
        h[5] += f;
        h[6] += g;
        h[7] += q;
    }
    std::ostringstream out;
    for (auto v : h)
        out << std::hex << std::setw(8) << std::setfill('0') << v;
    return out.str();
}
Files Inventory(const fs::path &root)
{
    Files out;
    auto runtime = fs::canonical(root / "assets/runtime");
    for (auto &e : fs::recursive_directory_iterator(runtime))
        if (e.is_regular_file())
        {
            auto relative = fs::relative(e.path(), runtime);
            Require(!relative.empty() && !relative.native().starts_with(fs::path("..").native()),
                    "Runtime source escapes root");
            auto name = Key(relative.generic_string());
            Require(out.emplace(name, e.path()).second, "Duplicate runtime source");
        }
    auto overlay = ReadJson(root / "assets/source-art/ai/runtime-overlay.json");
    Require(overlay["format"] == "pvz-ai-runtime-overlay-v1", "Invalid AI overlay");
    std::set<fs::path> used;
    for (auto &[name, value] : overlay["files"].items())
    {
        auto source = fs::canonical(root / value.get<std::string>());
        auto relative = fs::relative(source, fs::canonical(root / "assets/source-art/ai/runtime"));
        Require(!relative.empty() && *relative.begin() != ".." && used.insert(source).second,
                "Invalid AI overlay source");
        Require(out.emplace(Key(name), source).second, "AI overlay duplicates source");
    }
    return out;
}
} // namespace pvz::build
