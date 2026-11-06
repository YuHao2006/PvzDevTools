"""Compile final offline PAV6 records into runtime particle binaries."""
from __future__ import annotations
from game_fixture import game_root

import math
from pathlib import Path
import re
import struct
from fractions import Fraction

import build_numeric_runtime_pak64 as pak
CURVES = ("Constant", "Linear", "EaseIn", "EaseOut", "EaseInOut",
          "EaseInOutWeak", "FastInOut", "FastInOutWeak", "Constant",
          "Bounce", "BounceFastMiddle", "BounceSlowMiddle", "SinWave", "EaseSinWave")
SYSTEM_TRACKS = (
    "SystemDuration", "CrossFadeDuration", "SpawnRate", "SpawnMinActive",
    "SpawnMaxActive", "SpawnMaxLaunched", "EmitterRadius", "EmitterOffsetX",
    "EmitterOffsetY", "EmitterBoxX", "EmitterBoxY", "EmitterSkewX", "EmitterSkewY",
    "EmitterPath", "ParticleDuration", "LaunchSpeed", "LaunchAngle", "SystemRed",
    "SystemGreen", "SystemBlue", "SystemAlpha", "SystemBrightness")
PARTICLE_TRACKS = (
    "ParticleRed", "ParticleGreen", "ParticleBlue", "ParticleAlpha",
    "ParticleBrightness", "ParticleSpinAngle", "ParticleSpinSpeed", "ParticleScale",
    "ParticleStretch", "CollisionReflect", "CollisionSpin", "ClipTop", "ClipBottom",
    "ClipLeft", "ClipRight", "AnimationRate")
DEFAULTS = {
    **dict.fromkeys(("SpawnMinActive", "SpawnMaxActive", "SpawnMaxLaunched"), -1.0),
    "ParticleDuration": 100.0,
    **dict.fromkeys(("SystemRed", "SystemGreen", "SystemBlue", "SystemAlpha",
                     "SystemBrightness", "ParticleRed", "ParticleGreen", "ParticleBlue",
                     "ParticleAlpha", "ParticleBrightness", "ParticleScale",
                     "ParticleStretch"), 1.0),
}
RENDER_TRACKS = (
    "SystemRed", "SystemGreen", "SystemBlue", "SystemAlpha", "SystemBrightness",
    "ParticleRed", "ParticleGreen", "ParticleBlue", "ParticleAlpha", "ParticleBrightness",
    "ParticleScale", "ParticleStretch", "ParticleSpinSpeed", "ParticleSpinAngle")


def _registry() -> tuple[int, int]:
    header = (game_root() /
              "pvzPerf/Sexy.TodLib/generated/ParticleEmitterName.h").read_text(encoding="utf-8")
    count = re.search(r"\btop\s*=\s*(\d+)", header)
    hash_ = re.search(r"\bkParticleEmitterNameTableHash\s*=\s*(\d+)", header)
    pak.require(count is not None and hash_ is not None, "Missing particle emitter name registry")
    return int(count[1]), int(hash_[1])


def _parse(path: str, data: bytes, name_count: int, name_hash: int) -> dict:
    atlas = pak.parse_definition_atlas(path, data)
    pak.require(atlas is not None and "variant_rectangles" in atlas
                and data[:4] == b"\xdc\xfe\xad\xde",
                f"Static particles require PAV6: {path}")
    reader = pak.Reader(data[20:atlas["appearance_offset"]])
    magic, total, hash_, count, body_size = reader.unpack("<5I")
    authored = magic == 0x46524449
    pak.require(magic in (0x36524449, 0x46524449) and hash_ == name_hash
                and count == atlas["emitter_count"] and total == atlas["slot_count"],
                f"Invalid static IDR6 header: {path}")
    body = pak.Reader(reader.take(body_size))

    def track() -> list[tuple]:
        nodes = [body.unpack("<2i2f2I" if authored else "<3f2I")
                 for _ in range(body.count(24 if authored else 20, 100000))]
        if not authored:
            nodes = [(*Fraction(node[0]).limit_denominator(100000).as_integer_ratio(),
                      node[1], node[2], *node[3:])
                     for node in nodes]
        pak.require(all(node[1] > 0 and math.isfinite(node[2]) and math.isfinite(node[3])
                        and node[4] < len(CURVES) and node[5] < len(CURVES)
                        for node in nodes), "Invalid static IDR6 track node")
        return nodes

    emitters = []
    for index in range(count):
        offset, frames, image_frames, animated, flags, type_ = body.unpack("<2I3iI")
        pak.require(offset == atlas["slot_offsets"][index]
                    and frames == atlas["frame_counts"][index]
                    and image_frames > 0 and animated in (0, 1)
                    and 0 <= flags < (1 << 12) and type_ <= 4,
                    "Invalid static IDR6 emitter")
        tracks = dict(zip(SYSTEM_TRACKS, (track() for _ in SYSTEM_TRACKS)))
        fields = []
        for _ in range(2):
            row = []
            for _ in range(body.count(12, 4)):
                type_id = body.word()
                pak.require(0 < type_id < 12, "Invalid static IDR6 field type")
                row.append((type_id, track(), track()))
            fields.append(row)
        tracks.update(zip(PARTICLE_TRACKS, (track() for _ in PARTICLE_TRACKS)))
        for name, value in DEFAULTS.items():
            if not tracks[name]:
                tracks[name] = [(0, 1, value, value, 0, 1)]
        mask = sum(1 << bit for bit, name in enumerate(RENDER_TRACKS)
                   if tracks[name] and CURVES[tracks[name][0][4]] != "Constant")
        emitters.append({"offset": offset, "frames": frames, "image_frames": image_frames,
                         "animated": animated, "flags": flags, "type": type_,
                         "tracks": tracks, "fields": fields, "mask": mask})
    body.finish()
    pak.require(reader.unpack("<2I") == (0x33544449, count), "Invalid static IDT3 section")
    for emitter in emitters:
        target = reader.word()
        pak.require(target == 0xFFFFFFFF or target < count, "Invalid static duration target")
        emitter["duration"] = -1 if target == 0xFFFFFFFF else target
    pak.require(reader.unpack("<2I") == (0x35544449, count), "Invalid static IDT5 section")
    lookup = [-1] * name_count
    for index, emitter in enumerate(emitters):
        name = reader.word()
        pak.require(name < name_count, "Invalid static emitter name ID")
        emitter["name"] = name
        if lookup[name] == -1:
            lookup[name] = index
    reader.finish()
    return {"atlas": atlas, "emitters": emitters, "lookup": lookup}


def compiled_record(definition: dict, variants: list[dict]) -> bytes:
    """PTC1 is an explicit little-endian stream, independent of C++ layout."""
    atlas, emitters = definition["atlas"], definition["emitters"]
    output = bytearray(struct.pack("<6I", 0x31435450, *_registry(),
                                   len(emitters), atlas["variant_count"], atlas["slot_count"]))

    def track(nodes):
        output.extend(struct.pack("<I", len(nodes)))
        for numerator, denominator, low, high, curve, distribution in nodes:
            output.extend(struct.pack("<dffII", numerator / denominator,
                                      low, high, curve, distribution))

    for emitter in emitters:
        output.extend(struct.pack("<2I3i4I", emitter["offset"], emitter["frames"],
                                  emitter["image_frames"], emitter["animated"], emitter["flags"],
                                  emitter["type"], emitter["name"],
                                  emitter["duration"] & 0xFFFFFFFF, emitter["mask"]))
        for name in SYSTEM_TRACKS:
            track(emitter["tracks"][name])
        for fields in emitter["fields"]:
            output.extend(struct.pack("<I", len(fields)))
            for type_, x, y in fields:
                output.extend(struct.pack("<I", type_))
                track(x)
                track(y)
        for name in PARTICLE_TRACKS:
            track(emitter["tracks"][name])
    pak.require(len(variants) == atlas["variant_count"], "Particle atlas metadata mismatch")
    for metadata, dimensions, rectangles in zip(
            variants, atlas["variant_dimensions"], atlas["variant_rectangles"]):
        width, height = dimensions
        path = metadata["atlas_path"].encode("utf-8")
        pak.require(b"\0" not in path and len(path) <= 4096, "Invalid particle atlas path")
        output.extend(struct.pack("<3I", width, height, len(path)))
        output.extend(path)
        for x, y, w, h in rectangles:
            uv = (x / width, y / height, (x + w) / width, (y + h) / height) if width and height else (0.,) * 4
            output.extend(struct.pack("<4f2i", *uv, w, h))
    return bytes(output)


def generate(config, records: dict[str, bytes], reports: dict | None = None) -> dict:
    order = config.definition_orders.get("particle")
    pak.require(bool(order), "Compiled particles require definition_order(particle ...)")
    runtime = {}
    summary = dict(definitions=len(order), emitters=0, nodes=0, fields=0,
                   variants=0, rectangles=0, rectangle_bytes=0, shards=0)
    expected = set()
    for _, name in order:
        path = f"compiled/particles/{name.casefold()}.compiled"
        pak.require(path not in expected, f"Duplicate compiled particle root: {path}")
        expected.add(path)
        pak.require(path in records, f"Missing compiled particle record: {path}")
        definition = _parse(path, records[path], *_registry())
        atlas = definition["atlas"]
        variants = reports[path].get("variants", [reports[path]]) if reports else [
            {"atlas_path": ""} for _ in range(atlas["variant_count"])]
        runtime[f"compiled/particle-runtime/{name.casefold()}.compiled"] = compiled_record(definition, variants)
        summary["emitters"] += len(definition["emitters"])
        summary["variants"] += atlas["variant_count"]
        summary["rectangles"] += atlas["variant_count"] * atlas["slot_count"]
        for emitter in definition["emitters"]:
            summary["nodes"] += sum(map(len, emitter["tracks"].values()))
            for fields in emitter["fields"]:
                summary["fields"] += len(fields)
                summary["nodes"] += sum(len(x) + len(y) for _, x, y in fields)
    summary["rectangle_bytes"] = summary["rectangles"] * 24
    summary["unindexed_records"] = sorted(path for path in records
                                         if pak.numeric_kind(path) == "particles" and path not in expected)
    return dict(files={}, declarations=[], initializers=[], runtime_records=runtime, summary=summary)
