"""Search or prove minimum-area, nonrotating integer atlas layouts offline.

Native/legacy layouts are feasible incumbents only. Strict mode requires an
optimal CP-SAT result or equality with a mathematical lower bound. Explicit
bounded policies may reuse a completed search without claiming optimality.
No image pixels or runtime resources enter this module.
"""
from __future__ import annotations
from game_fixture import game_root

import copy
import hashlib
import importlib.metadata
import json
import math
import os
from pathlib import Path
import time
import tempfile


SCHEMA = "pvz-exact-atlas-v2"
MODEL_VERSION = "integer-nonrotating-padded-global-area-v2"


class UnprovenAtlasError(RuntimeError):
    """The incumbent is usable as a hint, but must never be published."""


def _version() -> str:
    try:
        return importlib.metadata.version("ortools")
    except importlib.metadata.PackageNotFoundError:
        return "unavailable"


def _cp_api():
    try:
        from ortools.sat.python import cp_model
    except ImportError as error:
        raise RuntimeError("Exact atlas compilation requires the Python ortools package") from error
    return cp_model


def _integer(value: object, label: str, minimum: int = 0) -> int:
    if isinstance(value, bool) or not isinstance(value, int) or value < minimum:
        raise ValueError(f"Invalid exact atlas {label}: {value!r}")
    return value


def _validate(sizes: list[tuple[int, int]], layout: dict, padding: int, max_side: int) -> None:
    width = _integer(layout.get("width"), "width")
    height = _integer(layout.get("height"), "height")
    regions = layout.get("regions")
    if not isinstance(regions, (list, tuple)) or len(regions) != len(sizes):
        raise ValueError("Exact atlas region count differs from input")
    if not sizes:
        if width or height:
            raise ValueError("Empty exact atlas must have zero dimensions")
        return
    if not 0 < width <= max_side or not 0 < height <= max_side:
        raise ValueError("Exact atlas dimensions outside configured limit")
    occupied = []
    for size, region in zip(sizes, regions):
        if not isinstance(region, (list, tuple)) or len(region) != 4:
            raise ValueError("Invalid exact atlas region")
        x, y, w, h = (_integer(v, "region coordinate") for v in region)
        if (w, h) != size:
            raise ValueError("Exact atlas must preserve input dimensions and orientation")
        if x < padding or y < padding or x + w + padding > width or y + h + padding > height:
            raise ValueError("Exact atlas padded region lies outside atlas")
        rectangle = (x - padding, y - padding, x + w + padding, y + h + padding)
        if any(rectangle[0] < old[2] and old[0] < rectangle[2] and
               rectangle[1] < old[3] and old[1] < rectangle[3] for old in occupied):
            raise ValueError("Exact atlas padded regions overlap")
        occupied.append(rectangle)


def _quality(layout: dict) -> tuple:
    width, height = layout["width"], layout["height"]
    return width * height, max(width, height), width, height


def _lower_bound(padded: list[tuple[int, int]], max_side: int, upper: int) -> int:
    if not padded:
        return 0
    min_width = max(w for w, _ in padded)
    min_height = max(h for _, h in padded)
    area = sum(w * h for w, h in padded)
    # Any packing no worse than the known incumbent has min_width <= W <=
    # min(max_side, upper // min_height), H >= min_height, W*H >= total area.
    # Minimize W*max(min_height, ceil(total/W)) over ALL such integer widths.
    # Omitting H<=max_side only enlarges this relaxation, never raises its bound.
    # Since an incumbent exists, excluding areas above upper cannot remove the
    # global optimum. No rotation or incumbent-specific aspect ratio is assumed.
    return min(w * max(min_height, (area + w - 1) // w)
               for w in range(min_width, min(max_side, upper // min_height) + 1))


def _metadata(status: str, bound: int, layout: dict, *, optimal: bool,
              elapsed: float, key: str, hit: bool, version: str) -> dict:
    area = layout["width"] * layout["height"]
    return {"status": status, "best_bound": bound, "optimal": optimal,
            "gap": (area - bound) / area if area else 0.0,
            "elapsed_seconds": elapsed, "cache_hit": hit, "cache_key": key,
            "solver": "ortools-cp-sat", "solver_version": version,
            "model_version": MODEL_VERSION,
            "search_complete": False,
            "solve_budget_seconds": None,
            "proof_scope": "minimum integer atlas area; fixed orientation; configured padding and max_side",
            "objective_area": area}


def _layout_digest(layout: dict) -> str:
    return hashlib.sha256(json.dumps(layout, sort_keys=True, separators=(",", ":")).encode()).hexdigest()


def _read_cache(path: Path, key: str, sizes: list[tuple[int, int]],
                padding: int, max_side: int, version: str) -> dict | None:
    try:
        record = json.loads(path.read_text(encoding="utf-8"))
        if not isinstance(record, dict) or record.get("format") != SCHEMA or record.get("key") != key:
            return None
        result = record["layout"]
        if not isinstance(result, dict):
            return None
        if record.get("layout_sha256") != _layout_digest(result):
            return None
        _validate(sizes, result, padding, max_side)
        exact = result["exact"]
        if not isinstance(exact, dict):
            return None
        area = result["width"] * result["height"]
        if (not isinstance(exact["optimal"], bool) or exact.get("model_version") != MODEL_VERSION
                or exact.get("solver_version") != version or exact.get("solver") != "ortools-cp-sat"):
            return None
        if (isinstance(exact["best_bound"], bool) or not isinstance(exact["best_bound"], int)
                or not 0 <= exact["best_bound"] <= area):
            return None
        if (_integer(exact["objective_area"], "objective area") != area or exact["cache_key"] != key):
            return None
        gap = exact["gap"]
        if (isinstance(gap, bool) or not isinstance(gap, (int, float)) or not math.isfinite(gap)
                or not math.isclose(gap, (area - exact["best_bound"]) / area if area else 0.0,
                                    rel_tol=1e-12, abs_tol=1e-12)):
            return None
        if not exact["optimal"] and exact["status"] not in ("FEASIBLE", "UNKNOWN"):
            return None
        if "search_complete" in exact and not isinstance(exact["search_complete"], bool):
            return None
        if exact["optimal"] and (exact["status"] not in ("OPTIMAL", "LOWER_BOUND_PROVEN") or
                                 exact["best_bound"] != area):
            return None
        if exact["optimal"] and exact["status"] == "LOWER_BOUND_PROVEN":
            padded = [(w + 2 * padding, h + 2 * padding) for w, h in sizes]
            if _lower_bound(padded, max_side, area) != area:
                return None
        return result
    except (OSError, ValueError, TypeError, KeyError):
        return None


def _finite_budget(value: object, *, allow_zero: bool = False) -> bool:
    return (not isinstance(value, bool) and isinstance(value, (int, float))
            and math.isfinite(value) and (value >= 0 if allow_zero else value > 0))


def _cache_key(sizes: list[tuple[int, int]], padding: int, max_side: int, version: str) -> str:
    settings = {"format": SCHEMA, "model_version": MODEL_VERSION, "sizes": sizes,
                "padding": padding, "max_side": max_side, "solver_version": version}
    return hashlib.sha256(json.dumps(settings, sort_keys=True).encode("ascii")).hexdigest()


def _manifest_geometry(manifest: dict, group: dict) -> tuple[list[tuple[int, int]], int, int]:
    source_sizes = {}
    for member in group["members"]:
        reports = {**manifest["definition_atlases"], **manifest.get("scene_atlases", {})}
        for row in reports[member]["images"]:
            size = (_integer(row["width"], "source width", 1),
                    _integer(row["height"], "source height", 1))
            if row["source"] in source_sizes and source_sizes[row["source"]] != size:
                raise ValueError("Inconsistent published source dimensions")
            source_sizes[row["source"]] = size
    return ([source_sizes[name] for name in sorted(source_sizes)],
            _integer(group.get("padding", 2), "padding"),
            _integer(group.get("max_side", 16384), "maximum side", 1))


def import_completed_budget(cache: Path, manifest: dict) -> int:
    """Import old v2 completion evidence from a successfully published manifest.

    Never infers completion from elapsed time alone. The published finite,
    relaxed policy AND its full final exact metadata must match the intact
    cache. Geometry and the geometry/model/version key are independently
    reconstructed. This only attests completed search effort, never a proof.
    """
    imported = 0
    version = _version()
    for group in manifest.get("atlas_groups", {}).values():
        try:
            seconds, final = group["time_limit_seconds"], group["exact"]
            if (group["solver"] != "exact" or group["require_optimal"] is not False
                    or not _finite_budget(seconds) or seconds > 86400
                    or final.get("optimal") is not False
                    or final.get("status") not in ("FEASIBLE", "UNKNOWN")):
                continue
            # A checkpoint can happen near the time limit. Only an identical
            # published final record is evidence; elapsed is an extra guard.
            elapsed = final["elapsed_seconds"]
            if not _finite_budget(elapsed, allow_zero=True) or elapsed < seconds:
                continue
            sizes, padding, max_side = _manifest_geometry(manifest, group)
            key = _cache_key(sizes, padding, max_side, version)
            if final.get("cache_key") != key:
                continue
            path = Path(cache) / (key + ".json")
            layout = _read_cache(path, key, sizes, padding, max_side, version)
            if (layout is None or layout["exact"] != final or layout["width"] != group["width"]
                    or layout["height"] != group["height"]):
                continue
            exact = layout["exact"]
            if "search_complete" in exact:
                # An explicit checkpoint must never be upgraded by a manifest.
                # New-format final caches already carry their own evidence.
                continue
            exact["search_complete"] = True
            exact["solve_budget_seconds"] = seconds
            exact["completion_evidence"] = "matching-published-manifest"
            _write_cache(path, key, layout)
            imported += 1
        except (OSError, ValueError, TypeError, KeyError):
            continue
    return imported


def _write_cache(path: Path, key: str, layout: dict, *, geometry: dict | None = None,
                 replace_existing: bool = True) -> bool:
    path.parent.mkdir(parents=True, exist_ok=True)
    with tempfile.NamedTemporaryFile(mode="w", encoding="utf-8", dir=path.parent,
                                     prefix=".exact-atlas-", suffix=".tmp", delete=False) as stream:
        temporary = Path(stream.name)
        record = {"format": SCHEMA, "key": key, "layout": layout,
                  "layout_sha256": _layout_digest(layout)}
        if geometry is not None:
            record["geometry"] = geometry
        stream.write(json.dumps(record, sort_keys=True, indent=2) + "\n")
    try:
        if replace_existing:
            temporary.replace(path)
        else:
            # Link publication is atomic and fails if any local search/cache
            # appeared since inspection. Never replace that file, even if it
            # is an incomplete or invalid checkpoint.
            try:
                os.link(temporary, path)
            except FileExistsError:
                return False
        return True
    finally:
        temporary.unlink(missing_ok=True)


def _completed_or_proven(layout: dict) -> bool:
    exact = layout["exact"]
    return (exact["optimal"] is True or
            (exact.get("search_complete") is True
             and _finite_budget(exact.get("solve_budget_seconds"))
             and exact["solve_budget_seconds"] <= 86400))


def _portable_layout(layout: dict, key: str, version: str) -> dict:
    """Whitelist geometry/solver facts; never export host paths or debug extras."""
    result = {field: copy.deepcopy(layout[field]) for field in (
        "width", "height", "regions", "shelf_area", "search_attempts", "search_microseconds")
        if field in layout}
    result["method"] = "native-maxrects"
    exact = layout["exact"]
    elapsed = exact.get("elapsed_seconds", 0)
    if not _finite_budget(elapsed, allow_zero=True):
        raise ValueError("Invalid completed solve elapsed time")
    result["exact"] = _metadata(
        exact["status"], exact["best_bound"], result, optimal=exact["optimal"],
        elapsed=elapsed, key=key, hit=False, version=version)
    result["exact"]["search_complete"] = True
    result["exact"]["solve_budget_seconds"] = exact.get("solve_budget_seconds")
    return result


def export_completed_cache(cache: Path, manifest: dict, destination: Path) -> int:
    """Export only current manifest groups' proven/completed portable v2 records.

    Unknown historical files are not copied. Existing unrelated destination
    files are not deleted; use a fresh curated directory for a clean export.
    """
    import_completed_budget(cache, manifest)
    version, written = _version(), set()
    fields = ("status", "best_bound", "optimal", "gap", "objective_area",
              "cache_key", "solver", "solver_version", "model_version")
    for group in manifest.get("atlas_groups", {}).values():
        try:
            if group["solver"] != "exact":
                continue
            sizes, padding, max_side = _manifest_geometry(manifest, group)
            key = _cache_key(sizes, padding, max_side, version)
            layout = _read_cache(Path(cache) / (key + ".json"), key, sizes, padding, max_side, version)
            if (layout is None or not _completed_or_proven(layout)
                    or layout["width"] != group["width"] or layout["height"] != group["height"]):
                continue
            final, exact = group["exact"], layout["exact"]
            if any(final[field] != exact[field] for field in fields):
                continue
            if not exact["optimal"]:
                seconds = group["time_limit_seconds"]
                if (group["require_optimal"] is not False or not _finite_budget(seconds)
                        or seconds > 86400 or seconds > exact["solve_budget_seconds"]):
                    continue
            portable = _portable_layout(layout, key, version)
            _write_cache(Path(destination) / (key + ".json"), key, portable,
                         geometry={"padding": padding, "max_side": max_side})
            written.add(key)
        except (OSError, ValueError, TypeError, KeyError):
            continue
    return len(written)


def seed_completed_cache(source: Path, cache: Path) -> int:
    """Seed missing local caches only; all existing files win over repo seeds."""
    version, count = _version(), 0
    for path in sorted(Path(source).glob("*.json")):
        try:
            if path.is_symlink():
                continue
            record = json.loads(path.read_text(encoding="utf-8"))
            geometry = record.get("geometry", {"padding": 2, "max_side": 16384})
            padding = _integer(geometry["padding"], "padding")
            max_side = _integer(geometry["max_side"], "maximum side", 1)
            regions = record["layout"]["regions"]
            sizes = [(_integer(region[2], "source width", 1),
                      _integer(region[3], "source height", 1)) for region in regions]
            key = _cache_key(sizes, padding, max_side, version)
            if path.name != key + ".json":
                continue
            layout = _read_cache(path, key, sizes, padding, max_side, version)
            if layout is None or not _completed_or_proven(layout):
                continue
            if _write_cache(Path(cache) / (key + ".json"), key, _portable_layout(layout, key, version),
                            geometry={"padding": padding, "max_side": max_side},
                            replace_existing=False):
                count += 1
        except (OSError, ValueError, TypeError, KeyError, IndexError, AttributeError):
            continue
    return count


def _cached_incumbent(cache: Path, sizes: list[tuple[int, int]], padding: int,
                      max_side: int, initial: dict) -> dict:
    """Old formats/solver versions provide geometry only, NEVER a proof/bound."""
    result = copy.deepcopy(initial)
    for path in sorted(cache.glob("*.json")):
        try:
            candidate = json.loads(path.read_text(encoding="utf-8"))["layout"]
            _validate(sizes, candidate, padding, max_side)
            if _quality(candidate) < _quality(result):
                result.update({field: candidate[field] for field in ("width", "height", "regions")})
        except (OSError, ValueError, TypeError, KeyError):
            continue
    return result


def _solve(sizes: list[tuple[int, int]], initial: dict, padding: int, max_side: int,
           seconds: float | None, workers: int, lower: int, progress=None) -> tuple[dict, str, int, bool]:
    cp = _cp_api()
    model = cp.CpModel()
    padded = [(w + 2 * padding, h + 2 * padding) for w, h in sizes]
    upper = initial["width"] * initial["height"]
    min_w, min_h = max(w for w, _ in padded), max(h for _, h in padded)
    max_w, max_h = min(max_side, upper // min_h), min(max_side, upper // min_w)
    # These are area-derived bounds, not initial.width / initial.height bounds.
    # They include every orientation-preserving packing that could beat upper.
    width = model.new_int_var(min_w, max_w, "atlas_width")
    height = model.new_int_var(min_h, max_h, "atlas_height")
    area = model.new_int_var(lower, upper, "atlas_area")
    model.add_multiplication_equality(area, [width, height])
    x_vars, y_vars, x_intervals, y_intervals = [], [], [], []
    for index, (w, h) in enumerate(padded):
        x = model.new_int_var(0, max_w - w, f"x_{index}")
        y = model.new_int_var(0, max_h - h, f"y_{index}")
        model.add(x + w <= width)
        model.add(y + h <= height)
        x_vars.append(x)
        y_vars.append(y)
        x_intervals.append(model.new_fixed_size_interval_var(x, w, f"column_{index}"))
        y_intervals.append(model.new_fixed_size_interval_var(y, h, f"row_{index}"))
    model.add_no_overlap_2d(x_intervals, y_intervals)
    model.add_max_equality(width, [x + w for x, (w, _) in zip(x_vars, padded)])
    model.add_max_equality(height, [y + h for y, (_, h) in zip(y_vars, padded)])
    # Translation to min(x)=min(y)=0 and cropping unused right/bottom space
    # preserve feasibility and cannot worsen area. Thus a global optimum has
    # this canonical representative; these are not aspect-ratio restrictions.
    model.add_min_equality(0, x_vars)
    model.add_min_equality(0, y_vars)
    model.minimize(area)

    # Equal-size rectangles are interchangeable geometrically; canonical hints
    # and lexicographic positions remove this symmetry without rotating images.
    hints = [(region[0] - padding, region[1] - padding) for region in initial["regions"]]
    equal_sizes: dict[tuple[int, int], list[int]] = {}
    for index, size in enumerate(padded):
        equal_sizes.setdefault(size, []).append(index)
    for indices in equal_sizes.values():
        points = sorted(hints[index] for index in indices)
        for index, point in zip(indices, points):
            hints[index] = point
        for before, after in zip(indices, indices[1:]):
            model.add(x_vars[before] * (max_h + 1) + y_vars[before] <=
                      x_vars[after] * (max_h + 1) + y_vars[after])
    model.add_hint(width, initial["width"])
    model.add_hint(height, initial["height"])
    model.add_hint(area, upper)
    for x, y, (hint_x, hint_y) in zip(x_vars, y_vars, hints):
        model.add_hint(x, hint_x)
        model.add_hint(y, hint_y)
    solver = cp.CpSolver()
    solver.parameters.num_search_workers = workers
    solver.parameters.random_seed = 0
    solver.parameters.randomize_search = False
    solver.parameters.absolute_gap_limit = 0
    solver.parameters.relative_gap_limit = 0
    solver.parameters.log_search_progress = progress is not None
    if seconds is not None:
        solver.parameters.max_time_in_seconds = seconds
    class Observer(cp.CpSolverSolutionCallback):
        def on_solution_callback(self):
            if progress is not None:
                candidate = {"width": self.value(width), "height": self.value(height),
                             "regions": [(self.value(x) + padding, self.value(y) + padding, w, h)
                                         for x, y, (w, h) in zip(x_vars, y_vars, sizes)]}
                progress(candidate, max(lower, math.floor(self.best_objective_bound)))

    status = solver.solve(model, Observer() if progress is not None else None)
    name = solver.status_name(status)
    if status in (cp.MODEL_INVALID, cp.INFEASIBLE):
        raise ValueError(f"Exact atlas solver rejected a known feasible incumbent: {name}")
    # Conservative integer bound. Never round a floating solver bound upward.
    bound = max(lower, min(upper, math.floor(solver.best_objective_bound)))
    result = copy.deepcopy(initial)
    if status in (cp.FEASIBLE, cp.OPTIMAL):
        candidate = {"width": solver.value(width), "height": solver.value(height),
                     "regions": [(solver.value(x) + padding, solver.value(y) + padding, w, h)
                                 for x, y, (w, h) in zip(x_vars, y_vars, sizes)]}
        _validate(sizes, candidate, padding, max_side)
        if _quality(candidate) < _quality(result):
            result.update(candidate)
    optimal = status == cp.OPTIMAL
    if optimal:
        if solver.value(area) != result["width"] * result["height"]:
            raise ValueError("Exact solver's optimal objective contradicts the validated incumbent")
        bound = result["width"] * result["height"]
    return result, name, bound, optimal


def optimize_layouts(batches: list[list[tuple[int, int]]], initial_layouts: list[dict], *,
                     max_side: int = 16384, padding: int = 2,
                     time_limit_seconds: float | None = None,
                     cache: Path | None = None, workers: int = 1,
                     require_optimal: bool = True, progress=None) -> list[dict]:
    """Refine each native layout, preserve order, and return native-compatible dicts.

    Default behavior requires a global optimum. A finite budget may be used
    with require_optimal=False under an explicit bounded publication policy.
    A completed same-model cache may satisfy an equal/smaller bounded budget;
    unfinished checkpoints and all strict-mode unproven caches solve again.
    Proof cache identity includes model revision, ordered geometry and solver
    version. Search budget/workers affect effort, never the mathematical model.
    """
    _integer(max_side, "maximum side", 1)
    _integer(padding, "padding")
    _integer(workers, "worker count", 1)
    if not isinstance(require_optimal, bool):
        raise ValueError("require_optimal must be boolean")
    if time_limit_seconds is not None and (isinstance(time_limit_seconds, bool) or
            not isinstance(time_limit_seconds, (int, float)) or
            not math.isfinite(time_limit_seconds) or time_limit_seconds < 0):
        raise ValueError("Exact atlas time limit must be nonnegative or None")
    if len(batches) != len(initial_layouts):
        raise ValueError("Exact atlas batch count differs from native layouts")
    version = _version()
    results = []
    for raw_sizes, native in zip(batches, initial_layouts):
        sizes = []
        for pair in raw_sizes:
            if not isinstance(pair, (list, tuple)) or len(pair) != 2:
                raise ValueError("Invalid exact atlas source dimensions")
            sizes.append((_integer(pair[0], "source width", 1),
                          _integer(pair[1], "source height", 1)))
        _validate(sizes, native, padding, max_side)
        padded = [(w + 2 * padding, h + 2 * padding) for w, h in sizes]
        key = _cache_key(sizes, padding, max_side, version)
        path = Path(cache) / (key + ".json") if cache is not None else None
        started = time.monotonic()
        cached = _read_cache(path, key, sizes, padding, max_side, version) if path is not None else None
        initial = _cached_incumbent(Path(cache), sizes, padding, max_side, native) if cache else copy.deepcopy(native)
        upper = initial["width"] * initial["height"]
        lower = _lower_bound(padded, max_side, upper)
        if cached is not None and cached["exact"]["optimal"] and cached["exact"]["objective_area"] > upper:
            # A new feasible packing contradicts this cached proof. Do not
            # silently trust metadata over a physically validated incumbent.
            raise ValueError("Cached optimal proof contradicts a better validated incumbent")
        completed_bounded = (
            cached is not None and not require_optimal
            and _finite_budget(time_limit_seconds, allow_zero=True)
            and cached["exact"].get("search_complete") is True
            and _finite_budget(cached["exact"].get("solve_budget_seconds"))
            and cached["exact"]["solve_budget_seconds"] >= time_limit_seconds
            and _quality(cached) <= _quality(initial))
        if cached is not None and (cached["exact"]["optimal"] or completed_bounded):
            result = copy.deepcopy(native)
            result.update({field: cached[field] for field in ("width", "height", "regions", "exact")})
            result["exact"]["cache_hit"] = True
            if completed_bounded:
                # Preserve original solve effort; a fast lookup did not itself
                # exhaust another budget or establish any new proof.
                result["exact"]["cache_lookup_seconds"] = time.monotonic() - started
            else:
                result["exact"]["elapsed_seconds"] = time.monotonic() - started
        else:
            def checkpoint(candidate, bound):
                _validate(sizes, candidate, padding, max_side)
                hint = copy.deepcopy(native)
                hint.update(candidate)
                hint["exact"] = _metadata("FEASIBLE", min(bound, candidate["width"] * candidate["height"]),
                                          hint, optimal=False, elapsed=time.monotonic() - started,
                                          key=key, hit=False, version=version)
                if path is not None:
                    _write_cache(path, key, hint)
                if progress is not None:
                    progress(hint["exact"])

            if progress is not None:
                progress(_metadata("SOLVING", lower, initial, optimal=False, elapsed=0,
                                   key=key, hit=cached is not None, version=version))
            if lower == upper:
                result, status, bound, optimal = copy.deepcopy(initial), "LOWER_BOUND_PROVEN", lower, True
            elif time_limit_seconds == 0:
                result, status, bound, optimal = copy.deepcopy(initial), "UNKNOWN", lower, False
            else:
                result, status, bound, optimal = _solve(sizes, initial, padding, max_side,
                                                       time_limit_seconds, workers, lower,
                                                       checkpoint if path is not None or progress is not None else None)
            result["exact"] = _metadata(status, bound, result, optimal=optimal,
                                        elapsed=time.monotonic() - started, key=key,
                                        hit=cached is not None, version=version)
            result["exact"]["search_complete"] = True
            result["exact"]["solve_budget_seconds"] = time_limit_seconds
            if path is not None:
                _write_cache(path, key, result)
        _validate(sizes, result, padding, max_side)
        if _quality(result) > _quality(native):
            raise ValueError("Exact atlas result regresses native incumbent")
        if progress is not None:
            progress(result["exact"])
        if require_optimal and not result["exact"]["optimal"]:
            raise UnprovenAtlasError(
                f"Atlas optimum is unproven ({result['exact']['status']}, area={result['exact']['objective_area']}, "
                f"bound={result['exact']['best_bound']}, key={key}); incumbent cached only; publication refused")
        results.append(result)
    return results
