"""Compile a bounded, licensed 32px antialiased glyph subset, not UI screenshots."""
from pathlib import Path
from PIL import Image, ImageDraw, ImageFont

def strings(value):
    if isinstance(value,str): yield value
    elif isinstance(value,dict):
        for v in value.values(): yield from strings(v)
    elif isinstance(value,list):
        for v in value: yield from strings(v)

def add_fonts(root,runtime,application=None,max_chars=91):
    repo=Path(__file__).resolve().parents[2]
    base=repo/'firmware/managed_components/lvgl__lvgl/scripts/built_in_font'
    font=ImageFont.truetype(str(base/'SourceHanSansSC-Normal.otf'),32)
    # Prioritize every large dynamic headline before the smaller body labels.
    ordered=['0123456789:%.-/°C— ']
    library=bool(application and application.get('engine')=='pc-app-services-v3' and application.get('service')=='knowledge')
    if library:
        ordered += ['每日新知选择分类换一条确认读本页停止朗收藏上一项下一返回物理化学生历史人文经济心理健康天文地理百科节选本轮不重复']
    dashboard=runtime['dashboard']
    for key in ('headline','metricValue','badge','metricLabel','metricUnit','title','eyebrow','note'):
        ordered.extend(strings(dashboard.get(key,'')))
        if key=='headline' and application and not library:
            ordered.extend(c.get('title','') for c in application.get('content',[]))
    ordered.extend(n.get('text','') for n in runtime['surface']['elements'])
    chars=[]
    for text in ordered:
        for c in text:
            if c.isprintable() and ord(c)<0x10000 and c not in chars:chars.append(c)
    chars=chars[:max_chars] # A smaller bank leaves room for interactive sprites.
    fonts=[]
    for bank,start in enumerate(range(0,len(chars),49)):
        selected=chars[start:start+49];rows=(len(selected)+6)//7
        atlas=Image.new('RGBA',(224,rows*32));metrics=[]
        for i,c in enumerate(selected):
            cell=Image.new('L',(32,32));draw=ImageDraw.Draw(cell)
            # Keep the full em and descenders inside the cell; no per-glyph resizing.
            draw.text((0,28),c,font=font,fill=255,anchor='ls')
            bounds=cell.getbbox();advance=min(32,max(4,round(font.getlength(c))))
            metrics.append([advance,bounds[1] if bounds else 0,bounds[3] if bounds else 0])
            rgba=Image.new('RGBA',(32,32),(255,255,255,0));rgba.putalpha(cell)
            atlas.paste(rgba,(i%7*32,i//7*32))
        name=f'type{bank}';atlas.save(root/f'assets/{name}.png',optimize=True)
        runtime['surface']['images'].append({'id':name,'asset':f'assets/{name}.png','frame_width':atlas.width,'frame_height':atlas.height,'frames':1,'fps':1})
        fonts.append({'image':name,'chars':''.join(selected),'metrics':metrics})
    runtime['surface'].update(typography='p4-app-type-v1',fonts=fonts)
    (root/'licenses').mkdir(exist_ok=True)
    (root/'licenses/petui-type-OFL.txt').write_text((base/'font_license/SourceHanSansSC/LICENSE.txt').read_text(),encoding='utf-8')
