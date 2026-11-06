"""Build authored definitions, generated C++ and the runtime reference closure."""
from __future__ import annotations
from game_fixture import game_root

import argparse
from contextlib import contextmanager
import hashlib
import io
import json
import math
from pathlib import Path
import struct
import subprocess
import sys
import tempfile
import time
import threading
import zipfile

ROOT = game_root()
sys.path.insert(0, str(Path(__file__).resolve().parents[2]))
import build_numeric_runtime_pak64 as pak
import compile_numeric_fonts as fonts
import compile_numeric_resources as resources
import definition_atlas
import resource_build_config
import exact_atlas
import runtime_asset_sources
import projectile_atlas
import zombatar_atlas
import scene_atlas
import definition_catalog
import static_definitions
import compact_runtime_package
from stage_runtime_package import PARTICLE_COMPILER_FIXTURES


def publish(path: Path, data: bytes) -> None:
    if not path.exists() or path.read_bytes() != data:
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".tmp")
        temporary.write_bytes(data)
        temporary.replace(path)


def emit_definition_image_bindings(path: Path, definitions: dict[str, dict]) -> None:
    """Offline verification metadata; generated loading code uses direct paths."""
    rows = [f"    {{{json.dumps(name)}, {index}u, {json.dumps(row['atlas_path'])}}},"
            for name, report in sorted(definitions.items())
            for index, row in enumerate(report.get("variants", [report]))]
    extents = {}
    for report in definitions.values():
        for row in report.get("variants", [report]):
            if row["atlas_path"]:
                dimensions = (row["atlas_width"], row["atlas_height"])
                if row["atlas_path"] in extents:
                    pak.require(extents[row["atlas_path"]] == dimensions,
                                "Shared static definition atlas dimensions disagree")
                extents[row["atlas_path"]] = dimensions
    extent_rows = [f"    {{{json.dumps(name)}, {width}u, {height}u}},"
                   for name, (width, height) in sorted(extents.items())]
    text = (
        "// Generated offline asset verification metadata; not included by the game.\n"
        "#pragma once\n#include <array>\n#include <cstdint>\n#include <string_view>\n\n"
        "namespace DefinitionImageBindings {\n"
        "struct Binding { std::string_view definition; std::uint32_t variant; std::string_view image; };\n"
        f"inline constexpr std::array<Binding, {len(rows)}> Bindings{{{{\n"
        + "\n".join(rows) + "\n}};\n"
        "struct Extent { std::string_view image; unsigned width,height; };\n"
        f"inline constexpr std::array<Extent, {len(extent_rows)}> Extents{{{{\n"
        + "\n".join(extent_rows) + "\n}};\n}\n"
    )
    publish(path, text.encode("ascii"))


def definition_image_pruning_sources(definition_symbols: dict[str, list[str]],
                                     frame_provenance: dict) -> set[str]:
    """Attribute used split frames to parent symbols solely for global pruning."""
    used = {source.casefold() for sources in definition_symbols.values() for source in sources}
    pak.require(frame_provenance.get("format") == "pvz-particle-frame-images-v1"
                and isinstance(frame_provenance.get("images"), list),
                "Invalid particle frame provenance")
    parents = {}
    for row in frame_provenance["images"]:
        path, source = row["path"], row["source"]
        pak.require(isinstance(path, str) and isinstance(source, str) and source.startswith("IMAGE_"),
                    "Invalid particle frame provenance source")
        key = pak.file_key(path)
        pak.require(key not in parents, f"Duplicate particle frame provenance: {path}")
        parents[key] = source.casefold()
    return used | {parents[source] for source in used if source in parents}


def run(*arguments: object) -> None:
    subprocess.run([str(arg) for arg in arguments], cwd=ROOT, check=True)


def require_publishable_atlas(unit: dict, layout: dict) -> None:
    """Enforce proof or explicitly bounded fallback without relabeling results."""
    settings = unit["settings"]
    exact = layout.get("exact", {})
    area = layout["width"] * layout["height"]
    message = f"Atlas {unit['name']} fails configured solve policy; publication refused"
    bound, gap = exact.get("best_bound"), exact.get("gap")
    pak.require(settings.solver == "exact"
                and exact.get("objective_area") == area
                and exact.get("model_version") == exact_atlas.MODEL_VERSION
                and exact.get("solver_version") == exact_atlas._version()
                and exact.get("solver") == "ortools-cp-sat"
                and type(bound) is int and 0 <= bound <= area
                and type(gap) in (int, float) and math.isfinite(gap)
                and math.isclose(gap, (area - bound) / area if area else 0.0,
                                 rel_tol=1e-12, abs_tol=1e-12),
                message)
    proven = (exact.get("optimal") is True
              and exact.get("status") in {"OPTIMAL", "LOWER_BOUND_PROVEN"} and bound == area)
    unproven = (exact.get("optimal") is False
                and exact.get("status") in {"FEASIBLE", "UNKNOWN"})
    bounded = (type(settings.time_limit_seconds) in (int, float)
               and math.isfinite(settings.time_limit_seconds)
               and 0 < settings.time_limit_seconds <= 86400)
    pak.require(isinstance(settings.require_optimal, bool)
                and (settings.require_optimal or bounded)
                and (proven or (not settings.require_optimal and bounded and unproven)), message)


@contextmanager
def atlas_proof_progress(cache: Path, name: str, index: int, total: int, started: float, *,
                         require_optimal: bool = True, time_limit_seconds: float | None = None):
    """Heartbeat remains observable even during a long proof with no new solution."""
    stopped, lock, state = threading.Event(), threading.Lock(), {}

    def update(exact=None):
        with lock:
            if exact is not None:
                state["exact"] = exact
            publish(cache / "atlas-proof-progress.json", (json.dumps({
                "atlas": name, "index": index, "total": total, "phase": "solving",
                "elapsed_seconds": time.monotonic() - started,
                "exact": state.get("exact"),
                "require_optimal": require_optimal, "time_limit_seconds": time_limit_seconds,
                "publication": "previous package and generated headers retained until all configured atlas policies pass",
            }, indent=2) + "\n").encode())

    def heartbeat():
        while not stopped.wait(30):
            update()

    update()
    thread = threading.Thread(target=heartbeat, name="atlas-proof-progress", daemon=True)
    thread.start()
    try:
        yield update
    finally:
        stopped.set()
        thread.join()


def build(args: argparse.Namespace, work: Path) -> None:
    cache = args.cache.resolve()
    cache.mkdir(parents=True, exist_ok=True)
    game = ROOT / "pvzPerf"
    definitions = ROOT / "assets/definitions"
    config = resource_build_config.load(args.config)
    definition_catalog.validate_runtime_orders(config, ROOT)
    seeded = exact_atlas.seed_completed_cache(
        definitions / "atlas-cache", cache / "atlas-layout-cache")
    if seeded:
        print(f"Seeded {seeded} validated atlas searches from the source cache", flush=True)
    # Published manifests identify completed legacy searches. Live checkpoints
    # have no such evidence and must continue solving, even in bounded mode.
    manifest_path = cache / "manifest.json"
    if manifest_path.exists():
        try:
            previous = json.loads(manifest_path.read_text(encoding="utf-8"))
        except (OSError, ValueError):
            previous = {}
        imported = exact_atlas.import_completed_budget(cache / "atlas-layout-cache", previous)
        if imported:
            print(f"Recovered {imported} completed atlas search budgets from the published manifest",
                  flush=True)
    deferred_headers = {}
    generated = game / "Sexy.TodLib/generated"
    source_files = [p for p in game.rglob("*") if p.suffix in {".cpp", ".h", ".hpp", ".inl"}
                    and "generated" not in p.parts]

    # Named image globals are independent of all offline definition records.
    image_names = resources.read_names(ROOT)
    code_images = resources.code_image_roots(ROOT, image_names)

    reanim = work / "reanim"
    particles = work / "particles"
    reanim_list, particle_list = work / "reanim-inputs.txt", work / "particle-inputs.txt"
    publish(reanim_list, ("\n".join(str(path) for path in config.sources["reanim"]) + "\n").encode())
    publish(particle_list, ("\n".join(str(path) for path in config.sources["particle"]) + "\n").encode())
    type_map = work / "reanim-types.txt"
    publish(type_map, definition_catalog.reanim_type_map(config))
    run(args.compiler, "compile-v2", "--input-list", reanim_list,
        "--output", reanim, "--type-map", type_map,
        "--family-file", ROOT / "assets/definitions/toolchain/reanim_track_families.txt",
        "--font-registry", ROOT / "assets/definitions/toolchain/font_symbol_ids.bin")
    run(args.compiler, "compile-particle-v2", "--input-list", particle_list,
        "--output", particles, "--gold64", definitions / "particle-reference")
    projectile_sources = None
    if config.sources["projectile"]:
        projectile_json = work / "projectiles.json"
        run(args.compiler, "compile-projectile", "--input", config.sources["projectile"][0],
            "--output", projectile_json)
        projectile_sources = json.loads(projectile_json.read_text(encoding="utf-8"))
        projectile_images = projectile_atlas.validate_sources(projectile_sources)
    pak.require(len(config.sources["zombatar"]) == 1,
                "Production resource build requires one combined Zombatar definition")
    zombatar_sources = json.loads(config.sources["zombatar"][0].read_text(encoding="utf-8-sig"))
    zombatar_images = zombatar_atlas.validate_sources(zombatar_sources)
    for name in ("ReanimTrackId.h", "ReanimTrackMphfGlobal.h"):
        deferred_headers[game / "Sexy.TodLib" / name] = (reanim / name).read_bytes()
    deferred_headers[generated / "ParticleEmitterName.h"] = (particles / "ParticleEmitterName.h").read_bytes()
    deferred_headers[generated / "ParticleVariantId.h"] = (particles / "ParticleVariantId.h").read_bytes()
    deferred_headers[generated / "ReanimVariantId.h"] = (reanim / "ReanimVariantId.h").read_bytes()

    strings = work / "strings"
    run(args.compiler, "bake-strings", "--input", definitions / "LawnStrings.txt",
        "--output-dir", strings)
    for name in ("LawnStrings.h", "LawnStrings.cpp"):
        deferred_headers[generated / name] = (strings / name).read_bytes()

    records = {}
    source_names = {}
    for kind, directory in (("reanim", reanim), ("particles", particles)):
        source_image_names = (directory / "image_sources.txt").read_text(encoding="utf-8").splitlines()
        pak.require(len(set(source_image_names)) == len(source_image_names) and all(source_image_names),
                    f"Invalid compiler source image names: {kind}")
        source_names[kind] = dict(enumerate(source_image_names, 1))
        for path in directory.glob("*.source-record"):
            name = path.stem.casefold() + ".compiled"
            if name in PARTICLE_COMPILER_FIXTURES:
                continue
            records[f"compiled/{kind}/{name}"] = path.read_bytes()
    if projectile_sources is not None:
        records[projectile_atlas.PATH] = b""  # Geometry is written after the shared atlas solve.
    inheritance = json.loads((particles / "particle_inheritance.json").read_text(encoding="utf-8"))
    particle_metadata = json.loads((particles / "particle_variants.json").read_text(encoding="utf-8"))["roots"]
    reanim_metadata = json.loads((reanim / "reanim_variants.json").read_text(encoding="utf-8"))["roots"]
    action_path = definitions / "reanim-variant-actions.json"
    reanim_actions = json.loads(action_path.read_text(encoding="utf-8")) if action_path.exists() else {}
    virtual_variants = {}
    for root, metadata in reanim_metadata.items():
        for variant in metadata["variants"]:
            path = f"compiled/reanim/{variant['name'].casefold()}.compiled"
            pak.require(path not in virtual_variants, "Duplicate reanim appearance name")
            virtual_variants[path] = (root, variant)
    pak.require(isinstance(inheritance, dict) and all(
        pak.canonical_particle_path(child) and pak.canonical_particle_path(parent)
        and child in records and parent in records for child, parent in inheritance.items()),
        "Invalid compiler particle inheritance sidecar")
    font_corpus = fonts.load_corpus(definitions / "fonts")
    tag_ids = fonts.tag_registry(font_corpus)
    for font in font_corpus:
        records[fonts.resolve_compiled_font_path(font.source_path).casefold()] = font.encode(tag_ids)

    raw_files = runtime_asset_sources.inventory(ROOT)
    scene_documents, scene_sources = scene_atlas.prepare(
        [scene_atlas.parse(path.read_text(encoding="utf-8-sig")) for path in config.sources["scene"]]
        + [scene_atlas.common_ui()], (definitions / "resources.json").read_bytes(), raw_files)
    pak.require({row["name"] for row in scene_documents.values()} == set(scene_atlas.MODULES),
                "Scene build requires authored Magnet, Almanac and Achievements atlases")
    resource_data, load_plan, crc, names = resources.compile_manifest(
        ROOT, (definitions / "resources.json").read_bytes(),
        list(raw_files),
        image_names, tag_ids)
    source_images = definition_atlas.authoring_descriptors(
        (definitions / "resources.json").read_bytes(), raw_files,
        {name for names in source_names.values() for name in names.values()}
        | (set(projectile_images) if projectile_sources is not None else set()))
    # Explicit projectile PNG paths retain the original loader's automatic alpha
    # composition. They never resolve through the program's image globals.
    for image in projectile_images if projectile_sources is not None else ():
        source_images[image] = definition_atlas.ImageDescriptor(
            image, image, "", "", resources.AUTO_ALPHA, 1, 1, 0xFFFFFF)
    source_images.update(zombatar_atlas.descriptors(zombatar_sources))
    source_images.update(scene_sources)
    scene_reports = {}
    atlas_report = {}
    definition_names = sorted({name for name in records
                               if pak.numeric_kind(name) in ("reanim", "particles", "projectiles")}
                              | set(virtual_variants) | {zombatar_atlas.PATH} | set(scene_documents))
    dimensions, definition_symbols = {}, {}
    for name in definition_names:
        if name in scene_documents:
            symbols = sorted({row["source"] for row in scene_documents[name]["sprites"]})
            definition_symbols[name] = symbols
            for symbol in symbols:
                if symbol not in dimensions:
                    image, _ = definition_atlas.logical_image(source_images[symbol], raw_files)
                    dimensions[symbol] = image.size
            continue
        if name == zombatar_atlas.PATH:
            definition_symbols[name] = zombatar_images
            for symbol in zombatar_images:
                if symbol not in dimensions:
                    image, _ = definition_atlas.logical_image(source_images[symbol], raw_files)
                    dimensions[symbol] = image.size
            continue
        if name == projectile_atlas.PATH:
            definition_symbols[name] = projectile_images
            for symbol in projectile_images:
                if symbol not in dimensions:
                    image, _ = definition_atlas.logical_image(source_images[symbol], raw_files)
                    dimensions[symbol] = image.size
            continue
        if name in virtual_variants:
            _, variant = virtual_variants[name]
            symbols = sorted({label for track in variant["images"] for label in track if label is not None})
            for symbol in symbols:
                if symbol not in dimensions:
                    image, _ = definition_atlas.logical_image(source_images[symbol], raw_files)
                    dimensions[symbol] = image.size
            definition_symbols[name] = symbols
            continue
        body = records[name][12:]
        offsets = definition_atlas.image_offsets(pak.numeric_kind(name), body)
        handles = source_names[pak.numeric_kind(name)]
        referenced = {struct.unpack_from("<I", body, offset)[0] for offset in offsets} - {0}
        pak.require(referenced <= set(handles), f"Unknown compiler source image handle: {name}")
        # Particle offsets enumerate the ordered image list. Atlas packing owns
        # each source once, while compile_definition preserves repeated frames.
        symbols = sorted({handles[index] for index in referenced})
        for symbol in symbols:
            if symbol not in dimensions:
                image, _ = definition_atlas.logical_image(source_images[symbol], raw_files)
                dimensions[symbol] = image.size
        definition_symbols[name] = symbols
    grouped = set()
    units = []
    for group, members in resource_build_config.resolve_atlases(config, set(definition_names)).items():
        grouped.update(members)
        units.append({"name": group, "path": f"compiled/atlases/{group}.atlas.png",
                      "members": members, "settings": config.settings.get(group, config.defaults)})
    common_ui = scene_atlas.record_path("CommonUI")
    if common_ui not in grouped:
        pak.require(not any(unit["name"] == "common_ui" for unit in units),
                    "The common_ui atlas group name is reserved for built-in CommonUI")
        grouped.add(common_ui)
        units.append({"name": "common_ui", "path": "compiled/atlases/common_ui.atlas.png",
                      "members": [common_ui], "settings": config.defaults})
    units.extend({"name": name, "path": "", "members": [name], "settings": config.defaults}
                 for name in definition_names if name not in grouped)
    for unit in units:
        unit["symbols"] = sorted({symbol for name in unit["members"] for symbol in definition_symbols[name]})
    batches = [[dimensions[symbol] for symbol in unit["symbols"]] for unit in units]
    layouts = definition_atlas.native_layouts(args.compiler, batches)
    proof_started = time.monotonic()
    progress_path = cache / "atlas-proof-progress.json"
    for index, (unit, sizes) in enumerate(zip(units, batches)):
        settings = unit["settings"]
        print(f"Solving atlas {index + 1}/{len(units)}: {unit['name']} "
              f"({len(sizes)} images, time limit={settings.time_limit_seconds or 'unlimited'}, "
              f"proof required={settings.require_optimal})", flush=True)

        try:
            pak.require(settings.solver == "exact",
                        "Production atlases require the exact solver; MaxRects is only an incumbent")
            with atlas_proof_progress(
                    cache, unit["name"], index + 1, len(units), proof_started,
                    require_optimal=settings.require_optimal,
                    time_limit_seconds=settings.time_limit_seconds) as progress:
                layouts[index] = exact_atlas.optimize_layouts(
                    [sizes], [layouts[index]], max_side=settings.max_side, padding=settings.padding,
                    time_limit_seconds=settings.time_limit_seconds,
                    cache=cache / "atlas-layout-cache", workers=settings.workers,
                    require_optimal=settings.require_optimal, progress=progress)[0]
            exact_atlas._validate(sizes, layouts[index], settings.padding, settings.max_side)
            require_publishable_atlas(unit, layouts[index])
            result = layouts[index]["exact"]
            print(f"Atlas result {unit['name']}: {result['status']}, "
                  f"area={result['objective_area']}, bound={result['best_bound']}, "
                  f"gap={result['gap']:.6%}, optimal={result['optimal']}", flush=True)
        except BaseException as error:
            publish(progress_path, (json.dumps({
                "atlas": unit["name"], "index": index + 1, "total": len(units),
                "phase": "interrupted" if isinstance(error, KeyboardInterrupt) else "failed",
                "error": str(error), "elapsed_seconds": time.monotonic() - proof_started,
                "publication": "previous package and generated headers retained",
            }, indent=2) + "\n").encode())
            raise
        pak.require(layouts[index]["width"] <= settings.max_side and layouts[index]["height"] <= settings.max_side,
                    f"Atlas {unit['name']} exceeds configured dimension limit")
    # All atlas layouts must pass their configured policy before publication.
    # Checkpoint caches/progress are the only durable writes during solving.
    for path, data in deferred_headers.items():
        publish(path, data)
    variant_layouts, variant_reports, image_metadata = {}, {}, {}
    for unit, layout in zip(units, layouts):
        path = unit["path"] or unit["members"][0].removesuffix(".compiled") + ".atlas.png"
        if unit["symbols"]:
            packed, _ = definition_atlas.pack(
                [definition_atlas.logical_image(source_images[symbol], raw_files)[0]
                 for symbol in unit["symbols"]], layout=layout)
            buffer = io.BytesIO()
            packed.save(buffer, format="PNG", compress_level=9)
            records[path] = buffer.getvalue()
        for name in unit["members"]:
            if name in scene_documents:
                scene_reports[name] = scene_atlas.compile_definition(
                    scene_documents[name], source_images, raw_files, layout, unit["symbols"], path)
                continue
            if name == zombatar_atlas.PATH:
                atlas_report[name] = zombatar_atlas.compile_definition(
                    zombatar_sources, source_images, raw_files, layout, unit["symbols"], path)
                continue
            if name == projectile_atlas.PATH:
                records[name], atlas_report[name] = projectile_atlas.compile_definition(
                    projectile_sources, source_images, raw_files, layout,
                    unit["symbols"], path)
                continue
            if name in virtual_variants:
                symbols = definition_symbols[name]
                regions = dict(zip(unit["symbols"], layout["regions"]))
                images, used = [], set()
                for symbol in symbols:
                    if symbol not in image_metadata:
                        image, dependencies = definition_atlas.logical_image(source_images[symbol], raw_files)
                        descriptor = source_images[symbol]
                        image_metadata[symbol] = {
                            "source": symbol, "width": image.width, "height": image.height,
                            "rows": descriptor.rows, "cols": descriptor.cols,
                            "rgba_bytes": image.width * image.height * 4,
                            "rgba_sha256": hashlib.sha256(image.tobytes()).hexdigest(),
                            "files": sorted(dependencies)}
                    row = dict(image_metadata[symbol])
                    row["region"] = (*regions[symbol], row["rows"], row["cols"])
                    images.append(row)
                    used.update(row["files"])
                variant_layouts[name] = {"layout": layout, "symbols": unit["symbols"]}
                variant_reports[name] = {
                    "name": virtual_variants[name][1]["name"],
                    "atlas_path": path if symbols else "",
                    "atlas_width": layout["width"] if symbols else 0,
                    "atlas_height": layout["height"] if symbols else 0,
                    "atlas_shared": bool(unit["path"]) and bool(symbols),
                    "region_count": len(images), "images": images,
                    "source_files": sorted(used), "frames": []}
                continue
            records[name], png, atlas_report[name] = definition_atlas.compile_definition(
                name, records[name], source_images, raw_files, layout=layout,
                atlas_symbols=unit["symbols"], atlas_path=unit["path"],
                source_names=source_names[pak.numeric_kind(name)],
                emit_png=False)
    for root, metadata in reanim_metadata.items():
        name = f"compiled/reanim/{root.casefold()}.compiled"
        members = [f"compiled/reanim/{row['name'].casefold()}.compiled" for row in metadata["variants"]]
        records[name], summary = definition_atlas.compile_reanim_variants(
            name, records[name], metadata, [variant_layouts[member] for member in members],
            source_names["reanim"], source_images, reanim_actions.get(root))
        reports = [variant_reports[member] for member in members]
        atlas_report[name] = {**reports[0], **summary, "variants": reports}
    for name, metadata in particle_metadata.items():
        if name not in records:  # Compiler-only structural fixtures.
            continue
        rows, reports = [], []
        for variant in metadata["variants"]:
            member = variant["path"]
            parsed = pak.parse_definition_atlas(member, records[member])
            rows.append({"width": parsed["width"], "height": parsed["height"],
                         "frame_counts": parsed["frame_counts"], "rectangles": parsed["frame_fields4I"]})
            reports.append({**atlas_report[member], "name": variant["name"], "path": member})
        records[name], summary = definition_atlas.compile_particle_variants(name, records[name], rows)
        atlas_report[name] = {**reports[0], **summary, "variants": reports,
                              "variant_names": [row["name"] for row in metadata["variants"]]}
        for variant in metadata["variants"][1:]:
            del records[variant["path"]]
            del atlas_report[variant["path"]]
    # Particle behavior/appearance and GPU reanimation transport are runtime binaries.
    gpu_records = {}
    native_files, native_summary = static_definitions.generate(
        config, records, atlas_report, runtime_records=gpu_records)
    zombatar_report = atlas_report[zombatar_atlas.PATH]
    zombatar_files = zombatar_atlas.generate(zombatar_sources, zombatar_report)
    native_files.update(zombatar_files)
    scene_files = scene_atlas.generate(scene_reports)
    native_files.update(scene_files)
    native_summary["zombatar_images"] = len(zombatar_report["images"])
    native_summary["atlases"] = sorted(set(native_summary["atlases"]) | {zombatar_report["atlas_path"]}
                                      | {row["atlas_path"] for row in scene_reports.values()})
    native_summary["generated_files"].update({
        name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
        for name, data in {**zombatar_files, **scene_files}.items()
    })
    for filename, data in native_files.items():
        publish(generated / filename, data)
    # Retire obsolete generated particle sources so source discovery cannot keep
    # embedding the previous corpus alongside the runtime records.
    for obsolete in (generated / "definitions").glob("ParticleData*.cpp"):
        obsolete.unlink()
    runtime_roots = {
        f"compiled/{'reanim' if kind == 'reanim' else 'particles'}/{name.casefold()}.compiled"
        for kind in ("reanim", "particle") for _, name in config.definition_orders[kind]
    } | {projectile_atlas.PATH, zombatar_atlas.PATH}
    atlas_report = {name: row for name, row in atlas_report.items() if name in runtime_roots}
    for kind in ("reanim", "particle"):
        directory = "reanim" if kind == "reanim" else "particles"
        for index, (_, definition) in enumerate(config.definition_orders[kind]):
            name = f"compiled/{directory}/{definition.casefold()}.compiled"
            parsed = pak.parse_definition_atlas(name, records[name])
            atlas_report[name]["definition_id"] = index
            atlas_report[name]["slot_count"] = parsed["slot_count"]
            for row, rectangles in zip(atlas_report[name]["variants"], parsed["variant_rectangles"]):
                row["frame_rectangles"] = rectangles
    emit_definition_image_bindings(generated / "DefinitionImageBindings.h", atlas_report)
    retained_roots = resources.production_image_roots(image_names, code_images)
    # Definition source labels and their atlas closure never retain global
    # image bindings. Only actual named C++ consumers produce load records.
    definition_sources = definition_image_pruning_sources(
        definition_symbols, json.loads(
            (definitions / "particle-frame-images.json").read_text(encoding="utf-8")))
    retired_images = set(image_names) - retained_roots
    records[pak.RESOURCE_PATH] = resource_data
    # The generator owns these source files; unchanged content keeps its mtime.
    resources.emit_headers(ROOT, load_plan, crc, names)
    run(sys.executable, "-B", ROOT / "tools/gen_reanim_attacher_masks.py")

    offline_records = {name: data for name, data in records.items()
                       if pak.numeric_kind(name) in ("reanim", "particles", "projectiles")}
    for name in offline_records:
        del records[name]
    records.update(gpu_records)
    runtime_atlases = set(native_summary["atlases"])
    for name in list(records):
        if name.endswith(".atlas.png") and name not in runtime_atlases:
            del records[name]
    expected = pak.expected_counts(ROOT)
    counts = {kind: sum(pak.numeric_kind(name) == kind for name in records) for kind in expected}
    if counts != expected:
        raise ValueError(f"Incomplete runtime resource corpus: {counts}")
    audit = pak.source_audit(ROOT, {}, set(records))
    references = set()
    # Validate the entire offline definition corpus before discarding it.
    for name, data in {**records, **offline_records}.items():
        if pak.numeric_kind(name) is None:
            continue
        record_references = references if name in records or name in runtime_roots else set()
        pak.validate_numeric(name, data, audit["registries"], record_references,
                             atlas_paths=[row["atlas_path"] for row in atlas_report[name]["variants"]]
                             if name in atlas_report and "variants" in atlas_report[name] else None,
                             atlas_path=atlas_report[name]["atlas_path"] if name in atlas_report else None)
    pak.validate_particle_inheritance(offline_records)
    references.update(("atlas", name) for name in runtime_atlases)
    selected = set()
    available = set(raw_files) | set(records)
    for kind, name in references:
        selected.update(pak.physical_references(kind, name, available))
    for source in source_files:
        for _, value in pak.literals(pak.clean_comments(source.read_text(encoding="utf-8-sig"))):
            normalized = value.replace("\\", "/").casefold()
            if normalized in available:
                selected.add(normalized)
            elif normalized.startswith(("images/", "reanim/", "particles/", "data/")):
                path = Path(normalized)
                stems = (normalized, path.with_name("_" + path.name).as_posix(), normalized + "_")
                if any(stem + suffix in available for stem in stems
                       for suffix in (".png", ".jpg", ".gif", ".tga")):
                    selected.update(pak.physical_references("image", normalized, available))
            elif normalized.startswith("sounds/") and any(
                    normalized + suffix in available for suffix in (".wav", ".ogg", ".au")):
                selected.update(pak.physical_references("sound", normalized, available))
    # Resource descriptions are compile inputs. Runtime UI terms remain package data.
    for name in available:
        if Path(name).suffix.casefold() not in {".png", ".jpg", ".jpeg", ".gif", ".bmp",
                                              ".ogg", ".wav", ".au", ".mo3"}:
            if name != "properties/lawnstrings.txt":
                selected.add(name)
    for name in sorted(selected):
        if name in raw_files:
            records[name] = raw_files[name].read_bytes()
    output = cache / "main.pak64"
    temporary = output.with_suffix(".tmp")
    with zipfile.ZipFile(temporary, "w", compression=zipfile.ZIP_STORED) as archive:
        for name, data in sorted(records.items()):
            info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
            info.create_system, info.external_attr = 0, 0x20
            archive.writestr(info, data)
    temporary.replace(output)
    image_occurrences = [image for definition in {**atlas_report, **scene_reports}.values()
                         for row in definition.get("variants", [definition]) for image in row["images"]]
    unique_pixels = {(image["rgba_sha256"], image["width"], image["height"]): image["rgba_bytes"]
                     for image in image_occurrences}
    global_overlap = {(image["rgba_sha256"], image["width"], image["height"]): image["rgba_bytes"]
                      for image in image_occurrences
                      if image["source"] in retained_roots}
    used_units = [(unit, layout, sizes) for unit, layout, sizes in zip(units, layouts, batches)
                  if (unit["path"] or unit["members"][0].removesuffix(".compiled") + ".atlas.png")
                  in runtime_atlases]
    source_bytes = sum(dimensions[symbol][0] * dimensions[symbol][1] * 4
                       for unit, _, _ in used_units for symbol in unit["symbols"])
    atlas_bytes = sum(layout["width"] * layout["height"] * 4 for _, layout, _ in used_units)
    padded_bytes = sum((width + 4) * (height + 4) * 4
                       for _, _, sizes in used_units for width, height in sizes)
    report = {
        "format": "pvz-resource-build-static-v1",
        "definition_source_format": "IDTF-IDRF-float-v1",
        "numeric_counts": counts,
        "code_images": sorted(code_images),
        "retained_program_images": sorted(retained_roots),
        "runtime_inputs": len(raw_files),
        "runtime_included": len(set(raw_files) & selected),
        "runtime_omitted": sorted(set(raw_files) - selected),
        "retired_images": sorted(retired_images),
        "definition_atlases": atlas_report,
        "scene_atlases": scene_reports,
        "static_definitions": native_summary,
        "build_config": str(config.path.relative_to(ROOT)),
        "atlas_groups": {unit["name"]: {"members": unit["members"],
                                      "path": unit["path"], "images": len(unit["symbols"]),
                                      "width": layout["width"], "height": layout["height"],
                                      "solver": unit["settings"].solver,
                                      "time_limit_seconds": unit["settings"].time_limit_seconds,
                                      "require_optimal": unit["settings"].require_optimal,
                                      "exact": layout.get("exact")}
                         for unit, layout, _ in used_units},
        "atlas_memory": {
            "rgba_bytes": atlas_bytes, "source_occurrence_rgba_bytes": source_bytes,
            "unique_source_rgba_bytes": sum(unique_pixels.values()),
            "duplicate_source_rgba_bytes": source_bytes - sum(unique_pixels.values()),
            "source_pixels_also_retained_globally_rgba_bytes": sum(global_overlap.values()),
            "padding_and_unused_rgba_bytes": atlas_bytes - source_bytes,
            "extrusion_rgba_bytes": padded_bytes - source_bytes,
            "unused_gap_rgba_bytes": atlas_bytes - padded_bytes,
            "padded_region_occupancy": padded_bytes / atlas_bytes if atlas_bytes else 1.0,
            "definition_atlas_count": len(runtime_atlases),
            "packing_shelf_rgba_bytes": sum(layout["shelf_area"] * 4 for _, layout, _ in used_units),
            "packing_saved_rgba_bytes": sum(layout["shelf_area"] * 4 for _, layout, _ in used_units) - atlas_bytes,
            "packing_search_attempts": sum(layout["search_attempts"] for _, layout, _ in used_units),
            "packing_search_microseconds": sum(layout["search_microseconds"] for _, layout, _ in used_units),
        },
        "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                  for name, data in sorted(records.items())},
    }
    publish(cache / "manifest.json", (json.dumps(report, indent=2) + "\n").encode())
    (cache / "resources.stamp").write_text("complete\n", encoding="ascii")
    publish(progress_path, (json.dumps({
        "phase": "published", "total": len(units),
        "elapsed_seconds": time.monotonic() - proof_started,
        "publication": "all configured atlas policies passed",
        "unproven_atlases": [unit["name"] for unit, layout in zip(units, layouts)
                             if not layout["exact"]["optimal"]],
    }, indent=2) + "\n").encode())
    print(f"Resource package: {output}; {len(records)} entries; omitted {len(set(raw_files) - selected)} inputs")


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--compiler", type=Path, required=True)
    parser.add_argument("--cache", type=Path, required=True)
    parser.add_argument("--config", type=Path, default=ROOT / "assets/definitions/ResourceBuild.pvzbuild")
    parser.add_argument("--compact", choices=("lossless", "light-lossy"),
                        help="Also emit a separate main-compact.pak64; normal package stays unchanged")
    parser.add_argument("--compact-output", type=Path)
    parser.add_argument("--compact-workers", type=int, default=2)
    args = parser.parse_args()
    args.cache = args.cache.resolve()
    args.cache.mkdir(parents=True, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="compile-", dir=args.cache) as temporary:
        build(args, Path(temporary))
    if args.compact:
        report = compact_runtime_package.compact_package(
            args.cache / "main.pak64", args.compact_output or args.cache / "main-compact.pak64",
            args.compact, args.compact_workers)
        print(f"Compact package: {report['output']}; {report['source_bytes']} -> {report['output_bytes']} bytes")


if __name__ == "__main__":
    main()
