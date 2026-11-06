"""Turn final offline RAV7 records into aggregate C++ definition storage."""
from __future__ import annotations
from game_fixture import game_root

import math
import struct

import build_numeric_runtime_pak64 as pak
from static_definition_codegen import array, cpp_float, cpp_string, texture_region

SHARD_COUNT = 8
_DEFAULTS = (0.0, 0.0, 0.0, 0.0, 1.0, 1.0, 0.0, 1.0)


def _parse(path: str, data: bytes, type_count: int) -> dict:
    atlas = pak.parse_definition_atlas(path, data)
    pak.require(atlas is not None and "action_keys" in atlas,
                f"Static reanimation requires RAV7: {path}")
    reader = pak.Reader(data[atlas["body_offset"]:atlas["appearance_offset"]])
    authored = data[atlas["body_offset"]:][:4] == b"IDTF"
    magic, _, image, _, fps, count = reader.unpack("<4IfI")
    pak.require(magic in (0x36544449, 0x46544449) and image == 0 and math.isfinite(fps) and fps > 0
                and 0 < count <= 1 << 20, "Invalid static IDT6 header")
    tracks = []
    for _ in range(count):
        track, frames, stride, flags = reader.unpack("<4I")
        mode, storage = pak.reanim_track_flags(flags, authored=authored)
        pak.require(stride == 32 and 0 < frames <= 1 << 20,
                    "Invalid static IDT6 track")
        previous = _DEFAULTS
        transforms = []
        for _ in range(frames):
            values = reader.unpack("<6fif" if authored else "<8f")
            pak.require(all(math.isfinite(value) for value in values), "Nonfinite IDT6 transform")
            previous = tuple(old if value == -10000 else value
                             for i, (old, value) in enumerate(zip(previous, values)))
            pak.require(previous[6] == int(previous[6]), "Noninteger reanim cel")
            transforms.append({"values": (*previous[:6], int(previous[6]), previous[7])})
        tracks.append({"id": track, "mode": mode, "storage": storage, "frames": transforms})
    for track in tracks:
        previous_font, previous_text = 0, None
        for frame in track["frames"]:
            slot, font = reader.unpack("<2I")
            pak.require(slot < atlas["slot_count"] and font <= 1, "Invalid IDT6 resource ID")
            if font:
                previous_font = font
            if track["mode"]:
                text = reader.take(reader.count(limit=0x7FFFFFFE))
                pak.require(b"\0" not in text, "NUL in IDT6 UI caption")
                if text:
                    try:
                        previous_text = text.decode("utf-8")
                    except UnicodeDecodeError as error:
                        raise ValueError("Invalid UTF-8 IDT6 UI caption") from error
            frame.update(slot=slot, font=previous_font, text=previous_text)
    segments = []
    previous_track = -1
    for _ in range(reader.count(8, count)):
        index, segment_count = reader.unpack("<2I")
        pak.require(previous_track < index < count, "Invalid IDT6 attacher track index")
        previous_track = index
        track = tracks[index]
        frames = len(track["frames"])
        pak.require(track["mode"] == 0 and segment_count <= frames,
                    "Invalid IDT6 attacher track or count")
        previous_end = 0
        for _ in range(segment_count):
            start, end, target, action, rate, loop, run_start, run_end = reader.unpack("<4If3I")
            pak.require(previous_end <= start < end <= frames and run_start <= start
                        and end <= run_end <= frames and run_start < run_end
                        and (target == 0xFFFFFFFF or target < type_count)
                        and math.isfinite(rate) and loop <= 2, "Invalid IDT6 attacher segment")
            previous_end = end
            segments.append((index, start, end, target, action, rate, loop, run_start, run_end))
    reader.finish()
    return {"atlas": atlas, "fps": fps, "tracks": tracks, "segments": segments}


def _frame_range(definition: dict, action: int) -> tuple[int, int]:
    # MPHF maps repeated IDs to their first authored track.
    track = next((track for track in definition["tracks"] if track["id"] == action), None)
    pak.require(track is not None, f"Attacher action track missing in target definition: {action}")
    visible = [i for i, frame in enumerate(track["frames"]) if frame["values"][6] >= 0.0]
    return (visible[0], visible[-1] - visible[0] + 1) if visible else (0, 1)


def generate(config, records: dict[str, bytes]) -> dict:
    """Return eight deterministic source shards and central-table fragments."""
    order = config.definition_orders.get("reanim")
    pak.require(bool(order), "Static reanimations require definition_order(reanim ...)")
    definitions = []
    expected = []
    for _, name in order:
        path = f"compiled/reanim/{name.casefold()}.compiled"
        pak.require(path in records, f"Missing static reanimation record: {path}")
        expected.append(path)
        definitions.append(_parse(path, records[path], len(order)))
    shards = [[
        '#include "../GeneratedDefinitions.h"',
        '#include "../GeneratedDefinitionStorage.h"',
        "",
        "namespace GeneratedDefinitions {",
    ] for _ in range(SHARD_COUNT)]
    declarations, initializers, font_bindings, font_resets, attacher_initializers = [], [], [], [], []
    summary = {"definitions": len(order), "tracks": 0, "frames": 0, "variants": 0,
               "rectangles": 0, "attacher_segments": 0, "font_frames": 0,
               "storage_tracks": {"both": 0, "cpu": 0, "gpu": 0},
               "unindexed_records": sorted(name for name in records
                                            if pak.numeric_kind(name) == "reanim" and name not in expected)}
    for index, definition in enumerate(definitions):
        code = shards[index % SHARD_COUNT]
        prefix = f"Reanim{index}"
        atlas, tracks = definition["atlas"], definition["tracks"]

        def emit(type_name, suffix, values, *, readonly=False):
            return array(declarations, code, type_name, prefix + suffix, values, readonly=readonly)

        track_values = []
        for t, track in enumerate(tracks):
            storage = track["storage"]
            summary["storage_tracks"][("both", "cpu", "gpu")[storage]] += 1
            values = []
            for f, frame in enumerate(track["frames"]):
                text = "nullptr" if frame["text"] is None else cpp_string(frame["text"])
                values.append("{" + ", ".join([*(str(v) if i == 6 else cpp_float(v)
                                                for i, v in enumerate(frame["values"])),
                                              f'{frame["slot"]}u', "nullptr", text]) + "}")
                if frame["font"]:
                    target = f"{prefix}Frames{t}[{f}].mFont"
                    font_bindings.append(f"{target} = font;")
                    font_resets.append(f"{target} = nullptr;")
            frames_name = emit("ReanimatorSourceTransform", f"Frames{t}", values)
            track_values.append(f'{{static_cast<ReanimTrackId>({track["id"]}u), '
                                f'{{{frames_name}, {len(values)}}}}}')
            summary["frames"] += len(values)
        tracks_name = emit("ReanimatorTrack", "Tracks", track_values)
        variants, slots, actions = atlas["variant_count"], atlas["slot_count"], atlas["action_count"]
        textures = emit("Sexy::GpuTexture*", "Textures", ["nullptr"] * variants)
        rects = emit("Sexy::TextureRegion", "Rects", [
            texture_region(rect, *dimensions)
            for row, dimensions in zip(atlas["variant_rectangles"], atlas["variant_dimensions"])
            for rect in row], readonly=True)
        transitions = emit("std::uint32_t", "Transitions", [f"{v}u" for v in atlas["transitions"]])
        starts, counts = [0] * len(tracks), [0] * len(tracks)
        options = [0] * (variants * len(tracks))
        first_tracks = {}
        for t, track in enumerate(tracks):
            first_tracks.setdefault(track["id"], t)
        for a, (track_id, option) in enumerate(atlas["action_keys"]):
            t = first_tracks[track_id]
            if counts[t] == 0:
                starts[t] = a
            counts[t] += 1
            for v in range(variants):
                if atlas["transitions"][v * actions + a] == v:
                    options[v * len(tracks) + t] = option
        starts_name = emit("std::uint32_t", "ActionStarts", [f"{v}u" for v in starts])
        counts_name = emit("std::uint32_t", "ActionCounts", [f"{v}u" for v in counts])
        options_name = emit("std::uint32_t", "Options", [f"{v}u" for v in options])
        initializers.append(
            "{" + ", ".join([f"{{{tracks_name}, {len(tracks)}}}", cpp_float(definition["fps"]),
                             textures, f"{variants}u", f"{slots}u", f"{actions}u",
                             rects, transitions, starts_name, counts_name, options_name]) + "}")
        segment_values = []
        for segment in definition["segments"]:
            target, action = segment[3:5]
            cache = _frame_range(definitions[target], action) if target != 0xFFFFFFFF and action else (-1, -1)
            fields = [cpp_float(v) if i == 5 else f"{v}u" for i, v in enumerate(segment)]
            segment_values.append("{" + ", ".join([*fields, *(str(v) for v in cache)]) + "}")
        segments_name = emit("AttacherSegmentRT", "Segments", segment_values)
        attacher_initializers.append(f"{{{segments_name}, {len(segment_values)}}}" if segment_values else "{}")
        summary["tracks"] += len(tracks)
        summary["variants"] += variants
        summary["rectangles"] += variants * slots
        summary["attacher_segments"] += len(segment_values)
    summary["font_frames"] = len(font_bindings)
    summary["rectangle_bytes"] = summary["rectangles"] * 24
    files = {}
    for index, code in enumerate(shards):
        code.append("}")
        files[f"ReanimData{index:02d}.cpp"] = ("\n".join(code) + "\n").encode("utf-8")
    return {"files": files, "declarations": declarations, "initializers": initializers,
            "font_bindings": font_bindings, "font_resets": font_resets,
            "attacher_initializers": attacher_initializers, "summary": summary}
