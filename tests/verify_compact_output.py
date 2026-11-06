"""Exercise native stored-ZIP/PNG compression without touching game assets."""
import argparse
import io
from pathlib import Path
import subprocess
import sys
import tempfile
import zipfile
from PIL import Image, PngImagePlugin


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--builder', type=Path, required=True)
    args = ap.parse_args()
    sys.path.insert(0, str(Path(__file__).parent / 'reference/python'))
    from compact_runtime_package import chunks
    with tempfile.TemporaryDirectory(prefix='pvz-native-compact-') as directory:
        root = Path(directory)
        image = Image.new('RGBA', (37, 29))
        image.putdata([(x*7 % 256, y*11 % 256, (x+y)*3 % 256, (x*y) % 256)
                       for y in range(image.height) for x in range(image.width)])
        metadata = PngImagePlugin.PngInfo()
        metadata.add_text('Purpose', 'native compression regression')
        png = io.BytesIO()
        image.save(png, 'PNG', pnginfo=metadata, compress_level=1)
        source = {'compiled/atlases/test.atlas.png': png.getvalue(),
                  'data/font.png': png.getvalue(), 'sounds/test.bin': b'unchanged\0payload'}
        archive = root / 'main.pak64'
        with zipfile.ZipFile(archive, 'w', compression=zipfile.ZIP_STORED) as z:
            for name, data in source.items():
                info = zipfile.ZipInfo(name, (1980, 1, 1, 0, 0, 0))
                info.create_system, info.external_attr = 0, 0x20
                z.writestr(info, data)
        for mode in ('lossless', 'light-lossy'):
            output = root / (mode + '.pak64')
            subprocess.run([str(args.builder), 'compact', '--input', str(archive),
                            '--output', str(output), '--mode', mode], check=True)
            with zipfile.ZipFile(output) as z:
                assert set(z.namelist()) == set(source)
                for name, before in source.items():
                    after = z.read(name)
                    if not name.endswith('.png'):
                        assert after == before
                        continue
                    old = Image.open(io.BytesIO(before)).convert('RGBA')
                    new = Image.open(io.BytesIO(after)).convert('RGBA')
                    assert old.size == new.size
                    assert old.getchannel('A').tobytes() == new.getchannel('A').tobytes()
                    maximum = max(abs(a-b) for a, b in zip(old.tobytes(), new.tobytes()))
                    assert maximum <= (2 if mode == 'light-lossy' and name.startswith('compiled/') else 0)
                    assert [(k,v) for k,v in chunks(before) if k != b'IDAT'] == [
                        (k,v) for k,v in chunks(after) if k != b'IDAT']
                assert all(i.compress_type == zipfile.ZIP_STORED for i in z.infolist())
            assert archive.read_bytes()  # Source remains independently readable.
        # Truncation must fail cleanly and preserve a previously published output.
        (root / 'invalid.pak64').write_bytes(b'PK\x03\x04broken')
        previous = (root / 'lossless.pak64').read_bytes()
        result = subprocess.run([str(args.builder), 'compact', '--input', str(root / 'invalid.pak64'),
                                 '--output', str(root / 'lossless.pak64'), '--mode', 'lossless'],
                                capture_output=True)
        assert result.returncode and (root / 'lossless.pak64').read_bytes() == previous
    print('PASS: lossless/lossy RGB limits, alpha, PNG metadata, stored ZIP and failure preservation')


if __name__ == '__main__':
    main()
