"""Small shared formatting helpers for native static definition generators."""
from __future__ import annotations
from game_fixture import game_root

import json
import math
import struct

import build_numeric_runtime_pak64 as pak


def cpp_float(value: float) -> str:
    pak.require(math.isfinite(value), "Nonfinite static definition value")
    value = struct.unpack("<f", struct.pack("<f", value))[0]
    text = format(value, ".9g")
    if "." not in text and "e" not in text:
        text += ".0"
    return text + "f"


def cpp_string(value: str) -> str:
    # A byte escape per UTF-8 byte also handles supplementary characters and
    # control characters that C++ universal-character escapes cannot represent.
    if any(ord(char) >= 127 or ord(char) < 32 and char not in "\b\f\n\r\t" for char in value):
        return '"' + "".join(f"\\x{byte:02x}" for byte in value.encode("utf-8")) + '"'
    return json.dumps(value, ensure_ascii=True)


def texture_region(rect, atlas_width: int, atlas_height: int) -> str:
    """Bake normalized sample endpoints while retaining exact logical extents."""
    x, y, width, height = rect
    if atlas_width == 0 or atlas_height == 0:
        pak.require((x, y, width, height) == (0, 0, 0, 0),
                    "Nonempty texture region requires atlas dimensions")
        endpoints = (0.0, 0.0, 0.0, 0.0)
    else:
        pak.require(atlas_width > 0 and atlas_height > 0,
                    "Invalid texture region atlas dimensions")
        endpoints = (x / atlas_width, y / atlas_height,
                     (x + width) / atlas_width, (y + height) / atlas_height)
    return "{" + ", ".join([*(cpp_float(value) for value in endpoints),
                            str(width), str(height)]) + "}"


def array(declarations: list[str], code: list[str], type_name: str, name: str,
          values: list[str], *, readonly: bool = False) -> str:
    if not values:
        return "nullptr"
    qualifier = "const " if readonly else ""
    declarations.append(f"extern {qualifier}{type_name} {name}[{len(values)}];")
    code.append(f"constinit {qualifier}{type_name} {name}[{len(values)}] = {{")
    code.extend("    " + value + "," for value in values)
    code.append("};")
    return name
