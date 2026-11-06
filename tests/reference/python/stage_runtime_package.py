#!/usr/bin/env python3
"""Stage production numeric assets from a package or an existing compiler stage.

This is an explicit offline rename, never a runtime compatibility route.
Inputs and existing output directories are never modified. Built-in shader
binaries belong to the CMake build output, outside the staged package tree.
"""

from __future__ import annotations
from game_fixture import game_root

import argparse
from collections import Counter
import json
import os
from pathlib import Path
import tempfile
import zipfile

import build_numeric_runtime_pak64 as pak

PARTICLE_COMPILER_FIXTURES = {
    name.casefold() + ".compiled" for name in (
        "ArtChallengePlant", "CircleEvenSpacing", "KernelSplat",
        "PerfTest", "SeedPacketPickup", "SimpleParticle",
    )
}


def compiler_records(directory: Path, kind: str) -> dict[str, Path]:
    pak.require(kind in ("reanim", "particles") and directory.is_dir(),
                f"Missing compiler {kind} output: {directory}")
    pak.require(not directory.is_symlink() and
                not getattr(directory, "is_junction", lambda: False)(),
                f"Linked compiler output: {directory}")
    records = {}
    gold_suffix = ".reanim.v2.compiled" if kind == "reanim" else ".xml.v2.compiled"
    for path in sorted(directory.rglob("*")):
        pak.require(not path.is_symlink() and not getattr(path, "is_junction", lambda: False)(),
                    f"Linked compiler output: {path}")
        if not path.is_file() or path.suffix.casefold() != ".compiled":
            continue
        if path.name.endswith(gold_suffix):
            continue
        pak.require(path.parent == directory and
                    pak.numeric_kind(f"compiled/{kind}/{path.name}") == kind,
                    f"Compiler must emit <stem>.compiled directly: {path}")
        normalized = pak.file_key(path.name)
        pak.require(normalized not in records, f"Compiler output collision: {path}")
        records[normalized] = path
    pak.require(bool(records), f"Empty numeric compiler output: {directory}")
    return records


def stage_runtime(source: Path, output: Path,
                  replacements: dict[str, Path] | None = None, *, root: Path = pak.ROOT) -> dict:
    source, output = Path(source).resolve(), Path(output).absolute()
    pak.require(not os.path.lexists(output), f"Choose a fresh staging directory: {output}")
    pak.require(output.parent.is_dir(), f"Missing output parent: {output.parent}")
    pak.require(source.is_file() or source.is_dir(), f"Missing staging input: {source}")
    pak.require(not source.is_dir() or not output.resolve().is_relative_to(source),
                "Staging output must be outside the input tree")
    replacements = replacements or {}
    overlays = {kind: compiler_records(Path(directory), kind)
                for kind, directory in replacements.items()}
    used = {kind: set() for kind in overlays}
    rows, omitted, counts, names = [], [], Counter(), set()
    with tempfile.TemporaryDirectory(prefix=".runtime-stage-", dir=output.parent) as temporary:
        target = Path(temporary) / "stage"
        target.mkdir()

        def emit(name: str, data: bytes) -> None:
            source_hash = pak.digest(data)
            rebuilt_from = None
            normalized = pak.file_key(name)
            if pak.retired_effect_binary(normalized):
                omitted.append({"name": name, "reason": "retired_scale_binary",
                                "sha256": source_hash, "bytes": len(data)})
                return
            mirror = pak.matching_external_dds(normalized, data, root)
            if mirror:
                omitted.append({"name": name, "reason": "external_scale_dds",
                                "external": str(mirror.resolve()),
                                "sha256": source_hash, "bytes": len(data)})
                return
            migrated = pak.migrated_numeric_path(normalized)
            if migrated:
                kind = pak.numeric_kind(migrated)
                # Preserve spelling for files retained verbatim in the source pak.
                if normalized == migrated:
                    spelling = name
                elif kind == "resources":
                    spelling = pak.RESOURCE_PATH
                else:
                    suffix = {"reanim": ".reanim.v4.compiled", "particles": ".xml.v4.compiled",
                              "fonts": ".font.compiled"}[kind]
                    spelling = name[:-len(suffix)] + ".compiled"
                counts[kind] += 1
                if kind in overlays:
                    filename = Path(migrated).name
                    pak.require(filename in overlays[kind], f"Compiler output missing runtime record: {migrated}")
                    rebuilt_from = overlays[kind][filename]
                    data = rebuilt_from.read_bytes()
                    used[kind].add(filename)
            elif normalized.startswith("compiled/"):
                raise ValueError(f"Unrecognized compiled record: {name}")
            elif normalized.startswith("effects/") and normalized.endswith(".dds"):
                spelling = name
            else:
                return
            spelling = spelling.replace("\\", "/")
            output_key = migrated or normalized
            pak.require(output_key not in names, f"Staged path collision: {name}")
            names.add(output_key)
            destination = target / spelling
            destination.parent.mkdir(parents=True, exist_ok=True)
            with destination.open("xb") as stream:
                stream.write(data)
            rows.append({"input": name, "output": spelling,
                         "bytes": len(data), "sha256": pak.digest(data),
                         "input_sha256": source_hash,
                         **({"rebuilt_from": str(rebuilt_from.resolve())} if rebuilt_from else {})})

        if source.is_file():
            with zipfile.ZipFile(source) as archive:
                index = pak.archive_index(archive)
                for name, info in index.items():
                    if not info.is_dir() and (name.startswith(("compiled/", "effects/"))):
                        emit(info.filename, archive.read(info))
        else:
            for directory in ("compiled", "effects"):
                base = source / directory
                pak.require(not base.is_symlink() and not getattr(base, "is_junction", lambda: False)(),
                            f"Linked input tree: {base}")
                for path in sorted(base.rglob("*")):
                    pak.require(not path.is_symlink() and not getattr(path, "is_junction", lambda: False)(),
                                f"Linked staged input: {path}")
                    if path.is_file():
                        emit(path.relative_to(source).as_posix(), path.read_bytes())
        pak.require(dict(counts) == pak.COUNTS,
                    f"Expected numeric corpus {pak.COUNTS}, found {dict(counts)}")
        for kind, paths in overlays.items():
            extra = set(paths) - used[kind]
            pak.require(not extra or kind == "particles" and extra <= PARTICLE_COMPILER_FIXTURES,
                        f"Unexpected compiler {kind} records: {sorted(extra)}")
        report = {"format": "runtime-package-stage-v1", "input": str(source),
                  "counts": dict(counts), "files": rows,
                  "omitted": omitted,
                  "compiler_replacements": {kind: len(paths) for kind, paths in used.items()},
                  "scope": "Offline staging; compiler replacements are explicit; run the pak builder for semantic validation"}
        (target / "stage.manifest.json").write_text(
            json.dumps(report, indent=2, sort_keys=True) + "\n", encoding="utf-8")
        os.rename(target, output)
    return report


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("source", type=Path)
    parser.add_argument("output", type=Path)
    parser.add_argument("--reanim", type=Path, help="clean numeric animation compiler output")
    parser.add_argument("--particles", type=Path, help="clean numeric particle compiler output")
    parser.add_argument("--root", type=Path, default=pak.ROOT)
    args = parser.parse_args()
    try:
        replacements = {kind: directory for kind, directory in
                        (("reanim", args.reanim), ("particles", args.particles)) if directory}
        report = stage_runtime(args.source, args.output, replacements, root=args.root)
    except (ValueError, OSError, zipfile.BadZipFile) as error:
        parser.exit(1, f"Runtime staging failed: {error}\n")
    print(f"Staged {sum(report['counts'].values())} numeric records: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
