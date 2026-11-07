"""Verify bounded atlases and lossless font folding against a prior package."""
import argparse
import io
import struct
import zipfile
import zlib
from PIL import Image


class Reader:
    def __init__(self, data): self.data, self.at = data, 0
    def values(self, fmt):
        result = struct.unpack_from('<' + fmt, self.data, self.at)
        self.at += struct.calcsize('<' + fmt)
        return result
    def word(self): return self.values('I')[0]
    def string(self):
        size = self.word()
        value = self.data[self.at:self.at + size].decode()
        self.at += size
        return value


def font(data):
    r = Reader(data)
    magic, version, size, crc, point, chars, layers, tags = r.values('4Ii3I')
    assert magic == 0x544e464e and version == 1 and size == len(data) - 32
    assert zlib.crc32(data[32:]) == crc
    mapping = [r.values('II') for _ in range(chars)]
    result = []
    for _ in range(layers):
        layer = dict(id=r.word(), image=r.string())
        layer['tags'] = [[r.word() for _ in range(r.word())] for _ in range(2)]
        layer['metrics'] = r.values('22i')
        glyphs = {}
        for _ in range(r.word()):
            ch, *geometry, count = r.values('I8iI')
            glyphs[ch] = (geometry[:4], geometry[4:], [r.values('Ii') for _ in range(count)])
        layer['glyphs'] = glyphs
        result.append(layer)
    assert r.at == len(data)
    return (point, mapping, tags), result


def image(package, path):
    path = path.lower()
    def read(stem):
        for name in [stem] + [stem + ext for ext in ('.tga', '.jpg', '.png', '.gif')]:
            if name in package.namelist():
                return Image.open(io.BytesIO(package.read(name))).convert('RGBA')
    value = read(path)
    directory, name = path.rsplit('/', 1)
    mask = read(directory + '/_' + name) or read(path + '_')
    if mask is not None:
        if value is None: value = Image.new('RGBA', mask.size, 'white')
        value.putalpha(mask.getchannel('B'))
    assert value is not None, path
    return value


def main():
    p = argparse.ArgumentParser()
    p.add_argument('--package', required=True)
    p.add_argument('--reference', required=True)
    args = p.parse_args()
    with zipfile.ZipFile(args.reference) as before, zipfile.ZipFile(args.package) as after:
        images = glyphs = moved = 0
        for name in after.namelist():
            if name.endswith(('.png', '.jpg', '.gif')):
                with Image.open(io.BytesIO(after.read(name))) as decoded:
                    assert max(decoded.size) <= 4096, (name, decoded.size)
                    images += 1
            if not name.startswith('compiled/fonts/') or not name.endswith('.compiled'): continue
            old_meta, old_layers = font(before.read(name))
            new_meta, new_layers = font(after.read(name))
            assert old_meta == new_meta and len(old_layers) == len(new_layers)
            for a, b in zip(old_layers, new_layers):
                assert (a['id'], a['tags'], a['metrics']) == (b['id'], b['tags'], b['metrics'])
                assert a['glyphs'].keys() == b['glyphs'].keys()
                source, dest = image(before, a['image']), image(after, b['image'])
                moved += a['image'] != b['image']
                for ch in a['glyphs']:
                    ar, am, ak = a['glyphs'][ch]
                    br, bm, bk = b['glyphs'][ch]
                    assert am == bm and ak == bk and ar[2:] == br[2:]
                    def crop(im, r): return im.crop((r[0], r[1], r[0] + r[2], r[1] + r[3])).tobytes()
                    assert crop(source, ar) == crop(dest, br), (name, ch)
                    glyphs += 1
        assert moved > 0
        print(f'PASS: {images} images bounded by 4096; {glyphs} glyph pixels/metrics exact; {moved} folded layers')


if __name__ == '__main__': main()
