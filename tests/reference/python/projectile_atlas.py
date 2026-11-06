"""Compile projectile source paths into one atlas and dense runtime rectangles."""
from __future__ import annotations
from game_fixture import game_root

import hashlib
import math
from pathlib import PurePosixPath
import struct

import build_numeric_runtime_pak64 as pak
import definition_atlas

FORMAT = "pvz-projectile-sources-v1"
PATH = "compiled/projectiles/projectiles.compiled"
MAGIC = 0x314A5250
NAMES = (
    "PROJECTILE_PEA", "PROJECTILE_SNOWPEA", "PROJECTILE_CABBAGE",
    "PROJECTILE_MELON", "PROJECTILE_PUFF", "PROJECTILE_WINTERMELON",
    "PROJECTILE_FIREBALL", "PROJECTILE_STAR", "PROJECTILE_SPIKE",
    "PROJECTILE_BASKETBALL", "PROJECTILE_KERNEL", "PROJECTILE_COBBIG",
    "PROJECTILE_BUTTER", "PROJECTILE_ZOMBIE_PEA",
)


def validate_sources(document: dict) -> list[str]:
    """Validate the compiler interchange, independently of the runtime registry."""
    pak.require(isinstance(document, dict)
                and set(document) == {"format", "name", "projectiles", "shadows"}
                and document["format"] == FORMAT and document["name"] == "Projectiles",
                "Expected the Projectiles projectile resource definition")
    rows, shadows = document["projectiles"], document["shadows"]
    pak.require(isinstance(rows, list) and len(rows) == len(NAMES)
                and isinstance(shadows, list) and len(shadows) == 2,
                "Projectile resource requires fourteen types and two shadows")
    images = set()
    for index, (row, name) in enumerate(zip(rows, NAMES)):
        pak.require(isinstance(row, dict) and set(row) == {"name", "image", "scale"}
                    and row["name"] == name,
                    f"Projectile source order differs at {name}")
        scale = row["scale"]
        pak.require(type(scale) in (int, float) and math.isfinite(scale)
                    and 0 < scale <= 3.4028234663852886e38,
                    f"Invalid projectile scale: {name}")
        # Check the actual float32 representation consumed by the game.
        pak.require(struct.unpack("<f", struct.pack("<f", scale))[0] > 0,
                    f"Projectile scale underflows float32: {name}")
        pak.require((row["image"] is None) == (index == 6),
                    "Only fire pea uses an attachment instead of a projectile image")
        if row["image"] is not None:
            images.add(image_path(row["image"]))
    for row, name in zip(shadows, ("day", "night")):
        pak.require(isinstance(row, dict) and set(row) == {"name", "image", "rect"}
                    and row["name"] == name,
                    f"Invalid projectile shadow source: {name}")
        images.add(image_path(row["image"]))
        rect = row["rect"]
        pak.require(isinstance(rect, list) and len(rect) == 4
                    and all(type(value) is int and 0 <= value <= 0x7FFFFFFF for value in rect)
                    and rect[2] > 0 and rect[3] > 0,
                    f"Invalid projectile shadow source rectangle: {name}")
    return sorted(images)


def image_path(value: str) -> str:
    pak.require(isinstance(value, str) and bool(value) and "\\" not in value
                and ":" not in value and not PurePosixPath(value).is_absolute()
                and all(part not in ("", ".", "..") for part in value.split("/"))
                and all(ord(char) >= 32 for char in value)
                and PurePosixPath(value).suffix.casefold() == ".png",
                f"Invalid projectile PNG source path: {value!r}")
    return value


def compile_definition(document: dict, descriptors: dict, files: dict,
                       layout: dict, atlas_symbols: list[str], atlas_path: str) -> tuple[bytes, dict]:
    """Use the shared packer's final positions; no runtime symbol IDs are emitted."""
    symbols = validate_sources(document)
    pak.require(bool(atlas_path) and set(symbols) <= set(atlas_symbols),
                "Projectile atlas is missing source images")
    regions = dict(zip(atlas_symbols, layout["regions"]))
    width, height = layout["width"], layout["height"]
    images, source_files = [], set()
    dimensions = {}
    for symbol in symbols:
        image, dependencies = definition_atlas.logical_image(descriptors[symbol], files)
        dimensions[symbol] = image.size
        rectangle = regions[symbol]
        pak.require(tuple(rectangle[2:]) == image.size,
                    f"Projectile atlas changed source dimensions: {symbol}")
        source_files.update(dependencies)
        images.append({
            "source": symbol, "width": image.width, "height": image.height,
            "rows": 1, "cols": 1, "rgba_bytes": image.width * image.height * 4,
            "rgba_sha256": hashlib.sha256(image.tobytes()).hexdigest(),
            "files": sorted(dependencies), "region": (*rectangle, 1, 1),
        })
    payload = bytearray(struct.pack("<5I", MAGIC, 1, len(NAMES), width, height))
    projectile_rows = []
    for row in document["projectiles"]:
        rectangle = regions[row["image"]] if row["image"] is not None else (0, 0, 0, 0)
        payload.extend(struct.pack("<4if", *rectangle, row["scale"]))
        projectile_rows.append({**row, "rect": list(rectangle)})
    shadow_rows = []
    for row in document["shadows"]:
        x, y, w, h = row["rect"]
        source_width, source_height = dimensions[row["image"]]
        pak.require(x + w <= source_width and y + h <= source_height,
                    f"Projectile shadow exceeds its source image: {row['name']}")
        atlas_x, atlas_y, _, _ = regions[row["image"]]
        rectangle = (atlas_x + x, atlas_y + y, w, h)
        payload.extend(struct.pack("<4i", *rectangle))
        shadow_rows.append({"name": row["name"], "rect": list(rectangle)})
    return bytes(payload), {
        "atlas_path": atlas_path, "atlas_width": width, "atlas_height": height,
        "atlas_shared": True, "region_count": len(images), "images": images,
        "source_files": sorted(source_files), "projectiles": projectile_rows,
        "shadows": shadow_rows,
    }
