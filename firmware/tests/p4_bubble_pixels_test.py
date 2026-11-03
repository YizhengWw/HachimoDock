"""Production receiver + real cJSON/pthread mutex; bounded synthetic SDK wrappers."""
import base64,json,os,subprocess
from pathlib import Path
import pytest
ROOT=Path(__file__).resolve().parents[1]
BOOT='0102030401020304';CLIENT='a'*32;OWNER='1'*16;VERSION='0'*15+'1';TRANSFER='b'*32

def key(style,text):
    h=14695981039346656037
    for b in b'p4-text-alpha-v2:rle4:1:'+bytes([style])+text.encode():h=((h^b)*1099511628211)&((1<<64)-1)
    return f'{h:016x}'
def runs(values):
    out=bytearray()
    for v in values:
        if out and out[-1]>>4==v and out[-1]&15<15:out[-1]+=1
        else:out.append(v<<4)
    return bytes(out)
def solid(style=4,alpha=15):return runs([alpha]*((474*38) if style==1 else (348*38) if style==3 else (496*54)))
def desire(owner=OWNER,version=VERSION,client=CLIENT,scope='state/working',route=1,assets=None):
    assets=assets if assets is not None else [(4,'a'),(4,'body'),(1,'title'),(2,'caption')]
    return {'desire':{'textPixels':{'boot':BOOT,'client':client,'owners':[{'id':owner,'version':version,'assets':[{'style':style,'id':key(style,text)} for style,text in assets]}]}},'scope':scope,'route':route}
def transfer(text,data=None,style=4,owner=OWNER,version=VERSION,client=CLIENT,transfer=TRANSFER,page=0,pages=1):
    data=solid(style) if data is None else data;h=2166136261
    for b in data:h=((h^b)*16777619)&0xffffffff
    base=dict(boot=BOOT,client=client,owner=owner,version=version,transfer=transfer,id=key(style,text),style=style)
    return [{**base,'phase':'begin','pageSizes':[len(data)],'totalBytes':len(data),'checksum':f'{h:08x}','pageIndex':page,'pageCount':pages}]+[
        {**base,'phase':'chunk','offset':i,'data':base64.b64encode(data[i:i+512]).decode()} for i in range(0,len(data),512)]+[{**base,'phase':'end'}]
def send(packets,route=1):return [{'payload':p,'ok':True,'route':route} for p in packets]
def select(title='',body='',pixels=False,now=0,style=4,hold=False):return {'select':{'owner':'view:'+title+body,'title':title,'body':body,'pixels':pixels,'now':now,'style':style,'hold':hold}}

@pytest.fixture(scope='module')
def receiver(tmp_path_factory):
    t=tmp_path_factory.mktemp('bubble-owned');(t/'freertos').mkdir();(t/'mbedtls').mkdir()
    (t/'freertos/FreeRTOS.h').write_text('#define portMAX_DELAY 0\n')
    (t/'freertos/semphr.h').write_text('''#include <assert.h>
#include <pthread.h>
#include <stdlib.h>
typedef pthread_mutex_t *SemaphoreHandle_t;
static inline SemaphoreHandle_t xSemaphoreCreateMutex(void){SemaphoreHandle_t m=malloc(sizeof(*m));assert(m&&!pthread_mutex_init(m,0));return m;}
static inline int xSemaphoreTake(SemaphoreHandle_t m,int t){(void)t;assert(!pthread_mutex_lock(m));return 1;}
static inline void xSemaphoreGive(SemaphoreHandle_t m){assert(!pthread_mutex_unlock(m));}
''')
    (t/'esp_random.h').write_text('#include <stdint.h>\nstatic inline uint32_t esp_random(void){return 0x01020304U;}\n')
    (t/'esp_heap_caps.h').write_text('''#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int fail_alloc;
static inline void *heap_caps_malloc(size_t n,int c){(void)c;if(fail_alloc){fail_alloc=0;return NULL;}return malloc(n);}
''')
    (t/'mbedtls/base64.h').write_text('#include <stddef.h>\nint mbedtls_base64_decode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);\n')
    h=t/'main.c';h.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include "pet_p4_bubble_pixels.h"
int fail_alloc;
int mbedtls_base64_decode(unsigned char*out,size_t cap,size_t*n,const unsigned char*in,size_t len){
 const char*a="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";unsigned v=0,bits=0;*n=0;
 for(size_t i=0;i<len;i++){if(in[i]=='=')break;const char*p=strchr(a,in[i]);if(!p)return -1;v=(v<<6)|(unsigned)(p-a);bits+=6;if(bits>=8){bits-=8;if(*n>=cap)return -1;out[(*n)++]=(unsigned char)(v>>bits);}}return 0;}
static const char *str(const cJSON*j,const char*n){const cJSON*v=cJSON_GetObjectItemCaseSensitive(j,n);return cJSON_IsString(v)?v->valuestring:"";}
static unsigned num(const cJSON*j,const char*n){const cJSON*v=cJSON_GetObjectItemCaseSensitive(j,n);return cJSON_IsNumber(v)?(unsigned)v->valuedouble:0;}
static uint16_t framebuffer[640*480+2];
static void *race_writer(void *context){
 const cJSON*r=context;
 for(unsigned round=0;round<80;round++){for(unsigned which=0;which<2;which++){
  const cJSON*b=cJSON_GetArrayItem(r,(int)which);pet_bubble_pixels_desire(cJSON_GetObjectItemCaseSensitive(b,"desire"),(void*)1,"state/working",0);
  const cJSON*packets=cJSON_GetObjectItemCaseSensitive(b,"packets");cJSON*p;
  cJSON_ArrayForEach(p,packets){cJSON*reply=cJSON_CreateObject();(void)pet_bubble_pixels_handle_owned(p,(void*)1,0,reply);cJSON_Delete(reply);}
 }}
 return NULL;
}
static void *race_reader(void *unused){
 (void)unused;
 for(unsigned round=0;round<160;round++){
  pet_bubble_text_view_t view;pet_bubble_pixels_select("race",4,"title","body",0,&view);
  if(view.pixels){assert(pet_bubble_pixels_draw_pinned(&view,1,0,framebuffer+1,0,0,496,0));assert(pet_bubble_pixels_draw_pinned(&view,4,0,framebuffer+1,0,0,496,0));}
  pet_bubble_pixels_release(&view);
 }return NULL;
}

int main(int argc,char**argv){
 assert(argc==2&&pet_bubble_pixels_init());assert(!strcmp(pet_bubble_pixels_boot(),"0102030401020304"));
 FILE*f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long n=ftell(f);rewind(f);char*data=calloc((size_t)n+1,1);assert(data&&fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);cJSON*commands=cJSON_Parse(data);free(data);assert(commands);
 pet_bubble_text_view_t held={.title_pin=-1,.body_pin=-1};bool holding=false;
 cJSON*c;cJSON_ArrayForEach(c,commands){
  cJSON*races=cJSON_GetObjectItemCaseSensitive(c,"race");
  if(races){pthread_t writer,reader;assert(!pthread_create(&writer,NULL,race_writer,races));assert(!pthread_create(&reader,NULL,race_reader,NULL));assert(!pthread_join(writer,NULL));assert(!pthread_join(reader,NULL));}
  unsigned route=num(c,"route");if(!route)route=1;
  cJSON*need=cJSON_GetObjectItemCaseSensitive(c,"need");if(need){pet_bubble_need_t out;bool ok=pet_bubble_pixels_next_need(num(need,"now"),&out);assert(ok==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(need,"ok")));if(ok){assert(out.style==num(need,"style"));assert(out.page==num(need,"page"));}}
  cJSON*v=cJSON_GetObjectItemCaseSensitive(c,"desire");if(v)pet_bubble_pixels_desire(v,(void*)(uintptr_t)route,str(c,"scope"),num(c,"now"));
  v=cJSON_GetObjectItemCaseSensitive(c,"maintain");if(v)pet_bubble_pixels_maintain(num(v,"now"),cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"pause")));
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"failAlloc")))fail_alloc=1;
  v=cJSON_GetObjectItemCaseSensitive(c,"payload");if(v){cJSON*r=cJSON_CreateObject();bool ok=pet_bubble_pixels_handle_owned(v,(void*)(uintptr_t)route,num(c,"now"),r);if(ok!=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"ok"))){fprintf(stderr,"phase=%s expected=%d got=%d error=%s\n",str(v,"phase"),cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"ok")),ok,str(r,"error"));abort();}cJSON*expected=cJSON_GetObjectItemCaseSensitive(c,"reply");if(expected){cJSON*e;cJSON_ArrayForEach(e,expected)assert(cJSON_Compare(e,cJSON_GetObjectItemCaseSensitive(r,e->string),true));}cJSON_Delete(r);}
  v=cJSON_GetObjectItemCaseSensitive(c,"select");if(v){assert(!holding);pet_bubble_pixels_select(str(v,"owner"),num(v,"style"),str(v,"title"),str(v,"body"),num(v,"now"),&held);assert(!strcmp(held.title,str(v,"title"))&&!strcmp(held.body,str(v,"body")));assert(held.pixels==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"pixels")));holding=cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"hold"));if(!holding)pet_bubble_pixels_release(&held);}
  v=cJSON_GetObjectItemCaseSensitive(c,"drawHeld");if(v){assert(holding);unsigned page=num(v,"page");if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"pinPage")))pet_bubble_pixels_pin_page(&held,page);for(unsigned i=0;i<640*480+2;i++)framebuffer[i]=0xffff;framebuffer[0]=framebuffer[640*480+1]=0xabcd;const cJSON*x=cJSON_GetObjectItemCaseSensitive(v,"x"),*clip=cJSON_GetObjectItemCaseSensitive(v,"clip");bool ok=pet_bubble_pixels_draw_pinned(&held,num(v,"style"),page,framebuffer+1,cJSON_IsNumber(x)?x->valueint:0,0,cJSON_IsNumber(clip)?clip->valueint:496,0);assert(ok==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(v,"ok")));assert(framebuffer[0]==0xabcd&&framebuffer[640*480+1]==0xabcd);if(ok){assert(framebuffer[1]==num(v,"first"));}cJSON*expected=cJSON_GetObjectItemCaseSensitive(v,"expectedPixels");if(expected){assert(cJSON_GetArraySize(expected)<=640*480);for(int i=0;i<cJSON_GetArraySize(expected);i++)assert(framebuffer[i+1]==cJSON_GetArrayItem(expected,i)->valueint);} }
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c,"release"))){assert(holding);pet_bubble_pixels_release(&held);holding=false;}
 }
 if (holding) {
  pet_bubble_pixels_release(&held);
 }
 pet_bubble_pixels_maintain(20000,true);
 cJSON_Delete(commands);
 return 0;
}
''')
    vendor=ROOT/'tests/vendor/cjson';binary=t/'receiver'
    flags=['-std=c11','-O1','-Wall','-Wextra','-Werror','-fsanitize=address,undefined','-fno-omit-frame-pointer','-pthread']
    compiler=os.environ.get('CC','cc');vendor_object=t/'cJSON.o'
    # macOS SDK marks upstream cJSON's sprintf deprecated; limit this compiler
    # compatibility exception to unchanged third-party cJSON, never our receiver.
    subprocess.run([compiler,*flags,'-Wno-deprecated-declarations','-I',str(vendor),'-c',str(vendor/'cJSON.c'),'-o',str(vendor_object)],check=True)
    subprocess.run([compiler,*flags,'-I',str(t),'-I',str(vendor),'-I',str(ROOT/'main'),str(h),str(ROOT/'main/pet_p4_bubble_pixels.c'),str(vendor_object),'-lm','-o',str(binary)],check=True)
    count=0
    def run(commands):
        nonlocal count
        count+=1;p=t/f'case-{count}.json';p.write_text(json.dumps(commands));subprocess.run([str(binary),str(p)],check=True,timeout=15)
    return run

def test_ready_then_chunks_checksum_geometry_and_pinned_draw(receiver):
    packets=transfer('body')
    commands=[desire(),{'payload':packets[0],'ok':True,'reply':{'offset':0}}]+send(packets[1:])
    receiver(commands+[select(body='body',pixels=True,hold=True),{'drawHeld':{'style':4,'ok':True,'first':0}},{'release':True}])

def test_no_stage_overwrite_wrong_route_transfer_owner_and_stale_end(receiver):
    a=transfer('a');b=transfer('b',transfer='c'*32)
    receiver([desire(assets=[(4,'a'),(4,'b')])]+send(a[:1])+[{'payload':b[0],'ok':False},{'payload':a[1],'route':2,'ok':False},
        {'payload':{**a[1],'owner':'2'*16},'ok':False},{'payload':{**a[1],'transfer':'d'*32},'ok':False}]+send(a[1:])+[select(body='a',pixels=True)])
    receiver([desire()]+send(a[:2])+[desire(version='0'*15+'2'),{'payload':a[-1],'ok':False},select(body='a',pixels=False)])

def test_idle_absolute_expiry_abort_and_audio_cancel_are_bounded(receiver):
    a=transfer('a')
    for pause,now in [(False,3000),(False,10000),(True,1)]:
        receiver([desire()]+send(a[:1])+[{'maintain':{'now':now,'pause':pause}},{'payload':a[1],'ok':False,'now':now}])
    receiver([desire()]+send(a[:1])+send([{**a[0],'phase':'abort'}])+[{'payload':a[1],'ok':False}]+send(a)+[select(body='a',pixels=True)])

def test_wrong_boot_and_new_client_cancel_without_publishing(receiver):
    a=transfer('a')
    receiver([desire(),{'payload':{**a[0],'boot':'f'*16},'ok':False}]+send(a[:2])+[desire(client='c'*32),{'payload':a[-1],'ok':False}])

def test_metadata_alpha_checksum_and_oom_fail_closed(receiver):
    a=transfer('a')
    for bad in ({'totalBytes':32769},{'pageIndex':1},{'pageCount':17},{'pageSizes':[0]},{'checksum':'no'}):
        receiver([desire(),{'payload':{**a[0],**bad},'ok':False}])
    receiver([desire(),{'failAlloc':True},{'payload':a[0],'ok':False}]+send(a))
    receiver([desire()]+send([{**a[0],'checksum':'00000000'}])+send(a[1:-1])+[{'payload':a[-1],'ok':False}])
    receiver([desire()]+send(a[:1])+[{'payload':{**a[1],'data':'?'*4},'ok':False}])
    short=transfer('short',data=bytes([0]));receiver([desire(assets=[(4,'short')])]+send(short[:-1])+[{'payload':short[-1],'ok':False}])

def test_latest_text_immediate_and_pair_requires_both_assets(receiver):
    receiver([desire(assets=[(1,'old title'),(4,'old body'),(1,'new title'),(4,'new body')])]+send(transfer('old title',style=1))+send(transfer('old body'))+[select('old title','old body',True),
        select('new title','new body',False)]+send(transfer('new title',style=1))+[select('new title','new body',False)]+send(transfer('new body'))+[select('new title','new body',True)])

def test_pins_survive_peer_retirement_and_new_content_has_text_fallback(receiver):
    receiver([desire()]+send(transfer('title',style=1))+send(transfer('body'))+[select('title','body',True,hold=True),
        desire(client='c'*32),{'drawHeld':{'style':1,'ok':True,'first':0}},{'drawHeld':{'style':4,'ok':True,'first':0}},{'release':True},select('title','body',False)])

def test_full_pinned_cache_declines_then_release_retires_safely(receiver):
    commands=[]
    for i in range(8):
        owner=f'{i+1:016x}';commands+=[desire(owner=owner,scope=f'state/{i}',assets=[(4,str(i))])]+send(transfer(str(i),owner=owner))+[select(body=str(i),pixels=True)]
    # Eight recent displayed pages remain pinned by display metadata; no unbounded allocation.
    extra=transfer('overflow');receiver(commands+[desire(scope='state/overflow',assets=[(4,'overflow')])]+send(extra[:-1])+[{'payload':extra[-1],'ok':False},select(body='0',pixels=True)])

def test_page_assets_are_independent_lazy_and_missing_page_falls_back(receiver):
    receiver([desire()]+send(transfer('caption',style=2,page=0,pages=3))+[select(body='caption',style=2,pixels=True,hold=True),
        {'drawHeld':{'style':2,'page':1,'pinPage':True,'ok':False}},{'release':True}]+send(transfer('caption',style=2,page=1,pages=3))+
        [select(body='caption',style=2,pixels=True,hold=True),{'drawHeld':{'style':2,'page':1,'pinPage':True,'ok':True,'first':0}},{'release':True}])


def test_real_mutex_concurrent_draw_and_peer_retirement_keep_pins_safe(receiver):
    batches=[]
    for client in (CLIENT,'c'*32):
        batches.append({'desire':desire(client=client)['desire'],'packets':transfer('title',style=1,client=client)+transfer('body',client=client)})
    receiver([{'race':batches}])


def test_missing_is_bounded_coalesced_and_stops_during_audio(receiver):
    receiver([desire(),select(body='missing'),select(body='missing'),{'need':{'now':1,'ok':True,'style':4,'page':0}},
        select(body='missing'),{'need':{'now':500,'ok':False}},{'need':{'now':1001,'ok':True,'style':4,'page':0}},
        {'maintain':{'now':2001,'pause':True}},select(body='blocked'),{'need':{'now':3001,'ok':False}}])


def test_a_current_owner_cannot_stage_an_unlisted_content_asset(receiver):
    packets=transfer('unlisted');receiver([desire(),{'payload':packets[0],'ok':False}])


def test_state_to_speech_same_display_scope_retires_the_old_stage(receiver):
    old=transfer('a');new=transfer('b',version='0'*15+'2')
    receiver([desire(scope='state/working',assets=[(4,'a')])]+send(old[:1])+
        [desire(scope='speech/text',version='0'*15+'2',assets=[(4,'b')]),{'payload':old[1],'ok':False},
         {'payload':old[-1],'ok':False}]+send(new)+[select(body='b',pixels=True)])


def test_all_alpha_levels_and_negative_clip_preserve_framebuffer_guards(receiver):
    values=list(range(16))+[0]*(496*54-16);encoded=runs(values)
    def shade(alpha):
        r=(31*(15-alpha)+7)//15;g=(63*(15-alpha)+7)//15
        return (r<<11)|(g<<5)|r
    receiver([desire(assets=[(4,'mix')])]+send(transfer('mix',data=encoded))+
        [select(body='mix',pixels=True,hold=True),{'drawHeld':{'style':4,'ok':True,'first':65535,'expectedPixels':[shade(i) for i in range(16)]}},
         {'drawHeld':{'style':4,'ok':True,'x':-1,'clip':3,'first':shade(1),'expectedPixels':[shade(1),shade(2),65535]}},{'release':True}])


def test_complete_manifest_retires_omitted_owner_across_display_scopes(receiver):
    old=transfer('a');owner='2'*16;new=transfer('b',owner=owner)
    receiver([desire(scope='state/working',assets=[(4,'a')])]+send(old[:1])+
        [desire(scope='ui/conversation',owner=owner,assets=[(4,'b')]),{'payload':old[1],'ok':False}]+
        send(new)+[select(body='b',pixels=True)])
