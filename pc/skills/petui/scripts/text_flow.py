"""p4-longtext-v1 deterministic layout, the same rules as firmware/main/pet_p4_text_flow.h.

Metric model (normative, not a glyph table): scalars >= U+2E80 advance one font
size, others half a font size rounded up. Lines break after spaces, before/after
wide scalars and at '\\n'; trailing spaces are trimmed and leading spaces skipped.
"""
ENGINE='p4-longtext-v1'
MAX_BYTES=4096
MAX_LINES=256
LINE_BYTES=120
def valid_text(text,maximum=MAX_BYTES):
    if not isinstance(text,str):return False
    try:raw=text.encode('utf-8')
    except UnicodeError:return False
    if not 1<=len(raw)<=maximum:return False
    return not any((ord(c)<32 and c!='\n') or 127<=ord(c)<160 or 0xd800<=ord(c)<=0xdfff for c in text)
def wide(cp):return cp>=0x2e80
def advance(cp,font_size):return font_size if wide(cp) else (font_size+1)//2
def lines(text,width,font_size,max_lines=MAX_LINES):
    """Byte-aligned list of line strings (identical to the C byte ranges) and a truncated flag."""
    raw=text.encode('utf-8');out=[];truncated=False
    if not raw or width<font_size or font_size<10:return [],False
    table={};i=0
    for ch in text:
        n=len(ch.encode('utf-8'));table[i]=(n,ord(ch));i+=n
    total=i
    def emit(start,end):
        nonlocal truncated
        while end>start and raw[end-1]==32:end-=1
        if len(out)<max_lines:out.append(raw[start:end].decode('utf-8'))
        else:truncated=True
    line_start=0;line_width=0;last_break=0;i=0
    while i<total:
        n,cp=table[i]
        if cp==10:
            emit(line_start,i);i+=n;line_start=i;line_width=0;last_break=0
            if truncated:break
            continue
        w=advance(cp,font_size)
        if wide(cp) and i>line_start:last_break=i
        if (line_width+w>width or i+n-line_start>LINE_BYTES) and i>line_start:
            cut=last_break if last_break>line_start else i
            emit(line_start,cut)
            if truncated:break
            line_start=cut
            while line_start<total and raw[line_start]==32:line_start+=1
            line_width=0;last_break=0;k=line_start
            while k<i:
                m=table[k];line_width+=advance(m[1],font_size);k+=m[0]
            if line_start>i:i=line_start
            continue
        line_width+=w;i+=n
        if cp==32 or wide(cp):last_break=i
    if not truncated and (i>line_start or not out):emit(line_start,i)
    return out,truncated
def per_page(h,line_height):return max(1,h//line_height) if line_height else 1
def pages(count,per):return max(1,(count+per-1)//per) if per else 1
def page_index(value,count):return value%count if count else 0
def page_lines(text,width,font_size,h,line_height,value):
    rows,_=lines(text,width,font_size);per=per_page(h,line_height);total=pages(len(rows),per);at=page_index(value,total)
    return rows[at*per:(at+1)*per],total
