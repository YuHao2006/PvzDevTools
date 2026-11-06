"""Resolve game virtual paths to native assets and explicitly listed AI derivatives."""
from __future__ import annotations
from game_fixture import game_root

import json
from pathlib import Path, PurePosixPath

AI_OVERLAY_MANIFEST = Path("assets/source-art/ai/runtime-overlay.json")
AI_RUNTIME_ROOT = Path("assets/source-art/ai/runtime")


def _relative_path(value: str) -> PurePosixPath:
    if (not isinstance(value, str) or not value or "\\" in value or ":" in value
            or value.startswith("/") or any(part in ("", ".", "..") for part in value.split("/"))):
        raise ValueError(f"Invalid runtime asset relative path: {value!r}")
    return PurePosixPath(value)


def _unique_object(pairs):
    result = {}
    for key, value in pairs:
        if key in result:
            raise ValueError(f"Duplicate runtime overlay JSON key: {key}")
        result[key] = value
    return result


def inventory(root: Path) -> dict[str, Path]:
    """Return casefolded game paths; raw AI masters, inputs and prompts are excluded.

    An overlay is an explicit relocation, never an override. A virtual name
    present in both directories fails instead of silently choosing one copy.
    """
    root = Path(root).resolve()
    runtime = root / "assets/runtime"
    ai_runtime = root / AI_RUNTIME_ROOT
    if not runtime.is_dir():
        raise ValueError(f"Missing runtime asset directory: {runtime}")
    result = {}
    for path in sorted(runtime.rglob("*")):
        if not path.is_file():
            continue
        if not path.resolve().is_relative_to(runtime.resolve()):
            raise ValueError(f"Runtime asset escapes its source directory: {path}")
        virtual = str(_relative_path(path.relative_to(runtime).as_posix())).casefold()
        if virtual in result:
            raise ValueError(f"Duplicate runtime asset: {virtual}")
        result[virtual] = path

    manifest_path = root / AI_OVERLAY_MANIFEST
    if not manifest_path.is_file():
        raise ValueError(f"Missing AI runtime overlay manifest: {manifest_path}")
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"), object_pairs_hook=_unique_object)
    if (not isinstance(manifest, dict) or set(manifest) != {"format", "files"}
            or manifest["format"] != "pvz-ai-runtime-overlay-v1"
            or not isinstance(manifest["files"], dict)):
        raise ValueError("Invalid AI runtime overlay manifest")
    sources = set()
    for name, source in manifest["files"].items():
        virtual = str(_relative_path(name)).casefold()
        relative = _relative_path(source)
        if not relative.is_relative_to(PurePosixPath(AI_RUNTIME_ROOT.as_posix())):
            raise ValueError(f"AI runtime overlay source is outside cooked assets: {source}")
        path = root / Path(relative)
        resolved = path.resolve()
        if not resolved.is_relative_to(ai_runtime.resolve()):
            raise ValueError(f"AI runtime overlay source escapes cooked assets: {source}")
        if not path.is_file():
            raise ValueError(f"Missing AI runtime overlay source: {source}")
        if virtual in result:
            raise ValueError(f"Duplicate runtime asset: {virtual}")
        if resolved in sources:
            raise ValueError(f"Duplicate AI runtime overlay source: {source}")
        sources.add(resolved)
        result[virtual] = path
    return dict(sorted(result.items()))
