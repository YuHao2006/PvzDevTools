"""Declarative resource build configuration, separate from definition DSLs."""
from __future__ import annotations
from game_fixture import game_root

from dataclasses import dataclass, field, replace
import json
from pathlib import Path
import re
import fnmatch


@dataclass(frozen=True)
class AtlasSettings:
    solver: str = "exact"
    padding: int = 2
    max_side: int = 16384
    time_limit_seconds: float | None = None
    workers: int = 1
    require_optimal: bool = True


@dataclass
class BuildConfig:
    path: Path
    sources: dict[str, list[Path]] = field(default_factory=lambda: {
        "reanim": [], "particle": [], "projectile": [], "zombatar": [], "scene": []})
    defaults: AtlasSettings = field(default_factory=AtlasSettings)
    atlases: dict[str, list[str]] = field(default_factory=dict)
    settings: dict[str, AtlasSettings] = field(default_factory=dict)
    definition_orders: dict[str, list[tuple[str, str]]] = field(default_factory=dict)


TOKEN = re.compile(r'\s+|#[^\n]*|"(?:\\.|[^"\\])*"|[()]|[^\s()"#]+')
SAFE_GROUP = re.compile(r"[a-z][a-z0-9_-]*\Z")
DEVICES = {"con", "prn", "aux", "nul"} | {
    prefix + str(index) for prefix in ("com", "lpt") for index in range(1, 10)}


def commands(text: str) -> list[tuple[str, list[str]]]:
    tokens = []
    at = 0
    while at < len(text):
        match = TOKEN.match(text, at)
        if match is None:
            raise ValueError(f"Invalid build configuration at line {text.count(chr(10), 0, at) + 1}")
        token = match.group()
        at = match.end()
        if token.isspace() or token.startswith("#"):
            continue
        tokens.append(json.loads(token) if token.startswith('"') else token)
    result, at = [], 0
    while at < len(tokens):
        command = tokens[at]
        if at + 1 >= len(tokens) or tokens[at + 1] != "(":
            raise ValueError(f"Expected '(' after build command {command!r}")
        at += 2
        arguments = []
        while at < len(tokens) and tokens[at] != ")":
            if tokens[at] == "(":
                raise ValueError("Build commands do not allow nested expressions")
            arguments.append(tokens[at])
            at += 1
        if at == len(tokens):
            raise ValueError(f"Unclosed build command {command!r}")
        result.append((command, arguments))
        at += 1
    return result


def settings(base: AtlasSettings, arguments: list[str]) -> AtlasSettings:
    if len(arguments) % 2:
        raise ValueError("Atlas settings require key/value pairs")
    values, seen = {}, set()
    for key, value in zip(arguments[::2], arguments[1::2]):
        if key in seen:
            raise ValueError(f"Duplicate atlas setting {key}")
        seen.add(key)
        if key == "solver":
            if value != "exact":
                raise ValueError("Production atlas solver must be exact; MaxRects is an incumbent only")
            values[key] = value
        elif key == "require_optimal":
            if value not in {"true", "false"}:
                raise ValueError("Atlas require_optimal must be true or false")
            values[key] = value == "true"
        elif key == "time_limit_seconds":
            values[key] = None if value == "unlimited" else float(value)
            if values[key] is not None and not 0 < values[key] <= 86400:
                raise ValueError("Solver time limit must be positive, at most one day, or unlimited")
        elif key in {"padding", "max_side", "workers"}:
            values[key] = int(value)
        else:
            raise ValueError(f"Unknown atlas setting {key}")
    result = replace(base, **values)
    if result.padding != 2:
        raise ValueError("Resource v5 requires two-pixel atlas extrusion")
    if not 1 <= result.max_side <= 16384 or not 1 <= result.workers <= 64:
        raise ValueError("Invalid atlas dimension limit or solver worker count")
    if not result.require_optimal and result.time_limit_seconds is None:
        raise ValueError("Unproven atlas publication requires an explicit finite time limit")
    return result


def load(path: Path) -> BuildConfig:
    path = path.resolve()
    config = BuildConfig(path)
    parsed = commands(path.read_text(encoding="utf-8-sig"))
    if not parsed or parsed[0] != ("resource_build", ["1"]):
        raise ValueError("Build configuration must start with resource_build(1)")
    seen_files, overrides, seen_defaults = set(), {}, False
    for command, arguments in parsed[1:]:
        if command == "sources":
            if len(arguments) < 2 or arguments[0] not in config.sources:
                raise ValueError("sources() requires a definition kind (including scene) and paths/globs")
            kind = arguments[0]
            for pattern in arguments[1:]:
                if Path(pattern).is_absolute() or ".." in Path(pattern).parts:
                    raise ValueError("Source paths must stay inside the configuration directory")
                matches = sorted(path.parent.glob(pattern))
                if not matches:
                    raise ValueError(f"Source pattern matched no files: {pattern}")
                for source in matches:
                    source = source.resolve()
                    suffix = ".json" if kind == "zombatar" else ".dsl"
                    if not source.is_relative_to(path.parent) or not source.is_file() or source.suffix != suffix:
                        raise ValueError(f"Invalid definition source: {source}")
                    if source in seen_files:
                        raise ValueError(f"Definition source selected more than once: {source}")
                    seen_files.add(source)
                    config.sources[kind].append(source)
        elif command == "definition_order":
            if len(arguments) < 2 or arguments[0] not in ("reanim", "particle"):
                raise ValueError("definition_order() requires reanim or particle and ENUM:Definition pairs")
            kind = arguments[0]
            if kind in config.definition_orders:
                raise ValueError(f"Duplicate definition_order({kind})")
            order, enums, names = [], set(), set()
            for value in arguments[1:]:
                pair = value.split(":")
                if (len(pair) != 2 or not re.fullmatch(r"[A-Z][A-Z0-9_]*", pair[0])
                        or not re.fullmatch(r"[A-Za-z0-9_-]+", pair[1])):
                    raise ValueError(f"Invalid definition order member: {value}")
                enum, name = pair
                if not enum.startswith("REANIM_" if kind == "reanim" else "PARTICLE_"):
                    raise ValueError(f"Wrong enum namespace in definition order: {value}")
                if enum in enums or name.casefold() in names:
                    raise ValueError(f"Duplicate definition order member: {value}")
                enums.add(enum)
                names.add(name.casefold())
                order.append((enum, name))
            config.definition_orders[kind] = order
        elif command == "atlas_defaults":
            if seen_defaults:
                raise ValueError("Only one atlas_defaults() command is allowed")
            config.defaults = settings(config.defaults, arguments)
            seen_defaults = True
        elif command == "atlas":
            if len(arguments) < 2:
                raise ValueError("atlas() requires a name and definition members")
            name = arguments[0]
            if not SAFE_GROUP.fullmatch(name) or name in DEVICES or name in config.atlases:
                raise ValueError(f"Invalid or duplicate atlas group: {name}")
            members = []
            for value in arguments[1:]:
                if ":" not in value:
                    raise ValueError("Atlas members use reanim:Name, particle:Name, projectile:Name or zombatar:Name")
                kind, definition = value.split(":", 1)
                if kind not in config.sources or not re.fullmatch(r"[A-Za-z0-9_-]+\*?", definition):
                    raise ValueError(f"Invalid atlas member: {value}")
                directory = {"particle": "particles", "reanim": "reanim", "projectile": "projectiles",
                             "zombatar": "zombatar", "scene": "scene"}[kind]
                member = f"compiled/{directory}/{definition.casefold()}.compiled"
                if member in members or any(member in group for group in config.atlases.values()):
                    raise ValueError(f"Definition belongs to multiple atlas groups: {value}")
                members.append(member)
            config.atlases[name] = members
        elif command == "atlas_settings":
            if not arguments or arguments[0] in overrides:
                raise ValueError("atlas_settings() requires a unique atlas group name")
            overrides[arguments[0]] = arguments[1:]
        else:
            raise ValueError(f"Unknown resource build command: {command}")
    if any(not config.sources[kind] for kind in ("reanim", "particle")):
        raise ValueError("Build configuration must select reanim and particle source files")
    if len(config.sources["projectile"]) > 1:
        raise ValueError("Build configuration selects one combined projectile definition")
    if len(config.sources["zombatar"]) > 1:
        raise ValueError("Build configuration selects one combined Zombatar definition")
    for name, arguments in overrides.items():
        if name not in config.atlases:
            raise ValueError(f"Settings refer to undefined atlas group {name}")
        config.settings[name] = settings(config.defaults, arguments)
    return config


def resolve_atlases(config: BuildConfig, definitions: set[str]) -> dict[str, list[str]]:
    """Resolve author component names to the compiler's published record names."""
    aliases = {}
    for kind, sources in config.sources.items():
        directory = {"particle": "particles", "reanim": "reanim", "projectile": "projectiles",
                     "zombatar": "zombatar", "scene": "scene"}[kind]
        for source in sources:
            text = source.read_text(encoding="utf-8-sig")
            component = re.search(r"^\s*export\s+component\s+(\w+)\s*\{", text, re.M)
            if not component:
                continue
            output = re.search(r'^\s*source-name\s*:\s*"([A-Za-z0-9_-]+)"\s*;', text, re.M)
            basename = output[1] if kind == "particle" and output else component[1]
            alias = f"compiled/{directory}/{component[1].casefold()}.compiled"
            target = f"compiled/{directory}/{basename.casefold()}.compiled"
            if alias in aliases and aliases[alias] != target:
                raise ValueError(f"Ambiguous atlas component: {component[1]}")
            aliases[alias] = target
    resolved, assigned = {}, set()
    for name, members in config.atlases.items():
        group = []
        for member in members:
            if "*" in member:
                matches = sorted(value for value in definitions if fnmatch.fnmatchcase(value, member))
                if not matches:
                    raise ValueError(f"Atlas group {name} has an empty explicit pattern: {member}")
                group.extend(matches)
            else:
                group.append(aliases.get(member, member))
        missing = set(group) - definitions
        if missing:
            raise ValueError(f"Atlas group {name} contains unknown definitions: {sorted(missing)}")
        if len(set(group)) != len(group) or assigned.intersection(group):
            raise ValueError(f"Definition belongs to multiple atlas groups after resolving {name}")
        assigned.update(group)
        resolved[name] = group
    return resolved
