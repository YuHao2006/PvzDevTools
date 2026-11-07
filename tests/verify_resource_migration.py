"""Compare native resource outputs with the frozen Python/reference package."""
from __future__ import annotations
import argparse
import io
import json
import os
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile

from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--game-root', type=Path, required=True)
    parser.add_argument('--builder', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--cache', type=Path, required=True)
    parser.add_argument('--generated', type=Path, required=True)
    parser.add_argument('--reference-package', type=Path, required=True)
    args = parser.parse_args()
    root = args.game_root.resolve()
    os.environ['PVZ_GAME_ROOT'] = str(root)
    sys.path.insert(0, str(Path(__file__).parent / 'reference/python'))
    import compile_numeric_fonts as fonts
    import compile_numeric_resources as resources
    import exact_atlas
    import resource_build_config
    import definition_catalog
    with tempfile.TemporaryDirectory(prefix='pvz-native-contract-') as temporary:
        temp = Path(temporary)
        def run(command, *extra):
            subprocess.run([str(args.builder), command, *map(str, extra)], check=True)
        run('config', '--root', root, '--output', temp / 'config.json')
        native = json.loads((temp / 'config.json').read_text())
        config = resource_build_config.load(root / 'assets/definitions/ResourceBuild.pvzbuild')
        definition_catalog.validate_runtime_orders(config, root)
        assert native['type_map'].encode() == definition_catalog.reanim_type_map(config)
        assert native['order'] == {k: [list(row) for row in v] for k, v in config.definition_orders.items()}
        run('fonts', '--root', root, '--output', temp)
        corpus = fonts.load_corpus(root / 'assets/definitions/fonts')
        tags = fonts.tag_registry(corpus)
        for font in corpus:
            path = fonts.resolve_compiled_font_path(font.source_path).lower()
            assert (temp / path).read_bytes() == font.encode(tags), path
        run('resources', '--root', root, '--output', temp)
        expected, plan, crc, names = resources.compile_manifest(
            root, (root / 'assets/definitions/resources.json').read_bytes(), [], font_tag_ids=tags)
        assert (temp / 'resources.compiled').read_bytes() == expected
        print(f'PASS: configuration, definition order, {len(corpus)} fonts and NRSC4 wire bytes')
        # Exercise the actual C++ solver, not only completed production caches.
        sizes = [[17, 11], [13, 19], [7, 5], [11, 9]]
        (temp / 'sizes.json').write_text(json.dumps(sizes))
        command = ['--input', temp / 'sizes.json', '--compiler', args.compiler,
                   '--cache', temp / 'solver', '--output', temp / 'layout.json',
                   '--seconds', 1, '--require-optimal', 'false']
        run('atlas', *command)
        layout = json.loads((temp / 'layout.json').read_text())
        exact_atlas._validate([tuple(v) for v in sizes], layout, 2, 4096)
        key = exact_atlas._cache_key(sizes, 2, 4096, exact_atlas._version())
        cached = exact_atlas._read_cache(temp / 'solver/atlas-layout-cache' / (key + '.json'),
                                        key, [tuple(v) for v in sizes], 2, 4096, exact_atlas._version())
        assert cached is not None, 'Native proof cache is incompatible with validated Python cache'
        run('atlas', *command)
        assert json.loads((temp / 'layout.json').read_text())['exact']['cache_hit']
        print('PASS: native CP-SAT search, legal bounds, compatible digest and completed-cache reuse')
    with zipfile.ZipFile(args.reference_package) as reference, zipfile.ZipFile(args.cache / 'main.pak64') as native:
        assert set(reference.namelist()) == set(native.namelist()), 'Resource closure differs'
        exact, images = 0, 0
        for name in reference.namelist():
            before, after = reference.read(name), native.read(name)
            if before == after:
                exact += 1
                continue
            assert name.endswith('.png'), f'Nonimage resource bytes differ: {name}'
            a = Image.open(io.BytesIO(before)).convert('RGBA')
            b = Image.open(io.BytesIO(after)).convert('RGBA')
            assert a.size == b.size and a.tobytes() == b.tobytes(), f'PNG pixels differ: {name}'
            images += 1
        print(f'PASS: {len(native.namelist())} package entries; {exact} byte-identical, {images} PNGs pixel-identical')
    for path in args.generated.rglob('*'):
        if not path.is_file() or path.name == 'ReanimAttacherMasks.h':
            continue  # Generator attribution changed; integer mask data is checked separately below.
        expected = root / 'pvzPerf/Sexy.TodLib/generated' / path.relative_to(args.generated)
        assert path.read_bytes() == expected.read_bytes(), f'Generated C++ differs: {path.name}'
    import re
    masks = re.compile(r'0x[0-9a-f]+ull')
    assert masks.findall((args.generated / 'ReanimAttacherMasks.h').read_text()) == masks.findall(
        (root / 'pvzPerf/Sexy.TodLib/generated/ReanimAttacherMasks.h').read_text())
    print('PASS: generated C++ contracts and duplicate-attacher masks')


if __name__ == '__main__':
    main()
