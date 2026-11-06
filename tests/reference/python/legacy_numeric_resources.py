#!/usr/bin/env python3
"""Historical NRSC2 fixture compiler. Never used for production resources.

Logical names exist only in authoring and generated C++ targets. The wire format
has no ResourceId, mixed resource slot, or persistent image-to-slot registry.
"""
from __future__ import annotations
from game_fixture import game_root
import argparse
import json
import re
import struct
import zlib
import zipfile
from pathlib import Path, PurePosixPath

ROOT = game_root()
MAGIC = 0x4353524E
VERSION = 2
NONE = 0xFFFFFFFF
PROGRAM, SYSTEM_FONT, REFERENCE_FONT, KEEP_BITS = 1, 2, 4, 8
PALLETIZE, A4, A8, DDS, PURGE, MIN_SUBDIVIDE, AUTO_ALPHA = 16, 32, 64, 128, 256, 512, 1024
PURGE_3D, PURGE_2D, UNAVAILABLE = 2048, 4096, 8192


def write_changed(path: Path, text: str) -> None:
    data = text.encode("utf-8")
    if not path.exists() or path.read_bytes() != data:
        path.write_bytes(data)


def read_authoring(data: bytes) -> dict:
    def unique_members(pairs: list[tuple[str, object]]) -> dict:
        result = {}
        for name, value in pairs:
            if name in result:
                raise ValueError(f'Duplicate author manifest member: {name}')
            result[name] = value
        return result

    author = json.loads(data, object_pairs_hook=unique_members)
    if (not isinstance(author, dict) or set(author) != {'format', 'manifest'}
            or author['format'] != 'pvz-resources-authoring-v1'):
        raise ValueError('Expected pvz-resources-authoring-v1 JSON manifest')

    def validate_node(node: object, tags: tuple[str, ...]) -> None:
        if (not isinstance(node, dict) or set(node) != {'tag', 'attrs', 'children'}
                or node['tag'] not in tags or not isinstance(node['attrs'], dict)
                or not isinstance(node['children'], list)):
            raise ValueError('Invalid author manifest node')
        if any(not isinstance(value, str) or '\0' in name or '\0' in value
               for name, value in node['attrs'].items()):
            raise ValueError('Author attributes must be strings without NUL')

    manifest = author['manifest']
    validate_node(manifest, ('ResourceManifest',))
    for group in manifest['children']:
        validate_node(group, ('Resources',))
        if not group['attrs'].get('id'):
            raise ValueError('Author resource group has no id')
        for node in group['children']:
            validate_node(node, ('SetDefaults', 'Image', 'Sound', 'Font'))
            if node['children'] or node['tag'] != 'SetDefaults' and 'path' not in node['attrs']:
                raise ValueError('Author resource must be a leaf with a path')
    return manifest


def program_symbols_path(root: Path) -> Path:
    program = root / 'pvzPerf/Sexy.TodLib/ProgramImageSymbols.h'
    return program if program.exists() else root / 'pvzPerf/Sexy.TodLib/generated/ImageSymbolId.h'


def read_symbols(path: Path) -> tuple[list[tuple[int, str]], int]:
    """Read program source IDs, or a historical binary registry for fixtures."""
    data = path.read_bytes()
    if path.suffix.casefold() == '.h':
        text = data.decode('utf-8-sig')
        block = re.search(r'enum\s+class\s+ImageSymbolId\s*:\s*std::uint32_t\s*\{(.*?)\};',
                          text, re.S)
        if not block:
            raise ValueError('Program ImageSymbolId enum not found')
        result = []
        for identifier, number, original in re.findall(
                r'^\s*(\w+)\s*=\s*(\d+)\s*,[ \t]*(?://[ \t]*registry-name:[ \t]*"([^"]+)")?',
                block[1], re.M):
            symbol = int(number)
            if symbol == 0 and identifier == 'None':
                continue
            if symbol != len(result) + 1:
                raise ValueError('Image symbols must preserve dense existing IDs')
            result.append((symbol, original or identifier))
        if not result:
            raise ValueError('Program image symbol enum is empty')
        canonical = bytearray(struct.pack('<II', 0x4953594D, len(result)))
        for symbol, name in result:
            raw_name = name.encode('utf-8')
            canonical.extend(struct.pack('<II', symbol, len(raw_name)))
            canonical.extend(raw_name)
        crc = zlib.crc32(canonical)
        declared = re.search(r'\bIMAGE_SYMBOL_ID_REGISTRY_CRC\s*=\s*(0x[0-9A-Fa-f]+|\d+)[uU]?\s*;', text)
        if declared and int(declared[1], 0) != crc:
            raise ValueError('Program image symbol CRC differs from the declared stable registry')
        return result, crc
    magic, count = struct.unpack_from('<II', data)
    if magic != 0x4953594D:
        raise ValueError('Invalid image symbol sidecar')
    result, offset = [], 8
    for expected in range(1, count + 1):
        symbol, length = struct.unpack_from('<II', data, offset)
        offset += 8
        name = data[offset:offset + length].decode('utf-8')
        offset += length
        if symbol != expected:
            raise ValueError('Image symbols must preserve dense existing IDs')
        result.append((symbol, name))
    if offset != len(data):
        raise ValueError('Trailing image symbol bytes')
    return result, zlib.crc32(data)


def group_ids(root: Path) -> dict[str, int]:
    text = (root / 'pvzPerf/ResourceGroupId.h').read_text(encoding='utf-8')
    block = text[text.index('enum class ResourceGroupId'):text.index('};')]
    result = {name: int(value) for name, value in re.findall(r'(\w+)\s*=\s*(\d+)', block)}
    if result.get('Invalid') != 0 or len(result) != 30:
        raise ValueError('Expected Invalid0 and 29 canonical resource groups')
    return result


def path_string(value: str) -> str:
    return value.replace(chr(92), '/')


def str_record(value: str) -> bytes:
    raw = value.encode('utf-8')
    return struct.pack('<I', len(raw)) + raw


def int_vector(values: list[int]) -> bytes:
    return struct.pack('<I', len(values)) + b''.join(struct.pack('<i', value) for value in values)


def vec(value: str) -> list[int]:
    return [int(v.strip()) for v in value.split(',')] if value else []


def compile_manifest(root: Path, authoring: bytes, archive_paths: list[str], sidecar: Path,
                     font_tag_ids: dict[str, int] | None = None) -> tuple[bytes, list[dict], int, list[str]]:
    manifest = read_authoring(authoring)
    symbols, registry_crc = read_symbols(sidecar)
    # Sidecar can contain case aliases; the author spelling receives its exact symbol.
    exact_symbols = {name: symbol for symbol, name in symbols}
    groups = group_ids(root)
    plan, names, seen_names, seen_symbols, seen_groups = [], [], set(), set(), set()
    default_path, default_prefix = '', ''
    variants: dict[str, int] = {}
    font_tags = font_tag_ids or {}
    def emit(symbol: int, kind: int, flags: int, ref: int,
             variant: int, path: str, alpha: str = '', alpha_grid: str = '', font_image: str = '',
             attrs: dict[str, str] | None = None) -> None:
        a = attrs or {}
        common = struct.pack('<5I', kind, symbol, flags, variant, ref)
        common += b''.join(str_record(p) for p in (path, alpha, alpha_grid, font_image))
        if kind == 0:
            rows, cols = int(a.get('rows', '1')), int(a.get('cols', '1'))
            if rows <= 0 or cols <= 0 or rows > 2147483647 or cols > 2147483647:
                raise ValueError('Invalid image grid')
            anim = {'none': 0, 'once': 1, 'pingpong': 2, 'loop': 3}[a.get('anim', 'none').lower()]
            common += struct.pack('<iiIIiii', rows, cols, int(a.get('alphacolor', 'FFFFFF'), 16),
                                  anim, int(a.get('framedelay', '0')), int(a.get('begindelay', '0')),
                                  int(a.get('enddelay', '0')))
            common += int_vector(vec(a.get('perframedelay', ''))) + int_vector(vec(a.get('framemap', '')))
        elif kind == 1:
            common += struct.pack('<di', float(a.get('volume', '-1')), int(a.get('pan', '0')))
        else:
            style = sum(bit for name, bit in [('bold', 1), ('italic', 2), ('underline', 4), ('shadow', 8)] if name in a)
            common += struct.pack('<iI', int(a.get('size', '0')), style)
            tags = re.findall(r'[^,\s]+', a.get('tags', ''))
            unknown = [tag for tag in tags if tag not in font_tags]
            if unknown:
                raise ValueError(f'Font tags need existing numeric registry: {unknown}')
            common += int_vector([font_tags[tag] for tag in tags])
        return {"name": name, "kind": kind, "symbol": symbol, "flags": flags,
                "variant": variant, "path": path, "data": common}

    for group in manifest['children']:
        group_name = group['attrs']['id']
        if group_name not in groups:
            if any(e['tag'] != 'SetDefaults' for e in group['children']):
                raise ValueError(f'Nonempty author group has no approved enum: {group_name}')
            continue
        if group_name in seen_groups:
            raise ValueError(f'Duplicate resource group: {group_name}')
        seen_groups.add(group_name)
        group_plan = {"group": groups[group_name], "name": group_name, "records": []}
        plan.append(group_plan)
        local_fonts = {}
        for elem in group['children']:
            a = elem['attrs']
            if elem['tag'] == 'SetDefaults':
                if 'path' in a:
                    default_path = path_string(a['path']).rstrip('/') + '/'
                default_prefix = a.get('idprefix', default_prefix).rstrip('/')
                continue
            original_path = path_string(a['path'])
            name = default_prefix + a.get('id', PurePosixPath(original_path).stem)
            if not re.fullmatch(r'[A-Za-z_]\w*', name) or name in seen_names:
                raise ValueError(f'Invalid or duplicate resource target: {name}')
            seen_names.add(name)
            kind = {'Image': 0, 'Sound': 1, 'Font': 2}[elem['tag']]
            symbol = exact_symbols.get(name, 0) if kind == 0 else 0
            if kind == 0 and not symbol:
                continue
            names.append(name)
            if symbol:
                if symbol in seen_symbols:
                    raise ValueError(f'Duplicate image symbol: {symbol}')
                seen_symbols.add(symbol)
            path = original_path if original_path.startswith('!') else default_path + original_path
            flags, ref = 0, NONE
            if path == '!program':
                flags |= PROGRAM
                path = ''
            elif path.startswith('!ref:'):
                if kind != 2 or path[5:] not in local_fonts:
                    raise ValueError(f'Font reference must name a prior font in the same group: {name}')
                flags |= REFERENCE_FONT
                ref = local_fonts[path[5:]]
                path = ''
            elif path.startswith('!sys:'):
                raise ValueError('System fonts require an offline compiled bitmap font')
            elif path.startswith('!'):
                raise ValueError(f'Unknown resource control token: {path}')
            if path.lower().startswith('reanim/'):
                flags |= KEEP_BITS
            if kind == 0:
                flags |= 0 if 'nopal' in a else PALLETIZE
                # 1073 editbox_.gif is 96x33, not the 90x42 editbox.gif's alpha.
                # Declare the asset policy; the runtime rejects mismatched sidecars.
                flags |= 0 if 'noalpha' in a or name == 'IMAGE_EDITBOX' else AUTO_ALPHA
                for key, bit in [('a4r4g4b4', A4), ('a8r8g8b8', A8), ('ddsurface', DDS),
                                 ('nobits', PURGE), ('minsubdivide', MIN_SUBDIVIDE), ('nobits3d', PURGE_3D), ('nobits2d', PURGE_2D)]:
                    if key in a:
                        flags |= bit
            variant_name = a.get('variant', '')
            if variant_name:
                variant = variants.setdefault(variant_name, len(variants) + 1)
            else:
                variant = 0
            record = emit(symbol, kind, flags, ref, variant, path,
                          default_path + a['alphaimage'] if 'alphaimage' in a else '',
                          default_path + a['alphagrid'] if 'alphagrid' in a else '',
                          a.get('image', ''), a)
            if kind == 2:
                local_fonts[name] = len(group_plan["records"])
            group_plan["records"].append(record)
    missing = {symbol for symbol, _ in symbols} - seen_symbols
    if missing:
        raise ValueError(f'Live image symbols missing from author manifest: {sorted(missing)}')
    # SharedImage caches by physical path/variant. Any CPU consumer applies to
    # every descriptor alias before the first upload can purge that shared data.
    records = [record for group in plan for record in group["records"]]
    retained = {(r["path"].casefold(), r["variant"]) for r in records
                if r["kind"] == 0 and r["flags"] & KEEP_BITS and r["path"]}
    for record in records:
        if record["kind"] == 0 and (record["path"].casefold(), record["variant"]) in retained:
            record["flags"] |= KEEP_BITS
            raw = bytearray(record["data"])
            struct.pack_into('<I', raw, 8, record["flags"])
            record["data"] = bytes(raw)
    # Group order is canonical; records preserve authoring order within each group.
    plan.sort(key=lambda group: group["group"])
    payload = bytearray()
    for group in plan:
        block = b''.join(struct.pack('<I', len(r["data"])) + r["data"] for r in group["records"])
        payload.extend(struct.pack('<3I', group["group"], len(group["records"]), len(block)))
        payload.extend(block)
    header = struct.pack('<8I', MAGIC, VERSION, len(symbols) + 1, len(plan),
                         len(records), registry_crc, zlib.crc32(payload), len(payload))
    return header + payload, plan, registry_crc, names


def production_image_roots(symbol_names: dict[int, str], code_symbols: set[str]) -> set[str]:
    """Keep actual consumers plus the intentionally retained QuickPlay artwork.

    Font layer bitmaps are referenced by compiled font records. Their file-path
    closure survives independently of declaration-only FONT_IMAGE_* globals.
    """
    registered = set(symbol_names.values())
    if not code_symbols <= registered:
        raise ValueError(f"Code image roots absent from registry: {sorted(code_symbols - registered)}")
    return code_symbols | {name for name in registered
                           if name.startswith("IMAGE_QUICKPLAY_")
                           or name == "IMAGE_SELECTORSCREEN_MOREWAYSTOPLAY_BG"}


def code_image_roots(root: Path, symbol_names: dict[int, str], *,
                     required_names: set[str] | None = None) -> set[str]:
    """Find actual named global and ImageSymbol enum consumers."""
    exact = set(symbol_names.values())
    enum_path = program_symbols_path(root)
    aliases = ({identifier: symbol_names[int(number)]
                for identifier, number in re.findall(r'^\s*(\w+)\s*=\s*(\d+),',
                    enum_path.read_text(encoding='utf-8'), re.M) if int(number) in symbol_names}
               if enum_path.exists() else {})
    roots = set()
    for source in (root / 'pvzPerf').rglob('*'):
        if source.suffix not in {'.cpp', '.h', '.hpp', '.inl'} or 'generated' in source.parts:
            continue
        if source.name in {'Resources.cpp', 'Resources.h', 'ProgramImageSymbols.h'}:
            continue
        # Import lazily: the standalone compiler has no package-audit dependency.
        import build_numeric_runtime_pak64 as pak
        text = pak.clean_comments(source.read_text(encoding='utf-8-sig'))
        for token in re.findall(r'\b(?:IMAGE_|FONT_IMAGE_)\w+', text):
            name = token[:-3] if token.endswith('_ID') else token
            name = aliases.get(name, name)
            if required_names is not None and name in required_names and name not in exact:
                raise ValueError(f'Code image absent from program symbol registry: {name} ({source})')
            if name in exact:
                roots.add(name)
    return roots


def emit_headers(root: Path, plan: list[dict], registry_crc: int, names: list[str]) -> None:
    """Bake explicit targets and plan checksum; no persistent descriptor tables."""
    payload = bytearray()
    for group in plan:
        block = b''.join(struct.pack('<I', len(r["data"])) + r["data"] for r in group["records"])
        payload.extend(struct.pack('<3I', group["group"], len(group["records"]), len(block)))
        payload.extend(block)
    generated = root / 'pvzPerf/Sexy.TodLib/generated'
    output = ['// Generated transient load-plan contract.', '#pragma once', '#include <cstdint>',
              f'inline constexpr std::uint32_t RESOURCE_LOAD_PLAN_CRC = 0x{zlib.crc32(payload):08X}u;',
              f'inline constexpr unsigned RESOURCE_LOAD_PLAN_RECORD_COUNT = {sum(len(g["records"]) for g in plan)};']
    write_changed(generated / 'NumericResourceBindings.h', '\n'.join(output) + '\n')
    output = ['// Generated explicit resource ownership bindings. Do not edit.',
              'namespace ResourceLoadBindings', '{',
              'bool LoadStep(Sexy::ResourceManager& manager, Sexy::ResourceManager::LoadPlan& plan,',
              '              ResourceGroupId group, unsigned step)', '{', '    switch (group)', '    {']
    kinds = ('Image', 'Sound', 'Font')
    for group in plan:
        output += [f'    case ResourceGroupId::{group["name"]}:', '        switch (step)', '        {']
        for step, record in enumerate(group["records"]):
            kind, name = kinds[record["kind"]], record["name"]
            symbol = (f', static_cast<std::uint32_t>(ImageSymbolId::{name})' if record["kind"] == 0 else '')
            output.append(f'        case {step}: return manager.Load{kind}(plan.m{kind}s[plan.mNext{kind}++], Sexy::{name}{symbol});')
        output += ['        default: return false;', '        }']
    output += ['    default: return false;', '    }', '}',
               'void ReleaseGroup(Sexy::ResourceManager& manager, ResourceGroupId group)',
               '{', '    switch (group)', '    {']
    for group in plan:
        output += [f'    case ResourceGroupId::{group["name"]}:']
        for record in reversed(group["records"]):
            kind, name = kinds[record["kind"]], record["name"]
            symbol = (f', static_cast<std::uint32_t>(ImageSymbolId::{name})' if record["kind"] == 0 else '')
            output.append(f'        manager.Release{kind}(Sexy::{name}{symbol});')
        output += ['        break;']
    output += ['    default: break;', '    }', '}', '}']
    write_changed(generated / 'ResourceLoadBindings.inc', '\n'.join(output) + '\n')


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--root', type=Path, default=ROOT)
    ap.add_argument('--archive', type=Path, default=ROOT / 'out/x64-win32-dx12/Release/main.pak64')
    ap.add_argument('--authoring', type=Path, help='ordered JSON resource author manifest')
    ap.add_argument('--sidecar', type=Path, help='program symbol header (historical .bin accepted for fixtures)')
    ap.add_argument('--output', type=Path)
    ap.add_argument('--headers', action='store_true')
    ap.add_argument('--verify', action='store_true', help='compare with existing output without writing')
    args = ap.parse_args()
    authoring = args.authoring or args.root / 'assets/definitions/resources.json'
    sidecar = args.sidecar or program_symbols_path(args.root)
    output = args.output or args.archive.parent / 'compiled/resources.compiled'
    with zipfile.ZipFile(args.archive) as archive:
        data, plan, crc, names = compile_manifest(args.root, authoring.read_bytes(), archive.namelist(), sidecar)
    if args.verify:
        if output.read_bytes() != data:
            raise ValueError(f'Numeric manifest is stale: {output}')
    else:
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_bytes(data)
        if args.headers:
            emit_headers(args.root, plan, crc, names)
    values = struct.unpack_from('<8I', data)
    print(f'image-symbols={values[2]-1} groups={values[3]} load-records={values[4]} bytes={len(data)} registry-crc={crc:08X}')
    print(output)
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
