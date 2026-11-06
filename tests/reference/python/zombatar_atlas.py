"""Compile the authored Zombatar image list into native atlas rectangles."""
from __future__ import annotations
from game_fixture import game_root

import hashlib
import re

import build_numeric_runtime_pak64 as pak
import compile_numeric_resources as resources
import definition_atlas
from projectile_atlas import image_path
from static_definition_codegen import cpp_string, texture_region

FORMAT = "pvz-zombatar-sources-v1"
# A build-graph identity only: no compiled metadata record is published.
PATH = "compiled/zombatar/zombatar.compiled"
IMAGE_COUNT = 209


def validate_sources(document: dict) -> list[str]:
    pak.require(isinstance(document, dict)
                and set(document) == {"format", "name", "images"}
                and document["format"] == FORMAT and document["name"] == "Zombatar",
                "Expected the authored Zombatar resource definition")
    rows = document["images"]
    pak.require(isinstance(rows, list) and len(rows) == IMAGE_COUNT,
                "Zombatar requires the frozen 209 editor and portrait images")
    names, images = set(), set()
    for row in rows:
        pak.require(isinstance(row, dict) and set(row) == {"name", "image", "auto_alpha"},
                    "Invalid Zombatar source image")
        name = row["name"]
        pak.require(isinstance(name, str) and re.fullmatch(r"[A-Z][A-Z0-9_]*", name)
                    and name not in names and name not in {"Count", "None"},
                    f"Invalid or duplicate Zombatar image name: {name}")
        pak.require(type(row["auto_alpha"]) is bool, f"Invalid Zombatar alpha policy: {name}")
        path = image_path(row["image"])
        pak.require(path not in images, f"Duplicate Zombatar PNG source: {path}")
        names.add(name)
        images.add(path)
    return sorted(images)


def descriptors(document: dict) -> dict:
    validate_sources(document)
    return {
        row["image"]: definition_atlas.ImageDescriptor(
            row["image"], row["image"], "", "", resources.AUTO_ALPHA if row["auto_alpha"] else 0,
            1, 1, 0xFFFFFF)
        for row in document["images"]
    }


def compile_definition(document: dict, sources: dict, files: dict,
                       layout: dict, atlas_symbols: list[str], atlas_path: str) -> dict:
    symbols = validate_sources(document)
    pak.require(bool(atlas_path) and set(symbols) <= set(atlas_symbols),
                "Zombatar atlas is missing authored source images")
    regions = dict(zip(atlas_symbols, layout["regions"]))
    images, source_files = [], set()
    for row in document["images"]:
        source = row["image"]
        image, dependencies = definition_atlas.logical_image(sources[source], files)
        rectangle = regions[source]
        pak.require(tuple(rectangle[2:]) == image.size,
                    f"Zombatar atlas changed source dimensions: {row['name']}")
        source_files.update(dependencies)
        images.append({
            "name": row["name"], "source": source, "width": image.width, "height": image.height,
            "rows": 1, "cols": 1, "rgba_bytes": image.width * image.height * 4,
            "rgba_sha256": hashlib.sha256(image.tobytes()).hexdigest(),
            "files": sorted(dependencies), "region": (*rectangle, 1, 1),
        })
    return {
        "atlas_path": atlas_path, "atlas_width": layout["width"],
        "atlas_height": layout["height"], "atlas_shared": True,
        "region_count": len(images), "images": images, "source_files": sorted(source_files),
    }


def generate(document: dict, report: dict) -> dict[str, bytes]:
    """Emit the dense enum, static geometry and shared-registry loader."""
    validate_sources(document)
    names = [row["name"] for row in document["images"]]
    pak.require(names == [row["name"] for row in report["images"]],
                "Zombatar report order differs from the authored enum")
    enum = [
        "// Generated from authored Zombatar sources. Do not edit.",
        "#pragma once", "#include <cstddef>", "#include <cstdint>",
        "enum class ZombatarImage : std::uint16_t {",
        *(f"    {name} = {index}," for index, name in enumerate(names)),
        f"    Count = {len(names)},", "    None = Count", "};",
    ]
    width, height = report["atlas_width"], report["atlas_height"]
    rectangles = [
        "        " + texture_region(row["region"][:4], width, height) + ","
        for row in report["images"]
    ]
    cpp = [
        "// Generated static Zombatar atlas and image ownership bindings. Do not edit.",
        '#include "Lawn/Widget/ZombatarResources.h"',
        '#include "Sexy.TodLib/DefinitionImageRegion.h"',
        "constinit ZombatarAtlasDefinition gZombatarAtlas {",
        "    {{", *rectangles, "    }}, nullptr", "};",
        f"static_assert(static_cast<std::size_t>(ZombatarImage::Count) == {len(names)});",
        "void ZombatarLoadResources() {",
        f"    DefinitionBindAtlasPath(gZombatarAtlas.mTexture, {width}, {height}, "
        f"{cpp_string(report['atlas_path'])});",
        "}", "void ZombatarFreeResources() {",
        "    DefinitionUnbindAtlas(gZombatarAtlas.mTexture);", "}",
    ]
    return {
        "ZombatarImage.h": ("\n".join(enum) + "\n").encode(),
        "ZombatarAtlasData.cpp": ("\n".join(cpp) + "\n").encode(),
    }
