//! Community-only transport. Respect OS proxy/TLS; retry bounded reads, never writes.
use reqwest::{blocking::{Client,Response},header::HeaderMap};
use serde_json::{json,Value};
use std::{error::Error,fs,io::Write,path::Path,sync::{Mutex,OnceLock},time::{Duration,Instant}};

static CLIENT:OnceLock<Mutex<Option<(Instant,Client)>>>=OnceLock::new();
static LOG:Mutex<()>=Mutex::new(());
fn client(refresh:bool)->Result<Client,String>{
    let mut cached=CLIENT.get_or_init(Default::default).lock().map_err(|_|"社区连接暂不可用")?;
    if !refresh {if let Some((at,c))=&*cached{if at.elapsed()<Duration::from_secs(60){return Ok(c.clone());}}}
    // Re-evaluate system proxy settings periodically and after connection failures.
    // Never bypass the user's proxy or disable certificate verification.
    let c=Client::builder().connect_timeout(Duration::from_secs(5)).timeout(Duration::from_secs(100))
        .retry(reqwest::retry::never())
        .pool_idle_timeout(Duration::from_secs(45)).pool_max_idle_per_host(4)
        .redirect(reqwest::redirect::Policy::none())
        .user_agent(concat!("HachimoDock/",env!("CARGO_PKG_VERSION")))
        .build().map_err(|_|"无法初始化社区连接")?;
    *cached=Some((Instant::now(),c.clone()));Ok(c)
}
#[derive(Debug,Clone,Copy,PartialEq)]
enum Kind{Certificate,ProxyAuth,Proxy,Timeout,Tls,Connect,Interrupted,Other}
impl Kind{
    fn code(self)->&'static str{match self{Self::Certificate=>"certificate",Self::ProxyAuth=>"proxy-auth",Self::Proxy=>"proxy-connect",Self::Timeout=>"timeout",Self::Tls=>"tls-connect",Self::Connect=>"connect",Self::Interrupted=>"connection-interrupted",Self::Other=>"transport"}}
    fn retryable(self)->bool{matches!(self,Self::Proxy|Self::Timeout|Self::Tls|Self::Connect|Self::Interrupted)}
    fn message(self)->&'static str{match self{
        Self::Certificate=>"社区 HTTPS 证书校验失败，请核对系统时间或代理证书；不是普通断网，未跳过安全校验。",
        Self::ProxyAuth=>"系统代理要求认证，请检查代理客户端；社区请求尚未确认成功。",
        Self::Proxy=>"连接社区的系统代理暂不可用，请检查代理客户端后重试；本地组件仍可使用。",
        Self::Timeout=>"社区请求超时；可能是服务响应慢或连接短暂不稳定，本地组件仍可使用。",
        Self::Tls=>"社区 HTTPS 连接建立中断；可能与代理或连接抖动有关，请稍后重试。",
        Self::Connect=>"暂时未能建立社区连接，请稍后重试；这不代表电脑整体断网，本地组件仍可使用。",
        Self::Interrupted=>"社区连接短暂中断，请稍后重试；本地组件仍可使用。",
        Self::Other=>"社区请求未完成，请稍后重试；本地组件仍可使用。",
    }}
}
fn classify(text:&str,timeout:bool,connect:bool)->Kind{
    let text=text.to_ascii_lowercase();
    if ["certificate","cert verify","unknown issuer","trust failure"].iter().any(|s|text.contains(s)){Kind::Certificate}
    else if text.contains("407")||text.contains("proxy authentication"){Kind::ProxyAuth}
    else if timeout{Kind::Timeout}
    else if text.contains("proxy")||text.contains("tunnel"){Kind::Proxy}
    else if text.contains("tls")||text.contains("ssl"){Kind::Tls}
    else if ["connection reset","connection closed","unexpected eof","incomplete message","broken pipe","forcibly closed"].iter().any(|s|text.contains(s)){Kind::Interrupted}
    else if connect{Kind::Connect}else{Kind::Other}
}
fn kind(error:reqwest::Error)->Kind{
    // User search terms in a failed URL must not masquerade as TLS/proxy errors.
    let error=error.without_url();
    let mut text=error.to_string();let mut source=error.source();
    while let Some(e)=source{text.push_str(&format!(" {e}"));source=e.source();}
    classify(&text,error.is_timeout(),error.is_connect())
}
fn record(home:&Path,read:bool,attempt:u8,reason:&str,retry:bool){
    // No URLs, search text, headers, identity or raw error strings in diagnostics.
    let Ok(_lock)=LOG.lock()else{return;};
    let dir=super::root(home);if fs::create_dir_all(&dir).is_err(){return;}
    let path=dir.join("connection-diagnostics.jsonl");
    if fs::metadata(&path).is_ok_and(|m|m.len()>64*1024){let _=fs::rename(&path,dir.join("connection-diagnostics.previous.jsonl"));}
    let mut options=fs::OpenOptions::new();options.create(true).append(true);
    #[cfg(unix)]{use std::os::unix::fs::OpenOptionsExt;options.mode(0o600).custom_flags(libc::O_NOFOLLOW|libc::O_NONBLOCK);}
    if let Ok(mut file)=options.open(path){let _=writeln!(file,"{}",json!({"at":chrono::Utc::now().timestamp(),"operation":if read{"read"}else{"write"},"attempt":attempt,"reason":reason,"retry":retry}));}
}
fn retry_status(response:&Response)->bool{
    matches!(response.status().as_u16(),502|503|504)&&!response.headers().contains_key(reqwest::header::RETRY_AFTER)
}
fn send_with(home:&Path,url:&str,body:Option<Value>,headers:HeaderMap,mut get_client:impl FnMut(bool)->Result<Client,String>,delay:Duration)->Result<Response,String>{
    let read=body.is_none();let binary=url.contains("/api/v1/images/")||url.contains("/api/v1/packages/");
    for attempt in 1..=2{
        let c=get_client(attempt>1)?;
        let request=if let Some(body)=&body{c.post(url).json(body)}else{c.get(url)}
            .headers(headers.clone()).timeout(Duration::from_secs(if read&&!binary{15}else{100}));
        match request.send(){
            Ok(response)=>{
                if read&&attempt==1&&retry_status(&response){record(home,read,attempt,&format!("http-{}",response.status().as_u16()),true);drop(response);std::thread::sleep(delay);continue;}
                if attempt>1{record(home,read,attempt,if response.status().is_success(){"recovered"}else{"http-error-after-retry"},false);}return Ok(response);
            }
            Err(error)=>{
                let kind=kind(error);let retry=read&&attempt==1&&kind.retryable();record(home,read,attempt,kind.code(),retry);
                if retry{std::thread::sleep(delay);continue;}
                return Err(format!("{}{}",kind.message(),if read{""}else{" 若刚提交发布或评论，请先刷新核对结果，避免重复提交；未自动重发。"}));
            }
        }
    }unreachable!("two bounded attempts")
}
pub(super) fn send(home:&Path,url:&str,body:Option<Value>,headers:HeaderMap)->Result<Response,String>{
    send_with(home,url,body,headers,client,Duration::from_millis(400))
}

#[cfg(test)]mod tests{
    use super::*;use std::{io::{Read,Write},net::TcpListener,thread};
    fn server(statuses:Vec<u16>)->(String,thread::JoinHandle<Vec<String>>){
        let listener=TcpListener::bind("127.0.0.1:0").unwrap();let url=format!("http://{}/api/v1/health",listener.local_addr().unwrap());
        let task=thread::spawn(move||{let mut methods=Vec::new();for status in statuses{
            let(mut stream,_)=listener.accept().unwrap();stream.set_read_timeout(Some(Duration::from_secs(3))).unwrap();let mut bytes=[0u8;4096];let n=stream.read(&mut bytes).unwrap();methods.push(String::from_utf8_lossy(&bytes[..n]).split_whitespace().next().unwrap().into());
            if status!=0{write!(stream,"HTTP/1.1 {status} Test\r\nContent-Length: 2\r\nConnection: close\r\n\r\n{{}}").unwrap();}
        }methods});(url,task)
    }
    #[test]fn readonly_gateway_failure_recovers_once_but_post_is_not_replayed(){
        for(body,statuses,expected,code)in[(None,vec![503,200],vec!["GET","GET"],200),(Some(json!({"publish":"test"})),vec![503],vec!["POST"],503),(None,vec![429],vec!["GET"],429),(None,vec![503,503],vec!["GET","GET"],503)]{
            let home=tempfile::tempdir().unwrap();let(url,task)=server(statuses);let c=Client::builder().no_proxy().build().unwrap();
            let response=send_with(home.path(),&url,body,HeaderMap::new(),|_|Ok(c.clone()),Duration::ZERO).unwrap();
            assert_eq!(response.status().as_u16(),code);assert_eq!(task.join().unwrap(),expected);
        }
    }
    #[test]fn interrupted_read_recovers_once(){
        let home=tempfile::tempdir().unwrap();let(url,task)=server(vec![0,200]);let c=Client::builder().no_proxy().retry(reqwest::retry::never()).build().unwrap();
        let response=send_with(home.path(),&url,None,HeaderMap::new(),|_|Ok(c.clone()),Duration::ZERO).unwrap();
        assert_eq!(response.status().as_u16(),200);assert_eq!(task.join().unwrap(),vec!["GET","GET"]);
    }
    #[test]fn writes_on_transport_failure_remain_uncertain_and_logs_are_sanitized(){
        let home=tempfile::tempdir().unwrap();let(url,task)=server(vec![0]);let c=Client::builder().no_proxy().build().unwrap();
        let error=send_with(home.path(),&url,Some(json!({"secret":"never-log-this"})),HeaderMap::new(),|_|Ok(c.clone()),Duration::ZERO).unwrap_err();
        assert!(error.contains("未自动重发"));assert_eq!(task.join().unwrap(),vec!["POST"]);
        let log=fs::read_to_string(super::super::root(home.path()).join("connection-diagnostics.jsonl")).unwrap();assert!(!log.contains("never-log-this"));assert!(!log.contains("127.0.0.1"));
    }
    #[test]fn certificate_and_proxy_auth_fail_closed(){
        assert_eq!(classify("TLS invalid certificate",false,true),Kind::Certificate);
        assert!(!classify("TLS invalid certificate",false,true).retryable());
        assert!(!classify("proxy authentication 407",false,true).retryable());
        assert!(classify("tunnel failed",false,true).retryable());
        assert_eq!(classify("timed out",true,true),Kind::Timeout);
    }
    #[test]fn search_query_cannot_change_diagnostic_kind(){
        let(url,task)=server(vec![0]);let c=Client::builder().no_proxy().retry(reqwest::retry::never()).build().unwrap();
        let error=c.get(format!("{url}?q=certificate-proxy-407")).send().unwrap_err();
        assert_eq!(kind(error),Kind::Interrupted);assert_eq!(task.join().unwrap(),vec!["GET"]);
    }
}
