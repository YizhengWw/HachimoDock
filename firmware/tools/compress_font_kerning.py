"""Generate exact sparse startup tables from the retained native LVGL maps."""
from pathlib import Path
import argparse
import re

ROOT=Path(__file__).resolve().parents[1]

def generated():
    source=(ROOT/"main/pet_p4_font_cn.c").read_text()
    arrays=[]
    for name in ["kern_left_class_mapping","kern_right_class_mapping"]:
        match=re.search(r"static const uint8_t "+name+r"\[\] =\s*\{(.*?)\};",source,re.S)
        if not match:raise ValueError("Original native kerning array missing")
        arrays.append([int(v) for v in re.findall(r"\b\d+\b",match[1])])
    left,right=arrays
    if len(left)!=len(right) or not left or len(left)>65535:raise ValueError("Invalid map bounds")
    value=2166136261
    for byte in left+right:
        if not 0<=byte<=255:raise ValueError("Invalid class byte")
        value=((value^byte)*16777619)&0xffffffff
    rows=[(i,l,r) for i,(l,r) in enumerate(zip(left,right)) if l or r]
    return ("/* Native generated sparse kerning data; original complete arrays remain in\n"
            " * pet_p4_font_cn.c for exact verification and unpacked builds. */\n"
            "#pragma once\n"
            f"#define PET_FONT_KERN_RECORDS {len(left)}u\n"
            f"#define PET_FONT_KERN_FNV UINT32_C(0x{value:08x})\n"
            "typedef struct {uint16_t index;uint8_t left,right;} pet_font_kern_pair_t;\n"
            "static const pet_font_kern_pair_t pet_font_kern_pairs[] = {\n"
            +"".join(f"  {{{i},{l},{r}}},\n" for i,l,r in rows)+"};\n")

if __name__=="__main__":
    parser=argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check",action="store_true")
    args=parser.parse_args()
    output=ROOT/"main/pet_p4_font_kerning_sparse.h"
    text=generated()
    if args.check:
        if not output.exists() or output.read_text()!=text:raise SystemExit("Sparse kerning table is stale")
    else:output.write_text(text)
