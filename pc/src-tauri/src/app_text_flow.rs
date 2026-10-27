//! Host-side wrapping mirrors P4 advances, including the package font subset.
//! Keeps legacy snapshot slots bounded; geometry remains owned by the package.
use serde::Deserialize;
use serde_json::Value;
use std::{collections::HashMap,sync::OnceLock};
#[derive(Deserialize)]#[serde(rename_all="camelCase")]
struct Metrics{glyphs:HashMap<u32,[i32;3]>,pairs:Vec<i32>,right_classes:usize}
fn metrics()->&'static Metrics{static M:OnceLock<Metrics>=OnceLock::new();M.get_or_init(||serde_json::from_str(include_str!("app-text-metrics.json")).unwrap())}
#[derive(Clone)]pub struct Flow{blocks:Vec<(usize,usize,usize)>,subset:HashMap<char,i32>}
#[derive(Debug)]pub struct Page{pub start:usize,pub parts:Vec<String>}
impl Flow{
 pub fn from_surface(surface:&Value)->Self{
  let mut subset=HashMap::new();
  if let Some(fonts)=surface["fonts"].as_array(){for f in fonts{if let Some(chars)=f["chars"].as_str(){for(i,c)in chars.chars().enumerate(){if let Some(w)=f["metrics"][i][0].as_i64(){subset.insert(c,w as i32);}}}}}
  let blocks=["note","footer"].iter().map(|bind|{
   let node=surface["elements"].as_array().and_then(|a|a.iter().find(|n|n["type"]=="text"&&n["bind"]==*bind));
   let get=|key:&str,default|node.and_then(|n|n[key].as_u64()).unwrap_or(default)as usize;
   let px=get("font_size",22).clamp(10,96);let lh=get("line_height",(px*5).div_ceil(4)as u64).max(px);
   (get("w",578).saturating_sub(2*get("padding",0)).max(1),px,(get("h",58)/lh).clamp(1,8))
  }).collect();Self{blocks,subset}
 }
 fn advance(&self,c:char,next:Option<char>)->usize{
  if let Some(w)=self.subset.get(&c){return *w as usize;}
  let symbol=match c{'·'=>6,'°'=>8,'℃'=>18,'–'|'−'=>10,'—'|'…'=>16,_=>0};if symbol>0{return symbol*2;}
  if matches!(c,'\u{fe0f}'|'\u{200d}'){return 0;}
  let m=metrics();let glyph=m.glyphs.get(&(c as u32)).copied().unwrap_or([if c as u32>=0x2300{256}else{160},0,0]);
  let right=next.and_then(|n|m.glyphs.get(&(n as u32))).map_or(0,|g|g[2]);
  let kern=if glyph[1]>0&&right>0{m.pairs[(glyph[1]as usize-1)*m.right_classes+right as usize-1]}else{0};
  2*((glyph[0]+kern+8)>>4).max(0)as usize
 }
 fn fits(&self,text:&str,block:(usize,usize,usize))->bool{
  let chars:Vec<_>=text.chars().collect();let(width,px,max)=block;let(mut row,mut units)=(1,0);
  for(i,c)in chars.iter().enumerate(){let step=self.advance(*c,chars.get(i+1).copied());if step*px>width*32{return false;}if (units+step)*px>width*32{row+=1;units=0;}units+=step;if row>max{return false;}}true
 }
 pub fn pages(&self,text:&str)->Vec<Page>{
  let mut pages=Vec::new();let(mut rest,mut offset)=(text,0);
  while !rest.is_empty(){let mut page=Page{start:offset,parts:Vec::new()};
   for &block in &self.blocks{let mut end=0;for(i,c)in rest.char_indices(){let next=i+c.len_utf8();if next>156||!self.fits(&rest[..next],block){break;}end=next;}
    // Keep closing punctuation with the preceding character at slot seams.
    // Move the word-ending character too, rather than leading the next block
    // with a comma just because its predecessor filled the last pixel.
    if end<rest.len()&&rest[end..].chars().next().is_some_and(|c|"，。！？；：、）》】」』,.!?;:%％".contains(c)){
     if let Some((previous,_))=rest[..end].char_indices().next_back(){if previous>0{end=previous;}}
    }
    // Validated package geometry always fits a glyph; retain lossless progress
    // even for a legacy malformed preview rather than looping forever.
    if end==0&&!rest.is_empty(){end=rest.chars().next().unwrap().len_utf8();}
    let part=&rest[..end];offset+=part.chars().count();page.parts.push(part.to_owned());rest=&rest[end..];
   }pages.push(page);
  }if pages.is_empty(){pages.push(Page{start:0,parts:vec![String::new();self.blocks.len()]});}pages
 }
 pub fn page_at(&self,text:&str,offset:usize)->usize{self.pages(text).iter().rposition(|p|p.start<=offset).unwrap_or(0)}
}
#[cfg(test)]mod tests{
 use super::*;
 fn flow()->Flow{let runtime:Value=serde_json::from_str(include_str!("../../builtin-clawpkgs/daily-news/runtime/widget.json")).unwrap();Flow::from_surface(&runtime["surface"])}
 #[test]fn chinese_fills_both_rows_before_continuing(){let f=flow();let text="知".repeat(150);let pages=f.pages(&text);assert_eq!(pages[0].parts[0].chars().count(),52);assert_eq!(pages[0].parts[1].chars().count(),52);assert_eq!(pages[1].start,104);assert_eq!(f.page_at(&text,103),0);assert_eq!(f.page_at(&text,104),1);}
 #[test]fn mixed_text_is_lossless_bounded_and_fits(){let f=flow();for text in ["AI算力增长2592EFLOPS，看看世界的新鲜事。".repeat(8),"WIFI IIII 2026。".repeat(10),"中文，标点：￥123！℃·—…".repeat(10)]{let pages=f.pages(&text);assert_eq!(pages.iter().flat_map(|p|&p.parts).cloned().collect::<String>(),text);for p in pages{for (i,part)in p.parts.iter().enumerate(){assert!(part.len()<=156);assert!(f.fits(part,f.blocks[i]));}}}}
 #[test]fn package_geometry_changes_page_boundaries(){let f=Flow::from_surface(&serde_json::json!({"elements":[{"type":"text","bind":"note","w":220,"h":29,"font_size":22,"line_height":29},{"type":"text","bind":"footer","w":220,"h":29,"font_size":22,"line_height":29}]}));assert_eq!(f.pages(&"知".repeat(50))[1].start,20);}
 #[test]fn closing_punctuation_does_not_start_second_block(){let f=flow();let text=format!("{}果，为用户提供支持。","知".repeat(51));let pages=f.pages(&text);assert!(pages[0].parts[1].starts_with("果，"));assert_eq!(pages[0].parts.concat(),text);}
}
