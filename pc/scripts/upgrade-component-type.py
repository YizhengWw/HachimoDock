"""Build real 48px numeric glyphs in an independent upgraded package directory.

No artwork resizing and no relaxed image budgets. Metrics stay in 32/em units.
"""
import json
import math
import sys
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

ROOT=Path(__file__).resolve().parents[2]
FONT=ROOT/'firmware/managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf'
TARGETS={'hachimi-garden','hachimi-break','drink-reminder','tomato-clock','token-usage'}

def bank(root, chars, ident, width, height, columns):
    font=ImageFont.truetype(str(FONT),height)
    image=Image.new('RGBA',(columns*width,math.ceil(len(chars)/columns)*height))
    metrics=[]
    for i,char in enumerate(chars):
        alpha=Image.new('L',(width,height));draw=ImageDraw.Draw(alpha)
        draw.text((0,round(height*.875)),char,font=font,fill=255,anchor='ls')
        box=alpha.getbbox()
        metrics.append([min(32,max(4,round(font.getlength(char)*32/height))),
                        math.floor(box[1]*32/height) if box else 0,
                        min(32,math.ceil(box[3]*32/height)) if box else 0])
        tile=Image.new('RGBA',alpha.size,(255,255,255,0));tile.putalpha(alpha)
        image.paste(tile,(i%columns*width,i//columns*height))
    asset=f'assets/{ident}.png';image.save(root/asset,optimize=True)
    declaration={'id':ident,'asset':asset,'frame_width':image.width,'frame_height':image.height,'frames':1,'fps':1}
    spec={'image':ident,'chars':chars,'metrics':metrics}
    if (width,height)!=(32,32):spec['cell']=[width,height]
    return declaration,spec

def upgrade(root):
    file=root/'runtime/widget.json';runtime=json.loads(file.read_text())
    surface=runtime['surface'];old=surface['fonts'];oldids={f['image'] for f in old}
    others=[im for im in surface['images'] if im['id'] not in oldids]
    budget=98304-sum(im['frame_width']*im['frame_height'] for im in others if not im.get('encoding'))
    digits='0123456789:'
    primary,spec=bank(root,digits,'type0',32,48,4)
    budget-=primary['frame_width']*primary['frame_height']
    assert budget>=0 and len(others)<4
    # No internal variable names from dictionary keys; prioritize visible prose.
    chars=[]
    def text(value):
        if isinstance(value,str):yield value
        elif isinstance(value,dict):
            for key,v in value.items():
                if key in ('switch_state','switch_page'):yield from text(v)
                elif key not in ('var','fmt_mmss','fmt_hms','pct_of','of_max'):yield from text(v)
    for phrase in [*text(runtime['dashboard']),*(n.get('text','') for n in surface['elements'])]:
        for c in phrase:
            if c not in digits and c not in chars and c.isprintable() and ord(c)<65536:chars.append(c)
    max_rows=min(7,budget//(7*1024))
    chars=chars[:min(35 if root.name=='tomato-clock' else 49,max_rows*7)]
    images=[*others,primary];fonts=[spec]
    if chars and len(images)<4:
        secondary,second=bank(root,''.join(chars),'type1',32,32,7)
        images.append(secondary);fonts.append(second)
    used={im['asset'] for im in images}
    for im in surface['images']:
        if im['id'] in oldids and im['asset'] not in used:(root/im['asset']).unlink()
    surface['images']=images;surface['fonts']=fonts
    file.write_text(json.dumps(runtime,ensure_ascii=False,indent=2)+'\n')
    return {'id':root.name,'numericCell':[32,48],'palettePixels':sum(im['frame_width']*im['frame_height'] for im in images if not im.get('encoding')),'glyphCount':sum(len(f['chars']) for f in fonts)}

if __name__=='__main__':
    directory=Path(sys.argv[1]);report=[upgrade(directory/name) for name in sorted(TARGETS)]
    (directory.parent/'typography-report.json').write_text(json.dumps(report,indent=2)+'\n')
    print(json.dumps(report))
