//! Ephemeral host-owned image handles. Never fetch a URL or accept a path from
//! a package. Providers supply bounded image bytes; only selected handles leave PC.
use super::*;
use base64::{engine::general_purpose::STANDARD as B64,Engine};
use std::sync::{Arc,Mutex,OnceLock,atomic::{AtomicU64,Ordering}};
#[derive(Clone)]
struct Asset {encoded_hash:String,reference:ImageRef,pixels:Arc<Vec<u8>>,png:Arc<Vec<u8>>}
static STORE:OnceLock<Mutex<BTreeMap<(String,String),Asset>>>=OnceLock::new();
// Decoder limits apply to one decode, not the number of concurrent callers.
// Never hold STORE while decoding provider-controlled compressed bytes.
static DECODE:Mutex<()>=Mutex::new(());
static RETIRE_EPOCH:AtomicU64=AtomicU64::new(0);
fn store()->&'static Mutex<BTreeMap<(String,String),Asset>>{STORE.get_or_init(Default::default)}
pub(crate) fn opaque_image(image:&image::DynamicImage)->bool{
    !match image {
        image::DynamicImage::ImageLumaA8(v)=>v.pixels().any(|p|p[1]!=u8::MAX),
        image::DynamicImage::ImageRgba8(v)=>v.pixels().any(|p|p[3]!=u8::MAX),
        image::DynamicImage::ImageLumaA16(v)=>v.pixels().any(|p|p[1]!=u16::MAX),
        image::DynamicImage::ImageRgba16(v)=>v.pixels().any(|p|p[3]!=u16::MAX),
        image::DynamicImage::ImageRgba32F(v)=>v.pixels().any(|p|p[3]!=1.0),
        _=>false,
    }
}
pub fn register(owner:&str,slot:&str,data:&str)->Result<ImageRef>{
    if !name(owner,47)||!name(slot,47)||data.len()>3*1024*1024{return Err(fail());}
    let encoded_hash=format!("{:x}",Sha256::digest(data.as_bytes()));
    if let Some(asset)=store().lock().map_err(|_|fail())?.get(&(owner.into(),slot.into())){if asset.encoded_hash==encoded_hash{return Ok(asset.reference.clone());}}
    let _decode=DECODE.lock().map_err(|_|fail())?;
    let epoch=RETIRE_EPOCH.load(Ordering::Acquire);
    {
        let all=store().lock().map_err(|_|fail())?;
        let index=(owner.into(),slot.into());
        if let Some(asset)=all.get(&index){if asset.encoded_hash==encoded_hash{return Ok(asset.reference.clone());}}
        if !all.contains_key(&index)&&(all.len()>=32||all.keys().filter(|(id,_)|id==owner).count()>=2){return Err("动态图片缓存达到配额".into());}
    }
    let (encoded,format)=if let Some(encoded)=data.strip_prefix("data:image/png;base64,"){
        (encoded,image::ImageFormat::Png)
    }else if let Some(encoded)=data.strip_prefix("data:image/jpeg;base64,"){
        (encoded,image::ImageFormat::Jpeg)
    }else{return Err(fail());};
    let bytes=B64.decode(encoded).map_err(|_|fail())?;if bytes.len()>2*1024*1024{return Err(fail());}
    // Do not let a PNG/JPEG declaration enable any other registered decoder.
    if image::guess_format(&bytes).map_err(|_|fail())?!=format{return Err(fail());}
    let mut reader=image::ImageReader::with_format(std::io::Cursor::new(&bytes),format);
    let mut limits=image::Limits::default();limits.max_image_width=Some(4096);limits.max_image_height=Some(4096);limits.max_alloc=Some(64*1024*1024);reader.limits(limits);
    let image=reader.decode().map_err(|_|fail())?;
    // Reject alpha in the original image: resampling can round away a small
    // translucent region. Iterate without allocating a full-size RGBA copy.
    if !opaque_image(&image){return Err("动态图片需要不透明背景".into());}
    let image=image.thumbnail(192,192);
    let rgb=image.to_rgb8();let (width,height)=rgb.dimensions();if width<8||height<8{return Err(fail());}
    let mut pixels=Vec::with_capacity((width*height*2)as usize);
    for p in rgb.pixels(){let n=((u16::from(p[0])>>3)<<11)|((u16::from(p[1])>>2)<<5)|(u16::from(p[2])>>3);pixels.extend_from_slice(&n.to_le_bytes());}
    let revision=format!("{:x}",Sha256::digest(&pixels));
    let key=digest(&(owner,slot,&revision))?;let key=format!("image-{}",&key[..32]);
    let reference=ImageRef{key,revision,width:width as u16,height:height as u16};
    let mut png=std::io::Cursor::new(vec![]);image.write_to(&mut png,image::ImageFormat::Png).map_err(|_|fail())?;
    commit_asset(owner,slot,Asset{encoded_hash,reference,pixels:Arc::new(pixels),png:Arc::new(png.into_inner())},epoch)
}
fn commit_asset(owner:&str,slot:&str,asset:Asset,epoch:u64)->Result<ImageRef>{
    let mut all=store().lock().map_err(|_|fail())?;
    if RETIRE_EPOCH.load(Ordering::Acquire)!=epoch{return Err("图片视图已变化，旧解码结果已丢弃".into());}
    let index=(owner.into(),slot.into());
    if !all.contains_key(&index)&&(all.len()>=32||all.keys().filter(|(id,_)|id==owner).count()>=2){return Err("动态图片缓存达到配额".into());}
    let reference=asset.reference.clone();all.insert(index,asset);Ok(reference)
}

fn get(owner:&str,r:&ImageRef)->Result<Asset>{store().lock().map_err(|_|fail())?.iter().find(|((id,_),a)|id==owner&&a.reference.key==r.key&&a.reference.revision==r.revision&&a.reference.width==r.width&&a.reference.height==r.height).map(|(_,a)|a.clone()).ok_or_else(fail)}
pub fn preview(owner:&str,r:&ImageRef)->Result<Value>{let a=get(owner,r)?;Ok(json!({"key":r.key,"revision":r.revision,"dataUrl":format!("data:image/png;base64,{}",B64.encode(a.png.as_ref()))}))}
pub fn clear(owner:&str){if let Ok(mut s)=store().lock(){RETIRE_EPOCH.fetch_add(1,Ordering::AcqRel);s.retain(|(id,_),_|id!=owner);}}
pub fn send(usb:&crate::usb_serial::UsbSerialManager,board:&str,owner:&str,instance:&str,context:&str,images:&Value)->Result<()>{
    let Some(images)=images.as_object()else{return Ok(())};
    if images.len()>2||!name(instance,31)||context.len()!=64{return Err(fail());}
    for (alias,value) in images {
        if !name(alias,15){return Err(fail());}let r=ImageRef::parse(value)?;let asset=get(owner,&r)?;
        let query=json!({"operation":"query","componentId":owner,"instance":instance,"context":context,"alias":alias,"key":r.key,"revision":r.revision,"width":r.width,"height":r.height});
        if usb.service_image_request(board,query)?["code"]=="ready"{continue;}
        for (i,bytes) in asset.pixels.chunks(1024).enumerate(){
            // Each chunk rechecks exact current instance/context on the device.
            // No retry on unknown ACK; a later read-only snapshot may republish.
            let v=json!({"componentId":owner,"instance":instance,"context":context,"alias":alias,"key":r.key,"revision":r.revision,"width":r.width,"height":r.height,"offset":i*1024,"data":B64.encode(bytes)});
            usb.service_image_request(board,v)?;
        }
    }Ok(())
}

#[cfg(test)]
mod tests {
    use super::*;
    #[test]
    fn production_commit_discards_decode_completed_after_retirement(){
        let owner="bridge-retired-image-test";let epoch=RETIRE_EPOCH.load(Ordering::Acquire);
        let reference=ImageRef{key:"image-test".into(),revision:"revision-test".into(),width:8,height:8};
        let asset=Asset{encoded_hash:"encoded".into(),reference:reference.clone(),pixels:Arc::new(vec![0;128]),png:Arc::new(vec![])};
        clear(owner);
        assert!(commit_asset(owner,"cover",asset,epoch).is_err());
        assert!(preview(owner,&reference).is_err());
    }
    #[test]
    fn local_cover_opacity_is_checked_at_original_bit_depth_before_resampling(){
        let mut pixels=image::ImageBuffer::<image::Rgba<u16>,Vec<u16>>::from_pixel(8,8,image::Rgba([0,0,0,u16::MAX]));
        pixels.put_pixel(0,0,image::Rgba([0,0,0,u16::MAX-1]));
        assert!(!opaque_image(&image::DynamicImage::ImageRgba16(pixels)));
        let opaque=image::DynamicImage::ImageRgb8(image::RgbImage::new(8,8));assert!(opaque_image(&opaque));
    }
    #[test]
    fn unregistered_reference_cannot_read_other_owner_pixels(){
        let reference=ImageRef{key:"image-not-registered".into(),revision:"revision".into(),width:8,height:8};
        assert!(preview("unrelated-owner",&reference).is_err());
    }
}
