#!/usr/bin/env python3
"""Package audited numeric runtime assets without modifying any input.

Usage: python -B tools/build_numeric_runtime_pak64.py SOURCE STAGE OUTPUT --root ROOT

OUTPUT must be a new, separately named archive, not main.pak64. The adjacent
manifest is also exclusively created, before the archive publication marker.
The two names are not a crash-atomic transaction. No compiler, game, or subprocess runs.
This is a packaging/reference audit, NOT a complete compiled-string audit.
Font author removals come from the validated staged numeric resource manifest.
Legacy XML entries are removal-only bytes, never parsed.
"""

from __future__ import annotations
from game_fixture import game_root

import argparse
import hashlib
import io
import json
import math
import os
from pathlib import Path, PurePosixPath
import posixpath
import re
import stat
import struct
import tempfile
import zipfile
import zlib


ROOT = game_root()
COUNTS = {"reanim": 146, "particles": 106, "fonts": 20, "resources": 1}
RESOURCE_PATH = "compiled/resources.compiled"
PROJECTILE_PATH = "compiled/projectiles/projectiles.compiled"
PROJECTILE_ATLAS_PATH = "compiled/atlases/projectiles.atlas.png"
ZOMBATAR_DEFINITION_PATH = "compiled/zombatar/zombatar.compiled"
LEGACY_RESOURCE_PATH = "compiled/properties/resources.numeric.resources"
RESOURCE_KIND_NONE = 3
RESOURCE_SLOT_NONE = 0xFFFFFFFF
SHADER_HEADERS = (b"VSFX", b"\x03\x02\x23\x07", b"DXBC", b"DXIL")
TEST_MACROS = (
    "SEXY_DEFINITION_COMPAT_TEST_ONLY",
    "SEXY_FONT_TEXT_COMPAT_TEST_ONLY",
)
TOKEN = re.compile(
    r'R"(?P<delimiter>[^ ()\\\t\r\n]{0,16})\(.*?\)(?P=delimiter)"'
    r'|"(?:\\.|[^"\\])*"|\'(?:\\.|[^\'\\])*\'|//[^\n]*|/\*.*?\*/',
    re.S,
)
CPP_SUFFIXES = {".cpp", ".c", ".h", ".hpp", ".inl", ".inc"}


def require(condition: bool, message: str) -> None:
    if not condition:
        raise ValueError(message)


def reanim_track_flags(flags: int, *, authored: bool) -> tuple[int, int]:
    """IDTF keeps text in bit 0 and storage (both/cpu/gpu) in bits 1..2."""
    require(flags >= 0 and flags & ~7 == 0 and flags >> 1 <= 2
            and (authored or flags <= 1), "Invalid reanimation track flags")
    return flags & 1, flags >> 1


def key(name: str) -> str:
    path = name.replace("\\", "/")
    parts = (path[:-1] if path.endswith("/") else path).split("/")
    require(bool(path) and not path.startswith("/") and "\0" not in path
            and all(p and p not in (".", "..") and not any(c in p for c in ':<>|"*?')
                    and p == p.rstrip(" .") and not any(ord(c) < 32 for c in p)
                    and not re.fullmatch(r"(?i:con|prn|aux|nul|com[1-9]|lpt[1-9])", p.split(".")[0])
                    for p in parts), f"Unsafe archive path: {name!r}")
    return "/".join(parts).casefold()


def file_key(name: str) -> str:
    require(not name.endswith(("/", "\\")), f"File reference names a directory: {name!r}")
    return key(name)


def read_definition_image_variant_bindings(root: Path) -> dict[tuple[str, int], str]:
    """Read compiler-owned program bindings; compiled definitions carry no paths."""
    path = root / "pvzPerf/Sexy.TodLib/generated/DefinitionImageBindings.h"
    if not path.exists():
        return {}
    text = clean_comments(path.read_text(encoding="utf-8-sig"))
    result = {}
    for definition, variant, image in re.findall(
            r'\{\s*"([^"]+)"\s*,\s*(?:(\d+)u?\s*,\s*)?"([^"]*)"\s*\}', text):
        name = file_key(definition)
        identity = (name, int(variant or 0))
        require((numeric_kind(name) in ("reanim", "particles", "projectiles")
                 or name == ZOMBATAR_DEFINITION_PATH) and identity not in result
                and (numeric_kind(name) != "projectiles" or identity[1] == 0),
                f"Invalid or duplicate program definition binding: {definition}")
        if image:
            image = file_key(image)
            require(image.startswith("compiled/") and image.endswith(".atlas.png"),
                    f"Invalid program definition atlas path: {image}")
        result[identity] = image
    require(bool(result), "Program definition image bindings are empty or malformed")
    return result


def read_definition_image_bindings(root: Path) -> dict[str, str]:
    return {name: image for (name, variant), image in read_definition_image_variant_bindings(root).items()
            if variant == 0}


def read_definition_image_binding_rows(root: Path) -> dict[str, list[str]]:
    result = {}
    for (name, variant), image in sorted(read_definition_image_variant_bindings(root).items()):
        rows = result.setdefault(name, [])
        require(variant == len(rows), "Noncontiguous program appearance bindings")
        rows.append(image)
    return result


def read_definition_atlas_extents(root: Path) -> dict[str, tuple[int, int]]:
    path = root / "pvzPerf/Sexy.TodLib/generated/DefinitionImageBindings.h"
    if not path.exists():
        return {}
    text = path.read_text(encoding="utf-8-sig")
    return {name: (int(width), int(height)) for name, width, height in re.findall(
        r'\{"([^"]+\.atlas\.png)",\s*(\d+)u,\s*(\d+)u\}', text)}


def read_scene_atlas_metadata(root: Path) -> tuple[dict, list[dict]]:
    path = root / "pvzPerf/Sexy.TodLib/generated/SceneAtlasBindings.h"
    if not path.exists():
        return {}, []
    text = path.read_text(encoding="utf-8-sig")
    extents = {name: (int(width), int(height)) for name, width, height in re.findall(
        r'\{"([^"]+\.atlas\.png)",\s*(\d+)u,\s*(\d+)u\}', text)}
    sheets = [{"atlas": atlas, "source": source, "rect": tuple(map(int, (x, y, width, height))),
               "rgba_sha256": sha}
              for atlas, source, x, y, width, height, sha in re.findall(
                  r'\{"([^"]+\.atlas\.png)",\s*"([^"]+)",\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*(\d+)u,\s*"([a-f0-9]{64})"\}',
                  text)]
    require(bool(extents) and bool(sheets), "Incomplete scene atlas source-sheet audit")
    require({sheet["atlas"] for sheet in sheets} == set(extents), "Scene source sheet atlas mismatch")
    return extents, sheets

def digest(data: bytes) -> str:
    return hashlib.sha256(data).hexdigest()


PARTICLE_VARIANTS = "pvzPerf/Sexy.TodLib/ParticleImageVariants.inc"


def particle_image_variants(root: Path) -> list[dict[str, str]]:
    """Parse the restricted author/converter table, without interpreting C++."""
    path = root / PARTICLE_VARIANTS
    if not path.exists():
        return []  # Older sources and synthetic fixtures retain the base corpus.
    text = clean_comments(path.read_text(encoding="utf-8-sig"))
    pattern = re.compile(
        r"PARTICLE_IMAGE_VARIANT\s*\(\s*(PARTICLE_\w+)\s*,\s*(PARTICLE_\w+)"
        r"\s*,\s*([A-Za-z_]\w*)\s*,\s*(IMAGE_\w+)\s*\)", re.ASCII)
    entries, effects, components = [], set(), set()
    at = 0
    while at < len(text):
        if text[at].isspace():
            at += 1
            continue
        match = pattern.match(text, at)
        require(match is not None, f"Malformed particle image variant table at {path}:{text.count(chr(10), 0, at) + 1}")
        effect, base, component, image = match.groups()
        require(effect not in effects and component.casefold() not in components,
                f"Duplicate particle image variant effect/component: {effect}/{component}")
        effects.add(effect)
        components.add(component.casefold())
        entries.append({"effect": effect, "base": base, "component": component, "image": image,
                        "path": definition_path(f"particles/{component}.xml")})
        at = match.end()
    require(bool(entries), f"Empty particle image variant table: {path}")
    return entries


def uses_static_definitions(root: Path) -> bool:
    path = root / "pvzPerf/Sexy.TodLib/Reanimator.cpp"
    return path.exists() and "GeneratedDefinitions::Reanimations" in path.read_text(encoding="utf-8-sig")


def expected_counts(root: Path) -> dict[str, int]:
    if uses_static_definitions(root):
        return {"reanim": 0, "particles": 0, "fonts": COUNTS["fonts"],
                "resources": 1, "projectiles": 0}
    counts = {**COUNTS, "particles": COUNTS["particles"] + len(published_particle_variants(root))}
    # Frozen offline fixtures predate the projectile runtime route.
    if ((root / "assets/definitions/projectiles/Projectiles.projectile.dsl").exists()
            or PROJECTILE_PATH in read_definition_image_bindings(root)):
        counts["projectiles"] = 1
    return counts


def published_particle_variants(root: Path) -> list[dict[str, str]]:
    """Older offline fixtures can retain expanded tables; current roots do not."""
    variants = particle_image_variants(root)
    path = root / "pvzPerf/Sexy.TodLib/TodParticle.cpp"
    source = clean_comments(path.read_text(encoding="utf-8-sig")) if path.exists() else ""
    effects = set(re.findall(r'\{\s*ParticleEffect::(\w+)\s*,\s*"particles\\\\', source))
    return variants if ('#include "ParticleImageVariants.inc"' in source or
                        any(row["effect"] in effects for row in variants)) else []


def fingerprint(path: Path) -> dict:
    with path.open("rb") as stream:
        h = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
        return {"sha256": h.hexdigest(), "bytes": stream.tell()}


def clean_comments(text: str) -> str:
    return TOKEN.sub(lambda m: re.sub(r"[^\n]", " ", m[0])
                     if m[0].startswith(("//", "/*")) else m[0], text)


def production_text(text: str) -> str:
    """Exclude only proven-off test gates; retain BOTH unknown branches."""
    text = clean_comments(text)
    stack = []
    active = True
    result = []
    for line in text.splitlines(keepends=True):
        directive = re.match(r"\s*#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", line)
        if not directive:
            if active:
                require(not re.match(r"\s*#\s*define\s+(?:" + "|".join(TEST_MACROS)
                                     + r")\b", line), "Production enables a test-only importer")
                result.append(line)
            else:
                result.append("\n" if line.endswith("\n") else "")
            continue
        op, expression = directive.groups()
        expression = expression.strip()
        if op in ("if", "ifdef", "ifndef", "elif"):
            value = None
            if expression in ("0", "1") and op in ("if", "elif"):
                value = expression == "1"
            for macro in TEST_MACROS:
                if expression == macro and op in ("ifdef", "ifndef"):
                    value = op == "ifndef"
                elif re.fullmatch(r"!?\s*defined\s*(?:\(\s*" + macro
                                  + r"\s*\)|" + macro + r")", expression):
                    value = expression.startswith("!")
                elif macro in expression:
                    raise ValueError(f"Unsupported test-gate expression: {line.strip()}")
        if op in ("if", "ifdef", "ifndef"):
            stack.append([active, value, False])
            active = active and value is not False
        elif op == "elif":
            require(bool(stack) and not stack[-1][2], "Unbalanced #elif")
            parent, previous, _ = stack[-1]
            active = parent and previous is not True and value is not False
            stack[-1][1] = True if previous is True or value is True else (
                False if previous is False and value is False else None)
        elif op == "else":
            require(bool(stack) and not stack[-1][2], "Unbalanced #else")
            parent, value, _ = stack[-1]
            stack[-1][2] = True
            active = parent and value is not True
        else:
            require(bool(stack), "Unbalanced #endif")
            active = stack.pop()[0]
        result.append("\n" if line.endswith("\n") else "")
    require(not stack, "Unclosed production preprocessor gate")
    return "".join(result)


def literals(text: str):
    for match in TOKEN.finditer(text):
        value = match[0]
        if value.startswith('R"'):
            delimiter = match["delimiter"]
            yield match, value[len(delimiter) + 3:-(len(delimiter) + 2)]
        elif value.startswith('"'):
            value = value[1:-1]
            value = re.sub(r'\\([\\"])', r"\1", value)
            yield match, value


def source_audit(root: Path, removed: dict[str, str], numeric_names: set[str], *,
                 legacy_fixture: bool = False) -> dict:
    game = root / "pvzPerf"
    static_definitions = uses_static_definitions(root) and not legacy_fixture
    paths = {p for p in game.iterdir() if p.is_file() and p.suffix in CPP_SUFFIXES}
    for directory in ("Lawn", "Sexy.TodLib", "SexyAppFramework"):
        require((game / directory).is_dir(), f"Missing runtime source tree: {directory}")
        paths.update(p for p in (game / directory).rglob("*")
                     if p.is_file() and p.suffix in CPP_SUFFIXES)
    variant_path = root / PARTICLE_VARIANTS
    variants = particle_image_variants(root)
    if variant_path.exists():
        paths.add(variant_path)
    paths.update(root.glob("CMakeLists.txt"))
    paths.update((root / "CMake").rglob("*.cmake"))
    for directory in ("Lawn", "Sexy.TodLib", "SexyAppFramework"):
        paths.update((game / directory).rglob("CMakeLists.txt"))
    raw, production, records = {}, {}, []
    for path in sorted(paths):
        relative = path.relative_to(game if path.is_relative_to(game) else root).as_posix()
        data = path.read_bytes()
        raw[relative] = data.decode("utf-8-sig")
        records.append({"name": relative, "sha256": digest(data), "bytes": len(data)})
        if path.suffix in CPP_SUFFIXES:
            production[relative] = production_text(raw[relative])
        elif path.suffix != ".inc":
            text = re.sub(r"#[^\n]*", "", raw[relative])
            # These explicit offline targets may bake assets; they must never
            # enable the authoring macro on, or link into, a production target.
            offline = {"compile_numeric_scale", "sexy_scale_authoring"}
            text = re.sub(
                r"target_compile_definitions\s*\(\s*(?:compile_numeric_scale|sexy_scale_authoring)"
                r"\s+(?:PRIVATE|PUBLIC)\s+SEXY_SCALE_EFFECT_AUTHORING=1\s*\)", "", text)
            require(not any(m in text for m in TEST_MACROS),
                    f"Build source mentions a disabled importer: {relative}")
            for command in re.finditer(r"target_link_libraries\s*\(\s*(\S+)\s+([^)]*)\)", text):
                target, dependencies = command.groups()
                if set(dependencies.split()) & offline:
                    require(target in offline or target.endswith("_tests"),
                            f"Production links an offline authoring target: {relative}: {target}")

    routes = {
        "LawnApp.cpp": ['ParseResourcesFile("compiled/resources.compiled")'],
        "Sexy.TodLib/Reanimator.cpp": [
            'DefinitionCompiledPath(gReanimationParamArray[(int)theReanimType].mReanimFileName)',
            "DefinitionOpenCompiledFile(aV5Path.c_str())",
            "0x36544449u", "0x37564152u", "ReanimationTryLoadDefinitionV6(theReanimType, aReanimDef)",
        ],
        "Sexy.TodLib/TodParticle.cpp": [
            'DefinitionCompiledPath(params[i].mParticleFileName)',
            "DefinitionOpenCompiledFile(path.c_str())",
            "0x36524449u", "0x36564150u",
            "TodParticleTryLoadDefinitionV6(&gParticleDefArray[index], paths[index])",
        ],
        "SexyAppFramework/graphics/ImageFont.cpp": [
            "ResolveCompiledFontPath(theFontDescFileName)", '"compiled/fonts/"',
            'const std::string suffix = ".compiled"', "0x544E464Eu",
        ],
        "SexyAppFramework/misc/ResourceManager.cpp": [
            ("return ParseNumericResourcesFile(theFilename)" if legacy_fixture else
             "return ParseNumericResourcesFile(filename)"), "0x4353524Eu",
        ],
        "SexyAppFramework/effect/ScaleEffectCompiler.cpp": [
            'path+=".vfx"', "0x58465356u",
        ],
        "SexyAppFramework/SexyAppBase.cpp": [
            "sourceCompiler.CompileFile(sourcePath,{},metadata,error)",
            "sourceCompiler.CompileD3d12(sourcePath,{},metadata,error)",
            "metadata.mSourcePath=sourcePath;",
        ],
        "SexyAppFramework/graphics/vulkan/VulkanScalePipeline.cpp": [
            "std::filesystem::path(effect.mSourcePath).parent_path()/texture.mSource",
        ],
    }
    if static_definitions:
        routes["Sexy.TodLib/Reanimator.cpp"] = [
            "GeneratedDefinitions::Reanimations", "GeneratedDefinitions::LoadReanimationImages"]
        routes["Sexy.TodLib/TodParticle.cpp"] = [
            "GeneratedDefinitions::Particles", "GeneratedDefinitions::LoadParticleImages"]
    for name, markers in routes.items():
        require(name in production and all(m in production[name] for m in markers),
                f"Numeric production route changed or missing: {name}")
    binary_readers, offline_readers = [], []
    for name, text in production.items():
        if "ScaleEffectBinary::Load" not in text:
            continue
        if "/tests/" in name:
            offline_readers.append(name)
        else:
            without_definition = re.sub(r"\bbool\s+ScaleEffectBinary::Load\s*\(",
                                        "bool OfflineScaleLoadDefinition(", text)
            if "ScaleEffectBinary::Load" in without_definition:
                binary_readers.append(name)
    require(not binary_readers, f"Production scale binary reader prevents VFX retirement: {binary_readers}")
    for backend in ("vulkan", "d3d12"):
        name = f"SexyAppFramework/graphics/{backend}/CMakeLists.txt"
        require(re.search(r"target_compile_definitions\s*\(\s*sexy_" + backend
                          + r"_effects\s+PUBLIC\s+SEXY_SCALE_EFFECT_AUTHORING=1\s*\)",
                          re.sub(r"#[^\n]*", "", raw.get(name, ""))),
                f"Missing startup HLSL compiler definition: {name}")
    forbidden = r"\b(?:LoadDescriptor|LoadTextForTest|DoParseResources|DefinitionReadXML|DefinitionLoadXML)\s*\("
    accepted = []
    reanim_types = 0
    table_paths = {"reanim": set(), "particles": set()}
    for name, text in production.items():
        if not legacy_fixture:
            require(not re.search(
                r"\b(?:ImageSymbolId|ResourceId|ResourceSlot|IMAGE_SYMBOL_ID_MAX"
                r"|IMAGE_SYMBOL_ID_REGISTRY_CRC|TodDefinitionLoadFontById)\b", text),
                f"Resource identity registry remains in production: {name}")
        require(not re.search(r"\b(?:XMLParser|XMLElement|SEXY_RESOURCE_XML_TEST_IMPORTER)\b", text),
                f"XML importer in production: {name}")
        calls = text
        if PurePosixPath(name).suffix in (".h", ".hpp"):
            calls = re.sub(
                r"\b(?:virtual\s+)?bool\s+(?:LoadDescriptor|LoadTextForTest|DoParseResources"
                r"|DefinitionReadXML|DefinitionLoadXML)\s*\([^;{}]*\)\s*;", "", calls)
        require(not re.search(forbidden, calls), f"Author importer in production: {name}")
        # The generic legacy reader may be defined, but may not have callers.
        occurrences = len(re.findall(r"\bDefinitionReadCompiledFile\s*\(", text))
        require(occurrences <= (1 if name in ("Sexy.TodLib/Definition.cpp", "Sexy.TodLib/Definition.h") else 0),
                f"Old compiled-definition consumer in production: {name}")
        for match, value in literals(text):
            if not value or "\n" in value:
                continue
            particle_parent_suffix = (
                name == "Sexy.TodLib/TodParticle.cpp" and value == ".compiled" and
                re.search(r'bool\s+IsCanonicalParticlePath\s*\(\s*std::string_view\s+path\s*\)\s*'
                          r'\{\s*constexpr\s+std::string_view\s+prefix\s*=\s*'
                          r'"compiled/particles/"\s*,\s*suffix\s*=\s*$',
                          text[max(0, match.start() - 300):match.start()]) is not None)
            require(value != ".v4.compiled" and
                    (value != ".compiled" or particle_parent_suffix or name in
                     ("Sexy.TodLib/Definition.cpp", "SexyAppFramework/graphics/ImageFont.cpp")),
                    f"Legacy compiled suffix in production: {name}")
            try:
                normalized = key(value)
            except ValueError:
                continue
            before = text[max(0, match.start() - 180):match.start()]
            after = text[match.end():match.end() + 90]
            reanim_table = (
                name == "Sexy.TodLib/Reanimator.cpp"
                and re.search(r"\{\s*ReanimationType::\w+\s*,\s*$", before)
                and re.match(r"\s*[,}]", after)
            )
            particle_table = (
                name == "Sexy.TodLib/TodParticle.cpp"
                and re.search(r"\{\s*ParticleEffect::\w+\s*,\s*$", before)
                and re.match(r"\s*\}", after)
            )
            table = bool(reanim_table or particle_table)
            if table:
                numeric_path = definition_path(normalized)
                require(static_definitions or numeric_path in numeric_names,
                        f"Production definition lacks numeric equivalent: {value}")
                table_paths["reanim" if reanim_table else "particles"].add(numeric_path)
                if reanim_table:
                    reanim_types += 1
            if normalized not in removed:
                continue
            font = (removed[normalized] == "font_dsl"
                    and re.search(r"(?:ImageFont(?:\s+\w+)?|ResolveCompiledFontPath)\s*\([^;{}]*$", before)
                    and font_path(normalized) in numeric_names)
            require(bool(table or font), f"Removed file still has a production reference: {name}: {value}")
            accepted.append({"source": name, "path": value, "consumer": "numeric_path_resolver"})
    particle_source = production.get("Sexy.TodLib/TodParticle.cpp", "")
    includes_variants = '#include "ParticleImageVariants.inc"' in particle_source
    literal_effects = re.findall(
        r'\{\s*ParticleEffect::(\w+)\s*,\s*"particles\\\\([^"]+)\.xml"\s*\}',
        particle_source)
    require(len(literal_effects) == len({effect for effect, _ in literal_effects})
            == len(table_paths["particles"]),
            "Particle table identities or paths are duplicated")
    if includes_variants:
        require(variant_path.exists(), "Particle image variant table and runtime include disagree")
        require(len(table_paths["particles"]) == COUNTS["particles"],
                "The original 106-entry particle table is incomplete or duplicated")
        # Accept exactly the known stringizing expansion. Never evaluate arbitrary
        # macros and never treat its image metadata as a program image root.
        compact = re.sub(r"\s+", "", re.sub(r"\\\r?\n", "", particle_source))
        expansion = (r'#definePARTICLE_IMAGE_VARIANT(effect,base,component,image)'
                     r'{ParticleEffect::effect,"particles\\"#component".xml"},'
                     r'#include"ParticleImageVariants.inc"#undefPARTICLE_IMAGE_VARIANT')
        require(expansion in compact, "Unsupported particle image variant runtime expansion")
        base_effects = re.findall(r'\{\s*ParticleEffect::(\w+)\s*,\s*"particles\\\\[^"]+\.xml"\s*\}',
                                  particle_source)
        require(len(base_effects) == COUNTS["particles"] and len(set(base_effects)) == len(base_effects),
                "The original particle effect identities are incomplete or duplicated")
        for entry in variants:
            require(entry["base"] in base_effects and entry["effect"] not in base_effects,
                    f"Particle image variant has unknown base or reused effect: {entry['effect']}")
            require(entry["path"] not in table_paths["particles"],
                    f"Particle image variant aliases an original definition: {entry['component']}")
            table_paths["particles"].add(entry["path"])
            require(entry["path"] in numeric_names,
                    f"Production particle variant lacks numeric equivalent: {entry['component']}")
    else:
        # The v3 catalog contains every effect explicitly, in enum order.
        # Variant metadata still describes offline appearance/save conversion.
        expanded = len(literal_effects) != COUNTS["particles"]
        require(len(literal_effects) == COUNTS["particles"] + (len(variants) if expanded else 0),
                "The original 106-entry particle table or explicit variants are incomplete")
        effect_paths = {effect: definition_path(f"particles/{component}.xml")
                        for effect, component in literal_effects}
        variant_effects = {entry["effect"] for entry in variants}
        for entry in variants:
            require(entry["base"] in effect_paths and entry["base"] not in variant_effects,
                    f"Particle image variant has unknown base: {entry['effect']}")
            if expanded:
                require(effect_paths.get(entry["effect"]) == entry["path"],
                        f"Particle image variant has incorrect runtime binding: {entry['effect']}")
            else:
                require(entry["effect"] not in effect_paths and entry["path"] not in numeric_names,
                        f"Particle appearance must not publish an independent effect: {entry['effect']}")
    require(static_definitions or table_paths["particles"] ==
            {n for n in numeric_names if numeric_kind(n) == "particles"},
            "Staged IDP5 corpus must exactly match the live particle table")
    require(bool(table_paths["reanim"]), "Missing live reanimation table")
    projectile_names = {n for n in numeric_names if numeric_kind(n) == "projectiles"}
    require(projectile_names == ({PROJECTILE_PATH} if expected_counts(root).get("projectiles", 0) else set()),
            "Staged projectile corpus differs from the production route")

    def constant(name: str, source: str) -> int:
        match = re.search(r"\b" + name + r"\s*=\s*(0[xX][0-9A-Fa-f]+|\d+)[uU]?\s*;", production.get(source, ""))
        require(match is not None, f"Missing registry constant: {name}")
        return int(match[1], 0)

    if legacy_fixture:
        font_hash = re.search(r"TodFontSymbolRegistryHash\(\)\s*\{\s*return\s+(0x[0-9A-Fa-f]+)[uU]?\s*;",
                             production.get("Sexy.TodLib/TodCommon.cpp", ""))
        require(font_hash is not None, "Historical fixture font registry hash changed")
        font_crc = int(font_hash[1], 0)
    else:
        # Only offline reanimation intermediates still carry the font DSL
        # compiler's checksum. Production C++ binds FONT_BRIANNETOD32 directly.
        font_path = root / ("tools/font_symbol_ids.bin" if legacy_fixture else "assets/definitions/toolchain/font_symbol_ids.bin")
        font_crc = zlib.crc32(font_path.read_bytes())
    registries = {
        "track": constant("PVZ_REANIM_TRACK_TABLE_HASH", "Sexy.TodLib/ReanimTrackId.h"),
        "font": font_crc,
        "emitter": constant("kParticleEmitterNameTableHash", "Sexy.TodLib/generated/ParticleEmitterName.h"),
        "track_max": constant("PVZ_REANIM_TRACK_ID_COUNT", "Sexy.TodLib/ReanimTrackId.h"),
        "track_capacity": constant("kReanimActiveMaskCapacityBits", "Sexy.TodLib/Reanimator.h"),
        "emitter_count": constant("kParticleEmitterNameCount", "Sexy.TodLib/generated/ParticleEmitterName.h") + 1,
        "groups": constant("RESOURCE_GROUP_COUNT", "ResourceGroupId.h"),
        "reanim_types": reanim_types,
    }
    bindings = production["Sexy.TodLib/generated/NumericResourceBindings.h"]
    if legacy_fixture:
        # Explicit opt-in for frozen NRSC1 container tests, never production.
        registries["image"] = constant("IMAGE_SYMBOL_REGISTRY_CRC", "Sexy.TodLib/generated/NumericResourceBindings.h")
        registries["image_max"] = constant("IMAGE_SYMBOL_ID_MAX", "Sexy.TodLib/generated/ImageSymbolId.h")
        import importlib.util
        fixture_path = ROOT / "tools/native-tests/fixtures/legacy_numeric_resources.py"
        spec = importlib.util.spec_from_file_location("legacy_numeric_fixture", fixture_path)
        fixture = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(fixture)
        registries.update(fixture.binding_registries(production, constant, registries))
    else:
        registries["resource_version"] = 4
        registries["binding_crc"] = constant("RESOURCE_LOAD_BINDING_CRC", "Sexy.TodLib/generated/NumericResourceBindings.h")
        registries["image_count"] = constant("RESOURCE_LOAD_IMAGE_COUNT", "Sexy.TodLib/generated/NumericResourceBindings.h")
        registries["plan_crc"] = constant("RESOURCE_LOAD_PLAN_CRC", "Sexy.TodLib/generated/NumericResourceBindings.h")
        registries["plan_records"] = constant("RESOURCE_LOAD_PLAN_RECORD_COUNT", "Sexy.TodLib/generated/NumericResourceBindings.h")
        require(not any(token in bindings for token in
                        ("ResourceId", "ResourceSlot", "gNumericResourceKinds", "NUMERIC_RESOURCE_SLOT_COUNT")),
                "Historical resource slots remain in production bindings")
    track_source = production["Sexy.TodLib/ReanimTrackId.h"]
    attacher = re.search(r"\battacher__\s*\{\s*ReanimTrackId::(\w+)\s*,\s*ReanimTrackId::(\w+)\s*\}",
                        track_source)
    require(attacher is not None, "Missing numeric attacher track range")
    bounds = []
    for enum in attacher.groups():
        match = re.search(r"\b" + enum + r"\s*=\s*(\d+)\s*[,}]", track_source)
        require(match is not None, f"Missing numeric track enum: {enum}")
        bounds.append(int(match[1]))
    require(0 < bounds[0] <= bounds[1] <= registries["track_max"] + 1,
            "Invalid numeric attacher track range")
    registries["attacher_range"] = bounds
    scene_extents, scene_sheets = read_scene_atlas_metadata(root) if static_definitions else ({}, [])
    return {"method": "conservative lexical reference audit; unknown preprocessor branches retained",
            "disabled_test_macros": list(TEST_MACROS), "files": records,
            "fingerprint": digest(json.dumps(records, sort_keys=True).encode()),
            "accepted_numeric_references": accepted,
            "scale_binary_readers": binary_readers, "offline_scale_binary_readers": offline_readers,
            "numeric_definition_tables": {k: sorted(v) for k, v in table_paths.items()},
            "registries": registries,
            "definition_images": read_definition_image_bindings(root),
            "definition_variant_images": read_definition_image_binding_rows(root),
            "static_definitions": static_definitions,
            "scene_atlases": scene_extents, "scene_sheets": scene_sheets,
            "static_atlas_extents": {**read_definition_atlas_extents(root), **scene_extents}
                                    if static_definitions else {}}


def font_path(name: str) -> str:
    normalized = file_key(name)
    require(not normalized.endswith(".font.compiled"), f"Legacy font path is offline-only: {name}")
    if normalized.endswith(".compiled"):
        require(numeric_kind(normalized) == "fonts", "Font numeric path outside compiled/fonts")
        return normalized
    filename = PurePosixPath(normalized).name
    if filename.endswith(".txt"):
        filename = filename[:-4]
    return "compiled/fonts/" + filename + ".compiled"


class Reader:
    def __init__(self, data: bytes):
        self.data, self.at = data, 0

    def take(self, size: int) -> bytes:
        require(0 <= size <= len(self.data) - self.at, "Truncated numeric asset")
        result = self.data[self.at:self.at + size]
        self.at += size
        return result

    def word(self) -> int:
        return struct.unpack("<I", self.take(4))[0]

    def unpack(self, format_: str) -> tuple:
        return struct.unpack(format_, self.take(struct.calcsize(format_)))

    def vector(self, limit: int = 16384) -> tuple:
        count = self.count(4, limit)
        return self.unpack(f"<{count}i")

    def count(self, stride: int = 1, limit: int = 1 << 20) -> int:
        count = self.word()
        require(count <= limit and count * stride <= len(self.data) - self.at,
                "Invalid numeric collection size")
        return count

    def path(self) -> str:
        raw = self.take(self.count(limit=4096))
        require(b"\0" not in raw, "NUL in numeric file path")
        return raw.decode("utf-8")

    def finish(self) -> None:
        require(self.at == len(self.data), "Trailing numeric asset bytes")


def definition_path(name: str) -> str:
    normalized = file_key(name)
    require(normalized.startswith(("reanim/", "particles/")) and
            normalized.endswith((".reanim", ".xml")), f"Invalid definition source path: {name}")
    return "compiled/" + normalized.rsplit(".", 1)[0] + ".compiled"


def numeric_kind(name: str) -> str | None:
    normalized = key(name)
    if any(part in normalized.split(".") for part in ("xml", "reanim", "trail", "v4")):
        return None
    if normalized.startswith("compiled/reanim/") and normalized.endswith(".compiled"):
        return "reanim"
    if normalized.startswith("compiled/particles/") and normalized.endswith(".compiled"):
        return "particles"
    if (normalized.startswith("compiled/fonts/") and normalized.endswith(".compiled")
            and not normalized.endswith(".font.compiled")):
        return "fonts"
    if normalized == RESOURCE_PATH:
        return "resources"
    if normalized == PROJECTILE_PATH:
        return "projectiles"
    return None


def parse_projectiles(name: str, data: bytes) -> dict:
    """Decode the fixed PRJ1 wire record; it has no envelope or image IDs."""
    require(numeric_kind(name) == "projectiles", f"Invalid projectile record path: {name}")
    require(len(data) == 20 + 14 * 20 + 2 * 16, f"Invalid projectile record size: {name}")
    magic, version, count, width, height = struct.unpack_from("<5I", data)
    require((magic, version, count) == (0x314A5250, 1, 14),
            f"Invalid projectile record header: {name}")
    require(0 < width <= 0x7FFFFFFF and 0 < height <= 0x7FFFFFFF,
            f"Invalid projectile atlas dimensions: {name}")

    def rectangle(rect: tuple[int, ...], empty: bool = False) -> None:
        x, y, w, h = rect
        require(rect == (0, 0, 0, 0) if empty else
                x >= 2 and y >= 2 and w > 0 and h > 0
                and x + w + 2 <= width and y + h + 2 <= height,
                f"Invalid projectile atlas rectangle: {name}")

    regions, scales = [], []
    for index in range(count):
        x, y, w, h, scale = struct.unpack_from("<4if", data, 20 + 20 * index)
        rect = (x, y, w, h)
        rectangle(rect, index == 6)
        require(math.isfinite(scale) and scale > 0, f"Invalid projectile scale: {name}")
        regions.append(rect)
        scales.append(scale)
    shadows = [struct.unpack_from("<4i", data, 300 + 16 * index) for index in range(2)]
    for rect in shadows:
        rectangle(rect)
    return {"width": width, "height": height, "count": count, "regions": regions,
            "scales": scales, "shadows": shadows}


def validate_projectile_atlas_image(record: dict, data: bytes, name: str) -> None:
    from PIL import Image
    with Image.open(io.BytesIO(data)) as image:
        require(image.format == "PNG" and image.mode == "RGBA" and
                image.size == (record["width"], record["height"]),
                f"Invalid projectile atlas image: {name}")
        image.load()


def validate_reanim(reader: Reader, count: int, registries: dict,
                    image_ids: set[int] | None = None, *, inline_images: bool = False,
                    slot_count: int | None = None, authored: bool = False) -> None:
    tracks = []
    lo, hi = registries["attacher_range"]
    for _ in range(count):
        track, frames, stride, mode = reader.unpack("<4I")
        mode, storage = reanim_track_flags(mode, authored=authored)
        require(track <= registries["track_max"] and stride == 32
                and not (lo <= track < hi and mode != 0) and 0 < frames <= 1 << 20,
                "Invalid IDT5 track header")
        values = reader.take(frames * 32)
        require(all(math.isfinite(v) for frame in struct.iter_unpack("<6fif" if authored else "<8f", values) for v in frame),
                "Nonfinite IDT5 transform")
        tracks.append((track, frames, mode))
    for _, frames, mode in tracks:
        for _ in range(frames):
            image = 0 if inline_images else reader.word()
            if inline_images:
                reader.take(24)
            font = reader.word()
            require(font <= 1 and (image < slot_count if slot_count is not None else inline_images or image <= registries["image_max"]
                    and (image == 0 or not registries.get("legacy_fixture", False)
                         or registries["image_slots"][image] != RESOURCE_SLOT_NONE)),
                    "Invalid IDT5 resource ID")
            if image_ids is not None and image and slot_count is None:
                image_ids.add(image)
            if mode:
                length = reader.count(limit=0x7FFFFFFE)
                require(b"\0" not in reader.take(length), "NUL in IDT5 UI caption")
    previous_track = -1
    for _ in range(reader.count(8, count)):
        index, segments = reader.unpack("<2I")
        require(previous_track < index < count, "Invalid IDT5 attacher track index")
        previous_track = index
        track, frames, mode = tracks[index]
        require(lo <= track < hi and mode == 0 and segments <= frames,
                "Invalid IDT5 attacher track or count")
        previous_end = 0
        for _ in range(segments):
            start, end, target, action, rate, loop, run_start, run_end = reader.unpack("<4If3I")
            require(previous_end <= start < end <= frames and run_start <= start
                    and end <= run_end <= frames and run_start < run_end
                    and (target == 0xFFFFFFFF or target < registries["reanim_types"])
                    and action <= registries["track_max"] and math.isfinite(rate) and loop <= 2,
                    "Invalid IDT5 attacher segment")
            previous_end = end
    reader.finish()


def validate_particle(reader: Reader, count: int, body_size: int, registries: dict,
                      image_ids: set[int] | None = None, *, inline_images: bool = False,
                      explicit_frames: bool = False, slot_offsets: bool = False,
                      authored: bool = False) -> None:
    body = Reader(reader.take(body_size))

    def track() -> None:
        for _ in range(body.count(24 if authored else 20, 100000)):
            if authored:
                time, denominator, low, high, curve, distribution = body.unpack("<2i2f2I")
                require(denominator > 0, "Invalid IDRF node time denominator")
            else:
                time, low, high, curve, distribution = body.unpack("<3f2I")
            require(all(math.isfinite(v) for v in (time, low, high))
                    and curve <= 13 and distribution <= 13, "Invalid IDP5 track node")

    for _ in range(count):
        image = 0 if inline_images or explicit_frames else body.word()
        if slot_offsets:
            body.take(8)  # Prefix offset and frame count were checked by PAV6.
            body.take(12)
        elif explicit_frames:
            for _ in range(body.count(16 if inline_images else 4, 100000)):
                if inline_images:
                    x, y, w, h = body.unpack("<4i")
                    require(x >= 0 and y >= 0 and w > 0 and h > 0, "Invalid particle frame rectangle")
                else:
                    handle = body.word()
                    require(handle != 0, "Null particle frame image handle")
                    if image_ids is not None:
                        image_ids.add(handle)
            body.take(12)
        elif inline_images:
            body.take(24)
            body.take(20)
        else:
            body.take(20)
        require(body.word() <= 4 and (inline_images or explicit_frames or image <= registries["image_max"]
                and (image == 0 or not registries.get("legacy_fixture", False)
                     or registries["image_slots"][image] != RESOURCE_SLOT_NONE)),
                "Invalid IDP5 emitter identity")
        if image_ids is not None and image:
            image_ids.add(image)
        for _ in range(22):
            track()
        for _ in range(2):
            for _ in range(body.count(12, 4)):
                require(0 < body.word() < 12, "Invalid IDP5 field type")
                track()
                track()
        for _ in range(16):
            track()
    body.finish()
    require(reader.word() == 0x33544449 and reader.word() == count, "Invalid IDP5 IDT3 section")
    for _ in range(count):
        target = reader.word()
        require(target == 0xFFFFFFFF or target < count, "Invalid IDP5 duration target")
    require(reader.word() == 0x35544449 and reader.word() == count, "Invalid IDP5 IDT5 section")
    for _ in range(count):
        require(reader.word() < registries["emitter_count"], "Invalid IDP5 emitter name ID")
    reader.finish()


def validate_font(reader: Reader, maps: int, layers: int, tags: int,
                  references: set[tuple[str, str]]) -> None:
    seen = set()
    for _ in range(maps):
        source, target = reader.unpack("<2I")
        require(source <= 255 and target <= 255 and source not in seen, "Invalid font character map")
        seen.add(source)
    seen = set()
    for _ in range(layers):
        layer = reader.word()
        require(layer < layers and layer not in seen, "Invalid font layer ID")
        seen.add(layer)
        path = reader.path()
        require(bool(path), "Font layer has no image path")
        file_key(path)
        references.add(("image", path))
        for _ in range(2):
            for _ in range(reader.count(4, 4096)):
                require(reader.word() < tags, "Invalid font tag ID")
        scalars = reader.unpack("<22i")
        require(-1 <= scalars[0] <= 1 and scalars[13] in (0, 1), "Invalid font scalar flags")
        glyphs = set()
        for _ in range(reader.count(40, 128)):
            glyph = reader.word()
            require(glyph < 128 and glyph not in glyphs, "Invalid font glyph ID")
            glyphs.add(glyph)
            glyph_values = reader.unpack("<8i")
            require(all(v >= 0 for v in glyph_values[:4]), "Invalid font glyph rectangle")
            kerns = set()
            for _ in range(reader.count(8, 256)):
                next_, _ = reader.unpack("<Ii")
                require(next_ <= 255 and next_ not in kerns, "Invalid font kerning ID")
                kerns.add(next_)
    reader.finish()


def canonical_particle_path(name: str) -> bool:
    if not isinstance(name, str) or not name.startswith("compiled/particles/") or not name.endswith(".compiled"):
        return False
    leaf = name[len("compiled/particles/"):-len(".compiled")]
    try:
        name.encode("utf-8")
    except UnicodeEncodeError:
        return False
    return (bool(leaf) and leaf not in (".", "..") and
            all(ord(c) >= 128 or c in "abcdefghijklmnopqrstuvwxyz0123456789_.-" for c in leaf))


def parse_reanim_variants(name: str, data: bytes) -> dict:
    envelope = Reader(data[12:])
    magic, behavior_bytes = envelope.unpack("<2I")
    require(magic == 0x37564152, "Invalid RAV7 header")
    behavior = envelope.take(behavior_bytes)
    appearance_offset = 12 + envelope.at
    reader = Reader(envelope.take(len(envelope.data) - envelope.at))
    variants, slots, actions = reader.unpack("<3I")
    require(0 < variants <= 1 << 20 and
            0 < slots <= 1 << 20 and actions <= 1 << 20, "Invalid RAV6 counts")
    dimensions = [reader.unpack("<2I") for _ in range(variants)]
    action_keys = [reader.unpack("<2I") for _ in range(actions)]
    require(action_keys == sorted(set(action_keys)), "Invalid RAV6 action order")
    options = {}
    for track, option in action_keys:
        options.setdefault(track, []).append(option)
    require(all(values == list(range(len(values))) for values in options.values()),
            "Invalid RAV6 contiguous action options")
    transitions = [reader.word() for _ in range(variants * actions)]
    require(all(target < variants for target in transitions), "Invalid RAV6 transition target")
    states = []
    for v in range(variants):
        state = {}
        for a, (track, option) in enumerate(action_keys):
            if transitions[v * actions + a] == v:
                require(track not in state, "Ambiguous RAV6 variant option")
                state[track] = option
        require(set(state) == set(options), "Missing RAV6 variant option")
        states.append(state)
    require(all(option == 0 for option in states[0].values()), "Invalid RAV6 default options")
    for v in range(variants):
        for a, (track, option) in enumerate(action_keys):
            target = transitions[v * actions + a]
            require(states[target] == {**states[v], track: option},
                    "RAV6 transition changes unrelated option")
    rectangles, offsets, counts = [], [], []
    for v, (width, height) in enumerate(dimensions):
        row, row_offsets = [], []
        for slot in range(slots):
            row_offsets.append(appearance_offset + reader.at)
            rect = reader.unpack("<4i")
            x, y, w, h = rect
            if slot == 0 or not any(rect):
                require(rect == (0, 0, 0, 0) and (slot == 0 or v != 0),
                        "Invalid RAV6 blank/default rectangle")
            else:
                require(0 < width <= 16384 and 0 < height <= 16384 and x >= 2 and y >= 2
                        and w > 0 and h > 0 and x + w + 2 <= width and y + h + 2 <= height,
                        "RAV6 rectangle outside atlas")
            row.append(rect)
        count = sum(any(rect) for rect in row)
        require((count > 0 and 0 < width <= 16384 and 0 < height <= 16384) or
                (count == 0 and width == height == 0), "Invalid RAV6 atlas dimensions")
        rectangles.append(row)
        offsets.append(row_offsets)
        counts.append(count)
    reader.finish()
    body_offset = 20
    reader = Reader(behavior)
    authored = behavior[:4] == b"IDTF"
    inner, _, image, _, fps, track_count = reader.unpack("<4IfI")
    require(inner in (0x36544449, 0x46544449) and image == 0 and math.isfinite(fps) and fps > 0
            and 0 < track_count <= 1 << 20, "Invalid IDT6 header")
    tracks = []
    for _ in range(track_count):
        track, frames, stride, mode = reader.unpack("<4I")
        mode, _ = reanim_track_flags(mode, authored=authored)
        require(stride == 32 and frames <= 1 << 20, "Invalid IDT6 track")
        reader.take(frames * 32)
        tracks.append((track, frames, mode))
    require(set(options) <= {track for track, _, _ in tracks}, "Unknown RAV6 action track")
    frame_slots = []
    for _, frames, mode in tracks:
        for _ in range(frames):
            slot = reader.word()
            require(slot < slots, "Invalid IDT6 image slot")
            frame_slots.append(slot)
            reader.take(4)
            if mode:
                reader.take(reader.word())
    return {"width": dimensions[0][0], "height": dimensions[0][1],
            "count": sum(counts), "regions": [], "image_fields": [], "image_offsets": [],
            "frame_fields4I": [rectangles[0][slot] for slot in frame_slots if slot],
            "frame_offsets": [offsets[0][slot] for slot in frame_slots if slot],
            "total_frame_count": sum(slot != 0 for slot in frame_slots),
            "body_offset": body_offset, "behavior_bytes": behavior_bytes, "appearance_offset": appearance_offset,
            "variant_count": variants, "slot_count": slots,
            "action_count": actions, "variant_dimensions": dimensions,
            "variant_rectangles": rectangles, "variant_counts": counts,
            "variant_rectangle_offsets": offsets, "frame_slots": frame_slots,
            "action_keys": action_keys, "transitions": transitions}


def parse_particle_variants(name: str, data: bytes) -> dict:
    envelope = Reader(data[12:])
    magic, behavior_bytes = envelope.unpack("<2I")
    require(magic == 0x36564150, "Invalid PAV6 header")
    behavior = Reader(envelope.take(behavior_bytes))
    inner, total, emitter_hash, emitters, body_size = behavior.unpack("<5I")
    require(inner in (0x36524449, 0x46524449) and emitters <= 1000, "Invalid IDR6/IDRF header")
    stride = 24 if inner == 0x46524449 else 20
    body = Reader(behavior.take(body_size))
    frame_counts, slot_offsets, offset = [], [], 0
    for _ in range(emitters):
        start, frames = body.unpack("<2I")
        require(start == offset and frames <= 100000 and start + frames <= total,
                "Invalid IDR6 emitter slot range")
        slot_offsets.append(start)
        frame_counts.append(frames)
        offset += frames
        body.take(16)
        for _ in range(22):
            body.take(body.count(stride, 100000) * stride)
        for _ in range(2):
            for _ in range(body.count(12, 4)):
                body.take(4)
                for _ in range(2):
                    body.take(body.count(stride, 100000) * stride)
        for _ in range(16):
            body.take(body.count(stride, 100000) * stride)
    body.finish()
    require(offset == total, "IDR6 total frame count differs")
    appearance_offset = 12 + envelope.at
    variants, rects = envelope.unpack("<2I")
    require(0 < variants <= 1 << 20 and rects == total, "Invalid PAV6 appearance counts")
    dimensions = [envelope.unpack("<2I") for _ in range(variants)]
    rectangles, offsets, counts = [], [], []
    for v, (width, height) in enumerate(dimensions):
        row, positions = [], []
        for _ in range(total):
            positions.append(12 + envelope.at)
            rect = envelope.unpack("<4i")
            x, y, w, h = rect
            require((v > 0 and rect == (0, 0, 0, 0)) or
                    (x >= 2 and y >= 2 and w > 0 and h > 0 and
                     x + w + 2 <= width and y + h + 2 <= height),
                    "PAV6 frame outside atlas or invalid default rectangle")
            row.append(rect)
        count = sum(any(rect) for rect in row)
        require((count > 0 and 0 < width <= 16384 and 0 < height <= 16384) or
                (count == 0 and width == height == 0), "Invalid PAV6 atlas dimensions")
        rectangles.append(row)
        offsets.append(positions)
        counts.append(count)
    envelope.finish()
    return {"width": dimensions[0][0], "height": dimensions[0][1], "count": sum(counts),
            "regions": [], "image_fields": [], "image_offsets": [],
            "frame_fields4I": rectangles[0], "frame_offsets": offsets[0],
            "frame_counts": frame_counts, "slot_offsets": slot_offsets,
            "total_frame_count": total, "emitter_count": emitters, "emitter_name_hash": emitter_hash,
            "body_offset": 20, "behavior_bytes": behavior_bytes, "appearance_offset": appearance_offset,
            "variant_count": variants,
            "slot_count": total, "variant_dimensions": dimensions, "variant_rectangles": rectangles,
            "variant_rectangle_offsets": offsets, "variant_counts": counts, "parent": None}


def parse_definition_atlas(name: str, data: bytes) -> dict | None:
    """Scan inline geometry. Texture paths and resource symbols are not stored."""
    kind = numeric_kind(name)
    if kind not in ("reanim", "particles") or len(data) < 12:
        return None
    cookie = struct.unpack_from("<I", data)[0]
    if cookie not in ((0xDEADFED9, 0xDEADFEDB) if kind == "reanim" else (0xDEADFEDA, 0xDEADFEDC)):
        return None
    envelope = Reader(data)
    _, size, crc = envelope.unpack("<3I")
    require(size == len(data) - 12 and zlib.crc32(data[12:]) == crc,
            f"Invalid {kind} envelope: {name}")
    if kind == "reanim" and struct.unpack_from("<I", data, 12)[0] == 0x37564152:
        require(cookie == 0xDEADFEDB, "RAV7 requires the v6 envelope")
        return parse_reanim_variants(name, data)
    if kind == "particles" and struct.unpack_from("<I", data, 12)[0] == 0x36564150:
        require(cookie == 0xDEADFEDC, "PAV6 requires the v6 envelope")
        return parse_particle_variants(name, data)
    require(cookie in (0xDEADFED9, 0xDEADFEDA), "Invalid v6 appearance payload")
    prefix = Reader(data[12:])
    magic, width, height = prefix.unpack("<3I")
    require(magic == (0x35564152 if kind == "reanim" else 0x35564150),
            f"Invalid v5 atlas header: {name}")
    reader = Reader(data[24:])
    fields, offsets, frame_fields, frame_offsets = [], [], [], []
    parent, frame_counts = None, []
    emitter_hash = None

    def image() -> None:
        offsets.append(24 + reader.at)
        fields.append(reader.unpack("<6I"))

    if kind == "reanim":
        inner, _, _, _, _, count = reader.unpack("<6I")
        require(inner in (0x35544449, 0x46544449) and count <= 1 << 20, "Invalid inline IDT5/IDTF header")
        tracks = []
        for _ in range(count):
            _, frames, stride, mode = reader.unpack("<4I")
            mode, _ = reanim_track_flags(mode, authored=inner == 0x46544449)
            require(stride == 32 and frames <= 1 << 20, "Invalid inline IDT5 track")
            reader.take(frames * 32)
            tracks.append((frames, mode))
        for frames, mode in tracks:
            for _ in range(frames):
                image()
                reader.take(4)  # Independent font ID.
                if mode:
                    reader.take(reader.word())
    else:
        inner, total_frames, registry_or_path_size, count, body_size = reader.unpack("<5I")
        if inner == 0x31564449:
            emitter_hash, path_size, total_frames, count = total_frames, registry_or_path_size, count, body_size
            require(0 < path_size <= 4096 and count <= 1000, "Invalid inherited particle header")
            try:
                parent = reader.take(path_size).decode("utf-8")
            except UnicodeDecodeError as error:
                raise ValueError("Invalid particle parent UTF-8") from error
            require(canonical_particle_path(parent), "Invalid particle parent path")
            for _ in range(count):
                frames = reader.count(16, 100000)
                frame_counts.append(frames)
                for _ in range(frames):
                    frame_offsets.append(24 + reader.at)
                    frame_fields.append(reader.unpack("<4i"))
            require(reader.at == len(reader.data) and total_frames == len(frame_fields),
                    "Invalid inherited particle size or frame count")
        else:
            emitter_hash = struct.unpack_from("<I", data, 32)[0]
        require(inner in (0x35504449, 0x35524449, 0x46524449, 0x31564449) and count <= 1000, "Invalid inline particle header")
        stride = 24 if inner == 0x46524449 else 20
        if inner in (0x35524449, 0x46524449):
            require(total_frames <= body_size // 16, "Invalid particle total frame count")
        end = reader.at + body_size

        def track() -> None:
            reader.take(reader.count(stride, 100000) * stride)

        for _ in range(0 if parent is not None else count):
            if inner in (0x35524449, 0x46524449):
                frames = reader.count(16, 100000)
                frame_counts.append(frames)
                for _ in range(frames):
                    frame_offsets.append(24 + reader.at)
                    frame_fields.append(reader.unpack("<4i"))
                reader.take(16)
            else:
                image()
                reader.take(24)  # Original emitter properties and type.
            for _ in range(22):
                track()
            for _ in range(2):
                for _ in range(reader.count(12, 4)):
                    reader.take(4)
                    track()
                    track()
            for _ in range(16):
                track()
        require(parent is not None or reader.at == end, "Invalid inline IDP5 body size")
        if inner in (0x35524449, 0x46524449):
            require(total_frames == len(frame_fields), "Particle total frame count differs")
    regions = list(dict.fromkeys(region for region in fields if any(region)))
    require((not regions and not frame_fields and width == height == 0) or
            ((regions or frame_fields) and 0 < width <= 16384 and 0 < height <= 16384),
            f"Invalid v5 atlas header: {name}")
    rectangles = []
    for x, y, w, h, rows, cols in regions:
        require(w > 0 and h > 0 and 0 < rows <= h and 0 < cols <= w
                and x >= 2 and y >= 2 and x + w + 2 <= width and y + h + 2 <= height,
                f"Invalid v5 atlas region: {name}")
        rectangle = (x - 2, y - 2, x + w + 2, y + h + 2)
        require(all(rectangle == old or rectangle[2] <= old[0] or old[2] <= rectangle[0]
                    or rectangle[3] <= old[1] or old[3] <= rectangle[1]
                    for old in rectangles), f"Overlapping v5 atlas regions: {name}")
        rectangles.append(rectangle)
    for x, y, w, h in frame_fields:
        require(x >= 0 and y >= 0 and w > 0 and h > 0 and x + w <= width and y + h <= height,
                f"Particle frame outside atlas: {name}")
    return {"width": width, "height": height, "count": len(regions) + len(frame_fields), "regions": regions,
            "image_fields": fields, "image_offsets": offsets, "body_offset": 24,
            "frame_fields4I": frame_fields, "frame_offsets": frame_offsets,
            "total_frame_count": len(frame_fields), "parent": parent,
            "frame_counts": frame_counts, "emitter_name_hash": emitter_hash,
            "emitter_count": count if kind == "particles" else None}


def validate_particle_inheritance(records: dict[str, bytes]) -> None:
    particles = {name: parse_definition_atlas(name, data) for name, data in records.items()
                 if numeric_kind(name) == "particles"}
    state = {}

    def visit(name):
        require(state.get(name) != 1, f"Particle inheritance cycle: {name}")
        if state.get(name) == 2:
            return
        state[name] = 1
        child = particles[name]
        if child and child["parent"] is not None:
            parent_name = child["parent"]
            require(parent_name in particles and particles[parent_name] is not None,
                    f"Missing particle parent: {name} -> {parent_name}")
            visit(parent_name)
            parent = particles[parent_name]
            require(child["emitter_count"] == parent["emitter_count"]
                    and len(parent["frame_counts"]) == parent["emitter_count"]
                    and child["frame_counts"] == parent["frame_counts"]
                    and child["emitter_name_hash"] == parent["emitter_name_hash"],
                    f"Particle inheritance emitter/frame counts or registry differ: {name}")
        state[name] = 2

    for name in particles:
        visit(name)


def resource_records(data: bytes, *, legacy_fixture: bool = False) -> list[tuple[int, int, tuple, tuple, bytes]]:
    """Decode NRSC4 metadata. NRSC2 is an explicit historical fixture route."""
    require(len(data) >= 32, "Truncated resource manifest")
    magic, version, _, groups, count, _, crc, size = struct.unpack_from("<8I", data)
    require(magic == 0x4353524E and version == (2 if legacy_fixture else 4) and size == len(data) - 32
            and zlib.crc32(data[32:]) == crc, "Invalid resource header")
    reader, result, previous_group = Reader(data[32:]), [], 0
    for _ in range(groups):
        group, records, block_bytes = reader.unpack("<3I")
        require(group > previous_group, "Resource groups must be unique and canonically ordered")
        previous_group = group
        block = Reader(reader.take(block_bytes))
        for step in range(records):
            record = Reader(block.take(block.word()))
            fields = record.unpack("<5I" if legacy_fixture else "<4I")
            paths = tuple(record.path() for _ in range(4))
            tail = record.take(len(record.data) - record.at)
            require(legacy_fixture or fields[0] != 0 or len(tail) == 4,
                    "Invalid NRSC4 image tail: expected only alpha color")
            result.append((group, step, fields, paths, tail))
        block.finish()
    reader.finish()
    require(len(result) == count, "Resource load record count differs")
    return result


def validate_legacy_resource_plan(data: bytes, registries: dict, references: set,
                           font_sources: set[str] | None = None) -> set[str]:
    require(len(data) >= 32, "Truncated NRSC2 resource manifest")
    magic, version, symbols, groups, count, image, crc, size = struct.unpack_from("<8I", data)
    require(magic == 0x4353524E and version == 2
            and symbols == registries["image_max"] + 1 and image == registries["image"]
            and 0 < groups <= registries["groups"]
            and count == registries["plan_records"] and crc == registries["plan_crc"],
            "Invalid NRSC2 baked load-plan contract")
    fonts, image_symbols, local_kinds = set(), set(), {}
    for group, step, fields, paths, tail in resource_records(data, legacy_fixture=True):
        kind, symbol, flags, variant, reference = fields
        require(group <= registries["groups"] and kind <= 2 and flags <= 16383
                and (0 < symbol < symbols and symbol not in image_symbols if kind == 0 else symbol == 0)
                and (reference == RESOURCE_SLOT_NONE or kind == 2
                     and reference < step and local_kinds.get((group, reference)) == 2),
                "Invalid NRSC2 resource identity or font dependency")
        local_kinds[group, step] = kind
        path, alpha, alpha_grid, font_image = paths
        active = not flags & (1 | 8192)
        require(not active or bool(path) or reference != RESOURCE_SLOT_NONE,
                "Resource has no file path")
        record = Reader(tail)
        if kind == 0:
            image_symbols.add(symbol)
            require(not flags & 6 and not font_image and reference == RESOURCE_SLOT_NONE,
                    "Invalid image resource flags/paths")
            rows, cols, _, anim, delay, begin, end = record.unpack("<iiII3i")
            per_frame, frame_map = record.vector(), record.vector()
            require(rows > 0 and cols > 0 and anim <= 3
                    and (anim == 0 or max(rows, cols) <= 16384)
                    and min(delay, begin, end, *per_frame) >= 0
                    and all(0 <= frame < max(rows, cols) for frame in frame_map),
                    "Invalid numeric image grid or animation")
            if active:
                references.add(("image", path))
                references.update(("image", p) for p in (alpha, alpha_grid) if p)
        elif kind == 1:
            require(not flags & 6 and not any((alpha, alpha_grid, font_image))
                    and reference == RESOURCE_SLOT_NONE, "Invalid sound resource flags/paths")
            volume, _ = record.unpack("<di")
            require(math.isfinite(volume), "Invalid numeric sound volume")
            if active:
                references.add(("sound", path))
        else:
            require(not any((alpha, alpha_grid, font_image)) and not flags & 2
                    and bool(flags & 4) == (reference != RESOURCE_SLOT_NONE),
                    "Invalid font resource flags/paths")
            _, style = record.unpack("<iI")
            require(style <= 15 and all(tag > 0 for tag in record.vector()),
                    "Invalid numeric font style/tags")
            if active and reference == RESOURCE_SLOT_NONE:
                fonts.add(font_path(path))
                if font_sources is not None:
                    font_sources.add(file_key(path))
        for path in paths:
            if path:
                file_key(path)
        record.finish()
    require(image_symbols == set(range(1, symbols)), "Missing live ImageSymbol load binding")
    return fonts


def validate_resource_plan(data: bytes, registries: dict, references: set,
                           font_sources: set[str] | None = None) -> set[str]:
    require(len(data) >= 32, "Truncated NRSC4 resource manifest")
    magic, version, images, groups, count, bindings, crc, size = struct.unpack_from("<8I", data)
    require(magic == 0x4353524E and version == 4
            and images == registries["image_count"] and bindings == registries["binding_crc"]
            and 0 < groups <= registries["groups"]
            and count == registries["plan_records"] and crc == registries["plan_crc"],
            "Invalid NRSC4 baked load-plan contract")
    fonts, local_kinds, image_count = set(), {}, 0
    variants = {}
    for group, step, fields, paths, tail in resource_records(data):
        kind, flags, variant, reference = fields
        require(group <= registries["groups"] and kind <= 2 and flags <= 16383
                and (kind == 0 or variant == 0)
                and (reference == RESOURCE_SLOT_NONE or kind == 2
                     and reference < step and local_kinds.get((group, reference)) == 2),
                "Invalid NRSC4 resource metadata or font dependency")
        local_kinds[group, step] = kind
        path, alpha, alpha_grid, font_image = paths
        active = not flags & (1 | 8192)
        require(not active or bool(path) or reference != RESOURCE_SLOT_NONE,
                "Resource has no file path")
        record = Reader(tail)
        if kind == 0:
            image_count += 1
            require(not flags & 6 and not font_image and reference == RESOURCE_SLOT_NONE,
                    "Invalid image resource flags/paths")
            # Different decoding recipes must never collide in the shared cache.
            recipe = (flags, paths, tail)
            key = (path.casefold(), variant)
            require(key not in variants or variants[key] == recipe,
                    "Conflicting image recipes share a cache variant")
            variants[key] = recipe
            record.word()  # Alpha color is the complete production image tail.
            if active:
                references.add(("image", path))
                references.update(("image", p) for p in (alpha, alpha_grid) if p)
        elif kind == 1:
            require(not flags & 6 and not any((alpha, alpha_grid, font_image))
                    and reference == RESOURCE_SLOT_NONE, "Invalid sound resource flags/paths")
            volume, _ = record.unpack("<di")
            require(math.isfinite(volume), "Invalid numeric sound volume")
            if active:
                references.add(("sound", path))
        else:
            require(not any((alpha, alpha_grid, font_image)) and not flags & 2
                    and bool(flags & 4) == (reference != RESOURCE_SLOT_NONE),
                    "Invalid font resource flags/paths")
            _, style = record.unpack("<iI")
            require(style <= 15 and all(tag > 0 for tag in record.vector()),
                    "Invalid numeric font style/tags")
            if active and reference == RESOURCE_SLOT_NONE:
                fonts.add(font_path(path))
                if font_sources is not None:
                    font_sources.add(file_key(path))
        for path in paths:
            if path:
                file_key(path)
        record.finish()
    require(image_count == images, "Image load record count differs from binding contract")
    return fonts


def validate_numeric(name: str, data: bytes, registries: dict,
                     references: set[tuple[str, str]] | None = None, *,
                     font_sources: set[str] | None = None,
                     image_ids: set[int] | None = None,
                     atlas_path: str | None = None,
                     atlas_paths: list[str] | None = None) -> set[str]:
    references = references if references is not None else set()
    kind = numeric_kind(name)
    require(kind is not None, f"Unrecognized numeric asset: {name}")
    if kind == "projectiles":
        parse_projectiles(name, data)
        path = PROJECTILE_ATLAS_PATH if atlas_path is None else atlas_path
        require(bool(path), f"Missing projectile atlas binding: {name}")
        path = file_key(path)
        require(path.startswith("compiled/") and path.endswith(".atlas.png"),
                f"Invalid projectile atlas binding: {path}")
        references.add(("atlas", path))
        return set()
    if kind in ("reanim", "particles"):
        require(len(data) >= (40 if kind == "reanim" else 48), f"Truncated {kind} header: {name}")
        cookie, size, crc = struct.unpack_from("<3I", data)
        v5 = cookie in ((0xDEADFED9, 0xDEADFEDB) if kind == "reanim" else (0xDEADFEDA, 0xDEADFEDC))
        require((v5 or cookie == (0xDEADFED7 if kind == "reanim" else 0xDEADFED8))
                and size == len(data) - 12 and zlib.crc32(data[12:]) == crc,
                f"Invalid {kind} envelope: {name}")
        body_offset = 12
        atlas = {}
        if v5:
            atlas = parse_definition_atlas(name, data)
            regions = atlas["count"]
            body_offset = atlas["body_offset"]
            size -= body_offset - 12
            registries = {**registries, "image": 0}
            if regions and atlas_path:
                references.add(("atlas", file_key(atlas_path)))
            require(regions or not atlas_path, f"Empty definition has an atlas binding: {name}")
            if "variant_count" in atlas and atlas_paths is not None:
                require(len(atlas_paths) == atlas["variant_count"], "Incomplete RAV6 atlas bindings")
                for index, path in enumerate(atlas_paths):
                    require(bool(path) == bool(atlas["variant_counts"][index]),
                            "RAV6 atlas binding differs from rectangle row")
                    if path:
                        references.add(("atlas", file_key(path)))
        if kind == "reanim":
            authored = data[body_offset:body_offset + 4] == b"IDTF"
            magic, track, image, font, fps, count = struct.unpack_from("<4IfI", data, body_offset)
            require(magic in (0x46544449, (0x36544449 if "variant_count" in atlas else 0x35544449)) and (track, image, font) ==
                    (registries["track"], registries["image"], registries["font"])
                    and math.isfinite(fps) and fps > 0 and 0 < count <= registries["track_capacity"],
                    f"Invalid IDT5 header: {name}")
            behavior_end = body_offset + atlas["behavior_bytes"] if "behavior_bytes" in atlas else len(data)
            validate_reanim(Reader(data[body_offset + 24:behavior_end]), count, registries,
                            None if v5 else image_ids, inline_images=v5 and "variant_count" not in atlas,
                            slot_count=atlas.get("slot_count") if v5 else None, authored=authored)
        else:
            magic, image, emitter, count, body = struct.unpack_from("<5I", data, body_offset)
            if magic in (0x36524449, 0x46524449) and "variant_count" in atlas:
                require(v5 and emitter == registries["emitter"] and image == atlas["total_frame_count"]
                        and count == atlas["emitter_count"] and body + 36 + 8 * count == atlas["behavior_bytes"],
                        f"Invalid IDR6 header: {name}")
                validate_particle(Reader(data[body_offset + 20:body_offset + atlas["behavior_bytes"]]),
                                  count, body, registries, inline_images=True, slot_offsets=True,
                                  authored=magic == 0x46524449)
                return set()
            if magic == 0x31564449:
                require(v5 and image == registries["emitter"], f"Invalid IDV1 emitter registry: {name}")
                return set()
            explicit_frames = magic in (0x34524449, 0x35524449, 0x46524449)
            require(magic in (0x35504449, 0x34524449, 0x35524449, 0x46524449) and
                    (magic != 0x34524449 or not v5) and (magic != 0x35524449 or v5) and
                    emitter == registries["emitter"] and
                    image == (atlas["total_frame_count"] if magic == 0x35524449 or (magic == 0x46524449 and v5) else
                              0 if explicit_frames else registries["image"])
                    and count <= 1000 and body + 36 + 8 * count == size,
                    f"Invalid IDP5 header: {name}")
            validate_particle(Reader(data[body_offset + 20:]), count, body, registries,
                              None if v5 else image_ids, inline_images=v5,
                              explicit_frames=explicit_frames, authored=magic == 0x46524449)
        return set()
    if kind == "fonts":
        require(32 <= len(data) <= 16 * 1024 * 1024, f"Invalid font size: {name}")
        magic, version, size, crc, _, maps, layers, tags = struct.unpack_from("<4Ii3I", data)
        require(magic == 0x544E464E and version == 1 and size == len(data) - 32
                and zlib.crc32(data[32:]) == crc and maps <= 256
                and 0 < layers <= 1024 and tags <= 4096, f"Invalid numeric font header: {name}")
        validate_font(Reader(data[32:]), maps, layers, tags, references)
        return set()
    if registries.get("legacy_fixture", False) and registries.get("resource_version") == 2:
        return validate_legacy_resource_plan(data, registries, references, font_sources)
    if not registries.get("legacy_fixture", False):
        return validate_resource_plan(data, registries, references, font_sources)
    # Only the explicitly opted-in, frozen offline NRSC1 fixture suite reaches
    # this decoder. Production packages always require NRSC4 above.
    require(len(data) >= 36, "Truncated numeric.resources")
    magic, version, base, symbols, slots, records, image, crc, size = struct.unpack_from("<9I", data)
    require(magic == 0x4353524E and version == 1 and base == 879 and symbols == registries["image_max"] + 1
            and slots == registries["slots"]
            and records == slots - registries["resource_kinds"].count(RESOURCE_KIND_NONE)
            and image == registries["image"]
            and size == len(data) - 36 and zlib.crc32(data[36:]) == crc,
            "Invalid numeric.resources header")
    expected_slots = {slot for slot in range(slots)
                      if slot >= base or registries["resource_kinds"][slot] != RESOURCE_KIND_NONE}
    expected_symbols = {symbol for symbol in range(1, symbols)
                        if registries["image_slots"][symbol] != RESOURCE_SLOT_NONE}
    legacy_image_slots = set(registries.get("legacy_image_slots", []))
    require(all(slot < base and registries["resource_kinds"][slot] == 0
                and slot not in {registries["image_slots"][symbol] for symbol in expected_symbols}
                for slot in legacy_image_slots), "Invalid legacy-only image resource slot")
    reader, fonts, seen = Reader(data[36:]), set(), set()
    image_symbols, kinds, font_targets = set(), {}, {}
    for _ in range(records):
        record = Reader(reader.take(reader.word()))
        slot, symbol, group, type_, flags, reference, _ = record.unpack("<7I")
        require(slot in expected_slots and slot not in seen and type_ <= 2 and flags <= 16383
                and type_ == (registries["resource_kinds"][slot] if slot < base else 0)
                and (0 < group <= registries["groups"] if slot < base else group == 0)
                and (reference == RESOURCE_SLOT_NONE or type_ == 2 and reference in expected_slots)
                and ((symbol == 0 and slot in legacy_image_slots or
                      symbol in expected_symbols and symbol not in image_symbols
                      and registries["image_slots"][symbol] == slot) if type_ == 0 else symbol == 0),
                "Invalid resource descriptor identity")
        seen.add(slot)
        kinds[slot] = type_
        path, alpha, alpha_grid, font_image = (record.path() for _ in range(4))
        active = not flags & (1 | 8192)
        require(not active or bool(path) or reference != 0xFFFFFFFF, "Resource has no file path")
        if type_ == 0:
            if symbol:
                image_symbols.add(symbol)
            require(not flags & 6 and not font_image, "Invalid image resource flags/paths")
            rows, cols, _, anim, delay, begin, end = record.unpack("<iiII3i")
            per_frame, frame_map = record.vector(), record.vector()
            require(rows > 0 and cols > 0 and anim <= 3
                    and (anim == 0 or max(rows, cols) <= 16384)
                    and min(delay, begin, end, *per_frame) >= 0
                    and all(0 <= f < max(rows, cols) for f in frame_map),
                    "Invalid numeric image grid or animation")
            if active:
                references.add(("image", path))
                references.update(("image", p) for p in (alpha, alpha_grid) if p)
        elif type_ == 1:
            require(not flags & 6 and not any((alpha, alpha_grid, font_image)),
                    "Invalid sound resource flags/paths")
            volume, _ = record.unpack("<di")
            require(math.isfinite(volume), "Invalid numeric sound volume")
            if active:
                references.add(("sound", path))
        else:
            require(not alpha and not alpha_grid and not font_image
                    and not flags & 2
                    and bool(flags & 4) == (reference != 0xFFFFFFFF),
                    "Invalid font resource flags/paths")
            size, style = record.unpack("<iI")
            require(style <= 15
                    and all(t > 0 for t in record.vector()), "Invalid numeric font style/tags")
            if active:
                if reference == 0xFFFFFFFF:
                    fonts.add(font_path(path))
                    if font_sources is not None:
                        font_sources.add(file_key(path))
                else:
                    font_targets[slot] = reference
        for p in (path, alpha, alpha_grid, font_image):
            if p:
                file_key(p)
        record.finish()
    reader.finish()
    require(seen == expected_slots, "Missing numeric resource slot")
    require(image_symbols == expected_symbols, "Missing numeric image symbol binding")
    done = set()
    for slot in font_targets:
        chain = set()
        while slot in font_targets and slot not in done:
            require(slot not in chain, "Cyclic numeric font reference")
            chain.add(slot)
            slot = font_targets[slot]
            require(kinds.get(slot) == 2, "Numeric font reference does not name a font slot")
        done.update(chain)
    return fonts


def effect_references(name: str, data: bytes) -> set[str]:
    """Walk the VSFX disk framing, paths and SPIR-V instructions, not text grep."""
    reader = Reader(data)
    require(reader.word() == 0x58465356 and reader.word() == 1, f"Invalid VSFX header: {name}")
    source = reader.path().replace("\\", "/")
    runtime_path = name.replace("\\", "/")[:-4]
    require(source.casefold() == runtime_path.casefold()
            or source.casefold().endswith("/" + runtime_path.casefold()),
            f"Effect bake path differs: {name}: {source}")
    # Production explicitly rebases this field before resolving DDS paths.
    key(runtime_path)
    input_, output = reader.unpack("<2I")
    protocol_keys = set()
    for index in range(reader.count(36)):
        protocol, id_, integer, default, minimum, maximum, step, inline = reader.unpack("<Q2I5f")
        require(id_ == index and integer <= 1 and protocol not in protocol_keys
                and all(math.isfinite(v) for v in (default, minimum, maximum, step, inline))
                and minimum <= maximum and step > 0, "Invalid VSFX parameter metadata")
        protocol_keys.add(protocol)

        def normalized(value: float) -> float:
            if integer:
                value = math.copysign(math.floor(abs(value) + 0.5), value)
                require(-2147483648 <= value <= 2147483647, "VSFX integer parameter overflow")
                value = struct.unpack("<f", struct.pack("<f", value))[0]
                require(-2147483648 <= value <= 2147483647, "VSFX integer parameter rounding overflow")
            require(minimum <= value <= maximum, "VSFX parameter outside range")
            return value

        normalized(default)
        require(normalized(inline) == inline, "Invalid VSFX inline parameter")

    def expression() -> None:
        count = reader.count(8)
        depth = 0
        for _ in range(count):
            op, literal = reader.unpack("<Ii")
            require(op <= 8 and (literal >= 0 if op == 0 else literal == 0),
                    "Invalid VSFX size expression")
            if op <= 4:
                depth += 1
            else:
                require(depth >= 2, "Invalid VSFX expression stack")
                depth -= 1
        require(count == 0 or depth == 1, "Invalid VSFX expression result")

    refs = set()
    textures = reader.count(20)
    require(input_ < textures and output < textures and input_ != output, "Invalid VSFX input/output IDs")
    produced, external = {input_}, set()
    # Disk values of the 20 VkFormats accepted by VulkanEffectFormats.h.
    formats = {109, 97, 91, 92, 103, 64, 122, 37, 38, 83, 77, 78, 100, 16, 17, 76, 70, 71, 9, 10}
    for index in range(textures):
        id_, format_ = reader.unpack("<2I")
        require(id_ == index and format_ in formats, "Invalid VSFX texture identity/format")
        path = reader.path().replace("\\", "/")
        if path:
            require(index not in (input_, output), "VSFX input/output has an external source")
            require(not path.startswith("/") and ":" not in path, "Absolute effect dependency is not portable")
            require(not path.endswith("/"), "Effect DDS dependency names a directory")
            relative = posixpath.normpath((PurePosixPath(runtime_path).parent / path).as_posix())
            normalized = key(relative)
            require(normalized.endswith(".dds"),
                    f"Effect dependency must be a DDS asset: {name}: {path}")
            refs.add(relative)
            produced.add(index)
            external.add(index)
        for _ in range(2):
            expression()
    for _ in range(reader.count(8)):
        filter_, address = reader.unpack("<2I")
        require(filter_ <= 1 and address <= 3, "Invalid VSFX sampler enums")
    passes = reader.count(40)
    require(passes > 0, "VSFX has no passes")
    for index in range(passes):
        id_, style, *dimensions = reader.unpack("<2I5i")
        require(id_ == index and style <= 1 and all(v > 0 for v in dimensions),
                "Invalid VSFX pass metadata")
        inputs = {reader.word() for _ in range(reader.count(4))}
        outputs = [reader.word() for _ in range(reader.count(4))]
        require(inputs <= produced and outputs and len(set(outputs)) == len(outputs)
                and all(v < textures and v != input_ and v not in external and v not in inputs
                        for v in outputs), "Invalid VSFX pass resource graph")
        produced.update(outputs)
        words = Reader(reader.take(reader.count(4) * 4))
        require(words.word() == 0x07230203, f"Invalid effect SPIR-V: {name}")
        words.take(16)
        while words.at < len(words.data):
            instruction = words.word()
            count, opcode = instruction >> 16, instruction & 65535
            # A three-word OpSource carries language/version enums only.
            require(count > 0 and opcode not in (2, 4, 5, 6, 7, 330)
                    and (opcode != 3 or count == 3),
                    f"Effect contains source/debug instructions or invalid SPIR-V: {name}")
            words.take((count - 1) * 4)
    reader.finish()
    require(output in produced, "VSFX output texture is never produced")
    return refs


def archive_index(archive: zipfile.ZipFile) -> dict:
    result = {}
    for info in archive.infolist():
        normalized = key(info.orig_filename)
        require(normalized not in result, f"Duplicate/casefold archive collision: {info.filename}")
        require(not info.flag_bits & (1 | 64) and info.compress_type == zipfile.ZIP_STORED
                and info.file_size == info.compress_size,
                f"Encrypted or non-stored input: {info.filename}")
        require(not stat.S_ISLNK(info.external_attr >> 16), f"Symlink archive entry: {info.filename}")
        result[normalized] = info
    return result


def removal_plan(archive: zipfile.ZipFile, index: dict,
                 particle_names: set[str], fonts: set[str], *,
                 variant_names: set[str] = frozenset()) -> tuple[dict, set[str]]:
    base_particles = particle_names - variant_names
    require(variant_names <= particle_names and len(base_particles) == COUNTS["particles"],
            "Staged particles lack the original 106 base counterparts")
    # A current production package is an authoritative physical-asset input.
    # Never resurrect images from the original archive while repacking it.
    if (RESOURCE_PATH in index or LEGACY_RESOURCE_PATH in index) and "properties/resources.xml" not in index:
        removed, definitions = {}, {"reanim": set(), "particles": set()}
        migrated_paths = set()
        for name, info in index.items():
            if external_shader(name):
                removed[name] = "external_shader"
                continue
            if info.is_dir():
                continue
            converted = migrated_numeric_path(name)
            kind = numeric_kind(converted) if converted else None
            if converted:
                require(converted not in migrated_paths, f"Numeric source path collision: {name}")
                migrated_paths.add(converted)
            if kind in definitions:
                definitions[kind].add(converted)
            if converted and converted != name:
                removed[name] = ("numeric_definition_rename" if kind in definitions
                                 else "numeric_asset_rename")
            require(not name.startswith("compiled/") or converted is not None or external_shader(name),
                    f"Unclassified compiled asset requires a separate source audit: {name}")
            require(not name.endswith((".xml", ".reanim", ".trail")),
                    f"Author definition in numeric production source: {name}")
        require(len(definitions["reanim"]) == COUNTS["reanim"] and
                base_particles <= definitions["particles"] <= particle_names,
                "Numeric source lacks the exact reanim/base particle counterparts")
        return removed, definitions["reanim"] | particle_names | {font_path(n) for n in fonts} | {RESOURCE_PATH}
    require("properties/resources.xml" in index and not index["properties/resources.xml"].is_dir(),
            "Source must contain the original resources XML as a removal-only entry")
    require(len(fonts) == COUNTS["fonts"],
            "Numeric resource font paths must identify exactly 20 physical font DSL files")
    for path in fonts:
        require(path.startswith("data/") and path.endswith(".txt") and path in index
                and not index[path].is_dir(),
                f"Numeric resource font paths name a missing/unrecognized original font DSL: {path}")
    require(len({font_path(n) for n in fonts}) == len(fonts), "Original font basename collision")
    removed = {}
    for name in index:
        if external_shader(name):
            removed[name] = "external_shader"
            continue
        if index[name].is_dir():
            continue
        if name.startswith("compiled/reanim/") and name.endswith(".reanim.compiled"):
            removed[name] = "old_reanim"
        elif name.startswith("compiled/particles/") and name.endswith((".xml.compiled", ".trail.compiled")):
            removed[name] = "old_particle_or_trail"
        elif name.startswith("reanim/") and name.endswith(".reanim"):
            removed[name] = "author_reanim"
        elif name.startswith("particles/") and name.endswith((".xml", ".trail")):
            removed[name] = "author_particle_or_trail"
        elif name == "properties/resources.xml":
            removed[name] = "resource_xml"
        elif name in fonts:
            removed[name] = "font_dsl"
        elif name.startswith("data/") and name.endswith(".txt"):
            require(not re.search(rb"(?im)(?:^|;)\s*CreateLayer(?:From)?\s+\S+",
                                  archive.read(index[name])),
                    f"Unlisted font DSL requires a separate source audit: {name}")
        require(not name.startswith("compiled/") or name in removed or numeric_kind(name),
                f"Unclassified compiled asset requires a separate source audit: {name}")
        require(not name.endswith(".xml") or name in removed,
                f"Unclassified XML author asset requires a separate source audit: {name}")
    reanims = {definition_path(n[len("compiled/"):-len(".compiled")])
               for n in removed if removed[n] == "old_reanim"}
    particles = {definition_path(n[len("compiled/"):-len(".compiled")]) for n in removed
                 if n.startswith("compiled/particles/") and n.endswith(".xml.compiled")}
    require(len(reanims) == COUNTS["reanim"] and base_particles <= particles,
            "Source lacks the expected 146 reanim/106 base particle counterparts")
    return removed, reanims | particle_names | {font_path(n) for n in fonts} | {RESOURCE_PATH}


def external_shader(name: str) -> bool:
    return name == "shaders" or name.endswith((".dxil", ".spv")) or name.startswith("shaders/")


def retired_effect_binary(name: str) -> bool:
    return name.startswith("effects/") and name.endswith(".vfx")


def matching_external_dds(name: str, data: bytes, root: Path) -> Path | None:
    if not name.startswith("effects/") or not name.endswith(".dds"):
        return None
    base = root / "effects"
    if not base.is_dir():
        return None
    require(not base.is_symlink() and not getattr(base, "is_junction", lambda: False)(),
            f"Linked external effects directory: {base}")
    matches = [p for p in base.rglob("*") if p.is_file() and
               key(p.relative_to(root).as_posix()) == name]
    require(len(matches) <= 1, f"External DDS casefold collision: {name}")
    if matches:
        path = matches[0]
        require(not path.is_symlink() and not getattr(path, "is_junction", lambda: False)(),
                f"Linked external DDS: {path}")
        if path.read_bytes() == data:
            return path
    return None


def effect_retirement(archive: zipfile.ZipFile, index: dict, root: Path) -> tuple[dict, list]:
    removed, mirrors = {}, []
    for name, info in index.items():
        if info.is_dir():
            continue
        if retired_effect_binary(name):
            removed[name] = "retired_scale_binary"
        elif name.startswith("effects/") and name.endswith(".dds"):
            data = archive.read(info)
            external = matching_external_dds(name, data, root)
            if external:
                removed[name] = "external_scale_dds"
                mirrors.append({"name": name, "external": str(external.resolve()),
                                "bytes": len(data), "sha256": digest(data)})
    if mirrors:
        script = root / "CMake/StageNumericEffects.cmake"
        staging = script.read_text(encoding="utf-8")
        require(all(token in staging for token in ('"${SOURCE}/*.hlsl"', '"${SOURCE}/*.hlsli"',
                                                    '"${SOURCE}/*.dds"', "COPYONLY")),
                "External effects source/DDS staging contract changed")
        framework = (root / "pvzPerf/SexyAppFramework/CMakeLists.txt").read_text(encoding="utf-8")
        require(all(token in framework for token in ("StageNumericEffects.cmake",
                    "-DSOURCE=${CMAKE_SOURCE_DIR}/effects", "/effects", "POST_BUILD")),
                "Game no longer stages external effects")
    return removed, mirrors


def migrated_numeric_path(name: str) -> str | None:
    """Offline import only. Production never probes historical names."""
    normalized = file_key(name)
    if normalized == LEGACY_RESOURCE_PATH:
        return RESOURCE_PATH
    if normalized == PROJECTILE_ATLAS_PATH:
        return normalized
    if normalized.startswith("compiled/fonts/") and normalized.endswith(".font.compiled"):
        return normalized[:-len(".font.compiled")] + ".compiled"
    for directory, suffix in (("reanim", ".reanim.v4.compiled"),
                              ("particles", ".xml.v4.compiled")):
        if normalized.startswith(f"compiled/{directory}/") and normalized.endswith(suffix):
            return normalized[:-len(suffix)] + ".compiled"
    return normalized if numeric_kind(normalized) else None


def physical_references(kind: str, path: str, available: set[str]) -> set[str]:
    normalized = file_key(path)
    if kind == "atlas":
        found = {normalized} & available
    elif kind == "sound":
        # DSoundManager appends an extension even when the supplied stem has one.
        found = {normalized + suffix for suffix in (".wav", ".ogg", ".au")} & available
    else:
        suffixes = (".tga", ".jpg", ".png", ".gif")

        def candidates(stem: str) -> set[str]:
            extension = PurePosixPath(stem).suffix
            return ({stem} if extension in suffixes else
                    {stem + suffix for suffix in suffixes} if not extension else set()) & available

        parent, filename = posixpath.split(normalized)
        found = candidates(normalized)
        # ImageLib supports alpha-only images as well as optional alpha companions.
        found |= candidates(posixpath.join(parent, "_" + filename))
        found |= candidates(normalized + "_")
    require(bool(found), f"Missing referenced {kind} asset: {path}")
    return found


def stage_inventory(stage: Path) -> dict[str, tuple[str, Path]]:
    result = {}
    for path in stage.rglob("*"):
        require(path.suffix.casefold() != ".xml",
                f"XML author file in numeric stage: {path.relative_to(stage)}")
        require(not external_shader(path.relative_to(stage).as_posix().casefold()),
                f"Shaders must remain outside pak64 staging: {path.relative_to(stage)}")
        require(not path.is_file() or not path.name.casefold().endswith(".vfx"),
                f"VFX shader artifacts must remain outside production staging: {path.relative_to(stage)}")
    for directory in ("compiled", "effects"):
        base = stage / directory
        if not os.path.lexists(base):
            require(directory != "compiled", "Missing staged compiled directory")
            continue
        require(not base.is_symlink() and not getattr(base, "is_junction", lambda: False)(),
                f"Symlink/junction in staged assets: {base}")
        require(base.is_dir(), f"Staged asset tree is not a directory: {base}")
        for path in base.rglob("*"):
            require(not path.is_symlink() and not getattr(path, "is_junction", lambda: False)(),
                    f"Symlink/junction in staged assets: {path}")
            if not path.is_file():
                continue
            name = path.relative_to(stage).as_posix()
            normalized = key(name)
            require(not external_shader(normalized), f"Shaders must remain outside pak64: {name}")
            require(normalized not in result, f"Staged casefold collision: {name}")
            result[normalized] = name, path
    return result


def central_records(archive: zipfile.ZipFile, stream) -> list[bytes]:
    stream.seek(archive.start_dir)
    records = []
    for _ in archive.infolist():
        fixed = stream.read(46)
        require(len(fixed) == 46 and fixed[:4] == b"PK\x01\x02", "Invalid ZIP central directory")
        lengths = struct.unpack_from("<3H", fixed, 28)
        require(struct.unpack_from("<H", fixed, 34)[0] == 0, "Multi-disk PAK64 is unsupported")
        variable = stream.read(sum(lengths))
        require(len(variable) == sum(lengths), "Truncated ZIP central directory")
        records.append(fixed + variable)
    return records


def relocate_central(record: bytes, offset: int) -> bytes:
    record = bytearray(record)
    old = struct.unpack_from("<I", record, 42)[0]
    if old != 0xFFFFFFFF and offset < 0xFFFFFFFF:
        struct.pack_into("<I", record, 42, offset)
        return bytes(record)
    name_size, extra_size = struct.unpack_from("<2H", record, 28)
    start, end = 46 + name_size, 46 + name_size + extra_size
    at = start
    while at < end:
        require(at + 4 <= end, "Malformed ZIP extra field")
        tag, size = struct.unpack_from("<2H", record, at)
        require(at + 4 + size <= end, "Malformed ZIP extra field size")
        if tag == 1:
            field = at + 4
            field += 8 * (struct.unpack_from("<I", record, 24)[0] == 0xFFFFFFFF)
            field += 8 * (struct.unpack_from("<I", record, 20)[0] == 0xFFFFFFFF)
            if old == 0xFFFFFFFF:
                require(field + 8 <= at + 4 + size, "Missing ZIP64 local offset")
                struct.pack_into("<Q", record, field, offset)
            else:
                require(field <= at + 4 + size and extra_size + 8 <= 65535,
                        "Invalid ZIP64 size field or overflowing extras")
                record[field:field] = struct.pack("<Q", offset)
                struct.pack_into("<H", record, at + 2, size + 8)
                struct.pack_into("<H", record, 30, extra_size + 8)
                struct.pack_into("<I", record, 42, 0xFFFFFFFF)
                record[4] = max(record[4], 45)
                record[6] = max(record[6], 45)
            return bytes(record)
        at += 4 + size
    require(old != 0xFFFFFFFF, "Missing ZIP64 extra field")
    require(extra_size + 12 <= 65535, "Overflowing ZIP64 extras")
    record[end:end] = struct.pack("<HHQ", 1, 8, offset)
    struct.pack_into("<H", record, 30, extra_size + 12)
    struct.pack_into("<I", record, 42, 0xFFFFFFFF)
    record[4] = max(record[4], 45)
    record[6] = max(record[6], 45)
    return bytes(record)


def local_size(stream, info: zipfile.ZipInfo) -> int:
    stream.seek(info.header_offset)
    header = stream.read(30)
    require(len(header) == 30 and header[:4] == b"PK\x03\x04", "Invalid local ZIP header")
    flags, method = struct.unpack_from("<2H", header, 6)
    require(flags == info.flag_bits and method == 0, "Local/central ZIP flags or compression differ")
    name_size, extra_size = struct.unpack_from("<2H", header, 26)
    name, extra = stream.read(name_size), stream.read(extra_size)
    require(len(name) == name_size and len(extra) == extra_size, "Truncated local ZIP name/extra")
    require(name.decode("utf-8" if flags & 2048 else "cp437") == info.orig_filename,
            "Local/central ZIP names differ")
    crc, compressed, uncompressed = struct.unpack_from("<3I", header, 14)
    wide = 0xFFFFFFFF in (compressed, uncompressed)
    if wide:
        at, values = 0, None
        while at < len(extra):
            require(at + 4 <= len(extra), "Malformed local ZIP extra field")
            tag, length = struct.unpack_from("<2H", extra, at)
            at += 4
            require(at + length <= len(extra), "Malformed local ZIP extra field size")
            if tag == 1:
                require(values is None, "Duplicate local ZIP64 extra field")
                values = Reader(extra[at:at + length])
            at += length
        require(values is not None, "Missing local ZIP64 size field")
        if uncompressed == 0xFFFFFFFF:
            uncompressed = values.unpack("<Q")[0]
        if compressed == 0xFFFFFFFF:
            compressed = values.unpack("<Q")[0]
    require((crc, compressed, uncompressed) == (info.CRC, info.compress_size, info.file_size)
            or flags & 8 and crc in (0, info.CRC) and compressed in (0, info.compress_size)
            and uncompressed in (0, info.file_size), "Local/central ZIP CRC or sizes differ")
    size = 30 + name_size + extra_size + info.compress_size
    if flags & 8:
        stream.seek(info.header_offset + size)
        signature = stream.read(4)
        has_signature = signature == b"PK\x07\x08"
        if not has_signature:
            stream.seek(-4, os.SEEK_CUR)
        fmt = "<IQQ" if wide else "<III"
        descriptor = stream.read(struct.calcsize(fmt))
        require(len(descriptor) == struct.calcsize(fmt)
                and struct.unpack(fmt, descriptor) == (info.CRC, info.compress_size, info.file_size),
                "Invalid stored ZIP data descriptor")
        size += len(descriptor) + 4 * has_signature
    return size


def write_end(stream, records: list[bytes], comment: bytes) -> None:
    offset = stream.tell()
    for record in records:
        stream.write(record)
    size = stream.tell() - offset
    count = len(records)
    if count >= 65535 or size >= 0xFFFFFFFF or offset >= 0xFFFFFFFF:
        end = stream.tell()
        stream.write(struct.pack("<4sQ2H2I4Q", b"PK\x06\x06", 44, 45, 45, 0, 0,
                                 count, count, size, offset))
        stream.write(struct.pack("<4sIQI", b"PK\x06\x07", 0, end, 1))
    stream.write(struct.pack("<4s4H2IH", b"PK\x05\x06", 0, 0, min(count, 65535),
                             min(count, 65535), min(size, 0xFFFFFFFF),
                             min(offset, 0xFFFFFFFF), len(comment)) + comment)


def entry_hash(archive: zipfile.ZipFile, info: zipfile.ZipInfo) -> str:
    with archive.open(info) as stream:
        h = hashlib.sha256()
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            h.update(block)
        return h.hexdigest()


def metadata(info: zipfile.ZipInfo) -> tuple:
    return (info.filename, info.date_time, info.compress_type, info.flag_bits,
            info.create_system, info.create_version, info.extract_version,
            info.reserved, info.volume, info.internal_attr, info.external_attr,
            info.extra, info.comment, info.CRC, info.file_size, info.compress_size)


def safe_destinations(source: Path, stage: Path, output: Path, manifest: Path, root: Path) -> None:
    require(source.is_file() and stage.is_dir() and root.is_dir(), "Missing source, stage, or source root")
    require(output.resolve() != manifest.resolve(), "Archive and manifest alias")
    for path in (output, manifest):
        require(not os.path.lexists(path), f"Refusing to overwrite existing output: {path}")
        require(path.resolve() != source.resolve(), "Input/output alias")
        require(path.name.casefold() != "main.pak64", "Output must not be a runtime main.pak64")
        resolved = path.resolve()
        require(not resolved.is_relative_to(stage.resolve()), "Output must be outside the staged input tree")
        for directory in ("compiled", "reanim", "particles", "properties", "data", "effects",
                          "images", "sounds", "music", "fonts"):
            require(not resolved.is_relative_to((root / directory).resolve()),
                    f"Output must not enter author/numeric assets: {path}")
        require(path.parent.is_dir(), f"Output parent must already exist: {path.parent}")


def build(source: Path, stage: Path, output: Path, root: Path = ROOT,
          manifest_path: Path | None = None, *, replace_definitions: bool = False) -> dict:
    source, stage, output, root = map(Path, (source, stage, output, root))
    manifest_path = Path(manifest_path) if manifest_path else output.with_suffix(".manifest.json")
    safe_destinations(source, stage, output, manifest_path, root)
    source_fingerprint = fingerprint(source)
    staged = stage_inventory(stage)
    with zipfile.ZipFile(source) as original:
        index = archive_index(original)
        with source.open("rb") as stream:
            ordered = sorted(original.infolist(), key=lambda i: i.header_offset)
            for index_, info in enumerate(ordered):
                boundary = ordered[index_ + 1].header_offset if index_ + 1 < len(ordered) else original.start_dir
                require(info.header_offset + local_size(stream, info) <= boundary,
                        "Local ZIP record overlaps another record or the central directory")
        numeric = {n for n in staged if numeric_kind(n)}
        require(RESOURCE_PATH in numeric,
                "Staged numeric paths differ from the complete source-declared corpus")
        # Registry validation precedes font-source extraction. Repeat the source
        # audit with the removal set so no removed-file consumer escapes it.
        initial_audit = source_audit(root, {}, numeric)
        resource_data = staged[RESOURCE_PATH][1].read_bytes()
        resource_references, font_sources = set(), set()
        font_refs = validate_numeric(RESOURCE_PATH, resource_data, initial_audit["registries"],
                                     resource_references, font_sources=font_sources)
        removed, required = removal_plan(original, index,
                                         {n for n in numeric if numeric_kind(n) == "particles"},
                                         font_sources,
                                         variant_names={entry["path"] for entry in published_particle_variants(root)})
        if "projectiles" in expected_counts(root):
            required.add(PROJECTILE_PATH)
        retired, external_dds = effect_retirement(original, index, root)
        removed.update(retired)
        require(numeric == required, "Staged numeric paths differ from the complete source-declared corpus")
        atlas_names = set(initial_audit.get("static_atlas_extents", {}))
        for name in numeric:
            if numeric_kind(name) == "projectiles":
                atlas = parse_projectiles(name, staged[name][1].read_bytes())
                image = initial_audit["definition_images"].get(name, PROJECTILE_ATLAS_PATH)
                require(bool(image) and image in staged, f"Missing staged projectile atlas: {image}")
                validate_projectile_atlas_image(atlas, staged[image][1].read_bytes(), image)
                atlas_names.add(image)
                continue
            if numeric_kind(name) not in ("reanim", "particles"):
                continue
            atlas = parse_definition_atlas(name, staged[name][1].read_bytes())
            if atlas and atlas["count"]:
                for variant in range(atlas.get("variant_count", 1)):
                    image = initial_audit["definition_variant_images"].get(name, [""])[variant]
                    count = atlas.get("variant_counts", [atlas["count"]])[variant]
                    require(bool(image) == bool(count), f"Missing program definition image binding: {name}")
                    if image:
                        atlas_names.add(image)
        require(atlas_names <= set(staged), "Missing staged definition atlas")
        require(not any(n.startswith("compiled/") and n not in numeric | atlas_names for n in staged),
                "Unexpected non-numeric file in staged compiled tree")
        audit = source_audit(root, removed, numeric)
        require(initial_audit["fingerprint"] == audit["fingerprint"],
                "Runtime sources changed during packaging")
        selected = set(numeric) | atlas_names | {n for n in staged if n.startswith("effects/") and
                                  n.endswith(".dds") and n not in retired}
        require(not any(n.startswith("effects/") and n.endswith((".hlsl", ".fx"))
                        for n in index), "Authored effect source in input; requires a separate source audit")
        existing_numeric = {n for n in index if numeric_kind(n)}
        require(existing_numeric <= required, "Unexpected numeric record in source")
        file_refs = {RESOURCE_PATH: resource_references}
        validated_stamps = {RESOURCE_PATH: {"sha256": digest(resource_data), "bytes": len(resource_data)}}
        for name in sorted(selected):
            if name == RESOURCE_PATH:
                continue
            data = staged[name][1].read_bytes() if name in selected else original.read(index[name])
            if name in selected:
                validated_stamps[name] = {"sha256": digest(data), "bytes": len(data)}
            if numeric_kind(name):
                dependencies = set()
                font_refs.update(validate_numeric(
                    name, data, audit["registries"], dependencies,
                    atlas_paths=audit["definition_variant_images"].get(name),
                    atlas_path=audit["definition_images"].get(name)))
                file_refs[name] = dependencies
        validate_particle_inheritance({name: staged[name][1].read_bytes() for name in numeric})
        require(font_refs == {n for n in required if numeric_kind(n) == "fonts"},
                "Numeric resource font paths do not cover the original 20 fonts")
        available = {n for n, info in index.items() if n not in removed and not info.is_dir()} | selected
        resolved_refs = {}
        for name, dependencies in sorted(file_refs.items()):
            resolved_refs[name] = sorted({p for kind, path in dependencies
                                          for p in physical_references(kind, path, available)})
        inputs, additions = [], []
        for name in sorted(selected):
            spelling, path = staged[name]
            stamp = fingerprint(path)
            if name in validated_stamps:
                require(stamp == validated_stamps[name], f"Staged input changed after validation: {spelling}")
            inputs.append({"name": spelling, **stamp})
            if name in index and name not in removed:
                require(spelling == index[name].filename,
                        f"Staged/archive collision differs in name or bytes: {spelling}")
                if stamp["sha256"] != entry_hash(original, index[name]):
                    require(replace_definitions and
                            (numeric_kind(name) in ("reanim", "particles", "projectiles") or name in atlas_names),
                            f"Staged/archive collision differs in name or bytes: {spelling}")
                    removed[name] = "numeric_record_rebuild"
                    additions.append(name)
            else:
                additions.append(name)
        retained = [i for i in original.infolist() if key(i.filename) not in removed]
        input_stamps = {i["name"]: i for i in inputs}
        retained_rows = [{"name": i.filename, "bytes": i.file_size,
                          "source_sha256": entry_hash(original, i)} for i in retained]
        removed_rows = [{"name": index[n].filename, "bytes": index[n].file_size,
                         "sha256": entry_hash(original, index[n]), "reason": removed[n]}
                        for n in index if n in removed]
        temporary = []
        published = []
        try:
            fd, temp_name = tempfile.mkstemp(prefix=".numeric-pak64-", dir=output.parent)
            temporary.append(Path(temp_name))
            with os.fdopen(fd, "wb") as target, source.open("rb") as stream:
                raw_records = central_records(original, stream)
                records = []
                # Retained local records are copied verbatim, including descriptors.
                for info, record in zip(original.infolist(), raw_records):
                    if key(info.filename) in removed:
                        continue
                    size = local_size(stream, info)
                    records.append(relocate_central(record, target.tell()))
                    stream.seek(info.header_offset)
                    remaining = size
                    while remaining:
                        block = stream.read(min(remaining, 1024 * 1024))
                        require(bool(block), "Truncated local ZIP record")
                        target.write(block)
                        remaining -= len(block)
                for name in additions:
                    spelling, path = staged[name]
                    data = path.read_bytes()
                    stamp = input_stamps[spelling]
                    require(digest(data) == stamp["sha256"], f"Staged input changed: {spelling}")
                    buffer = io.BytesIO()
                    info = zipfile.ZipInfo(spelling, (1980, 1, 1, 0, 0, 0))
                    info.create_system, info.external_attr = 0, 0x20
                    with zipfile.ZipFile(buffer, "w", compression=zipfile.ZIP_STORED) as single:
                        single.writestr(info, data)
                    buffer.seek(0)
                    with zipfile.ZipFile(buffer) as single:
                        central = central_records(single, buffer)[0]
                        records.append(relocate_central(central, target.tell()))
                        buffer.seek(0)
                        target.write(buffer.read(single.start_dir))
                write_end(target, records, original.comment)
                target.flush()
                os.fsync(target.fileno())
            with zipfile.ZipFile(temporary[0]) as rebuilt:
                output_index = archive_index(rebuilt)
                with temporary[0].open("rb") as stream:
                    copied_records = central_records(rebuilt, stream)
                require(set(output_index) == (set(index) - set(removed)) | set(additions),
                        "Output entry closure differs")
                require(not any(external_shader(n) or ".xml" in n or ".v4." in n
                                or n.endswith(".vfx")
                                or n.endswith(".font.compiled") or n == LEGACY_RESOURCE_PATH
                                for n in output_index), "Forbidden production package path")
                for info in rebuilt.infolist():
                    if not info.is_dir():
                        with rebuilt.open(info) as entry:
                            require(entry.read(4) not in SHADER_HEADERS,
                                    f"Shader payload under a non-shader filename: {info.filename}")
                require([i.filename for i in rebuilt.infolist()[:len(retained)]] ==
                        [i.filename for i in retained] and rebuilt.comment == original.comment,
                        "Retained order or archive comment changed")
                for index_, (info, row) in enumerate(zip(retained, retained_rows)):
                    copied = output_index[key(info.filename)]
                    row["output_sha256"] = entry_hash(rebuilt, copied)
                    # Compare the exact relocated central record: ZIP64 offsets
                    # and required version promotions are not metadata corruption.
                    row["metadata_preserved"] = records[index_] == copied_records[index_]
                    require(row["source_sha256"] == row["output_sha256"] and row["metadata_preserved"],
                            f"Retained bytes/metadata changed: {info.filename}")
                added_rows = []
                for name in additions:
                    info = output_index[name]
                    stamp = input_stamps[info.filename]
                    require(entry_hash(rebuilt, info) == stamp["sha256"], f"Added asset changed: {info.filename}")
                    added_rows.append({"name": info.filename, "bytes": info.file_size, "sha256": stamp["sha256"]})
            require(fingerprint(source) == source_fingerprint, "Source archive changed during packaging")
            require(stage_inventory(stage) == staged, "Staged file inventory changed during packaging")
            for stamp in inputs:
                require(fingerprint(staged[key(stamp["name"])][1]) ==
                        {k: stamp[k] for k in ("sha256", "bytes")}, f"Staged input changed: {stamp['name']}")
            reference_removals = {n: reason for n, reason in removed.items()
                                  if reason != "numeric_record_rebuild"}
            require(source_audit(root, reference_removals, numeric) == audit,
                    "Runtime sources changed during packaging")
            for mirror in external_dds:
                require(fingerprint(Path(mirror["external"])) ==
                        {k: mirror[k] for k in ("bytes", "sha256")},
                        f"External DDS changed during packaging: {mirror['name']}")
            manifest = {
                "format": "numeric-runtime-pak64-v2",
                "source": {"path": str(source.resolve()), **source_fingerprint},
                "output": {"path": str(output.resolve()), **fingerprint(temporary[0])},
                "staged_inputs": inputs, "staged_fingerprint": digest(json.dumps(inputs, sort_keys=True).encode()),
                "source_reference_audit": audit, "numeric_counts": expected_counts(root),
                "package_layout": {
                    "definitions": "compiled/{reanim,particles}/<stem>.compiled",
                    "definition_atlases": "program DefinitionImageBindings; inline compiled geometry only",
                    "fonts": "compiled/fonts/<stem>.compiled",
                    "resources": RESOURCE_PATH,
                    "built_in_shaders": "external build output; never packaged",
                    "custom_scale_hlsl": "startup compilation from external author sources",
                },
                "font_author_sources_from_numeric_resources": sorted(font_sources),
                "retained": retained_rows, "removed": removed_rows, "added": added_rows,
                "external_scale_dds": external_dds,
                "effect_file_references": {},
                "numeric_file_references": {n: p for n, p in resolved_refs.items() if p},
                "verification_scope": [
                    "Stored unencrypted archive, safe unique paths, retained order/metadata and per-entry SHA256 equality",
                    "Numeric envelope/header/CRC/registry and IDT5/IDP5/font/resource record framing, IDs and finite values",
                    "Resource font, image/alpha, sound and font-glyph file path closure; retained UI bytes are unchanged",
                    "Font DSL removals derive from validated staged numeric.resources; legacy XML is removal-only",
                    "No production VFX reader; retired VFX removed including embedded SPIR-V",
                    "Effects DDS removed only with byte-identical external copies and CMake deployment contract",
                    "NOT decoder/GPU/SPIR-V semantic validation or proof that every non-UI/non-path string is absent",
                    "Source audit assumes definition/font test importers are disabled; custom HLSL compiles at startup",
                    "Fsynced files; exclusive manifest-first/archive-last publication, NOT a two-file crash-atomic transaction",
                ],
            }
            fd, temp_name = tempfile.mkstemp(prefix=".numeric-manifest-", dir=manifest_path.parent)
            temporary.append(Path(temp_name))
            with os.fdopen(fd, "wb") as stream:
                stream.write((json.dumps(manifest, indent=2, sort_keys=True) + "\n").encode("utf-8"))
                stream.flush()
                os.fsync(stream.fileno())
            # The archive is the completion marker. Record ownership before link
            # so an interruption immediately after creation can still roll back.
            for temporary_path, destination in ((temporary[1], manifest_path), (temporary[0], output)):
                published.append((temporary_path.stat(), destination))
                os.link(temporary_path, destination)
            for path in temporary:
                path.unlink()
            temporary.clear()
            return manifest
        except BaseException:
            for identity, destination in reversed(published):
                if os.path.lexists(destination) and os.path.samestat(identity, destination.lstat()):
                    destination.unlink()
            raise
        finally:
            for path in temporary:
                path.unlink(missing_ok=True)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("stage", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--root", type=Path, default=ROOT)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--replace-definitions", action="store_true",
                        help="replace validated animation/particle records in a numeric source package")
    args = parser.parse_args()
    try:
        manifest = build(args.source, args.stage, args.output, args.root, args.manifest,
                         replace_definitions=args.replace_definitions)
    except (ValueError, OSError, zipfile.BadZipFile, struct.error) as error:
        parser.exit(1, f"numeric runtime packaging failed: {error}\n")
    print(f"Created {args.output}: retained={len(manifest['retained'])} "
          f"removed={len(manifest['removed'])} added={len(manifest['added'])}")
    print(f"Manifest: {args.manifest or args.output.with_suffix('.manifest.json')}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
