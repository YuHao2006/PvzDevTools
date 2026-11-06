"""Publish native static definitions and generated loading/free functions."""
from __future__ import annotations
from game_fixture import game_root

import hashlib

import build_numeric_runtime_pak64 as pak
import projectile_atlas
from static_definition_codegen import cpp_float, cpp_string, texture_region


def generate(config, records: dict[str, bytes], reports: dict, *,
             runtime_records: dict[str, bytes] | None = None) -> tuple[dict[str, bytes], dict]:
    import static_reanim_codegen
    import static_particle_codegen
    reanim = static_reanim_codegen.generate(config, records)
    particle = static_particle_codegen.generate(config, records, reports)
    if runtime_records is not None:
        runtime_records.update(particle["runtime_records"])
    rc = len(config.definition_orders["reanim"])
    pc = len(config.definition_orders["particle"])
    header = (
        "// Generated native definitions and loading functions. Do not edit.\n"
        "#pragma once\n"
        '#include "Sexy.TodLib/Reanimator.h"\n'
        '#include "Sexy.TodLib/TodParticle.h"\n'
        '#include "Lawn/ProjectileResources.h"\n'
        "#include <span>\n"
        "namespace GeneratedDefinitions {\n"
        f"inline constexpr int ReanimationCount={rc}, ParticleCount={pc};\n"
        "extern ReanimatorDefinition Reanimations[ReanimationCount];\n"
        "extern TodParticleDefinition Particles[ParticleCount];\n"
        "extern const char* const ParticlePaths[ParticleCount];\n"
        "extern ProjectileAtlasDefinition Projectiles;\n"
        "extern std::span<AttacherSegmentRT> AttacherSegments[ReanimationCount];\n"
        "void LoadReanimationImages(int type);\n"
        "void LoadParticleImages();\n"
        "void LoadProjectileImages();\n"
        "void LoadAllImages();\n"
        "void BindFonts();\n"
        "void ReleaseReanimationImages();\n"
        "void ReleaseParticleImages();\n"
        "void ReleaseProjectileImages();\n"
        "void ReleaseImages();\n"
        "}\n"
    )
    declarations = [
        "// Generated backing storage declarations; no runtime wrappers.",
        "#pragma once", '#include "GeneratedDefinitions.h"',
        "namespace GeneratedDefinitions {",
        *reanim["declarations"], *particle["declarations"], "}",
    ]
    objects = [
        "// Generated complete static definitions.",
        '#include "GeneratedDefinitionStorage.h"', "namespace GeneratedDefinitions {",
        "constinit ReanimatorDefinition Reanimations[ReanimationCount] = {",
        *("    " + row + "," for row in reanim["initializers"]), "};",
        "TodParticleDefinition Particles[ParticleCount]{};",
        "constinit const char* const ParticlePaths[ParticleCount] = {",
        *("    " + cpp_string(f"compiled/particle-runtime/{name.casefold()}.compiled") + ","
          for _, name in config.definition_orders["particle"]), "};",
        "constinit std::span<AttacherSegmentRT> AttacherSegments[ReanimationCount] = {",
        *("    " + row + "," for row in reanim["attacher_initializers"]), "};",
    ]
    projectile = pak.parse_projectiles(projectile_atlas.PATH, records[projectile_atlas.PATH])
    visuals = [
        "{" + texture_region(rect, projectile["width"], projectile["height"]) + "," + cpp_float(scale) + "}"
        for rect, scale in zip(projectile["regions"], projectile["scales"])
    ]
    shadows = [texture_region(rect, projectile["width"], projectile["height"])
               for rect in projectile["shadows"]]
    objects.extend([
        "constinit ProjectileAtlasDefinition Projectiles = {",
        "    {{" + ",".join(visuals) + "}},",
        "    {{" + ",".join(shadows) + "}}, nullptr", "};",
    ])
    for kind, typ, count in (("reanim", "ReanimationType", "NUM_REANIMS"),
                              ("particle", "ParticleEffect", "NUM_PARTICLES")):
        order = config.definition_orders[kind]
        objects.append(f"static_assert(static_cast<int>({typ}::{count})=={len(order)});")
        objects.extend(f"static_assert(static_cast<int>({typ}::{enum})=={index});"
                       for index, (enum, _) in enumerate(order))
    objects.append("}")
    load = [
        "// Generated image ownership bindings; behavior is already static.",
        '#include "GeneratedDefinitionStorage.h"',
        '#include "Sexy.TodLib/DefinitionImageRegion.h"',
        '#include "Sexy.TodLib/ParticleDefinitionCompiled.h"',
        '#include "Sexy.TodLib/TodCommon.h"',
        "#include <algorithm>", "#include <stdexcept>",
        "namespace GeneratedDefinitions {", "namespace {",
        "constinit bool reanimationImages[ReanimationCount]{};",
        "constinit bool particleImages=false, projectileImages=false, fonts=false;",
        "}", "void BindFonts() {", "    if (fonts) return;",
    ]
    if reanim["font_bindings"]:
        load.extend([
            "    auto* font=TodLoadCreditsFont();",
            '    if (!font) throw std::runtime_error("Static reanimation font is unavailable");',
            *("    " + line for line in reanim["font_bindings"]),
        ])
    load.extend(["    fonts=true;", "}", "void LoadReanimationImages(int type) {",
                 '    if (type<0 || type>=ReanimationCount) throw std::out_of_range("Reanimation type");',
                 "    if (reanimationImages[type]) return;", "    switch(type) {"])
    atlas_paths = set()
    for index, (_, name) in enumerate(config.definition_orders["reanim"]):
        path = f"compiled/reanim/{name.casefold()}.compiled"
        load.append(f"    case {index}:")
        rows = reports[path].get("variants", [reports[path]])
        for v, row in enumerate(rows):
            if row["atlas_path"]:
                atlas_paths.add(row["atlas_path"])
                load.append(
                    f"        DefinitionBindAtlasPath(Reanimations[{index}].mTextures[{v}], "
                    f"{row['atlas_width']}, {row['atlas_height']}, {cpp_string(row['atlas_path'])});")
        load.append("        break;")
    load.extend(["    }", "    reanimationImages[type]=true;", "}",
                 "void LoadParticleImages() {", "    if (particleImages) return;"])
    for index, (_, name) in enumerate(config.definition_orders["particle"]):
        path = f"compiled/particles/{name.casefold()}.compiled"
        for v, row in enumerate(reports[path].get("variants", [reports[path]])):
            if row["atlas_path"]:
                atlas_paths.add(row["atlas_path"])
    load.extend([
        "    try {",
        "        for(int i=0;i<ParticleCount;++i) TodParticleLoadCompiledDefinition(Particles[i],ParticlePaths[i]);",
        "    } catch (...) {",
        "        for(auto& definition:Particles) TodParticleFreeCompiledDefinition(definition);",
        "        throw;",
        "    }",
    ])
    projectile_report = reports[projectile_atlas.PATH]
    atlas_paths.add(projectile_report["atlas_path"])
    load.extend([
        "    particleImages=true;", "}", "void LoadProjectileImages() {",
        "    if (projectileImages) return;",
        f"    DefinitionBindAtlasPath(Projectiles.mTexture, {projectile['width']}, "
        f"{projectile['height']}, {cpp_string(projectile_report['atlas_path'])});",
        "    projectileImages=true;", "}", "void LoadAllImages() {",
        "    BindFonts();",
        "    for(int i=0;i<ReanimationCount;++i) LoadReanimationImages(i);",
        "    LoadParticleImages();", "    LoadProjectileImages();", "}",
        "void ReleaseReanimationImages() {",
        "    for(auto& definition:Reanimations)",
        "        for(unsigned v=0;v<definition.mVariantCount;++v) DefinitionUnbindAtlas(definition.mTextures[v]);",
        *("    " + line for line in reanim["font_resets"]),
        "    std::fill(std::begin(reanimationImages),std::end(reanimationImages),false);",
        "    fonts=false;", "}",
        "void ReleaseParticleImages() {",
        "    for(auto& definition:Particles) TodParticleFreeCompiledDefinition(definition);",
        "    particleImages=false;", "}",
        "void ReleaseProjectileImages() {",
        "    DefinitionUnbindAtlas(Projectiles.mTexture);",
        "    projectileImages=false;", "}",
        "void ReleaseImages() {",
        "    ReleaseReanimationImages();", "    ReleaseParticleImages();",
        "    ReleaseProjectileImages();", "}", "}",
    ])
    files = {
        "GeneratedDefinitions.h": header.encode(),
        "GeneratedDefinitionStorage.h": ("\n".join(declarations) + "\n").encode(),
        "GeneratedDefinitions.cpp": ("\n".join(objects) + "\n").encode(),
        "GeneratedDefinitionLoading.cpp": ("\n".join(load) + "\n").encode(),
        **{"definitions/" + name: data for name, data in reanim["files"].items()},
        **{"definitions/" + name: data for name, data in particle["files"].items()},
    }
    summary = {
        "format": "pvz-static-definitions-v1",
        "reanim": reanim["summary"], "particle": particle["summary"],
        "projectile_types": len(visuals), "atlases": sorted(atlas_paths),
        "generated_files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                            for name, data in files.items()},
    }
    return files, summary
