//! Bounded palette + RLE images. No frame strips: rotation is local to the device.
pub fn encode_background(image:&image::RgbaImage)->Result<Vec<u8>,String>{
    if image.dimensions()!=(640,480)||image.pixels().any(|p|p[3]!=255){return Err("背景必须为不透明 640×480".into());}
    let rgb=image::DynamicImage::ImageRgba8(image.clone()).to_rgb8();
    for quality in [88,84,80,76]{
        let mut out=b"P4J1".to_vec();out.extend(640u16.to_le_bytes());out.extend(480u16.to_le_bytes());out.extend([0;4]);
        image::codecs::jpeg::JpegEncoder::new_with_quality(&mut out,quality).encode_image(&rgb).map_err(|e|e.to_string())?;
        if out.len()<=100000{let size=out.len() as u32;out[8..12].copy_from_slice(&size.to_le_bytes());return Ok(out);}
    }
    Err("640×480 背景编码超过 100000 字节，请简化素材，不自动降低分辨率".into())
}
pub fn encode(image: &image::RgbaImage) -> Vec<u8> {
    let pixels: Vec<[u8;4]> = image.pixels().map(|p| if p[3]<8 {[0,0,0,0]} else {p.0}).collect();
    let mut buckets = vec![pixels.clone()];
    while buckets.len()<256 {
        let split = buckets.iter().enumerate().filter(|(_,b)|b.len()>1).max_by_key(|(_,b)| {
            let range=(0..4).map(|c|b.iter().map(|p|p[c]).max().unwrap()-b.iter().map(|p|p[c]).min().unwrap()).max().unwrap();
            usize::from(range)*b.len()
        }).map(|(i,_)|i);
        let Some(i)=split else {break};
        let mut b=buckets.swap_remove(i);
        let channel=(0..4).max_by_key(|&c|b.iter().map(|p|p[c]).max().unwrap()-b.iter().map(|p|p[c]).min().unwrap()).unwrap();
        b.sort_unstable_by_key(|p|p[channel]);
        let right=b.split_off(b.len()/2);buckets.push(b);buckets.push(right);
    }
    let mut palette=[[0u8;4];256];
    for (i,b) in buckets.iter().enumerate() {
        for c in 0..4 {palette[i][c]=(b.iter().map(|p|u64::from(p[c])).sum::<u64>()/b.len() as u64) as u8;}
    }
    let mut cache=std::collections::HashMap::new();
    let indices:Vec<u8>=pixels.iter().map(|p|*cache.entry(*p).or_insert_with(|| {
        (0..buckets.len()).min_by_key(|&i|(0..4).map(|c|{
            let d=i32::from(p[c])-i32::from(palette[i][c]); d*d*if c==3 {4}else{1}
        }).sum::<i32>()).unwrap() as u8
    })).collect();
    let mut out=if image.width()>240{
        let mut header=b"P4I2".to_vec();header.extend((image.width() as u16).to_le_bytes());header.extend((image.height() as u16).to_le_bytes());header.extend([0;4]);header
    }else{let mut header=b"P4I1".to_vec();header.extend([image.width() as u8,image.height() as u8,1,0,0,0,0,0]);header};
    for p in palette {out.extend(p);}
    let mut pos=0;
    while pos<indices.len() {
        let mut run=1;while run<128&&pos+run<indices.len()&&indices[pos+run]==indices[pos]{run+=1;}
        if run>=3 {out.extend([128|(run-1) as u8,indices[pos]]);pos+=run;}
        else {
            let start=pos;pos+=1;
            while pos<indices.len()&&pos-start<128 {
                if pos+2<indices.len()&&indices[pos]==indices[pos+1]&&indices[pos]==indices[pos+2]{break;}
                pos+=1;
            }
            out.push((pos-start-1) as u8);out.extend_from_slice(&indices[start..pos]);
        }
    }
    let size=out.len() as u32;out[8..12].copy_from_slice(&size.to_le_bytes());out
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    #[ignore="explicit isolated package directory, no device or audio"]
    fn optimized_package_images_fit_device_budget(){
        let root=std::path::PathBuf::from(std::env::var("PET_OPTIMIZED_PACKAGE_DIR").unwrap());
        let out=std::env::var("PET_OPTIMIZED_ENCODED_DIR").ok();
        for entry in std::fs::read_dir(&root).unwrap(){
            let entry=entry.unwrap();if !entry.file_type().unwrap().is_dir(){continue;}
            let path=entry.path();let raw=std::fs::read(path.join("runtime/widget.json")).unwrap();
            let r:serde_json::Value=serde_json::from_slice(&raw).unwrap();
            crate::component_extension::validate_app_runtime(&r).unwrap();
            let Some(s)=r.get("surface")else{continue;};
            if s["engine"]=="p4-app-surface-v1"{crate::component_extension::validate_app_surface(s,Some(&r)).unwrap();}
            else{crate::component_extension::validate_surface(s).unwrap();}
            let mut total=0;
            if let Some(images)=s["images"].as_array(){for im in images{
                let png=std::fs::read(path.join(im["asset"].as_str().unwrap())).unwrap();
                let image=crate::component_extension::decode_surface_image(im,&png).unwrap();
                let data=if im["encoding"]=="jpeg"{encode_background(&image).unwrap()}else{encode(&image)};
                total+=data.len();
                if let Some(out)=&out{
                    let dir=std::path::Path::new(out).join(entry.file_name());std::fs::create_dir_all(&dir).unwrap();
                    std::fs::write(dir.join(format!("{}.p4i",im["id"].as_str().unwrap())),&data).unwrap();
                }
            }}
            assert!(total<=128*1024,"{:?}: {} bytes",entry.file_name(),total);
            eprintln!("{}: {} image bytes",entry.file_name().to_string_lossy(),total);
        }
    }
    fn decode(p:&[u8])->Vec<[u8;4]>{
        assert_eq!(&p[..4],b"P4I1");assert_eq!(p.len(),u32::from_le_bytes(p[8..12].try_into().unwrap()) as usize);
        let mut out=Vec::new();let mut at=1036;
        while at<p.len(){let tag=p[at];at+=1;let n=usize::from(tag&127)+1;
            if tag&128!=0 {let i=usize::from(p[at]);at+=1;for _ in 0..n{out.push(p[12+i*4..16+i*4].try_into().unwrap());}}
            else {for _ in 0..n{let i=usize::from(p[at]);at+=1;out.push(p[12+i*4..16+i*4].try_into().unwrap());}}
        }out
    }
    #[test]
    fn bounded_codec_roundtrip_and_generated_fixtures(){
        for color in [[0,0,0,0],[255,230,180,255]]{
            let im=image::RgbaImage::from_pixel(240,240,image::Rgba(color));
            let encoded=encode(&im);let d=decode(&encoded);
            assert_eq!(d.len(),57600);assert!(d.iter().all(|p|*p==color));assert!(encoded.len()<2100);
        }
        let mut total=0;
        for name in ["checker","stripe"]{
            let path=std::path::Path::new(env!("CARGO_MANIFEST_DIR")).join(format!("../tests/fixtures/media-sample/assets/{name}.png"));
            let im=image::open(path).unwrap().to_rgba8();let packed=encode(&im);let d=decode(&packed);total+=packed.len();
            let error:f64=im.pixels().zip(&d).filter(|(a,_)|a[3]>250).map(|(a,b)|(0..3).map(|c|(f64::from(a[c])-f64::from(b[c])).powi(2)).sum::<f64>()).sum::<f64>()/(im.width()*im.height()*3) as f64;
            eprintln!("{name}: {} bytes, RGB RMSE {:.2}",packed.len(),error.sqrt());
            assert!(error.sqrt()<12.0);
            if let Ok(dir)=std::env::var("PET_SURFACE_TEST_OUTPUT"){
                let dir=std::path::Path::new(&dir);std::fs::create_dir_all(dir).unwrap();
                std::fs::write(dir.join(format!("{name}.p4i")),&packed).unwrap();
                let bytes=d.into_iter().flatten().collect();
                image::RgbaImage::from_raw(im.width(),im.height(),bytes).unwrap().save(dir.join(format!("{name}-encoded.png"))).unwrap();
            }
        }
        assert!(total<128*1024);
    }
}
