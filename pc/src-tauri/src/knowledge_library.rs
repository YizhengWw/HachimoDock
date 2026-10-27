//! Package-owned, bounded offline reading. Only the current card crosses IPC.
use rand::seq::SliceRandom;
use serde::{Deserialize,Serialize};
use serde_json::{json,Value};
use std::collections::{BTreeMap,HashSet};
#[derive(Clone,Default,PartialEq,Serialize,Deserialize)]
#[serde(default)]
pub struct Reading{
    pub category:String,pub article:String,pub page:usize,
    pub choosing:bool,pub candidate:usize,pub bags:BTreeMap<String,Vec<String>>,
    pub include_all:bool,
}
pub fn categories(cards:&[Value])->Vec<String>{
    let mut names=Vec::new();
    for c in cards{let name=c["category"].as_str().unwrap_or("");if !names.iter().any(|n|n==name){names.push(name.to_owned());}}
    names
}
fn choose(s:&mut Reading,cards:&[Value]){
    let ids:Vec<String>=cards.iter().filter(|c|matches_category(s,c,&s.category)).filter_map(|c|c["id"].as_str().map(str::to_owned)).collect();
    let bag=s.bags.entry(s.category.clone()).or_default();
    let allowed:HashSet<_>=ids.iter().collect();let mut seen=HashSet::new();
    bag.retain(|id|allowed.contains(id)&&seen.insert(id.clone()));
    if bag.is_empty(){
        *bag=ids;bag.shuffle(&mut rand::thread_rng());
        if bag.len()>1&&bag.last()==Some(&s.article){let last=bag.len()-1;bag.swap(0,last);}
    }
    s.article=bag.pop().unwrap_or_default();s.page=0;
}
/// Split by UTF-8 byte AND display-width budgets; concatenation is lossless.
/// Two segments form one device page (note + footer). No firmware string growth.
pub fn segments(text:&str)->Vec<String>{
    let mut out=Vec::new();let mut part=String::new();let mut width=0;let mut count=0;
    for ch in text.chars(){
        let w=if ch.is_ascii(){1}else{2};
        // Wide Latin capitals can be almost an em, not half an em. Also cap
        // scalar count so mixed English names cannot overflow the third line.
        if part.len()+ch.len_utf8()>144||width+w>94||count>=48{out.push(std::mem::take(&mut part));width=0;count=0;}
        part.push(ch);width+=w;count+=1;
    }
    if !part.is_empty(){out.push(part);}
    if out.is_empty(){out.push(String::new());}out
}
fn reading_segments(s:&Reading,text:&str)->Vec<String>{
    if !s.include_all{return segments(text);}
    // Books prefer sentence/clause boundaries between the two visible text
    // blocks. Keep the existing byte/width ceilings and never discard a char.
    let mut rest=text;let mut out=Vec::new();
    while !rest.is_empty(){
        let hard=segments(rest).into_iter().next().unwrap();
        let boundary=hard.char_indices().enumerate().filter(|(n,(_,ch))|*n>=22&&"。！？；，：!?;,".contains(*ch)).last().map(|(_, (i,ch))|i+ch.len_utf8());
        let end=if hard.len()==rest.len(){hard.len()}else{boundary.unwrap_or(hard.len())};
        out.push(rest[..end].to_owned());rest=&rest[end..];
    }
    if out.is_empty(){out.push(String::new());}out
}
fn matches_category(s:&Reading,c:&Value,category:&str)->bool{c["category"]==category||(s.include_all&&category=="全部书籍")}
fn menu(s:&Reading,cards:&[Value])->Vec<String>{let mut cats=categories(cards);if s.include_all&&!cats.is_empty(){cats.insert(0,"全部书籍".into());}cats}
pub fn current<'a>(s:&Reading,cards:&'a[Value])->Option<&'a Value>{cards.iter().find(|c|c["id"]==s.article&&matches_category(s,c,&s.category))}
pub fn apply(s:&mut Reading,cards:&[Value],op:&str){
    let cats=menu(s,cards);if cats.is_empty(){return;}
    s.bags.retain(|c,_|cats.contains(c));
    if !cats.contains(&s.category){s.category=cats[0].clone();s.article.clear();s.choosing=false;}
    let initialized=current(s,cards).is_none();
    if initialized{choose(s,cards);}
    s.candidate%=cats.len();
    match op{
        "category"=>{if !s.choosing{s.candidate=cats.iter().position(|c|c==&s.category).unwrap_or(0);}s.choosing=!s.choosing;},
        "random"=>{let selecting=s.choosing;if selecting{s.category=cats[s.candidate].clone();s.choosing=false;}if !initialized||selecting{choose(s,cards);}},
        "previous"|"next" if s.choosing=>s.candidate=(s.candidate+if op=="next"{1}else{cats.len()-1})%cats.len(),
        "previous"=>s.page=s.page.saturating_sub(1),
        "next"=>s.page=s.page.saturating_add(1),
        _=>{}
    }
    let count=current(s,cards).map(|c|reading_segments(s,c["text"].as_str().unwrap_or("")).len().div_ceil(2)).unwrap_or(1);
    s.page=s.page.min(count.saturating_sub(1));
}
pub fn info(s:&Reading,cards:&[Value])->Value{
    let cats=menu(s,cards);
    json!({"categories":cats.iter().map(|name|json!({"name":name,"count":cards.iter().filter(|c|matches_category(s,c,name)).count()})).collect::<Vec<_>>(),
        "category":s.category,"choosing":s.choosing,"candidate":cats.get(s.candidate),"current":current(s,cards),
        "page":s.page,"pages":current(s,cards).map(|c|reading_segments(s,c["text"].as_str().unwrap_or("")).len().div_ceil(2)).unwrap_or(0),"total":cards.len()})
}
pub fn view(s:&Reading,cards:&[Value],favorites:&[String],speaking:bool)->Value{
    let cats=menu(s,cards);let Some(c)=current(s,cards)else{return json!({"headline":"内容库暂无内容"});};
    if s.choosing{
        let name=&cats[s.candidate%cats.len()];let count=cards.iter().filter(|c|matches_category(s,c,name)).count();
        return json!({"eyebrow":"返回阅读","headline":name,"note":if s.include_all{format!("本类有 {count} 本书。左右选择分类，确认后随机抽取一本导读。")}else{format!("本类有 {count} 个知识点。左右选择分类，点“确认分类”后随机抽取一条。")},"footer":"浏览分类不会改变正在阅读的内容；点右上角或再按副键可取消。",
            "metricValue":"按分类随机 · 本轮不重复","badge":format!("{}/{}",s.candidate+1,cats.len()),"metricLabel":"确认分类","metricUnit":"读本页"});
    }
    let text=c["text"].as_str().unwrap_or("");let chunks=reading_segments(s,text);let pages=chunks.len().div_ceil(2);let page=s.page.min(pages-1);
    let han=text.chars().filter(|ch|('\u{3400}'..='\u{9fff}').contains(ch)).count();
    json!({"eyebrow":format!("{} · 分类",s.category),"headline":c["title"],"note":chunks[page*2],"footer":chunks.get(page*2+1).cloned().unwrap_or_default(),
        "metricValue":format!("{} · {han}字{}",if s.include_all{"原创导读"}else{"百科节选"},if favorites.iter().any(|id|c["id"]==*id){" · 已收藏"}else{""}),"badge":format!("{}/{}",page+1,pages),
        "metricLabel":if s.include_all{"换一本"}else{"换一条"},"metricUnit":if speaking{"停止朗读"}else{"读本页"}})
}
pub fn speech_page(s:&Reading,cards:&[Value])->String{
    let Some(c)=current(s,cards)else{return String::new();};let chunks=reading_segments(s,c["text"].as_str().unwrap_or(""));
    let page=s.page.min(chunks.len().div_ceil(2)-1);
    format!("{}。{}{}",c["title"].as_str().unwrap_or(""),chunks[page*2],chunks.get(page*2+1).map(String::as_str).unwrap_or(""))
}
#[cfg(test)]mod tests{
    use super::*;
    fn cards()->Vec<Value>{(0..2).flat_map(|cat|(0..100).map(move|i|json!({"id":format!("c{cat}-{i}"),"category":format!("分类{cat}"),"title":format!("知识{i}"),"text":"这是用于验证分页完整性的中文句子。".repeat(30)}))).collect()}
    #[test]fn random_cycle_does_not_repeat_and_status_does_not_advance(){
        let cards=cards();let mut s=Reading::default();let mut seen=HashSet::new();
        for _ in 0..100{apply(&mut s,&cards,"random");assert!(seen.insert(s.article.clone()));let before=s.clone();apply(&mut s,&cards,"status");assert!(s==before);}
        let last=s.article.clone();apply(&mut s,&cards,"random");assert_ne!(last,s.article);
    }
    #[test]fn menu_cancel_and_confirm_persist_and_pages_are_lossless(){
        let cards=cards();let mut s=Reading::default();apply(&mut s,&cards,"status");let original=s.article.clone();
        apply(&mut s,&cards,"category");apply(&mut s,&cards,"next");assert_eq!(s.category,"分类0");
        apply(&mut s,&cards,"category");assert_eq!(s.article,original);
        apply(&mut s,&cards,"category");apply(&mut s,&cards,"next");apply(&mut s,&cards,"random");assert_eq!(s.category,"分类1");assert!(!s.choosing);
        let restored:Reading=serde_json::from_slice(&serde_json::to_vec(&s).unwrap()).unwrap();assert!(s==restored);
        let text=current(&s,&cards).unwrap()["text"].as_str().unwrap();let chunks=segments(text);assert_eq!(chunks.concat(),text);assert!(chunks.iter().all(|c|c.len()<=144));
        for _ in 0..500{apply(&mut s,&cards,"next");}assert_eq!(s.page,chunks.len().div_ceil(2)-1);
        for _ in 0..500{apply(&mut s,&cards,"previous");}assert_eq!(s.page,0);
    }
    #[test]fn changed_catalog_and_mixed_utf8_do_not_break_reader(){
        let text="中文 ABC 😀 café 标点，。".repeat(45);assert_eq!(segments(&text).concat(),text);
        let cards=cards();let mut s=Reading{category:"旧分类".into(),article:"removed".into(),page:usize::MAX,candidate:usize::MAX,..Default::default()};
        apply(&mut s,&cards,"status");assert_eq!(s.category,"分类0");assert!(current(&s,&cards).is_some());
    }
    #[test]fn books_draw_from_all_categories_without_repetition_and_keep_original_attribution(){
        let cards=cards();let mut s=Reading{include_all:true,..Default::default()};let mut seen=HashSet::new();
        for _ in 0..cards.len(){apply(&mut s,&cards,"random");assert!(seen.insert(s.article.clone()));}
        assert_eq!(s.category,"全部书籍");assert_eq!(seen.len(),200);
        assert_eq!(info(&s,&cards)["categories"][0]["count"],200);
        assert!(view(&s,&cards,&[],false)["metricValue"].as_str().unwrap().starts_with("原创导读"));
        let last=s.article.clone();apply(&mut s,&cards,"random");assert_ne!(s.article,last);
        apply(&mut s,&cards,"category");apply(&mut s,&cards,"next");apply(&mut s,&cards,"random");
        assert_eq!(s.category,"分类0");assert!(current(&s,&cards).is_some());
        let persisted=serde_json::from_slice::<Reading>(&serde_json::to_vec(&s).unwrap()).unwrap();assert!(persisted==s);
    }
    #[test]#[ignore="100-book production package; isolated storage, no device or speech API"]
    fn book_trial_host_reader_and_narration_are_lossless(){
        let package=std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join("../new-components/book-pocket");
        let runtime:Value=serde_json::from_slice(&std::fs::read(package.join("runtime/widget.json")).unwrap()).unwrap();
        let raw=std::fs::read(package.join("pc/application.json")).unwrap();
        let d=crate::app_services::parse(&raw,&runtime).unwrap();assert_eq!(d.content.len(),100);assert!(d.auto_read);
        assert_eq!(categories(&d.content).len(),10);
        let mut draw=Reading{include_all:true,..Default::default()};let mut seen=HashSet::new();let mut total_pages=0;
        for _ in 0..100{apply(&mut draw,&d.content,"random");assert!(seen.insert(draw.article.clone()));}
        for card in &d.content{
            let text=card["text"].as_str().unwrap();let chunks=reading_segments(&draw,text);
            assert_eq!(chunks.concat(),text);assert!(chunks.iter().all(|c|c.len()<=144));total_pages+=chunks.len().div_ceil(2);
            let han=text.chars().filter(|c|('\u{3400}'..='\u{9fff}').contains(c)).count();assert!((800..=1500).contains(&han));
        }
        assert_eq!(crate::app_services::permissions(&d)["microphone"],false);
        let mut bad:Value=serde_json::from_slice(&raw).unwrap();bad["content"][0]["license"]="CC BY-SA 4.0".into();
        assert!(crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(),&runtime).is_err());
        let mut bad:Value=serde_json::from_slice(&raw).unwrap();bad["content"][0]["category"]="全部书籍".into();
        assert!(crate::app_services::parse(&serde_json::to_vec(&bad).unwrap(),&runtime).is_err());
        let dir=tempfile::tempdir().unwrap();let usb=crate::usb_serial::UsbSerialManager::new();
        let request=|op|crate::component_app::test_request(&package,dir.path(),json!({"operation":op}),&usb).unwrap();
        let first=request("status");let stable=request("status");assert_eq!(first["state"],stable["state"]);
        assert_eq!(first["content"],json!([]));assert!(first["state"]["knowledge"].get("bags").is_none());
        assert!(serde_json::to_vec(&first).unwrap().len()<12000);
        assert!(!first["ui"]["actions"].as_array().unwrap().contains(&json!("host.previous")));
        let mut snapshots=serde_json::Map::new();snapshots.insert("reading".into(),first.clone());
        let mut joined=String::new();let mut current=first.clone();let mut pages=0;
        loop{
            joined+=current["values"]["note"].as_str().unwrap();joined+=current["values"]["footer"].as_str().unwrap();pages+=1;
            if !current["ui"]["actions"].as_array().unwrap().contains(&json!("host.next")){break;}
            current=request("next");if pages==1{snapshots.insert("second".into(),current.clone());}
            assert!(pages<100);
        }
        assert_eq!(joined,first["knowledge"]["current"]["text"].as_str().unwrap());assert_eq!(request("next")["values"],current["values"]);
        snapshots.insert("last".into(),current.clone());
        let picker=request("category");assert_eq!(picker["ui"]["mode"],3);snapshots.insert("category".into(),picker);
        request("next");let cancelled=request("category");assert_eq!(cancelled["state"]["knowledge"]["page"],current["state"]["knowledge"]["page"]);
        request("category");request("next");let selected=request("random");assert_eq!(selected["knowledge"]["category"],"组织系统");assert_eq!(selected["knowledge"]["page"],0);
        assert!(crate::component_app::test_request(&package,dir.path(),json!({"operation":"favorite"}),&usb).is_err());
        let mut narration=crate::knowledge_narration::Narration::default();let mut spoken=narration.next(1,&first,"idle").unwrap();
        while let Some(part)=narration.next(1,&first,"completed"){assert!(part.chars().count()<=90);spoken+=&part;}
        assert_eq!(spoken,format!("{}。{}",first["knowledge"]["current"]["title"].as_str().unwrap(),first["knowledge"]["current"]["text"].as_str().unwrap()));
        assert!(narration.next(1,&first,"completed").is_none());
        if let Ok(path)=std::env::var("PET_BOOK_SNAPSHOTS"){std::fs::write(path,serde_json::to_vec_pretty(&snapshots).unwrap()).unwrap();}
        println!("PASS: 100 books / {total_pages} lossless pages, 100 unique random draws, {pages} live-host pages, real category/cancel/confirm, bounded narration without audio/API calls");
    }
    #[test]#[ignore="requires the completed source corpus; no network or user saves"]
    fn production_thousand_articles_roundtrip_and_all_categories(){
        let path=std::env::var("PET_KNOWLEDGE_CATALOG").unwrap();
        let cards:Vec<Value>=serde_json::from_slice(&std::fs::read(path).unwrap()).unwrap();
        assert_eq!(cards.len(),1000);let cats=categories(&cards);assert_eq!(cats.len(),10);
        let mut ids=HashSet::new();let mut texts=HashSet::new();let mut all_pages=0;
        for c in &cards{
            assert!(ids.insert(c["id"].as_str().unwrap()));assert!(texts.insert(c["text"].as_str().unwrap()));
            let text=c["text"].as_str().unwrap();assert!(text.chars().filter(|ch|('\u{3400}'..='\u{9fff}').contains(ch)).count()>=300);
            let chunks=segments(text);assert_eq!(chunks.concat(),text);
            assert!(chunks.iter().all(|p|p.len()<=144&&p.chars().map(|c|if c.is_ascii(){1}else{2}).sum::<usize>()<=94));
            let mut s=Reading{category:c["category"].as_str().unwrap().into(),article:c["id"].as_str().unwrap().into(),..Default::default()};
            let mut joined=String::new();
            for i in 0..chunks.len().div_ceil(2){
                s.page=i;let mut v=view(&s,&cards,&[],false);
                for k in ["note","footer"]{v[k]=crate::widget_data::bounded_text(v[k].as_str().unwrap(),159).into();joined.push_str(v[k].as_str().unwrap());}
                assert!(v["headline"].as_str().unwrap().len()<=159);all_pages+=1;
            }
            assert_eq!(joined,text);
        }
        for cat in &cats{
            assert_eq!(cards.iter().filter(|c|c["category"]==*cat).count(),100);
            let mut s=Reading{category:cat.clone(),..Default::default()};let mut seen=HashSet::new();
            for i in 0..100{
                apply(&mut s,&cards,"random");assert!(seen.insert(s.article.clone()));assert_eq!(&s.category,cat);
                if i%7==0{s=serde_json::from_slice(&serde_json::to_vec(&s).unwrap()).unwrap();}
            }
            let last=s.article.clone();apply(&mut s,&cards,"random");assert_ne!(s.article,last);
        }
        if let Ok(output)=std::env::var("PET_KNOWLEDGE_VIEWS"){
            let c=&cards[0];let mut s=Reading{category:c["category"].as_str().unwrap().into(),article:c["id"].as_str().unwrap().into(),..Default::default()};
            let first=view(&s,&cards,&[],false);s.page=1;let second=view(&s,&cards,&[],false);
            apply(&mut s,&cards,"category");apply(&mut s,&cards,"next");
            let menu=view(&s,&cards,&[],false);apply(&mut s,&cards,"random");
            std::fs::write(output,serde_json::to_vec(&json!({"reading":first,"second":second,"category":menu,"changed":view(&s,&cards,&[],false)})).unwrap()).unwrap();
        }
        println!("PASS: 10 × 100, ≥300 Han each, {all_pages} lossless device pages, unique sources/texts, 1,000 nonrepeating draws with persistence.");
    }
}
