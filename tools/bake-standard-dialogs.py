"""Author two compact standard dialog bodies from the original image pieces.

Run once when those source images change. Production resource builds pack the
resulting PNGs directly, with no Python or runtime render-target dependency.
"""
import argparse
import json
from pathlib import Path
from PIL import Image

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument('root', type=Path)
root = parser.parse_args().root.resolve()
folder = root/'assets/runtime/images'
for tall in (False, True):
    rows = ['top', 'center', 'bigbottom' if tall else 'bottom']
    pieces = [[Image.open(folder/f'dialog_{row}{col}.png').convert('RGBA')
               for col in ['left','middle','right']] for row in rows]
    heights = [max(im.height for im in row) for row in pieces]
    canvas = Image.new('RGBA', (320, sum(heights)))
    y = 0
    for row,height in zip(pieces,heights):
        x = 0
        for im in row:
            canvas.alpha_composite(im,(x,y))
            x += im.width
        y += height
    canvas.save(folder/('dialog_baked_tall.png' if tall else 'dialog_baked_normal.png'))
    print(('tall' if tall else 'normal'),canvas.size,len(canvas.tobytes()))
author = root/'assets/definitions/resources.json'
data = json.loads(author.read_text(encoding='utf-8'))
def walk(node):
    if isinstance(node,dict):
        children=node.get('children',[])
        if any(c.get('attrs',{}).get('id')=='DIALOG_HEADER' for c in children):
            children[:]=[c for c in children if c.get('attrs',{}).get('id') not in {'DIALOG_BAKED_NORMAL','DIALOG_BAKED_TALL'}]
            index=next(i for i,c in enumerate(children) if c.get('attrs',{}).get('id')=='DIALOG_HEADER')+1
            children[index:index]=[{'tag':'Image','attrs':{'id':'DIALOG_BAKED_'+kind,'path':'dialog_baked_'+kind.lower()},'children':[]} for kind in ['NORMAL','TALL']]
        for value in node.values():walk(value)
    elif isinstance(node,list):
        for item in node:walk(item)
walk(data)
author.write_text(json.dumps(data,ensure_ascii=False,indent=2)+'\n',encoding='utf-8')
