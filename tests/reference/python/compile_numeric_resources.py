#!/usr/bin/env python3
"""Compile NRSC4 load plans with direct named C++ ownership bindings."""
from __future__ import annotations
from game_fixture import game_root

import argparse
import json
import re
import struct
import zlib
import zipfile
from pathlib import Path, PurePosixPath

from resource_authoring import (
    MAGIC, NONE, PROGRAM, SYSTEM_FONT, REFERENCE_FONT, KEEP_BITS,
    PALLETIZE, A4, A8, DDS, PURGE, MIN_SUBDIVIDE, AUTO_ALPHA,
    PURGE_3D, PURGE_2D, UNAVAILABLE, write_changed, read_authoring,
    group_ids, path_string, str_record, int_vector, vec,
)

ROOT = game_root()
VERSION = 4


def program_images_path(root: Path) -> Path:
    return root / "pvzPerf/Resources.h"


def excluded_image(name: str) -> bool:
    return name.startswith("IMAGE_QUICKPLAY_") or name == "IMAGE_SELECTORSCREEN_MOREWAYSTOPLAY_BG"


def read_names(root: Path) -> list[str]:
    """Read ordinary image globals; declaration order has no numeric meaning."""
    text = program_images_path(root).read_text(encoding="utf-8-sig")
    names = re.findall(r"\bextern\s+Image\s*\*\s*((?:IMAGE_|FONT_IMAGE_)\w+)\s*;", text)
    if len(names) != len(set(names)) or not names:
        raise ValueError("Expected unique image pointer declarations in Resources.h")
    return [name for name in names if not excluded_image(name)]


def read_symbols(path: Path) -> tuple[list[tuple[int, str]], int]:
    """Explicit historical fixture reader, never a production image input."""
    import legacy_numeric_resources
    return legacy_numeric_resources.read_symbols(path)


def code_image_roots(root: Path, image_names, *, required_names: set[str] | None = None) -> set[str]:
    """Find named image consumers without inventing an image numbering system."""
    import build_numeric_runtime_pak64 as pak
    exact = set(image_names.values() if isinstance(image_names, dict) else image_names)
    roots = set()
    for source in (root / "pvzPerf").rglob("*"):
        if source.suffix not in {".cpp", ".h", ".hpp", ".inl"} or "generated" in source.parts:
            continue
        if source.name in {"Resources.cpp", "Resources.h"}:
            continue
        text = pak.clean_comments(source.read_text(encoding="utf-8-sig"))
        for name in re.findall(r"\b(?:IMAGE_|FONT_IMAGE_)\w+", text):
            if excluded_image(name):
                continue
            if required_names is not None and name in required_names and name not in exact:
                raise ValueError(f"Code image absent from Resources.h: {name} ({source})")
            if name in exact:
                roots.add(name)
    return roots


def production_image_roots(image_names, code_names: set[str]) -> set[str]:
    declared = set(image_names.values() if isinstance(image_names, dict) else image_names)
    if not code_names <= declared:
        raise ValueError(f"Code image roots absent from declarations: {sorted(code_names - declared)}")
    return {name for name in code_names if not excluded_image(name)}


def binding_contract_crc(plan: list[dict]) -> int:
    """Cover exact group, step, kind and named C++ assignment targets."""
    contract = [[group["group"], [[r["kind"], r["name"]] for r in group["records"]]]
                for group in plan]
    return zlib.crc32(json.dumps(contract, separators=(",", ":"), ensure_ascii=True).encode("ascii"))


def plan_payload(plan: list[dict]) -> bytes:
    payload = bytearray()
    for group in plan:
        block = b"".join(struct.pack("<I", len(r["data"])) + r["data"] for r in group["records"])
        payload.extend(struct.pack("<3I", group["group"], len(group["records"]), len(block)))
        payload.extend(block)
    return bytes(payload)


def compile_manifest(root: Path, authoring: bytes, archive_paths: list[str],
                     image_names: list[str] | Path | None = None,
                     font_tag_ids: dict[str, int] | None = None) -> tuple[bytes, list[dict], int, list[str]]:
    # Historical binary registries are accepted only by isolated fixture code.
    if isinstance(image_names, Path) and image_names.suffix.casefold() == ".bin":
        import legacy_numeric_resources
        return legacy_numeric_resources.compile_manifest(root, authoring, archive_paths, image_names, font_tag_ids)
    if isinstance(image_names, Path):
        if image_names.resolve() != program_images_path(root).resolve():
            raise ValueError("Production image bindings must come from Resources.h declarations")
        image_names = read_names(root)
    declared = set(read_names(root) if image_names is None else image_names)
    live = production_image_roots(declared, code_image_roots(root, declared))
    manifest, groups = read_authoring(authoring), group_ids(root)
    plan, names, seen_names, seen_images, seen_groups = [], [], set(), set(), set()
    default_path, default_prefix = "", ""
    font_tags = font_tag_ids or {}
    for group in manifest["children"]:
        group_name = group["attrs"]["id"]
        if group_name not in groups:
            if any(e["tag"] != "SetDefaults" for e in group["children"]):
                raise ValueError(f"Nonempty author group has no approved enum: {group_name}")
            continue
        if group_name in seen_groups:
            raise ValueError(f"Duplicate resource group: {group_name}")
        seen_groups.add(group_name)
        group_plan = {"group": groups[group_name], "name": group_name, "records": []}
        plan.append(group_plan)
        local_fonts = {}
        for elem in group["children"]:
            a = elem["attrs"]
            if elem["tag"] == "SetDefaults":
                if "path" in a:
                    default_path = path_string(a["path"]).rstrip("/") + "/"
                default_prefix = a.get("idprefix", default_prefix).rstrip("/")
                continue
            original_path = path_string(a["path"])
            name = default_prefix + a.get("id", PurePosixPath(original_path).stem)
            if not re.fullmatch(r"[A-Za-z_]\w*", name) or name in seen_names:
                raise ValueError(f"Invalid or duplicate resource target: {name}")
            seen_names.add(name)
            kind = {"Image": 0, "Sound": 1, "Font": 2}[elem["tag"]]
            if kind == 0:
                if name not in live:
                    continue
                seen_images.add(name)
            names.append(name)
            path = original_path if original_path.startswith("!") else default_path + original_path
            flags, ref = 0, NONE
            if path == "!program":
                flags |= PROGRAM
                path = ""
            elif path.startswith("!ref:"):
                if kind != 2 or path[5:] not in local_fonts:
                    raise ValueError(f"Font reference must name a prior font in the same group: {name}")
                flags |= REFERENCE_FONT
                ref = local_fonts[path[5:]]
                path = ""
            elif path.startswith("!sys:"):
                raise ValueError("System fonts require an offline compiled bitmap font")
            elif path.startswith("!"):
                raise ValueError(f"Unknown resource control token: {path}")
            if path.casefold().startswith("reanim/"):
                flags |= KEEP_BITS
            paths = (path, default_path + a["alphaimage"] if "alphaimage" in a else "",
                     default_path + a["alphagrid"] if "alphagrid" in a else "", a.get("image", ""))
            details = b""
            if kind == 0:
                flags |= 0 if "nopal" in a else PALLETIZE
                flags |= 0 if "noalpha" in a or name == "IMAGE_EDITBOX" else AUTO_ALPHA
                for key, bit in [("a4r4g4b4", A4), ("a8r8g8b8", A8), ("ddsurface", DDS),
                                 ("nobits", PURGE), ("minsubdivide", MIN_SUBDIVIDE),
                                 ("nobits3d", PURGE_3D), ("nobits2d", PURGE_2D)]:
                    if key in a:
                        flags |= bit
                rows, cols = int(a.get("rows", "1")), int(a.get("cols", "1"))
                if not 0 < rows <= 2147483647 or not 0 < cols <= 2147483647:
                    raise ValueError("Invalid image grid")
                if "alphagrid" in a and (rows != 1 or cols != 1):
                    raise ValueError(
                        f"Image {name} requires an offline expanded alpha mask: "
                        "production alphagrid supports only a full-size 1x1 mask")
                if (a.get("anim", "none").lower() != "none"
                        or any(int(a.get(key, "0")) != 0
                               for key in ("framedelay", "begindelay", "enddelay"))
                        or vec(a.get("perframedelay", ""))
                        or vec(a.get("framemap", ""))):
                    raise ValueError(f"Unsupported image animation metadata: {name}")
                # Author grids remain offline geometry only. Loading requires
                # a color for synthesized pixels, never frame or grid metadata.
                details = struct.pack("<I", int(a.get("alphacolor", "FFFFFF"), 16))
            elif kind == 1:
                details = struct.pack("<di", float(a.get("volume", "-1")), int(a.get("pan", "0")))
            else:
                style = sum(bit for key, bit in [("bold", 1), ("italic", 2), ("underline", 4), ("shadow", 8)] if key in a)
                details = struct.pack("<iI", int(a.get("size", "0")), style)
                tags = re.findall(r"[^,\s]+", a.get("tags", ""))
                if any(tag not in font_tags for tag in tags):
                    raise ValueError(f"Unknown compiled font tags: {tags}")
                details += int_vector([font_tags[tag] for tag in tags])
                local_fonts[name] = len(group_plan["records"])
            path_data = b"".join(str_record(p) for p in paths)
            # A cache recipe discriminator, never an image identity. Cover all
            # decoding, alpha and retention inputs. Offline geometry does not
            # split otherwise identical cache entries.
            recipe = struct.pack("<I", flags) + path_data + details
            if a.get("variant"):
                recipe += str_record(a["variant"])
            variant = 0
            data = struct.pack("<4I", kind, flags, variant, ref) + path_data + details
            group_plan["records"].append({"name": name, "kind": kind, "flags": flags,
                                          "variant": variant, "path": path, "data": data,
                                          "_recipe": recipe})
    if live - seen_images:
        raise ValueError(f"Live image globals missing from author manifest: {sorted(live - seen_images)}")
    plan.sort(key=lambda group: group["group"])
    # Cache variants discriminate recipes only within one physical path. The
    # usual single-recipe path is zero; no global image numbering is created.
    path_recipes = {}
    for group in plan:
        for record in group["records"]:
            if record["kind"] == 0:
                path_recipes.setdefault(record["path"].casefold(), set()).add(record["_recipe"])
    path_variants = {path: {recipe: index for index, recipe in enumerate(sorted(recipes))}
                     for path, recipes in path_recipes.items()}
    for group in plan:
        for record in group["records"]:
            recipe = record.pop("_recipe")
            if record["kind"] == 0:
                record["variant"] = path_variants[record["path"].casefold()][recipe]
                data = bytearray(record["data"])
                struct.pack_into("<I", data, 8, record["variant"])
                record["data"] = bytes(data)
    payload, crc = plan_payload(plan), binding_contract_crc(plan)
    records = [record for group in plan for record in group["records"]]
    header = struct.pack("<8I", MAGIC, VERSION, sum(r["kind"] == 0 for r in records), len(plan),
                         len(records), crc, zlib.crc32(payload), len(payload))
    return header + payload, plan, crc, names


def emit_headers(root: Path, plan: list[dict], contract_crc: int, names: list[str]) -> None:
    generated = root / "pvzPerf/Sexy.TodLib/generated"
    records = [r for g in plan for r in g["records"]]
    output = ["// Generated transient load-plan contract.", "#pragma once", "#include <cstdint>",
              f"inline constexpr std::uint32_t RESOURCE_LOAD_BINDING_CRC = 0x{contract_crc:08X}u;",
              f"inline constexpr std::uint32_t RESOURCE_LOAD_PLAN_CRC = 0x{zlib.crc32(plan_payload(plan)):08X}u;",
              f"inline constexpr unsigned RESOURCE_LOAD_IMAGE_COUNT = {sum(r['kind'] == 0 for r in records)};",
              f"inline constexpr unsigned RESOURCE_LOAD_PLAN_RECORD_COUNT = {len(records)};"]
    write_changed(generated / "NumericResourceBindings.h", "\n".join(output) + "\n")
    output = ["// Generated explicit resource ownership bindings. Do not edit.",
              "namespace ResourceLoadBindings", "{", "namespace {"]
    output += [f'Sexy::SharedImageRef owner_{r["name"]};' for r in records if r["kind"] == 0]
    output += ["}", "bool LoadStep(Sexy::ResourceManager& manager, Sexy::ResourceManager::LoadPlan& plan,",
               "              ResourceGroupId group, unsigned step)", "{", "    switch (group)", "    {"]
    kinds = ("Image", "Sound", "Font")
    for group in plan:
        output += [f'    case ResourceGroupId::{group["name"]}:', "        switch (step)", "        {"]
        for step, record in enumerate(group["records"]):
            kind, name = kinds[record["kind"]], record["name"]
            owner = f", owner_{name}" if record["kind"] == 0 else ""
            output.append(f"        case {step}: return manager.Load{kind}(plan.m{kind}s[plan.mNext{kind}++], Sexy::{name}{owner});")
        output += ["        default: return false;", "        }"]
    output += ["    default: return false;", "    }", "}",
               "void ReleaseGroup(Sexy::ResourceManager& manager, ResourceGroupId group)",
               "{", "    switch (group)", "    {"]
    for group in plan:
        output += [f'    case ResourceGroupId::{group["name"]}:']
        for record in reversed(group["records"]):
            kind, name = kinds[record["kind"]], record["name"]
            owner = f", owner_{name}" if record["kind"] == 0 else ""
            output.append(f"        manager.Release{kind}(Sexy::{name}{owner});")
        output += ["        break;"]
    output += ["    default: break;", "    }", "}"]
    if any(r["name"] == "IMAGE_TITLESCREEN" for r in records):
        output += ["void ReleaseTitleImage(Sexy::ResourceManager& manager)", "{",
                   "    manager.ReleaseImage(Sexy::IMAGE_TITLESCREEN, owner_IMAGE_TITLESCREEN);", "}"]
    output += ["}"]
    write_changed(generated / "ResourceLoadBindings.inc", "\n".join(output) + "\n")


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--root", type=Path, default=ROOT)
    ap.add_argument("--archive", type=Path, default=ROOT / "out/x64-win32-dx12/Release/main.pak64")
    ap.add_argument("--authoring", type=Path)
    ap.add_argument("--output", type=Path)
    ap.add_argument("--headers", action="store_true")
    ap.add_argument("--verify", action="store_true")
    args = ap.parse_args()
    authoring = args.authoring or args.root / "assets/definitions/resources.json"
    output = args.output or args.archive.parent / "compiled/resources.compiled"
    with zipfile.ZipFile(args.archive) as archive:
        data, plan, crc, names = compile_manifest(args.root, authoring.read_bytes(), archive.namelist())
    if args.verify:
        if output.read_bytes() != data:
            raise ValueError(f"Resource load plan is stale: {output}")
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        if args.headers:
            emit_headers(args.root, plan, crc, names)
    values = struct.unpack_from("<8I", data)
    print(f"images={values[2]} groups={values[3]} load-records={values[4]} bytes={len(data)} binding-crc={crc:08X}")
    print(output)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
