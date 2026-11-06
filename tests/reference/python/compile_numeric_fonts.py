#!/usr/bin/env python3
"""Expand authored 1073 font DSL offline into pointer-free numeric font records.

Only image FILE PATH strings survive in the binary. Defines are expanded and
assigned local lexical IDs offline; layers use local lexical IDs, tags use one
lexical registry across the entire input corpus. Runtime never parses the DSL.
"""
from __future__ import annotations
from game_fixture import game_root

import argparse
import copy
import re
import struct
import zlib
import zipfile
from pathlib import Path, PurePosixPath

ROOT = game_root()
MAGIC, VERSION = 0x544E464E, 1
SCALARS = ('draw_mode', 'offset_x', 'offset_y', 'spacing', 'min_size', 'max_size',
           'point_size', 'ascent', 'ascent_padding', 'height', 'default_height',
           'line_spacing', 'base_order', 'alpha')
COMMANDS = ('Define', 'CreateHorzSpanRectList', 'SetDefaultPointSize', 'SetCharMap',
            'CreateLayer', 'CreateLayerFrom', 'LayerRequireTags', 'LayerExcludeTags',
            'LayerPointRange', 'LayerSetPointSize', 'LayerSetHeight', 'LayerSetImage',
            'LayerSetDrawMode', 'LayerSetColorMult', 'LayerSetColorAdd', 'LayerSetAscent',
            'LayerSetAscentPadding', 'LayerSetLineSpacingOffset', 'LayerSetOffset',
            'LayerSetCharWidths', 'LayerSetSpacing', 'LayerSetImageMap',
            'LayerSetCharOffsets', 'LayerSetKerningPairs', 'LayerSetBaseOrder',
            'LayerSetCharOrders', 'LayerSetExInfo')


def norm(path: str) -> str:
    return path.replace('\\', '/').lower()


def resolve_compiled_font_path(path: str) -> str:
    path = path.replace('\\', '/')
    if path.lower().endswith('.font.compiled'):
        raise ValueError(f'Legacy font path is offline-only: {path}')
    if path.lower().endswith('.compiled'):
        return path
    stem = path.rsplit('/', 1)[-1]
    if stem.lower().endswith('.txt'):
        stem = stem[:-4]
    return 'compiled/fonts/' + stem + '.compiled'


def descriptor_commands(raw: bytes):
    # Match DescParser::LoadDescriptor's semicolon separator and line comments.
    text = raw.decode('latin1').replace('\r', '')
    current, quote, escaped, line_start, skip = '', '', False, True, False
    for ch in text:
        if line_start and ch in ' \t\n':
            continue
        if line_start:
            skip = ch == '#'
            line_start = False
        if ch == '\n':
            line_start = True
            if skip:
                skip = False
                continue
        if skip:
            continue
        if escaped:
            current += '\\' + ch
            escaped = False
            continue
        if ch == '\\' and quote:
            escaped = True
            continue
        if ch in "\"'" and (not quote or ch == quote):
            quote = '' if quote else ch
        if ch == ';' and not quote:
            if current.strip():
                yield parse_list(current)
            current = ''
        else:
            current += ch
    if quote or escaped:
        raise ValueError('Unterminated descriptor quote')
    if current.strip():
        yield parse_list(current)


def parse_list(text: str):
    # Quoted atoms retain delimiters, exactly as ParseToList before GetValues.
    pos = 0

    def parse(nested=False):
        nonlocal pos
        result, atom, quote, escaped = [], '', '', False
        while pos < len(text):
            ch = text[pos]
            pos += 1
            if escaped:
                atom += ch
                escaped = False
            else:
                if ch in "\"'" and (not quote or ch == quote):
                    quote = '' if quote else ch
                if ch == '\\':
                    escaped = True
                elif not quote and ch in ' \t\n,()':
                    if ch == '(' and atom:
                        raise ValueError('Unexpected list start')
                    if atom:
                        result.append(atom)
                        atom = ''
                    if ch == '(':
                        result.append(parse(True))
                    elif ch == ')':
                        if not nested:
                            raise ValueError('Unexpected list end')
                        return result
                else:
                    atom += ch
        if quote or nested:
            raise ValueError('Unterminated list/quote')
        if atom:
            result.append(atom)
        return result

    return parse()


def unquote(value: str) -> str:
    if not value or value[0] not in "\"'":
        return value
    quote = value[0]
    result, last = '', False
    for ch in value:
        if ch == quote:
            if last:
                result += ch
            last = True
        else:
            result += ch
            last = False
    return result


def integer(value: str) -> int:
    if not re.fullmatch(r'-?(?:[0-9]+|0[xX][0-9a-fA-F]+)', value):
        raise ValueError(f'Invalid integer {value!r}')
    result = int(value, 16 if 'x' in value.lower() else 10)
    if not -2147483648 <= result <= 4294967295:
        raise ValueError('Integer outside 32-bit font representation')
    return result if result < 2147483648 else result - 4294967296


def wide_chars(value: str) -> list[int]:
    # Current production UTF8StringToWString(CP_UTF8,0) substitutes invalid
    # legacy bytes. GetCharData then casts each UTF-16 unit to unsigned char.
    text = value.encode('latin1').decode('utf-8', errors='replace').split('\0', 1)[0]
    units = text.encode('utf-16-le')
    return [struct.unpack_from('<H', units, i)[0] & 255 for i in range(0, len(units), 2)]


def new_layer(name: str, order: int):
    scalars = dict.fromkeys(SCALARS, 0)
    scalars.update(draw_mode=-1, min_size=-1, max_size=-1, base_order=order, alpha=1)
    return dict(name=name, image='', required=[], excluded=[], scalars=scalars,
                mult=[255]*4, add=[0]*4, glyphs={})


def glyph(layer, ch):
    # Non-ASCII writes in the existing font parser target its static miss slot;
    # that slot is never consulted by ASCII rendering or atlas/default-height.
    if ch >= 128:
        return dict(rect=[0]*4, offset=[0]*2, width=0, order=0, kern={})
    return layer['glyphs'].setdefault(ch, dict(rect=[0]*4, offset=[0]*2,
                                              width=0, order=0, kern={}))


class FontCompiler:
    def __init__(self, source_path: str, raw: bytes):
        self.source_path, self.raw = source_path, raw
        self.defines, self.layers = {}, []
        self.layer_map, self.char_map = {}, {}
        self.point_size = 0
        self.commands = list(descriptor_commands(raw))

    def values(self, source):
        if not isinstance(source, list):
            source = self.defines[source.upper()]
            if not isinstance(source, list):
                raise ValueError('List required')
            return copy.deepcopy(source)
        result = []
        for value in source:
            if isinstance(value, list):
                result.append(self.values(value))
            elif value and value[0] in "\"'":
                result.append(unquote(value))
            elif value and value[0] in '0123456789-+':
                result.append(value)
            elif value:
                result.append(copy.deepcopy(self.defines[value.upper()]))
        return result

    def string(self, source):
        if isinstance(source, list):
            raise ValueError('Scalar required')
        value = self.defines.get(source.upper(), source)
        if isinstance(value, list):
            raise ValueError('Scalar define required')
        return unquote(value)

    def ints(self, source):
        return [integer(v) for v in self.values(source)]

    def run(self):
        expected = dict(zip((c.lower() for c in COMMANDS),
                            (2,3,1,2,1,2,2,2,3,2,2,2,2,2,2,2,2,2,2,3,2,3,3,3,2,3,0)))
        scalar_commands = {
            'layersetpointsize':'point_size', 'layersetheight':'height',
            'layersetdrawmode':'draw_mode', 'layersetascent':'ascent',
            'layersetascentpadding':'ascent_padding', 'layersetlinespacingoffset':'line_spacing',
            'layersetspacing':'spacing', 'layersetbaseorder':'base_order'}
        for params in self.commands:
            command, *a = params
            command = command.lower()
            if command not in expected:
                raise ValueError(f'Unknown font command {command}')
            if command != 'layersetexinfo' and len(a) != expected[command]:
                raise ValueError(f'Wrong argument count for {command}')
            if command == 'define':
                name, value = a
                name = name.upper()
                self.defines.pop(name, None)
                self.defines[name] = self.values(value) if isinstance(value,list) else copy.deepcopy(self.defines.get(value.upper(),value))
            elif command == 'createhorzspanrectlist':
                rect, widths = self.ints(a[1]), self.ints(a[2])
                x = rect[0]
                result = []
                for width in widths:
                    result.append([str(x), str(rect[1]), str(width), str(rect[3])])
                    x += width
                self.defines[a[0].upper()] = result
            elif command == 'setdefaultpointsize':
                self.point_size = integer(a[0])
            elif command == 'setcharmap':
                source, target = self.values(a[0]), self.values(a[1])
                if len(source) != len(target):
                    raise ValueError('Char map size mismatch')
                for from_, to in zip(source,target):
                    if len(from_.encode('latin1')) != 1 or len(to.encode('latin1')) != 1:
                        raise ValueError('Single byte char map required')
                    self.char_map.setdefault(ord(from_),ord(to))
            elif command in ('createlayer','createlayerfrom'):
                name = a[0].upper()
                if name in self.layer_map:
                    raise ValueError('Duplicate layer')
                layer = new_layer(name,len(self.layers)) if command == 'createlayer' else copy.deepcopy(self.layer_map[a[1].upper()])
                layer['name'], layer['scalars']['base_order'] = name,len(self.layers)
                self.layers.append(layer)
                self.layer_map[name] = layer
            elif command == 'layersetexinfo':
                pass  # The existing production command is intentionally a no-op.
            else:
                layer = self.layer_map[a[0].upper()]
                if command in scalar_commands:
                    value = integer(a[1])  # These setters do NOT dereference defines.
                    if command == 'layersetdrawmode' and value not in (0,1):
                        raise ValueError('Invalid draw mode')
                    layer['scalars'][scalar_commands[command]] = value
                elif command == 'layerpointrange':
                    layer['scalars']['min_size'],layer['scalars']['max_size'] = map(integer,a[1:])
                elif command == 'layersetoffset':
                    layer['scalars']['offset_x'],layer['scalars']['offset_y'] = self.ints(a[1])
                elif command in ('layerrequiretags','layerexcludetags'):
                    layer['required' if command == 'layerrequiretags' else 'excluded'] += [s.upper() for s in self.values(a[1])]
                elif command == 'layersetimage':
                    path = self.string(a[1]).replace('\\','/')
                    layer['image'] = path if ':' in path or path.startswith('/') else str(PurePosixPath(self.source_path).parent / path)
                elif command in ('layersetcolormult','layersetcoloradd'):
                    if isinstance(a[1],list):
                        color = [int(float(v)*255) for v in self.values(a[1])]
                        if len(color)!=4:
                            raise ValueError('RGBA list required')
                    else:
                        c = integer(a[1])
                        color = [(c>>16)&255,(c>>8)&255,c&255,((c>>24)&255) or 255]
                    layer['mult' if command=='layersetcolormult' else 'add'] = color
                else:
                    chars, values = self.values(a[1]), self.values(a[2])
                    if len(chars)!=len(values):
                        raise ValueError('Glyph list size mismatch')
                    for ch, value in zip(chars,values):
                        units = list(ch.encode('latin1')) if command == 'layersetcharorders' else wide_chars(ch)
                        if len(units)!=(2 if command=='layersetkerningpairs' else 1):
                            raise ValueError('Invalid character/pair')
                        data = glyph(layer,units[0])
                        if command=='layersetcharwidths':
                            data['width']=integer(value)
                        elif command=='layersetcharorders':
                            data['order']=integer(value)
                        elif command=='layersetkerningpairs':
                            data['kern'][units[1]]=integer(value)
                        elif command in ('layersetimagemap','layersetcharoffsets'):
                            data['rect' if command=='layersetimagemap' else 'offset'] = [integer(v) for v in value]
                        else:
                            raise ValueError(f'Unhandled font command {command}')
                    if command=='layersetimagemap':
                        if not layer['image']:
                            raise ValueError('Image map precedes image')
                        layer['scalars']['default_height'] = max([0]+[g['rect'][3]+g['offset'][1] for g in layer['glyphs'].values()])
        if not self.layers or any(not l['image'] for l in self.layers):
            raise ValueError('Each production layer needs an image path')
        self.define_ids = {name:i for i,name in enumerate(sorted(self.defines))}
        layer_ids = {name:i for i,name in enumerate(sorted(self.layer_map))}
        for layer in self.layers:
            layer['id']=layer_ids[layer['name']]
        return self

    def numeric_state(self, tag_ids):
        layers = []
        for layer in self.layers:
            layers.append(dict(id=layer['id'],image=layer['image'],
                               required=[tag_ids[t] for t in layer['required']],
                               excluded=[tag_ids[t] for t in layer['excluded']],
                               scalars=list(layer['scalars'][s] for s in SCALARS),
                               mult=layer['mult'],add=layer['add'],glyphs=layer['glyphs']))
        return dict(point_size=self.point_size,char_map=self.char_map,layers=layers)

    def encode(self, tag_ids):
        state = self.numeric_state(tag_ids)
        payload = b''.join(struct.pack('<II',f,t) for f,t in sorted(state['char_map'].items()))
        for layer in state['layers']:
            image=layer['image'].encode('utf-8')
            payload += struct.pack('<II',layer['id'],len(image))+image
            for key in ('required','excluded'):
                ids=layer[key]
                payload += struct.pack('<I',len(ids))+b''.join(struct.pack('<I',v) for v in ids)
            payload += struct.pack('<22i',*(layer['scalars']+layer['mult']+layer['add']))
            payload += struct.pack('<I',len(layer['glyphs']))
            for ch,g in sorted(layer['glyphs'].items()):
                payload += struct.pack('<I8iI',ch,*(g['rect']+g['offset']+[g['width'],g['order']]),len(g['kern']))
                payload += b''.join(struct.pack('<Ii',n,v) for n,v in g['kern'].items())
        return struct.pack('<4Ii3I',MAGIC,VERSION,len(payload),zlib.crc32(payload),
                           self.point_size,len(self.char_map),len(self.layers),len(tag_ids))+payload


def load_corpus(source_dir: Path):
    fonts=[]
    for source in sorted(source_dir.glob('*.txt')):
        raw=source.read_bytes()
        if not re.search(rb'(?i)\bCreateLayer\b',raw):
            raise ValueError(f'Invalid authored font description: {source}')
        fonts.append(FontCompiler('data/' + source.name,raw).run())
    if not fonts:
        raise ValueError('No authored font descriptions in archive')
    return fonts


def tag_registry(fonts):
    return {tag:i for i,tag in enumerate(sorted({t for font in fonts for layer in font.layers
                                               for t in layer['required']+layer['excluded']}))}


def main():
    ap=argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--archive',type=Path,default=ROOT/'out/x64-win32-dx12/Release/main.pak64')
    ap.add_argument('--source-dir',type=Path,default=ROOT/'assets/definitions/fonts')
    ap.add_argument('--output-dir',type=Path)
    ap.add_argument('--verify',action='store_true',help='Compare existing staged bytes without writing')
    args=ap.parse_args()
    fonts=load_corpus(args.source_dir)
    tags=tag_registry(fonts)
    output=args.output_dir or args.archive.parent
    for font in fonts:
        path=output/resolve_compiled_font_path(font.source_path)
        data=font.encode(tags)
        if args.verify:
            if path.read_bytes()!=data:
                raise ValueError(f'Stale compiled font: {path}')
        else:
            path.parent.mkdir(parents=True,exist_ok=True)
            path.write_bytes(data)
        print(f'{font.source_path}: layers={len(font.layers)} defines={len(font.define_ids)} bytes={len(data)} -> {path}')
    print(f'Compiled fonts={len(fonts)} global-tag-ids={tags} commands-supported={len(COMMANDS)}')


if __name__=='__main__':
    main()
