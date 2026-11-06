"""Shared author manifest primitives with no resource identity registry."""
from __future__ import annotations
from game_fixture import game_root

import json
import re
import struct
from pathlib import Path

MAGIC = 0x4353524E
NONE = 0xFFFFFFFF
PROGRAM, SYSTEM_FONT, REFERENCE_FONT, KEEP_BITS = 1, 2, 4, 8
PALLETIZE, A4, A8, DDS, PURGE, MIN_SUBDIVIDE, AUTO_ALPHA = 16, 32, 64, 128, 256, 512, 1024
PURGE_3D, PURGE_2D, UNAVAILABLE = 2048, 4096, 8192


def write_changed(path: Path, text: str) -> None:
    data = text.encode("utf-8")
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def read_authoring(data: bytes) -> dict:
    def unique_members(pairs):
        result = {}
        for name, value in pairs:
            if name in result:
                raise ValueError(f"Duplicate author manifest member: {name}")
            result[name] = value
        return result

    author = json.loads(data, object_pairs_hook=unique_members)
    if (not isinstance(author, dict) or set(author) != {"format", "manifest"}
            or author["format"] != "pvz-resources-authoring-v1"):
        raise ValueError("Expected pvz-resources-authoring-v1 JSON manifest")

    def validate_node(node, tags):
        if (not isinstance(node, dict) or set(node) != {"tag", "attrs", "children"}
                or node["tag"] not in tags or not isinstance(node["attrs"], dict)
                or not isinstance(node["children"], list)):
            raise ValueError("Invalid author manifest node")
        if any(not isinstance(value, str) or "\0" in name or "\0" in value
               for name, value in node["attrs"].items()):
            raise ValueError("Author attributes must be strings without NUL")

    manifest = author["manifest"]
    validate_node(manifest, ("ResourceManifest",))
    for group in manifest["children"]:
        validate_node(group, ("Resources",))
        if not group["attrs"].get("id"):
            raise ValueError("Author resource group has no id")
        for node in group["children"]:
            validate_node(node, ("SetDefaults", "Image", "Sound", "Font"))
            if node["children"] or node["tag"] != "SetDefaults" and "path" not in node["attrs"]:
                raise ValueError("Author resource must be a leaf with a path")
    return manifest


def group_ids(root: Path) -> dict[str, int]:
    text = (root / "pvzPerf/ResourceGroupId.h").read_text(encoding="utf-8")
    block = text[text.index("enum class ResourceGroupId"):text.index("};")]
    result = {name: int(value) for name, value in re.findall(r"(\w+)\s*=\s*(\d+)", block)}
    if result.get("Invalid") != 0 or len(result) != 30:
        raise ValueError("Expected Invalid0 and 29 canonical resource groups")
    return result


def path_string(value: str) -> str:
    return value.replace(chr(92), "/")


def str_record(value: str) -> bytes:
    raw = value.encode("utf-8")
    return struct.pack("<I", len(raw)) + raw


def int_vector(values: list[int]) -> bytes:
    return struct.pack("<I", len(values)) + b"".join(struct.pack("<i", value) for value in values)


def vec(value: str) -> list[int]:
    return [int(v.strip()) for v in value.split(",")] if value else []
