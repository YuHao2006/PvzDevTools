#include "NativeBuild.h"
#include <algorithm>
#include <atomic>
#include <mutex>
#include <thread>
#include <zlib.h>
#include <spng.h>

namespace pvz::build
{
namespace
{
unsigned Big(std::span<const unsigned char> b, std::size_t at)
{
    Require(at + 4 <= b.size(), "Truncated PNG chunk");
    return unsigned(b[at]) << 24 | unsigned(b[at + 1]) << 16 | unsigned(b[at + 2]) << 8 | b[at + 3];
}
void BigWord(Bytes &b, unsigned v)
{
    for (int i = 3; i >= 0; --i)
        b.push_back(static_cast<unsigned char>(v >> (i * 8)));
}
using Chunk = std::pair<std::string, Bytes>;
std::vector<Chunk> Chunks(const Bytes &bytes)
{
    Require(bytes.size() >= 8 && bytes[0] == 137 && bytes[1] == 'P' && bytes[2] == 'N' && bytes[3] == 'G',
            "Invalid PNG signature");
    std::vector<Chunk> result;
    std::size_t at = 8;
    while (at < bytes.size())
    {
        Require(bytes.size() - at >= 12, "Truncated PNG chunk header");
        auto size = Big(bytes, at);
        Require(size <= bytes.size() - at - 12, "Truncated PNG chunk payload");
        std::string kind(bytes.begin() + at + 4, bytes.begin() + at + 8);
        Require(Crc(std::span(bytes).subspan(at + 4, size + 4)) == Big(bytes, at + 8 + size),
                "Invalid PNG chunk CRC");
        result.emplace_back(kind, Bytes(bytes.begin() + at + 8, bytes.begin() + at + 8 + size));
        at += size + 12;
    }
    Require(!result.empty() && result.front().first == "IHDR" && result.back().first == "IEND",
            "Incomplete PNG");
    return result;
}
Bytes PixelStream(const std::vector<Chunk> &chunks)
{
    Bytes result;
    for (auto &[kind, data] : chunks)
        if (kind == "IDAT")
            Append(result, data);
    return result;
}
Bytes Replace(const Bytes &original, const std::vector<Chunk> &chunks, const Bytes &compressed)
{
    Bytes out(original.begin(), original.begin() + 8);
    bool emitted = false;
    for (auto &[kind, data] : chunks)
    {
        if (kind == "IDAT" && emitted)
            continue;
        const auto &payload = kind == "IDAT" ? compressed : data;
        if (kind == "IDAT")
            emitted = true;
        BigWord(out, unsigned(payload.size()));
        auto start = out.size();
        out.insert(out.end(), kind.begin(), kind.end());
        Append(out, payload);
        BigWord(out, Crc(std::span(out).subspan(start)));
    }
    return out;
}
Bytes Inflate(const Bytes &bytes)
{
    z_stream stream{};
    Require(inflateInit(&stream) == Z_OK, "Cannot initialize PNG inflater");
    stream.next_in = const_cast<unsigned char *>(bytes.data());
    stream.avail_in = unsigned(bytes.size());
    Bytes out;
    int status = Z_OK;
    do
    {
        std::array<unsigned char, 65536> block;
        stream.next_out = block.data();
        stream.avail_out = unsigned(block.size());
        status = inflate(&stream, Z_NO_FLUSH);
        out.insert(out.end(), block.begin(), block.begin() + block.size() - stream.avail_out);
    } while (status == Z_OK);
    inflateEnd(&stream);
    Require(status == Z_STREAM_END, "Invalid PNG deflate data");
    return out;
}
Bytes Deflate(const Bytes &bytes, int strategy)
{
    z_stream stream{};
    Require(deflateInit2(&stream, 9, Z_DEFLATED, 15, 9, strategy) == Z_OK,
            "Cannot initialize PNG compressor");
    stream.next_in = const_cast<unsigned char *>(bytes.data());
    stream.avail_in = unsigned(bytes.size());
    Bytes out;
    int status = Z_OK;
    do
    {
        std::array<unsigned char, 65536> block;
        stream.next_out = block.data();
        stream.avail_out = unsigned(block.size());
        status = deflate(&stream, Z_FINISH);
        out.insert(out.end(), block.begin(), block.begin() + block.size() - stream.avail_out);
    } while (status == Z_OK);
    deflateEnd(&stream);
    Require(status == Z_STREAM_END, "PNG compression failed");
    return out;
}
Bytes Optimize(const std::string &name, const Bytes &source, std::string_view mode)
{
    auto chunks = Chunks(source);
    for (auto &[kind, data] : chunks)
        if (kind == "acTL")
            return source;
    auto raw = Inflate(PixelStream(chunks));
    Bytes best = source;
    for (int strategy : {Z_DEFAULT_STRATEGY, Z_FILTERED})
    {
        auto candidate = Replace(source, chunks, Deflate(raw, strategy));
        if (candidate.size() < best.size())
            best = std::move(candidate);
    }
    // Preserve palette, color keys and every metadata chunk. Only RGBA8 images
    // can use this encoder without changing their IHDR/color representation.
    if (chunks[0].second.size() == 13 && chunks[0].second[8] == 8 && chunks[0].second[9] == 6)
    {
        auto *context = spng_ctx_new(0);
        Require(context, "Cannot allocate PNG decoder");
        Image image;
        int error = spng_set_png_buffer(context, source.data(), source.size());
        std::size_t size = 0;
        if (!error)
            error = spng_decoded_image_size(context, SPNG_FMT_RGBA8, &size);
        if (!error)
        {
            image.rgba.resize(size);
            error = spng_decode_image(context, image.rgba.data(), size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS);
        }
        spng_ctx_free(context);
        Require(!error, "Cannot decode compact PNG");
        image.width = Big(chunks[0].second, 0);
        image.height = Big(chunks[0].second, 4);
        auto attempt = [&] {
            auto encoded = Chunks(EncodePng(image));
            if (encoded[0] == chunks[0])
            {
                auto candidate = Replace(source, chunks, PixelStream(encoded));
                if (candidate.size() < best.size())
                    best = std::move(candidate);
            }
        };
        attempt();
        if (mode == "light-lossy" && name.starts_with("compiled/") && name.ends_with(".atlas.png"))
        {
            for (std::size_t i = 0; i < image.rgba.size(); ++i)
                if (i % 4 != 3)
                    image.rgba[i] =
                        static_cast<unsigned char>(std::min(255u, ((unsigned(image.rgba[i]) + 2) / 4) * 4));
            attempt();
        }
    }
    return best;
}
Records ReadStoredZip(const fs::path &path)
{
    auto bytes = Read(path);
    Reader r(bytes);
    Records records;
    while (r.at + 4 <= r.data.size() && GetWord(r.data, r.at) == 0x04034b50)
    {
        r.Skip(4);
        auto u16 = [&] {
            auto b = r.Take(2);
            return unsigned(b[0]) | (unsigned(b[1]) << 8);
        };
        u16();
        auto flags = u16(), method = u16();
        u16();
        u16();
        auto crc = r.Word(), packed = r.Word(), size = r.Word();
        auto length = u16(), extra = u16();
        auto nameBytes = r.Take(length);
        std::string name(nameBytes.begin(), nameBytes.end());
        Require(flags == 0 && method == 0 && packed == size,
                "Compact input must be a deterministic stored PAK64");
        r.Skip(extra);
        auto data = r.Take(size);
        Require(Crc(data) == crc && Key(name) == name && records.emplace(name, data).second,
                "Invalid/duplicate PAK64 entry");
    }
    Require(r.at + 4 <= r.data.size() && GetWord(r.data, r.at) == 0x02014b50,
            "Missing PAK64 central directory");
    return records;
}
} // namespace
void Compact(const fs::path &source, const fs::path &output, std::string_view mode, unsigned workers)
{
    Require(fs::absolute(source) != fs::absolute(output), "Compact output must differ from source package");
    Require((mode == "lossless" || mode == "light-lossy") && workers >= 1 && workers <= 8,
            "Invalid compact settings");
    auto records = ReadStoredZip(source);
    std::vector<std::pair<std::string, Bytes *>> items;
    for (auto &[name, data] : records)
        items.emplace_back(name, &data);
    std::atomic_size_t next = 0;
    std::exception_ptr failure;
    std::mutex mutex;
    Json rows = Json::array();
    std::vector<Json> results(items.size());
    {
        std::vector<std::jthread> threads;
        for (unsigned worker = 0; worker < workers; ++worker)
            threads.emplace_back([&] {
                try
                {
                    while (true)
                    {
                        auto index = next.fetch_add(1);
                        if (index >= items.size())
                            break;
                        auto &[name, bytes] = items[index];
                        auto original = bytes->size();
                        auto hash = Sha256(*bytes);
                        if (name.ends_with(".png"))
                            *bytes = Optimize(name, *bytes, mode);
                        results[index] = {{"path", name},
                                          {"before", original},
                                          {"after", bytes->size()},
                                          {"source_sha256", hash},
                                          {"output_sha256", Sha256(*bytes)}};
                    }
                }
                catch (...)
                {
                    std::lock_guard lock(mutex);
                    if (!failure)
                        failure = std::current_exception();
                }
            });
    }
    if (failure)
        std::rethrow_exception(failure);
    for (auto &row : results)
        rows.push_back(row);
    Zip(output, records);
    Json report = {{"format", "pvz-compact-package-v1"},
                   {"mode", mode},
                   {"source", source.string()},
                   {"output", output.string()},
                   {"source_bytes", fs::file_size(source)},
                   {"output_bytes", fs::file_size(output)},
                   {"files", rows}};
    Publish(output.string() + ".manifest.json", report.dump(2) + "\n");
}
} // namespace pvz::build
