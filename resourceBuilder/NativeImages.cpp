#include "NativeBuild.h"
#include <algorithm>
#include <cstring>
#include <cstdio>
#include <setjmp.h>
#include <spng.h>
#include <jpeglib.h>

namespace pvz::build
{
namespace
{
void PngCheck(int code)
{
    Require(code == 0, std::string("PNG: ") + spng_strerror(code));
}
struct JpegError
{
    jpeg_error_mgr error;
    jmp_buf jump;
    char message[JMSG_LENGTH_MAX];
};
void JpegFailure(j_common_ptr state)
{
    auto *error = reinterpret_cast<JpegError *>(state->err);
    state->err->format_message(state, error->message);
    longjmp(error->jump, 1);
}
Image Decode(const fs::path &path)
{
    auto bytes = Read(path);
    Image result;
    if (bytes.size() >= 8 && bytes[0] == 137 && std::memcmp(bytes.data() + 1, "PNG\r\n\032\n", 7) == 0)
    {
        auto *context = spng_ctx_new(0);
        Require(context, "Cannot allocate PNG decoder");
        try
        {
            PngCheck(spng_set_png_buffer(context, bytes.data(), bytes.size()));
            spng_ihdr header{};
            PngCheck(spng_get_ihdr(context, &header));
            result.width = header.width;
            result.height = header.height;
            std::size_t size = 0;
            PngCheck(spng_decoded_image_size(context, SPNG_FMT_RGBA8, &size));
            result.rgba.resize(size);
            PngCheck(spng_decode_image(context, result.rgba.data(), size, SPNG_FMT_RGBA8, SPNG_DECODE_TRNS));
        }
        catch (...)
        {
            spng_ctx_free(context);
            throw;
        }
        spng_ctx_free(context);
    }
    else if (bytes.size() >= 2 && bytes[0] == 255 && bytes[1] == 216)
    {
        jpeg_decompress_struct decoder{};
        JpegError error{};
        decoder.err = jpeg_std_error(&error.error);
        error.error.error_exit = JpegFailure;
        if (setjmp(error.jump))
        {
            jpeg_destroy_decompress(&decoder);
            throw std::runtime_error(error.message);
        }
        jpeg_create_decompress(&decoder);
        jpeg_mem_src(&decoder, bytes.data(), static_cast<unsigned long>(bytes.size()));
        jpeg_read_header(&decoder, TRUE);
        decoder.out_color_space = JCS_EXT_RGBA;
        decoder.dct_method = JDCT_ISLOW;
        jpeg_start_decompress(&decoder);
        result.width = decoder.output_width;
        result.height = decoder.output_height;
        result.rgba.resize(std::size_t(result.width) * result.height * 4);
        while (decoder.output_scanline < decoder.output_height)
        {
            JSAMPROW row = result.rgba.data() + std::size_t(decoder.output_scanline) * result.width * 4;
            jpeg_read_scanlines(&decoder, &row, 1);
        }
        jpeg_finish_decompress(&decoder);
        jpeg_destroy_decompress(&decoder);
    }
    else
        Require(false, "Unsupported atlas source encoding: " + path.string() + " (use lossless PNG or JPEG)");
    Require(result.width && result.height &&
                result.rgba.size() == std::size_t(result.width) * result.height * 4,
            "Invalid decoded image extent");
    return result;
}
std::string Normalize(std::string path)
{
    std::replace(path.begin(), path.end(), '\\', '/');
    return path;
}
std::string Slash(std::string path)
{
    path = Normalize(path);
    while (!path.empty() && path.back() == '/')
        path.pop_back();
    return path;
}
Image ReadImage(const std::string &path, const Files &files, bool automatic = true, unsigned color = 0xffffff)
{
    auto name = Key(path);
    auto decode = [&](const std::string &stem) {
        Image image;
        std::vector<std::string> candidates;
        auto ext = fs::path(stem).extension().string();
        if (ext.empty())
            for (auto suffix : {".tga", ".jpg", ".png", ".gif"})
                candidates.push_back(stem + suffix);
        else
            candidates.push_back(stem);
        for (auto &candidate : candidates)
            if (files.contains(candidate))
            {
                image = Decode(files.at(candidate));
                image.files.insert(candidate);
                break;
            }
        return image;
    };
    auto image = decode(name);
    Image mask;
    if (automatic)
    {
        auto leading =
            (fs::path(name).parent_path() / ("_" + fs::path(name).filename().string())).generic_string();
        mask = decode(leading);
        if (mask.rgba.empty())
        {
            auto p = fs::path(name);
            mask = decode(
                (p.parent_path() / (p.stem().string() + "_" + p.extension().string())).generic_string());
        }
    }
    if (!mask.rgba.empty())
    {
        if (image.rgba.empty())
        {
            image.width = mask.width;
            image.height = mask.height;
            image.rgba.resize(mask.rgba.size());
            for (std::size_t i = 0; i < image.rgba.size(); i += 4)
            {
                image.rgba[i] = (color >> 16) & 255;
                image.rgba[i + 1] = (color >> 8) & 255;
                image.rgba[i + 2] = color & 255;
            }
        }
        Require(image.width == mask.width && image.height == mask.height,
                "Image alpha dimensions differ: " + path);
        for (std::size_t i = 0; i < image.rgba.size(); i += 4)
            image.rgba[i + 3] = mask.rgba[i + 2];
        image.files.insert(mask.files.begin(), mask.files.end());
    }
    Require(!image.rgba.empty(), "Definition image missing: " + path);
    image.logicalWidth = image.width;
    image.logicalHeight = image.height;
    return image;
}
} // namespace

std::map<std::string, ImageDescriptor> ImageDescriptors(const Json &author, const Files &files,
                                                        const std::set<std::string> &names)
{
    std::map<std::string, ImageDescriptor> result;
    std::string defaultPath, prefix;
    for (auto &group : author.at("manifest").at("children"))
        for (auto &node : group["children"])
        {
            const auto &a = node["attrs"];
            if (node["tag"] == "SetDefaults")
            {
                if (a.contains("path"))
                    defaultPath = Slash(a["path"].get<std::string>()) + "/";
                prefix = Slash(a.value("idprefix", prefix));
                continue;
            }
            if (node["tag"] != "Image")
                continue;
            auto original = Normalize(a["path"].get<std::string>()),
                 name = prefix + a.value("id", fs::path(original).stem().string());
            if (!names.contains(name))
                continue;
            Require(!result.contains(name), "Duplicate author image label " + name);
            ImageDescriptor d;
            d.path = original.starts_with('!') ? original : defaultPath + original;
            if (name == "IMAGE_PARTNER_LOGO")
                d.path = "images/blank";
            d.flags = a.contains("noalpha") || name == "IMAGE_EDITBOX" ? 0 : 1024;
            if (d.path == "!program")
            {
                d.path.clear();
                d.flags |= 1;
            }
            else
                Require(!d.path.starts_with('!'), "Unsupported author image control path");
            auto rows = std::stoll(a.value("rows", "1")), cols = std::stoll(a.value("cols", "1"));
            Require(rows > 0 && rows <= 0x7fffffff && cols > 0 && cols <= 0x7fffffff,
                    "Invalid author image grid");
            d.rows = unsigned(rows);
            d.cols = unsigned(cols);
            auto color = std::stoull(a.value("alphacolor", "FFFFFF"), nullptr, 16);
            Require(color <= 0xffffffffULL, "Invalid alpha color");
            d.color = unsigned(color);
            if (a.contains("alphaimage"))
                d.alpha = defaultPath + Normalize(a["alphaimage"].get<std::string>());
            if (a.contains("alphagrid"))
                d.alphaGrid = defaultPath + Normalize(a["alphagrid"].get<std::string>());
            result[name] = d;
        }
    std::map<std::string, std::string> stems;
    for (auto &[name, path] : files)
    {
        auto ext = fs::path(name).extension().string();
        if (ext == ".png" || ext == ".jpg" || ext == ".jpeg" || ext == ".bmp" || ext == ".gif")
            stems.try_emplace(fs::path(name).replace_extension().generic_string(), name);
    }
    for (auto &name : names)
        if (!result.contains(name))
        {
            ImageDescriptor d;
            auto normalized = Normalize(name);
            if (normalized.find('/') != normalized.npos)
            {
                Require(files.contains(Key(normalized)), "Invalid direct author image path " + name);
                d.path = normalized;
            }
            else
            {
                bool reanim = name.starts_with("IMAGE_REANIM_");
                auto suffix = reanim                              ? name.substr(13)
                              : Upper(name).starts_with("IMAGE_") ? name.substr(6)
                                                                  : "";
                for (auto &directory : reanim ? std::vector<std::string>{"reanim", "images", "particles"}
                                              : std::vector<std::string>{"particles", "images", "reanim"})
                {
                    auto candidate = Lower(directory + "/" + suffix);
                    if (stems.contains(candidate))
                    {
                        d.path = fs::path(stems.at(candidate)).replace_extension().generic_string();
                        break;
                    }
                }
                if (d.path.empty())
                {
                    Require(name == "IMAGE_PROJECTILE" || name == "IMAGE_SUN",
                            "Author image label has no file path: " + name);
                    d.flags = 8192;
                }
                else
                    d.flags = 1024;
            }
            result[name] = d;
        }
    return result;
}

Image DecodeImage(const ImageDescriptor &d, const Files &files)
{
    Require(!(d.flags & (1 | 8192)), "Unavailable definition image: " + d.path);
    auto image =
        ReadImage(d.path, files, (d.flags & 1024) && d.alpha.empty() && d.alphaGrid.empty(), d.color);
    for (auto [path, cols, rows] : std::vector<std::tuple<std::string, unsigned, unsigned>>{
             {d.alpha, 1, 1}, {d.alphaGrid, d.cols, d.rows}})
    {
        if (path.empty())
            continue;
        auto mask = ReadImage(path, files);
        Require(mask.width == image.width / cols && mask.height == image.height / rows,
                "Explicit alpha dimensions differ: " + path);
        for (unsigned row = 0; row < rows; ++row)
            for (unsigned col = 0; col < cols; ++col)
                for (unsigned y = 0; y < mask.height; ++y)
                    for (unsigned x = 0; x < mask.width; ++x)
                        image.rgba[(std::size_t(row * mask.height + y) * image.width + col * mask.width + x) *
                                       4 +
                                   3] = mask.rgba[(std::size_t(y) * mask.width + x) * 4 + 2];
        image.files.insert(mask.files.begin(), mask.files.end());
    }
    if(d.logicalWidth && d.logicalHeight)
    {
        image.logicalWidth=d.logicalWidth;
        image.logicalHeight=d.logicalHeight;
    }
    if (!d.highResolution.empty())
    {
        // Replacement PNGs already include their alpha. Applying the old alpha image
        // would either reject the new extent or destroy the high-resolution edge.
        auto replacement = Decode(d.highResolution);
        Require(replacement.width >= image.width && replacement.height >= image.height,
                "High-resolution replacement is smaller than its logical canvas: " + d.path);
        Require(replacement.width % d.cols == 0 && replacement.height % d.rows == 0,
                "High-resolution replacement does not preserve the image grid: " + d.path);
        replacement.logicalWidth = image.logicalWidth;
        replacement.logicalHeight = image.logicalHeight;
        replacement.files = std::move(image.files);
        image = std::move(replacement);
    }
    return image;
}

void ApplyHighResolutionImages(const fs::path &root, std::map<std::string, ImageDescriptor> &descriptors)
{
    const auto manifest = root / "assets/high-resolution/images.json";
    if (!fs::exists(manifest))
        return;
    const auto document = ReadJson(manifest);
    Require(document.at("format") == "pvz-high-resolution-images-v1", "Invalid high-resolution image manifest");
    const auto directory = fs::weakly_canonical(root / "assets/high-resolution");
    for (auto &[name, descriptor] : descriptors)
    {
        if (descriptor.path.empty() || (descriptor.flags & (1u | 8192u)))
            continue;
        auto key = Key(descriptor.path);
        if (!document.at("images").contains(key))
            continue;
        const auto path = fs::weakly_canonical(directory / document["images"][key].get<std::string>());
        const auto relative = path.lexically_relative(directory);
        Require(!relative.empty() && *relative.begin() != ".." && fs::is_regular_file(path),
                "High-resolution image must be a file inside assets/high-resolution: " + key);
        descriptor.highResolution = path;
    }
}

Bytes EncodePng(const Image &image)
{
    auto *context = spng_ctx_new(SPNG_CTX_ENCODER);
    Require(context, "Cannot allocate PNG encoder");
    Bytes result;
    try
    {
        PngCheck(spng_set_option(context, SPNG_ENCODE_TO_BUFFER, 1));
        PngCheck(spng_set_option(context, SPNG_IMG_COMPRESSION_LEVEL, 9));
        spng_ihdr header{};
        header.width = image.width;
        header.height = image.height;
        header.bit_depth = 8;
        header.color_type = SPNG_COLOR_TYPE_TRUECOLOR_ALPHA;
        PngCheck(spng_set_ihdr(context, &header));
        PngCheck(spng_encode_image(context, image.rgba.data(), image.rgba.size(), SPNG_FMT_PNG,
                                   SPNG_ENCODE_FINALIZE));
        std::size_t size = 0;
        int error = 0;
        auto *buffer = static_cast<unsigned char *>(spng_get_png_buffer(context, &size, &error));
        PngCheck(error);
        result.assign(buffer, buffer + size);
        std::free(buffer);
    }
    catch (...)
    {
        spng_ctx_free(context);
        throw;
    }
    spng_ctx_free(context);
    return result;
}

Image PackImages(const std::vector<const Image *> &images, const Json &layout)
{
    Image out;
    out.width = layout["width"];
    out.height = layout["height"];
    out.rgba.resize(std::size_t(out.width) * out.height * 4);
    Require(images.size() == layout["regions"].size(), "Image/layout count differs");
    for (std::size_t i = 0; i < images.size(); ++i)
    {
        const auto &source = *images[i];
        const auto &r = layout["regions"][i];
        int x = r[0], y = r[1], w = r[2], h = r[3];
        Require(w == int(source.width) && h == int(source.height) && x >= 2 && y >= 2 &&
                    x + w + 2 <= int(out.width) && y + h + 2 <= int(out.height),
                "Atlas region outside image");
        for (int dy = -2; dy < h + 2; ++dy)
            for (int dx = -2; dx < w + 2; ++dx)
            {
                auto from = (std::size_t(std::clamp(dy, 0, h - 1)) * w + std::clamp(dx, 0, w - 1)) * 4;
                auto to = (std::size_t(y + dy) * out.width + x + dx) * 4;
                std::memcpy(out.rgba.data() + to, source.rgba.data() + from, 4);
            }
    }
    return out;
}
} // namespace pvz::build
