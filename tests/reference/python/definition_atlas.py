"""Offline v4 -> v5 definition atlases, using the runtime's image decode rules."""
from __future__ import annotations
from game_fixture import game_root

from dataclasses import dataclass
import hashlib
import io
import math
from pathlib import Path, PurePosixPath
import struct
import subprocess
import tempfile
import zlib

from PIL import Image

import build_numeric_runtime_pak64 as pak


@dataclass(frozen=True)
class ImageDescriptor:
    source_label: str | int
    path: str
    alpha: str
    alpha_grid: str
    flags: int
    rows: int
    cols: int
    color: int


def authoring_descriptors(authoring: bytes, files: dict[str, Path],
                          names: set[str]) -> dict[str, ImageDescriptor]:
    """Resolve author image labels without any program image symbol registry."""
    from compile_numeric_resources import read_authoring
    manifest = read_authoring(authoring)
    declared = {}
    default_path, default_prefix = "", ""
    for group in manifest["children"]:
        for node in group["children"]:
            attrs = node["attrs"]
            if node["tag"] == "SetDefaults":
                if "path" in attrs:
                    default_path = attrs["path"].replace("\\", "/").rstrip("/") + "/"
                default_prefix = attrs.get("idprefix", default_prefix).rstrip("/")
                continue
            if node["tag"] != "Image":
                continue
            original = attrs["path"].replace("\\", "/")
            name = default_prefix + attrs.get("id", PurePosixPath(original).stem)
            if name not in names:
                continue
            pak.require(name not in declared, f"Duplicate author image label: {name}")
            path = original if original.startswith("!") else default_path + original
            if name == "IMAGE_PARTNER_LOGO":
                path = "images/blank"
            flags = 0 if "noalpha" in attrs or name == "IMAGE_EDITBOX" else 1024
            if path == "!program":
                flags |= 1
                path = ""
            else:
                pak.require(not path.startswith("!"), f"Unsupported author image control path: {name}")
            rows, cols = int(attrs.get("rows", "1")), int(attrs.get("cols", "1"))
            pak.require(0 < rows <= 0x7FFFFFFF and 0 < cols <= 0x7FFFFFFF,
                        f"Invalid author image grid: {name}")
            color = int(attrs.get("alphacolor", "FFFFFF"), 16)
            pak.require(0 <= color <= 0xFFFFFFFF, f"Invalid author alpha color: {name}")
            declared[name] = ImageDescriptor(
                name, path, default_path + attrs["alphaimage"].replace("\\", "/")
                if "alphaimage" in attrs else "",
                default_path + attrs["alphagrid"].replace("\\", "/")
                if "alphagrid" in attrs else "", flags, rows, cols, color)
    paths = {}
    for filename in sorted(files):
        normalized = filename.replace("\\", "/")
        if PurePosixPath(normalized).suffix.casefold() in (".png", ".jpg", ".jpeg", ".bmp", ".gif"):
            paths.setdefault(str(PurePosixPath(normalized).with_suffix("")).casefold(), normalized)
    for name in sorted(names - set(declared)):
        normalized = name.replace("\\", "/")
        if "/" in normalized:
            pak.require(not PurePosixPath(normalized).is_absolute() and
                        ":" not in normalized and
                        all(part not in ("", ".", "..") for part in normalized.split("/")) and
                        pak.file_key(normalized) in files, f"Invalid direct author image path: {name}")
            declared[name] = ImageDescriptor(name, normalized, "", "", 0, 1, 1, 0xFFFFFF)
            continue
        reanim = name.startswith("IMAGE_REANIM_")
        suffix = (name[len("IMAGE_REANIM_"):] if reanim else
                  name[len("IMAGE_"):] if name.upper().startswith("IMAGE_") else "")
        directories = ("reanim", "images", "particles") if reanim else ("particles", "images", "reanim")
        candidates = [paths[f"{directory}/{suffix}".casefold()] for directory in directories
                      if f"{directory}/{suffix}".casefold() in paths]
        if not candidates:
            pak.require(name in ("IMAGE_PROJECTILE", "IMAGE_SUN"),
                        f"Author image label has no file path: {name}")
            declared[name] = ImageDescriptor(name, "", "", "", 8192, 1, 1, 0xFFFFFF)
        else:
            declared[name] = ImageDescriptor(name, str(PurePosixPath(candidates[0]).with_suffix("")),
                                             "", "", 1024, 1, 1, 0xFFFFFF)
    return declared


def descriptors(data: bytes, image_names: list[str] | None = None, *,
                legacy_fixture: bool = False) -> dict[int | str, ImageDescriptor]:
    """Decode NRSC4 load metadata; source grids belong to authoring_descriptors.

    Frozen NRSC2 fixtures require the explicit legacy flag and retain their old
    offline grid semantics. NRSC3 is an unsupported intermediate format.
    """
    result = {}
    if not legacy_fixture:
        pak.require(image_names is not None, "NRSC4 descriptors require named image targets")
    names = iter(image_names or ())
    for _, _, fields, paths, tail in pak.resource_records(data, legacy_fixture=legacy_fixture):
        if legacy_fixture:
            kind, symbol, flags, _, _ = fields
        else:
            kind, flags, _, _ = fields
            symbol = next(names, None) if kind == 0 else None
            pak.require(kind != 0 or symbol is not None, "Incomplete named image targets")
        path, alpha, grid, _ = paths
        if kind == 0 and symbol:
            record = pak.Reader(tail)
            if legacy_fixture:
                rows, cols, color = record.unpack("<iiI")
            else:
                rows, cols, color = 1, 1, record.word()
                record.finish()
            result[symbol] = ImageDescriptor(symbol, path, alpha, grid, flags,
                                             rows, cols, color)
    pak.require(legacy_fixture or next(names, None) is None, "Extra named image targets")
    return result


def image_offsets(kind: str, payload: bytes) -> list[int]:
    """Locate only image IDs; preserve all transforms, fonts, captions and names."""
    reader = pak.Reader(payload)
    offsets = []
    if kind == "reanim":
        magic, _, _, _, _, count = reader.unpack("<6I")
        pak.require(magic in (0x35544449, 0x46544449), "Expected IDT5/IDTF compiler payload")
        tracks = []
        for _ in range(count):
            _, frames, stride, mode = reader.unpack("<4I")
            mode, _ = pak.reanim_track_flags(mode, authored=magic == 0x46544449)
            pak.require(stride == 32, "Invalid compiler track")
            reader.take(frames * stride)
            tracks.append((frames, mode))
        for frames, mode in tracks:
            for _ in range(frames):
                offsets.append(reader.at)
                reader.take(8)
                if mode:
                    reader.take(reader.word())
    else:
        magic, _, _, count, body_bytes = reader.unpack("<5I")
        pak.require(kind == "particles" and magic in (0x35504449, 0x34524449, 0x46524449),
                    "Expected particle compiler payload")
        end = reader.at + body_bytes

        def track() -> None:
            reader.take(reader.word() * (24 if magic == 0x46524449 else 20))

        for _ in range(count):
            if magic in (0x34524449, 0x46524449):
                for _ in range(reader.count(4, 100000)):
                    offsets.append(reader.at)
                    pak.require(reader.word() != 0, "Null particle frame image handle")
                reader.take(16)
            else:
                offsets.append(reader.at)
                reader.take(4)
                reader.take(24)
            for _ in range(22):
                track()
            for _ in range(2):
                for _ in range(reader.word()):
                    reader.take(4)
                    track()
                    track()
            for _ in range(16):
                track()
        pak.require(reader.at == end, "Compiler particle body size differs")
    return offsets


def read_image(path: str, files: dict[str, Path], auto: bool = True,
               color: int = 0xFFFFFF) -> tuple[Image.Image, set[str]]:
    """Match ImageLib::GetImage, including alpha-only and blue-channel masks."""
    normalized = pak.file_key(path)
    suffixes = (".tga", ".jpg", ".png", ".gif")
    suffix = PurePosixPath(normalized).suffix

    def decode(stem: str) -> tuple[Image.Image | None, set[str]]:
        ext = PurePosixPath(stem).suffix
        candidates = [stem] if ext else [stem + extension for extension in suffixes]
        for candidate in candidates:
            if candidate in files and (not ext or ext in suffixes):
                with Image.open(files[candidate]) as image:
                    return image.convert("RGBA"), {candidate}
        return None, set()

    image, used = decode(normalized)
    mask = None
    if auto:
        filename = PurePosixPath(normalized).name
        leading = str(PurePosixPath(normalized).with_name("_" + filename))
        mask, mask_used = decode(leading)
        if mask is None:
            trailing = normalized[:-len(suffix)] + "_" + suffix if suffix else normalized + "_"
            mask, mask_used = decode(trailing)
        used |= mask_used
    if mask is not None:
        if image is None:
            image = Image.new("RGBA", mask.size,
                              ((color >> 16) & 255, (color >> 8) & 255, color & 255, 255))
        pak.require(image.size == mask.size, f"Image alpha dimensions differ: {path}")
        image.putalpha(mask.getchannel("B"))
    pak.require(image is not None, f"Definition image missing: {path}")
    return image, used


def logical_image(descriptor: ImageDescriptor, files: dict[str, Path]) -> tuple[Image.Image, set[str]]:
    pak.require(not descriptor.flags & (1 | 8192),
                f"Unavailable definition image: {descriptor.source_label}")
    image, used = read_image(descriptor.path, files,
                             bool(descriptor.flags & 1024) and not (descriptor.alpha or descriptor.alpha_grid),
                             descriptor.color)
    for path, cols, rows in ((descriptor.alpha, 1, 1),
                             (descriptor.alpha_grid, descriptor.cols, descriptor.rows)):
        if not path:
            continue
        mask, mask_used = read_image(path, files)
        used |= mask_used
        pak.require(mask.size == (image.width // cols, image.height // rows),
                    f"Explicit alpha dimensions differ: {path}")
        channel = mask.getchannel("B")
        # The runtime leaves any nondivisible grid remainder unchanged.
        tiled = image.getchannel("A")
        for row in range(rows):
            for col in range(cols):
                tiled.paste(channel, (col * channel.width, row * channel.height))
        image.putalpha(tiled)
    return image, used


def native_layouts(compiler: Path, batches: list[list[tuple[int, int]]],
                   padding: int = 2) -> list[dict]:
    """One compiler process optimizes a complete corpus; failures are fatal."""
    request = bytearray(struct.pack("<3I", 0x314B5041, 1, len(batches)))
    for sizes in batches:
        request.extend(struct.pack("<2I", padding, len(sizes)))
        for width, height in sizes:
            request.extend(struct.pack("<2I", width, height))
    with tempfile.TemporaryDirectory(prefix="atlas-layout-") as temporary:
        source, target = Path(temporary) / "batch.bin", Path(temporary) / "layout.bin"
        source.write_bytes(request)
        subprocess.run([str(compiler.resolve()), "pack-atlas-layout", "--input", str(source),
                        "--output", str(target)], check=True, timeout=180)
        reader = pak.Reader(target.read_bytes())
    pak.require(reader.unpack("<3I") == (0x314C5041, 1, len(batches)), "Invalid native atlas response")
    result = []
    for sizes in batches:
        width, height, count, attempts, baseline, micros = reader.unpack("<4I2Q")
        pak.require(count == len(sizes), "Native atlas region count differs")
        regions, rectangles = [], []
        for w, h in sizes:
            x, y = reader.unpack("<2I")
            pak.require(x >= padding and y >= padding and x + w + padding <= width and
                        y + h + padding <= height, "Native atlas region outside image")
            rect = (x - padding, y - padding, x + w + padding, y + h + padding)
            pak.require(all(rect[2] <= old[0] or old[2] <= rect[0] or rect[3] <= old[1]
                            or old[3] <= rect[1] for old in rectangles), "Native atlas regions overlap")
            rectangles.append(rect)
            regions.append((x, y, w, h))
        pak.require(width <= 16384 and height <= 16384 and (not baseline or width * height <= baseline) and
                    ((count == 0 and width == height == baseline == 0) or count > 0),
                    "Native atlas regresses shelf baseline or dimensions")
        result.append({"width": width, "height": height, "regions": regions,
                       "shelf_area": baseline, "search_attempts": attempts,
                       "search_microseconds": micros, "method": "native-maxrects"})
    reader.finish()
    return result


def shelf_layout(sizes: list[tuple[int, int]], padding: int = 2) -> dict:
    """Reference fixture layout; production calls the mandatory native batch."""
    if not sizes:
        return {"width": 0, "height": 0, "regions": []}
    order = sorted(range(len(sizes)), key=lambda i: (-sizes[i][1], -sizes[i][0], i))
    minimum = max(width + 2 * padding for width, _ in sizes)
    area = sum((width + 2 * padding) * (height + 2 * padding) for width, height in sizes)
    widths = {minimum, max(minimum, math.ceil(math.sqrt(area)))}
    widths.update(1 << power for power in range(5, 15) if 1 << power >= minimum)
    best = None
    for width in sorted(widths):
        if width > 16384:
            continue
        x = y = shelf = used_width = 0
        positions = {}
        for index in order:
            iw, ih = sizes[index]
            w, h = iw + 2 * padding, ih + 2 * padding
            if x + w > width:
                y += shelf
                x = shelf = 0
            positions[index] = (x + padding, y + padding, iw, ih)
            x += w
            used_width = max(used_width, x)
            shelf = max(shelf, h)
        height = y + shelf
        if height <= 16384:
            candidate = (used_width * height, max(used_width, height), used_width, height, positions)
            if best is None or candidate[:4] < best[:4]:
                best = candidate
    pak.require(best is not None, "Definition atlas exceeds 16384 pixels")
    _, _, width, height, positions = best
    return {"width": width, "height": height,
            "regions": [positions[index] for index in range(len(sizes))]}


def pack(images: list[Image.Image], padding: int = 2, *, compiler: Path | None = None,
         layout: dict | None = None) -> tuple[Image.Image | None, list[tuple[int, int, int, int]]]:
    """Apply an offline native layout and replicate its two-pixel RGBA edges."""
    sizes = [image.size for image in images]
    if layout is None:
        layout = native_layouts(compiler, [sizes], padding)[0] if compiler else shelf_layout(sizes, padding)
    if not images:
        return None, []
    width, height, positions = layout["width"], layout["height"], layout["regions"]
    pak.require(len(positions) == len(images), "Image/layout count differs")
    atlas = Image.new("RGBA", (width, height), (0, 0, 0, 0))
    for index, image in enumerate(images):
        x, y, w, h = positions[index]
        atlas.paste(image, (x, y))
        for left, top, right, bottom, box in (
            (0, 0, w, 1, (x, y - padding, w, padding)),
            (0, h - 1, w, h, (x, y + h, w, padding)),
            (0, 0, 1, h, (x - padding, y, padding, h)),
            (w - 1, 0, w, h, (x + w, y, padding, h)),
            (0, 0, 1, 1, (x - padding, y - padding, padding, padding)),
            (w - 1, 0, w, 1, (x + w, y - padding, padding, padding)),
            (0, h - 1, 1, h, (x - padding, y + h, padding, padding)),
            (w - 1, h - 1, w, h, (x + w, y + h, padding, padding)),
        ):
            bx, by, bw, bh = box
            atlas.paste(image.crop((left, top, right, bottom)).resize((bw, bh), Image.Resampling.NEAREST), (bx, by))
    return atlas, positions


def compile_reanim_variants(name: str, data: bytes, metadata: dict, variants: list[dict],
                           source_names: dict[int, str], descriptors: dict,
                           actions: dict | None = None) -> tuple[bytes, dict]:
    """Share behavior once; appearance metadata owns only texture geometry."""
    cookie, size, crc = struct.unpack_from("<3I", data)
    pak.require(cookie == 0xDEADFED7 and size == len(data) - 12 and
                zlib.crc32(data[12:]) == crc, "Invalid reanim source envelope")
    reader = pak.Reader(data[12:])
    magic, track_hash, image_hash, font_hash, fps_bits, track_count = reader.unpack("<6I")
    pak.require(magic in (0x35544449, 0x46544449) and image_hash == 0 and
                track_count == len(metadata["tracks"]), "Invalid reanim variant root")
    tracks = []
    for _ in range(track_count):
        track_id, frames, stride, mode = reader.unpack("<4I")
        pak.reanim_track_flags(mode, authored=magic == 0x46544449)
        pak.require(stride == 32, "Invalid reanim variant track")
        values = reader.take(frames * 32)
        tracks.append((track_id, frames, mode, values))
    phase1_end = reader.at
    authored = magic == 0x46544449
    body_parts = [struct.pack("<I", 0x46544449 if authored else 0x36544449), data[16:12 + phase1_end]]
    slot_keys, slot_map = [None], {}
    for t, (_, frames, mode, values) in enumerate(tracks):
        previous, previous_cel = "", 0.0
        originals = metadata["variants"][0]["images"][t]
        for f in range(frames):
            handle = reader.word()
            if handle:
                pak.require(handle in source_names, "Unknown reanim source image")
                previous = source_names[handle]
            cel = struct.unpack_from("<i" if authored else "<f", values, f * 32 + 24)[0]
            if cel == -10000.0:
                cel = previous_cel
            previous_cel = cel
            slot = 0
            if previous:
                descriptor = descriptors[previous]
                rounded = int(cel + 0.5 if cel > 0 else cel - 0.5)
                # Reanimations wrap horizontal columns; rows affect cel height
                # but do not change the selected row.
                cell = 0 if rounded < 0 else rounded % descriptor.cols
                key = (t, previous, cell)
                if key not in slot_map:
                    slot_map[key] = len(slot_keys)
                    pak.require(previous in originals, "Root image missing from author slot table")
                    slot_keys.append((t, originals.index(previous), previous, cell))
                slot = slot_map[key]
            body_parts.append(struct.pack("<I", slot))
            body_parts.append(reader.take(4))  # Font ID is shared behavior.
            if mode & 1:
                text = reader.take(reader.word())
                body_parts.append(struct.pack("<I", len(text)) + text)
    body_parts.append(reader.take(len(reader.data) - reader.at))
    variant_names = [row["name"] for row in metadata["variants"]]
    pak.require(len(variants) == len(variant_names) and
                len(set(variant_names)) == len(variant_names), "Invalid reanim variant rows")
    track_ids = {track: tracks[i][0] for i, track in enumerate(metadata["tracks"])}
    action_rows, transitions = [], []
    if actions:
        author_actions = actions["actions"]
        order = sorted(range(len(author_actions)),
                       key=lambda i: (track_ids[author_actions[i]["track"]], author_actions[i]["option"]))
        action_rows = [(track_ids[author_actions[i]["track"]], author_actions[i]["option"]) for i in order]
        pak.require(set(actions["transitions"]) == set(variant_names), "Incomplete reanim transition rows")
        indices = {value: index for index, value in enumerate(variant_names)}
        for variant in variant_names:
            targets = actions["transitions"][variant]
            pak.require(len(targets) == len(order), "Incomplete reanim transition actions")
            transitions.extend(indices[targets[i]] for i in order)
    rectangles, dimensions = [], []
    for v, row in enumerate(variants):
        regions = dict(zip(row["symbols"], row["layout"]["regions"]))
        images = metadata["variants"][v]["images"]
        has_images = any(label is not None for track in images for label in track)
        dimensions.append((row["layout"]["width"], row["layout"]["height"]) if has_images else (0, 0))
        rectangles.append((0, 0, 0, 0))
        for t, author_slot, original, cell in slot_keys[1:]:
            replacement = images[t][author_slot]
            if replacement is None:
                rectangles.append((0, 0, 0, 0))
                continue
            x, y, w, h = regions[replacement]
            if replacement == original:
                descriptor = descriptors[original]
                cw, ch = w // descriptor.cols, h // descriptor.rows
                x, y = x + cell % descriptor.cols * cw, y + cell // descriptor.cols * ch
                w, h = cw, ch
            rectangles.append((x, y, w, h))
    prefix = struct.pack("<3I", len(variants), len(slot_keys), len(action_rows))
    prefix += b"".join(struct.pack("<2I", *value) for value in dimensions + action_rows)
    prefix += b"".join(struct.pack("<I", target) for target in transitions)
    prefix += b"".join(struct.pack("<4i", *rect) for rect in rectangles)
    behavior = b"".join(body_parts)
    payload = struct.pack("<2I", 0x37564152, len(behavior)) + behavior + prefix
    output = struct.pack("<3I", 0xDEADFEDB, len(payload), zlib.crc32(payload)) + payload
    return output, {"variant_count": len(variants), "slot_count": len(slot_keys),
                    "action_count": len(action_rows), "variant_names": variant_names}


def compile_particle_variants(name: str, data: bytes, rows: list[dict]) -> tuple[bytes, dict]:
    """Retain one particle behavior followed by the complete appearance table."""
    atlas = pak.parse_definition_atlas(name, data)
    pak.require(atlas is not None and "frame_counts" in atlas and atlas.get("parent") is None,
                "Particle appearance root requires expanded IDR5")
    source = pak.Reader(data[24:])
    magic, total, emitter_hash, count, body_size = source.unpack("<5I")
    pak.require(magic in (0x35524449, 0x46524449), "Particle appearance root requires IDR5/IDRF")
    stride = 24 if magic == 0x46524449 else 20
    end = source.at + body_size
    parts, offset = [], 0
    for frames in atlas["frame_counts"]:
        pak.require(source.word() == frames, "Particle emitter frame count differs")
        source.take(frames * 16)
        start = source.at
        source.take(16)
        for _ in range(22):
            source.take(source.count(stride, 100000) * stride)
        for _ in range(2):
            for _ in range(source.count(12, 4)):
                source.take(4)
                for _ in range(2):
                    source.take(source.count(stride, 100000) * stride)
        for _ in range(16):
            source.take(source.count(stride, 100000) * stride)
        parts.append(struct.pack("<2I", offset, frames) + source.data[start:source.at])
        offset += frames
    pak.require(source.at == end and offset == total, "Particle root behavior size differs")
    body = b"".join(parts)
    behavior = (struct.pack("<5I", 0x46524449 if stride == 24 else 0x36524449, total, emitter_hash, count, len(body)) + body
                + source.take(len(source.data) - source.at))
    appearance = struct.pack("<2I", len(rows), total)
    appearance += b"".join(struct.pack("<2I", row["width"], row["height"]) for row in rows)
    for row in rows:
        pak.require(row["frame_counts"] == atlas["frame_counts"] and
                    len(row["rectangles"]) == total, "Particle appearance frame counts differ")
        appearance += b"".join(struct.pack("<4i", *rect) for rect in row["rectangles"])
    payload = struct.pack("<2I", 0x36564150, len(behavior)) + behavior + appearance
    return (struct.pack("<3I", 0xDEADFEDC, len(payload), zlib.crc32(payload)) + payload,
            {"variant_count": len(rows), "slot_count": total})


def compile_definition(name: str, data: bytes, source_images: dict[int | str, ImageDescriptor],
                       files: dict[str, Path], *, layout: dict | None = None,
                       atlas_symbols: list[int | str] | None = None, atlas_path: str = "",
                       emit_png: bool = True,
                       source_names: dict[int, str] | None = None) -> tuple[bytes, bytes | None, dict]:
    kind = pak.numeric_kind(name)
    cookie, size, crc = struct.unpack_from("<3I", data)
    pak.require(kind in ("reanim", "particles") and cookie == (0xDEADFED7 if kind == "reanim" else 0xDEADFED8)
                and size == len(data) - 12 and zlib.crc32(data[12:]) == crc,
                f"Invalid compiler definition: {name}")
    body = bytearray(data[12:])
    offsets = image_offsets(kind, body)
    handles = {struct.unpack_from("<I", body, offset)[0] for offset in offsets} - {0}
    if source_names is not None:
        pak.require(handles <= set(source_names), f"Unknown definition author image handle: {name}")
    labels = {handle: source_names[handle] if source_names is not None else handle for handle in handles}
    symbols = sorted(set(labels.values()))
    packed_symbols = symbols if atlas_symbols is None else atlas_symbols
    pak.require(set(symbols) <= set(packed_symbols), "Shared atlas misses a definition image")
    images, used, image_rows = [], set(), []
    for symbol in symbols:
        image, dependencies = logical_image(source_images[symbol], files)
        images.append(image)
        used |= dependencies
        image_rows.append({("source" if source_names is not None else "symbol"): symbol,
                           "width": image.width, "height": image.height,
                           "rows": source_images[symbol].rows, "cols": source_images[symbol].cols,
                           "rgba_bytes": image.width * image.height * 4,
                           "rgba_sha256": hashlib.sha256(image.tobytes()).hexdigest(),
                           "files": sorted(dependencies)})
    struct.pack_into("<I", body, 8 if kind == "reanim" else 4, 0)
    if layout is None:
        pak.require(packed_symbols == symbols, "Shared atlas requires a compiled layout")
        layout = shelf_layout([image.size for image in images])
    pak.require(len(layout["regions"]) == len(packed_symbols), "Shared atlas symbol/layout count differs")
    region_by_symbol = dict(zip(packed_symbols, layout["regions"]))
    regions = [region_by_symbol[symbol] for symbol in symbols]
    inline = {symbol: (*region_by_symbol[symbol], source_images[symbol].rows,
                       source_images[symbol].cols) for symbol in symbols}
    for row in image_rows:
        row["region"] = inline[row["source" if source_names is not None else "symbol"]]
    authored = struct.unpack_from("<I", body)[0] == 0x46524449
    explicit_frames = kind == "particles" and struct.unpack_from("<I", body)[0] in (0x34524449, 0x46524449)
    frame_rows = []
    if kind == "particles" and not explicit_frames:
        original_body_size = struct.unpack_from("<I", body, 16)[0]
        struct.pack_into("<I", body, 16, original_body_size + 20 * len(offsets))
    # Copy the body once. Inserting 20 extra bytes into the bytearray for every
    # frame repeatedly shifts the remaining transform data in large animations.
    rectangles = {handle: struct.pack("<6I", *inline[label])
                  for handle, label in labels.items()}
    rectangles[0] = bytes(24)
    parts, previous = [], 0
    for offset in offsets:
        handle = struct.unpack_from("<I", body, offset)[0]
        if explicit_frames:
            label = labels[handle]
            final = region_by_symbol[label]
            _, _, w, h = final
            frame_rows.append({"source": label, "source_rect": (0, 0, w, h), "atlas_rect": final})
            parts.extend((body[previous:offset], struct.pack("<4i", *final)))
            previous = offset + 4
        else:
            parts.extend((body[previous:offset], rectangles[handle]))
            previous = offset + 4
    parts.append(body[previous:])
    body = b"".join(parts)
    if explicit_frames:
        body = bytearray(body)
        struct.pack_into("<I", body, 0, 0x46524449 if authored else 0x35524449)
        struct.pack_into("<I", body, 4, len(offsets))
        struct.pack_into("<I", body, 16, struct.unpack_from("<I", body, 16)[0] + 12 * len(offsets))
        body = bytes(body)
    atlas = None
    if emit_png and packed_symbols:
        packed_images = (images if packed_symbols == symbols else
                         [logical_image(source_images[symbol], files)[0] for symbol in packed_symbols])
        atlas, _ = pack(packed_images, layout=layout)
    width, height = (layout["width"], layout["height"]) if symbols else (0, 0)
    prefix = struct.pack("<3I", 0x35564152 if kind == "reanim" else 0x35564150, width, height)
    payload = prefix + body
    output = struct.pack("<3I", 0xDEADFED9 if kind == "reanim" else 0xDEADFEDA,
                         len(payload), zlib.crc32(payload)) + payload
    png = None
    if atlas:
        buffer = io.BytesIO()
        atlas.save(buffer, format="PNG", compress_level=9)
        png = buffer.getvalue()
    return output, png, {"atlas_width": width, "atlas_height": height,
                         "atlas_path": (atlas_path or name.removesuffix(".compiled") + ".atlas.png") if symbols else "",
                         "atlas_shared": bool(atlas_path) and bool(symbols),
                         "packing": {key: value for key, value in (layout or {}).items()
                                     if key not in ("regions", "width", "height")},
                         "atlas_rgba_bytes": width * height * 4,
                         "padded_region_rgba_bytes": sum((image.width + 4) * (image.height + 4) * 4
                                                        for image in images),
                         "source_rgba_bytes": sum(row["rgba_bytes"] for row in image_rows),
                         "region_count": len(regions), "images": image_rows,
                         "frames": frame_rows,
                         "source_files": sorted(used)}
