"""Exercise the atlas-only CLI without building behavior definitions or a PAK."""
import argparse
import hashlib
import json
from pathlib import Path
import subprocess
from PIL import Image


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--builder', type=Path, required=True)
    parser.add_argument('--compiler', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    root = args.output.resolve()
    root.mkdir(parents=True, exist_ok=True)
    first = Image.new('RGBA', (16, 12))
    first.putdata([(x * 11, y * 13, 91, (x * 17 + y * 23) % 256)
                   for y in range(12) for x in range(16)])
    second = Image.new('RGBA', (9, 7), (23, 87, 144, 77))
    first.save(root / 'first.png')
    second.save(root / 'second.png')
    manifest = {'format': 'pvz-sprite-atlas-v1', 'name': 'TestAtlas',
                'time_limit_seconds': 1, 'require_optimal': False,
                'sprites': [{'name': 'FIRST', 'image': 'first.png', 'rect': [2, 3, 8, 6],
                             'logical_size': [4, 3]},
                            {'name': 'SECOND', 'image': 'second.png'}]}
    path = root / 'input.json'
    output = root / 'atlas'
    command = [str(args.builder.resolve()), 'pack-atlas', '--input', str(path),
               '--compiler', str(args.compiler.resolve()), '--output', str(output),
               '--cache', str(root / 'cache')]

    def run(document, expected=0):
        path.write_text(json.dumps(document), encoding='utf-8')
        result = subprocess.run(command, capture_output=True, text=True)
        assert (result.returncode == 0) == (expected == 0), result.stdout + result.stderr

    run(manifest)
    report = json.loads((output / 'TestAtlas.json').read_text())
    atlas = Image.open(output / 'TestAtlas.png').convert('RGBA')
    assert max(atlas.size) <= 4096
    assert [x['name'] for x in report['sprites']] == ['FIRST', 'SECOND']
    for row, source in zip(report['sprites'], [first.crop((2, 3, 10, 9)), second]):
        x, y, w, h = row['region']
        assert atlas.crop((x, y, x + w, y + h)).tobytes() == source.tobytes()
        assert row['rgba_sha256'] == hashlib.sha256(source.tobytes()).hexdigest()
        assert row['uv'] == [x / atlas.width, y / atlas.height,
                             (x + w) / atlas.width, (y + h) / atlas.height]
        # Gutter copies the exact neighboring RGBA, including transparent RGB.
        assert atlas.getpixel((x - 2, y - 2)) == source.getpixel((0, 0))
        assert atlas.getpixel((x + w + 1, y + h + 1)) == source.getpixel((w - 1, h - 1))
    assert report['sprites'][0]['logical_size'] == [4, 3]
    assert {p.name for p in output.iterdir()} == {'TestAtlas.png', 'TestAtlas.json',
                                                 'TestAtlas.h', 'TestAtlas.cpp'}
    snapshot = {p.name: p.read_bytes() for p in output.iterdir()}
    run(manifest)
    # A cache hit can legitimately change proof/provenance fields in JSON.
    for name in ['TestAtlas.png', 'TestAtlas.h', 'TestAtlas.cpp']:
        assert (output / name).read_bytes() == snapshot[name]
    assert json.loads((output / 'TestAtlas.json').read_text())['sprites'] == report['sprites']
    snapshot = {p.name: p.read_bytes() for p in output.iterdir()}
    for edit in [lambda d: d['sprites'][1].update(name='FIRST'),
                 lambda d: d['sprites'][1].update(image='missing.png'),
                 lambda d: d['sprites'][1].update(rect=[8, 0, 9, 7]),
                 lambda d: d['sprites'][1].update(logical_size=[0, 7]),
                 lambda d: d.update(name='class')]:
        invalid = json.loads(json.dumps(manifest))
        edit(invalid)
        run(invalid, expected=1)
        assert {p.name: p.read_bytes() for p in output.iterdir()} == snapshot
    print('PASS atlas-only crop, exact RGBA/alpha, gutter, UV, logical size, deterministic cache, '
          'no PAK/behavior output, invalid inputs preserve old outputs')


if __name__ == '__main__':
    main()
