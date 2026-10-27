// Export only advances/kerning, never glyph pixels, for host-side PetUI flow.
import fs from 'node:fs';
const source=new URL('../../firmware/main/pet_p4_font_cn.c',import.meta.url);
const text=fs.readFileSync(source,'utf8');
const array=name=>text.match(new RegExp(`\\b${name}\\[\\d*\\]\\s*=\\s*\\{([\\s\\S]*?)\\};`))[1].match(/-?0x[\da-f]+|-?\d+/gi).map(Number);
const advances=[...text.matchAll(/\.bitmap_index = \d+, \.adv_w = (\d+)/g)].map(m=>+m[1]);
const left=array('kern_left_class_mapping'),right=array('kern_right_class_mapping');
const glyphs={};
for(const m of text.matchAll(/\.range_start = (\d+), \.range_length = (\d+), \.glyph_id_start = (\d+),\s*\.unicode_list = (\w+), \.glyph_id_ofs_list = NULL, \.list_length = \d+, \.type = (\w+)/g)){
 const [start,length,gid]=m.slice(1,4).map(Number);
 const offsets=m[4]==='NULL'?Array.from({length},(_,i)=>i):array(m[4]);
 for(const [i,offset]of offsets.entries()){
  const g=gid+i,v=[advances[g],left[g],right[g]];
  if(v.some(n=>n===undefined))throw Error('Incomplete font metadata');
  if(v[0]!==256||v[1]||v[2])glyphs[start+offset]=v;
 }
}
const output=JSON.stringify({glyphs,pairs:array('kern_class_values'),rightClasses:46});
const destination=new URL('../src-tauri/src/app-text-metrics.json',import.meta.url);
if(process.argv.includes('--check')){
 if(fs.readFileSync(destination,'utf8')!==output+'\n')throw Error('P4 text metrics are stale');
}else fs.writeFileSync(destination,output+'\n');
console.log(`P4 text metrics: ${Object.keys(glyphs).length} non-default advances; ${output.length} bytes`);
