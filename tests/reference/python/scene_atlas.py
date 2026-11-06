"""Offline scene atlas authoring: pack entire decoded sheets, translate views."""
from __future__ import annotations
from game_fixture import game_root

from dataclasses import replace
import hashlib
import json
import math
from pathlib import Path, PurePosixPath
import re

import build_numeric_runtime_pak64 as pak
import definition_atlas
from static_definition_codegen import cpp_float, cpp_string, texture_region

MODULES = ("Magnet", "Almanac", "Achievements", "DesktopUI", "LandscapeUI", "CommonUI")
COMMON_UI_NAMES = (
    "BUTTON_LEFT", "BUTTON_MIDDLE", "BUTTON_RIGHT",
    "BUTTON_DOWN_LEFT", "BUTTON_DOWN_MIDDLE", "BUTTON_DOWN_RIGHT",
    "DIALOG_TOPLEFT", "DIALOG_TOPMIDDLE", "DIALOG_TOPRIGHT",
    "DIALOG_CENTERLEFT", "DIALOG_CENTERMIDDLE", "DIALOG_CENTERRIGHT",
    "DIALOG_BOTTOMLEFT", "DIALOG_BOTTOMMIDDLE", "DIALOG_BOTTOMRIGHT",
    "DIALOG_BIGBOTTOMLEFT", "DIALOG_BIGBOTTOMMIDDLE", "DIALOG_BIGBOTTOMRIGHT", "DIALOG_HEADER",
    "SEEDCHOOSER_BUTTON", "SEEDCHOOSER_BUTTON_DISABLED", "SEEDCHOOSER_BUTTON_GLOW",
    "SEEDCHOOSER_BUTTON2", "SEEDCHOOSER_BUTTON2_GLOW",
)
TOKEN = re.compile(r'\s+|//[^\n]*|\#[^\n]*|/\*.*?\*/|"(?:\\.|[^"\\])*"|'
                   r'[A-Za-z_]\w*|-?(?:\d+(?:\.\d*)?|\.\d+)(?:[eE][+-]?\d+)?|[{}:;,\[\]]', re.S)


def parse(text: str) -> dict:
    tokens, at = [], 0
    while at < len(text):
        match = TOKEN.match(text, at)
        pak.require(match is not None, f"Invalid scene DSL at offset {at}")
        token = match[0]
        at = match.end()
        if not token.isspace() and not token.startswith(("//", "/*", "#")):
            tokens.append(token)
    position = 0

    def take(expected=None):
        nonlocal position
        pak.require(position < len(tokens), "Truncated scene DSL")
        token = tokens[position]
        position += 1
        pak.require(expected is None or token == expected, f"Expected {expected}, got {token}")
        return token

    take("export")
    take("atlas")
    name = take()
    pak.require(name in MODULES[:-1], f"Unsupported authored scene module: {name}")
    take("{")
    sprites, names = [], set()
    while position < len(tokens) and tokens[position] != "}":
        take("sprite")
        sprite = take().removeprefix("IMAGE_")
        pak.require(re.fullmatch(r"[A-Za-z_]\w*", sprite) is not None and sprite not in names
                    and sprite not in {"Count", "None"}, f"Invalid or duplicate scene sprite: {sprite}")
        names.add(sprite)
        take("{")
        attrs = {}
        while position < len(tokens) and tokens[position] != "}":
            key = take()
            pak.require(key in {"image", "rect", "scale"} and key not in attrs,
                        f"Invalid or duplicate scene sprite field: {key}")
            take(":")
            if key == "image":
                value = take()
                pak.require(value.startswith('"'), "Scene image requires a quoted path or author label")
                attrs[key] = json.loads(value)
            elif key == "rect":
                take("[")
                values = []
                for index in range(4):
                    value = take()
                    pak.require(re.fullmatch(r"-?\d+", value) is not None, "Scene rect requires four integers")
                    values.append(int(value))
                    if index != 3:
                        take(",")
                take("]")
                pak.require(min(values[:2]) >= 0 and min(values[2:]) > 0,
                            "Scene rectangle must have positive extent and nonnegative origin")
                attrs[key] = values
            else:
                value = float(take())
                pak.require(math.isfinite(value) and value > 0, "Scene scale must be finite and positive")
                attrs[key] = value
            take(";")
        take("}")
        pak.require(bool(attrs.get("image")), f"Scene sprite has no image: {sprite}")
        pak.require(name == "Magnet" or attrs.get("scale", 1.0) == 1.0,
                    "Per-sprite scale is supported by Magnet; UI layout uses its existing draw sizes")
        source = attrs["image"]
        if not source.startswith("IMAGE_"):
            pak.file_key(source)
            pak.require(Path(source).suffix.casefold() in {".png", ".jpg", ".jpeg", ".gif", ".bmp"},
                        "Scene source must name an image file")
        sprites.append({"name": sprite, **attrs, "scale": attrs.get("scale", 1.0)})
    take("}")
    pak.require(position == len(tokens) and 0 < len(sprites) < 65536, "Empty or trailing scene DSL")
    return {"name": name, "sprites": sprites}


def common_ui() -> dict:
    return {"name": "CommonUI", "sprites": [
        {"name": name, "image": "IMAGE_" + name, "scale": 1.0} for name in COMMON_UI_NAMES] + [
        {"name": "TOUCH_FRAME_" + name, "image": "IMAGE_TOUCH_SELECTION_FRAME_BLUE",
         "rect": rect, "scale": 1.0} for name, rect in (
            ("TL", [0, 0, 42, 36]), ("TR", [62, 0, 42, 36]),
            ("BL", [0, 108, 42, 36]), ("BR", [62, 108, 42, 36]))]}


def record_path(module: str) -> str:
    return f"compiled/scene/{module.casefold()}.compiled"


def prepare(documents: list[dict], authoring: bytes, files: dict) -> tuple[dict, dict]:
    """Return per-module documents and unique full-sheet decode descriptors."""
    from compile_numeric_resources import read_authoring
    labels, prefix = set(), ""
    for group in read_authoring(authoring)["children"]:
        for node in group["children"]:
            attrs = node["attrs"]
            if node["tag"] == "SetDefaults":
                prefix = attrs.get("idprefix", prefix).rstrip("/")
            elif node["tag"] == "Image":
                labels.add(prefix + attrs.get("id", PurePosixPath(attrs["path"].replace("\\", "/")).stem))
    authored = definition_atlas.authoring_descriptors(authoring, files, labels)
    paths = {}
    for descriptor in authored.values():
        if descriptor.path:
            paths.setdefault(pak.file_key(descriptor.path), []).append(descriptor)
    modules, sources = {}, {}
    for document in documents:
        module = document["name"]
        pak.require(module in MODULES and record_path(module) not in modules,
                    f"Duplicate scene module: {module}")
        rows = []
        for sprite in document["sprites"]:
            source = sprite["image"]
            if source.startswith("IMAGE_"):
                pak.require(source in authored, f"Unknown scene author image label: {source}")
                descriptor = authored[source]
            else:
                normalized = pak.file_key(source)
                stem = str(Path(normalized).with_suffix("")).replace("\\", "/")
                candidates = paths.get(normalized, paths.get(stem, []))
                recipes = {(d.path, d.alpha, d.alpha_grid, d.flags, d.color) for d in candidates}
                pak.require(len(recipes) <= 1, f"Ambiguous scene source decode recipe: {source}")
                descriptor = (candidates[0] if candidates else
                              definition_atlas.ImageDescriptor(source, source, "", "", 1024, 1, 1, 0xFFFFFF))
            pak.require(not descriptor.flags & (1 | 8192), f"Unavailable scene source: {source}")
            key = "scene:" + pak.file_key(descriptor.path)
            pak.require(not descriptor.alpha_grid or (descriptor.rows == 1 and descriptor.cols == 1),
                        "Scene source requires an offline expanded full-size alpha mask")
            descriptor = replace(descriptor, source_label=key, rows=1, cols=1)
            pak.require(key not in sources or sources[key] == descriptor,
                        f"Conflicting scene decode recipes for sheet: {descriptor.path}")
            sources[key] = descriptor
            rows.append({**sprite, "source": key})
        modules[record_path(module)] = {"name": module, "sprites": rows}
    return modules, sources


def compile_definition(document: dict, sources: dict, files: dict, layout: dict,
                       source_order: list[str], path: str) -> dict:
    regions = dict(zip(source_order, layout["regions"]))
    images, sizes = [], {}
    for source in sorted({row["source"] for row in document["sprites"]}):
        image, used = definition_atlas.logical_image(sources[source], files)
        sizes[source] = image.size
        region = regions[source]
        pak.require(tuple(region[2:]) == image.size, "Scene packing must retain the entire source sheet")
        images.append({"source": source, "width": image.width, "height": image.height,
                       "rows": 1, "cols": 1, "rgba_bytes": image.width * image.height * 4,
                       "rgba_sha256": hashlib.sha256(image.tobytes()).hexdigest(),
                       "files": sorted(used), "region": list(region)})
    sprites = []
    for row in document["sprites"]:
        width, height = sizes[row["source"]]
        x, y, w, h = row.get("rect", [0, 0, width, height])
        pak.require(x >= 0 and y >= 0 and w > 0 and h > 0 and x + w <= width and y + h <= height,
                    f"Scene sprite outside its full source sheet: {row['name']}")
        origin = regions[row["source"]]
        sprites.append({**row, "source_rect": [x, y, w, h],
                        "region": [origin[0] + x, origin[1] + y, w, h]})
    return {"name": document["name"], "atlas_path": path,
            "atlas_width": layout["width"], "atlas_height": layout["height"],
            "atlas_shared": True, "region_count": len(images),
            "source_files": sorted({filename for row in images for filename in row["files"]}),
            "source_rgba_bytes": sum(row["rgba_bytes"] for row in images),
            "atlas_rgba_bytes": layout["width"] * layout["height"] * 4,
            "padded_region_rgba_bytes": sum((row["width"] + 4) * (row["height"] + 4) * 4
                                           for row in images),
            "images": images, "sprites": sprites, "packing": layout}


def generate(reports: dict) -> dict[str, bytes]:
    header = ["// Generated scene atlas geometry and direct texture bindings.", "#pragma once",
              '#include "SexyAppFramework/graphics/GpuBackend.h"',
              '#include "SexyAppFramework/graphics/TextureRegion.h"', "#include <cstddef>", "#include <cstdint>"]
    cpp = ['#include "SceneAtlasData.h"', '#include "Sexy.TodLib/DefinitionImageRegion.h"']
    extents, sheets = [], []
    for module in MODULES:
        report = reports[record_path(module)]
        rows = report["sprites"]
        width, height = report["atlas_width"], report["atlas_height"]
        namespace = module + "Atlas"
        header += [f"namespace {namespace} {{", "extern Sexy::GpuTexture* Texture;",
                   "enum class Sprite : std::uint16_t {",
                   *(f'    {row["name"]} = {i},' for i, row in enumerate(rows)),
                   f"    Count = {len(rows)},", "};",
                   f"inline constexpr std::size_t Count = {len(rows)};",
                   "extern const Sexy::TextureRegion Images[Count];"]
        cpp += [f"namespace {namespace} {{", "Sexy::GpuTexture* Texture = nullptr;",
                "const Sexy::TextureRegion Images[Count] = {",
                *("    " + texture_region(row["region"], width, height) + "," for row in rows), "};"]
        if module == "Achievements":
            indexed = [(int(match[1]), row) for row in rows
                       if (match := re.fullmatch(r"ICON_(\d+)", row["name"]))]
            icons = dict(indexed)
            pak.require(len(icons) == len(indexed) and set(icons) == set(range(len(icons))),
                        "Achievement ICON indices must be unique and contiguous from zero")
            header.append(f"inline constexpr std::size_t IconCount = {len(icons)};")
            if icons:
                header.append("extern const Sexy::TextureRegion Icons[IconCount];")
                cpp += ["const Sexy::TextureRegion Icons[IconCount] = {",
                        *("    " + texture_region(icons[index]["region"], width, height) + ","
                          for index in range(len(icons))), "};"]
        if any(row["scale"] != 1.0 for row in rows):
            header.append("extern const float Scales[Count];")
            cpp += ["const float Scales[Count] = {",
                    "    " + ", ".join(cpp_float(row["scale"]) for row in rows), "};"]
        header += ["void Load();", "void Free();", "}"]
        cpp += ["void Load() {",
                f'    DefinitionBindAtlasPath(Texture, {width}, {height}, {cpp_string(report["atlas_path"])});',
                "}", "void Free() { DefinitionUnbindAtlas(Texture); }", "}"]
        extents.append(f'    {{{cpp_string(report["atlas_path"])}, {report["atlas_width"]}u, {report["atlas_height"]}u}},')
        for image in report["images"]:
            x, y, width, height = image["region"]
            sheets.append("    {" + ", ".join([
                cpp_string(report["atlas_path"]), cpp_string(image["source"]),
                f"{x}u", f"{y}u", f"{width}u", f"{height}u",
                cpp_string(image["rgba_sha256"])]) + "},")
    header += ["void LoadSceneAtlases(bool landscapeUI);", "void FreeSceneAtlases();"]
    cpp += ["void LoadSceneAtlases(bool landscapeUI) {",
            *(f"    {module}Atlas::Load();" for module in MODULES if module not in {"DesktopUI", "LandscapeUI"}),
            "    if (landscapeUI) LandscapeUIAtlas::Load();",
            "    else DesktopUIAtlas::Load();", "}",
            "void FreeSceneAtlases() {", *(f"    {module}Atlas::Free();" for module in reversed(MODULES)), "}"]
    audit = ["// Offline scene extent audit; not included by the game.", "#pragma once", "#include <array>",
             "namespace SceneAtlasBindings {", "struct Extent { const char* image; unsigned width,height; };",
             f"inline constexpr std::array<Extent, {len(extents)}> Extents{{{{", *extents, "}};",
             "struct Sheet { const char* image; const char* source; unsigned x,y,width,height; const char* rgbaSha256; };",
             f"inline constexpr std::array<Sheet, {len(sheets)}> Sheets{{{{", *sheets, "}};", "}"]
    return {"SceneAtlasData.h": ("\n".join(header) + "\n").encode(),
            "SceneAtlasData.cpp": ("\n".join(cpp) + "\n").encode(),
            "SceneAtlasBindings.h": ("\n".join(audit) + "\n").encode()}
