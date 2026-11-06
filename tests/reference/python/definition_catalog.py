"""Validate the authored definition order and derive the offline reanimation map."""
from __future__ import annotations
from game_fixture import game_root

import json
from pathlib import PurePosixPath
import re

import build_numeric_runtime_pak64 as pak


def validate_runtime_orders(config, root) -> None:
    """Fail the offline build if a configured ID points at another runtime root."""
    for kind, filename, catalog, enum_type in (
            ("reanim", "Reanimator.cpp", "gLawnReanimationArray", "ReanimationType"),
            ("particle", "TodParticle.cpp", "gLawnParticleArray", "ParticleEffect")):
        text = pak.clean_comments((root / "pvzPerf/Sexy.TodLib" / filename).read_text(
            encoding="utf-8-sig"))
        match = re.search(r"\b" + catalog + r"\s*\[[^;]*?\]\s*=\s*\{(.*?)\n\};", text, re.S)
        pak.require(match is not None, f"Cannot locate runtime definition catalog: {catalog}")
        rows = re.findall(r"\{\s*(?:" + enum_type + r"::)?(\w+)\s*,\s*(\"(?:\\.|[^\"\\])*\")",
                          match[1])
        actual = [(enum, PurePosixPath(json.loads(path).replace("\\", "/")).stem.casefold())
                  for enum, path in rows]
        expected = [(enum, name.casefold()) for enum, name in config.definition_orders.get(kind, [])]
        pak.require(actual == expected,
                    f"Configured {kind} definition order differs from the runtime catalog")


def reanim_type_map(config) -> bytes:
    order = config.definition_orders.get("reanim")
    pak.require(bool(order), "Production compilation requires definition_order(reanim ...)")
    return ("reanim_none REANIM_NONE 4294967295\n"
            + "".join(f"{name.casefold()} {index}\n"
                      for index, (_, name) in enumerate(order))).encode("ascii")
