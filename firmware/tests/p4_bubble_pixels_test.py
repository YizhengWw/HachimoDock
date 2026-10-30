"""Execute the production image receiver/mixer, including interrupted transfers."""
import base64
import json
import os
import shlex
import subprocess
from pathlib import Path
import pytest

ROOT = Path(__file__).resolve().parents[1]


def key(style, text):
    value = 14695981039346656037
    for byte in bytes([style]) + text.encode():
        value = ((value ^ byte) * 1099511628211) & ((1 << 64) - 1)
    return f'{value:016x}'


def transfer(text, pages, style=2, checksum_override=None):
    data = b''.join(pages)
    checksum = 2166136261
    for byte in data:
        checksum = ((checksum ^ byte) * 16777619) & 0xffffffff
    ident = key(style, text)
    return [dict(phase='begin', id=ident, style=style, totalBytes=len(data),
                 pageSizes=list(map(len, pages)), checksum=checksum_override or f'{checksum:08x}')] + [
        dict(phase='chunk', id=ident, offset=i, data=base64.b64encode(data[i:i+1024]).decode())
        for i in range(0, len(data), 1024)
    ] + [dict(phase='end', id=ident)]


def runs(values):
    result = bytearray()
    for value in values:
        if result and result[-1] >> 4 == value and result[-1] & 15 < 15:
            result[-1] += 1
        else:
            result.append(value << 4)
    return bytes(result)


@pytest.fixture(scope='module')
def receiver(tmp_path_factory):
    tmp = tmp_path_factory.mktemp('bubble-receiver')
    (tmp / 'freertos').mkdir()
    (tmp / 'mbedtls').mkdir()
    (tmp / 'freertos/FreeRTOS.h').write_text('#define portMAX_DELAY 0\n')
    (tmp / 'freertos/semphr.h').write_text('''typedef void *SemaphoreHandle_t;
static inline void *xSemaphoreCreateMutex(void){return (void*)1;}
static inline int xSemaphoreTake(void *m,int t){(void)m;(void)t;return 1;}
static inline void xSemaphoreGive(void *m){(void)m;}
''')
    (tmp / 'esp_heap_caps.h').write_text('''#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int fail_alloc;
static inline void *heap_caps_malloc(size_t n,int c){(void)c;if(fail_alloc){fail_alloc=0;return NULL;}return malloc(n);}
''')
    (tmp / 'mbedtls/base64.h').write_text('''#include <stddef.h>
int mbedtls_base64_decode(unsigned char*,size_t,size_t*,const unsigned char*,size_t);
''')
    harness = tmp / 'main.c'
    harness.write_text(r'''
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "pet_p4_bubble_pixels.h"
int fail_alloc;
/* Transport-library stand-in; receiver/state/mixer are the production code. */
int mbedtls_base64_decode(unsigned char *out,size_t cap,size_t *n,const unsigned char *in,size_t len){
 const char *alphabet="ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
 unsigned value=0,bits=0;*n=0;
 for(size_t i=0;i<len;i++){if(in[i]=='=')break;const char *p=strchr(alphabet,in[i]);if(!p)return -1;
  value=(value<<6)|(unsigned)(p-alphabet);bits+=6;
  if(bits>=8){bits-=8;if(*n>=cap)return -1;out[(*n)++]=(unsigned char)(value>>bits);}}
 return 0;
}
static uint16_t framebuffer[640*480+2];
int main(int argc,char **argv){
 assert(argc==2);assert(pet_bubble_pixels_init());
 FILE *f=fopen(argv[1],"rb");assert(f);fseek(f,0,SEEK_END);long size=ftell(f);rewind(f);
 char *data=calloc((size_t)size+1,1);assert(data);assert(fread(data,1,(size_t)size,f)==(size_t)size);fclose(f);
 cJSON *commands=cJSON_Parse(data);assert(commands);free(data);
 cJSON *command;
 cJSON_ArrayForEach(command,commands){
  cJSON *expect=cJSON_GetObjectItemCaseSensitive(command,"expectPixels");
  if(expect)pet_bubble_pixels_expect(cJSON_IsTrue(expect),0);
  cJSON *select=cJSON_GetObjectItemCaseSensitive(command,"select");
  if(select){pet_bubble_text_view_t selected={0};
   pet_bubble_pixels_select(cJSON_GetObjectItemCaseSensitive(select,"owner")->valuestring,
    (unsigned)cJSON_GetObjectItemCaseSensitive(select,"style")->valueint,
    cJSON_GetObjectItemCaseSensitive(select,"title")->valuestring,
    cJSON_GetObjectItemCaseSensitive(select,"body")->valuestring,
    (uint64_t)cJSON_GetObjectItemCaseSensitive(select,"now")->valuedouble,&selected);
   assert(!strcmp(selected.title,cJSON_GetObjectItemCaseSensitive(select,"shownTitle")->valuestring));
   assert(!strcmp(selected.body,cJSON_GetObjectItemCaseSensitive(select,"shownBody")->valuestring));
   assert(selected.pixels==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(select,"pixels")));
  }
  cJSON *payload=cJSON_GetObjectItemCaseSensitive(command,"payload");
  if(payload){char error[96];bool ok=pet_bubble_pixels_handle(payload,error,sizeof(error));
   assert(ok==cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(command,"ok")));}
  if(cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(command,"failAlloc")))fail_alloc=1;
  cJSON *draw=cJSON_GetObjectItemCaseSensitive(command,"draw");
  if(draw){const char *text=cJSON_GetObjectItemCaseSensitive(draw,"text")->valuestring;
   unsigned style=(unsigned)cJSON_GetObjectItemCaseSensitive(draw,"style")->valueint;
   unsigned pages=(unsigned)cJSON_GetObjectItemCaseSensitive(draw,"pages")->valueint;
   assert(pet_bubble_pixels_pages(style,text)==pages);
   if(!cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(draw,"preserve")))
    for(int i=0;i<640*480+2;i++)framebuffer[i]=0xffff;
   framebuffer[0]=framebuffer[640*480+1]=0xabcd;
   cJSON *at=cJSON_GetObjectItemCaseSensitive(draw,"at");int x=at?cJSON_GetArrayItem(at,0)->valueint:0,y=at?cJSON_GetArrayItem(at,1)->valueint:0;
   cJSON *page=cJSON_GetObjectItemCaseSensitive(draw,"page");unsigned p=page?(unsigned)page->valueint:0;
   bool ok=pet_bubble_pixels_draw(style,text,p,framebuffer+1,x,y,496,0);
   assert(ok==(pages>0));assert(framebuffer[0]==0xabcd&&framebuffer[640*480+1]==0xabcd);
   cJSON *expected=cJSON_GetObjectItemCaseSensitive(draw,"pixels");
   if(expected)for(int i=0;i<cJSON_GetArraySize(expected);i++)assert(framebuffer[1+i]==cJSON_GetArrayItem(expected,i)->valueint);
  }
 }
 const char *preview=getenv("PET_BUBBLE_TEST_FRAME");
 if(preview){FILE *out=fopen(preview,"wb");assert(out);fwrite(framebuffer+1,2,640*480,out);fclose(out);}
 cJSON_Delete(commands);return 0;
}
''')
    vendor = ROOT / 'tests/vendor/cjson'
    binary = tmp / ('receiver.exe' if os.name == 'nt' else 'receiver')
    command = shlex.split(os.environ.get('CC', 'cc')) + ['-std=c11', '-Wall', '-Wextra', '-Werror',
        '-I', str(tmp), '-I', str(vendor), '-I', str(ROOT / 'main'),
        str(harness), str(ROOT / 'main/pet_p4_bubble_pixels.c'), str(vendor / 'cJSON.c'), '-lm', '-o', str(binary)]
    subprocess.run(command, check=True)
    def run(commands):
        fixture = tmp / 'commands.json'
        fixture.write_text(json.dumps(commands, ensure_ascii=False), encoding='utf-8')
        subprocess.run([str(binary), str(fixture)], check=True, timeout=30)
    return run


def send(packets):
    return [dict(payload=packet, ok=True) for packet in packets]


def draw(text='你好', pages=1, style=2, **kwargs):
    return dict(draw=dict(text=text, style=style, pages=pages, **kwargs))


def test_atomic_commit_unicode_keys_alpha_pages_and_clipping(receiver):
    assert key(2, '你好') == '41c0fcd4504ad4c3'
    page = runs([0, 15, 8] + [0] * (496 * 54 - 3))
    second = runs([15] * (496 * 54))
    packets = transfer('你好', [page, second])
    receiver([draw(pages=0)] + send(packets[:-1]) + [draw(pages=0)] + send(packets[-1:]) + [
        draw(pages=2, pixels=[65535, 0, 0x73ae]), draw(pages=2, page=1, pixels=[0, 0, 0]),
        draw(pages=2, page=1, at=[639, 479]), draw(text='另一条消息', pages=0)])


def test_missing_reordered_or_corrupt_chunks_never_replace_a_good_image(receiver):
    page = runs([15] * (496 * 54))
    good = transfer('你好', [page])
    broken = transfer('新内容', [page], checksum_override='00000000')
    receiver(send(good) + send(broken[:-1]) + [dict(payload=broken[-1], ok=False),
        draw(pixels=[0, 0]), draw(text='新内容', pages=0)])
    receiver(send(good[:1]) + [dict(payload=good[2], ok=False), dict(payload=good[-1], ok=False), draw(pages=0)])


def test_bad_metadata_run_lengths_and_allocation_failure_keep_text_fallback(receiver):
    good = transfer('你好', [runs([15] * (496 * 54))])
    malformed = transfer('坏数据', [b'\xff'])
    receiver(send(good) + send(malformed[:-1]) + [dict(payload=malformed[-1], ok=False),
        draw(text='坏数据', pages=0), dict(failAlloc=True),
        dict(payload=transfer('分配失败', [runs([0] * (496 * 54))])[0], ok=False), draw(pixels=[0])])
    for changes in [dict(style=0), dict(style=99), dict(totalBytes=131073), dict(pageSizes=[-1]), dict(pageSizes=[1.5])]:
        begin = {**good[0], **changes}
        receiver([dict(payload=begin, ok=False), draw(pages=0)])


def test_cache_byte_budget_evicts_occupied_entries_even_with_free_slots(receiver):
    # Four noisy pages occupy ~107 KiB; byte budget is reached before 32 slots.
    page = bytes([0xf0, 0x00]) * (496 * 54 // 2)
    commands = []
    for i in range(11):
        commands += send(transfer(str(i), [page] * 4))
    receiver(commands + [draw(text='0', pages=0), draw(text='10', pages=4)])


def test_agent_two_line_images_are_separate_from_paginated_captions(receiver):
    page = runs([15] * (496 * 54))
    receiver(send(transfer('你好', [page], style=4)) + [
        draw(style=4, pixels=[0, 0]), draw(style=2, pages=0)])
    receiver([dict(payload=transfer('你好', [page, page], style=4)[0], ok=False)])


def select(title, body, now=0, shown=None, pixels=True, owner='session-a', style=4):
    shown = (title, body) if shown is None else shown
    return dict(select=dict(owner=owner, style=style, title=title, body=body, now=now,
                            shownTitle=shown[0], shownBody=shown[1], pixels=pixels))


def title_image(title):
    return send(transfer(title, [runs([15] * (474 * 38))], style=1))


def body_image(body):
    return send(transfer(body, [runs([15] * (496 * 54))], style=4))


def test_new_title_and_body_commit_together_only_after_both_images_finish(receiver):
    packets = transfer('新正文', [runs([15] * (496 * 54))], style=4)
    receiver([dict(expectPixels=True), select('新标题', '新正文', shown=('', ''), pixels=False)]
             + title_image('新标题') + [select('新标题', '新正文', 100, shown=('', ''), pixels=False)]
             + send(packets[:-1]) + [select('新标题', '新正文', 200, shown=('', ''), pixels=False)]
             + send(packets[-1:]) + [select('新标题', '新正文', 300)])


def test_previous_pair_stays_visible_until_new_pair_is_complete_and_owners_are_isolated(receiver):
    old = ('旧标题', '旧正文')
    receiver([dict(expectPixels=True)] + title_image(old[0]) + body_image(old[1]) + [select(*old)]
             + title_image('新标题') + [select('新标题', '新正文', 10, shown=old),
                 select('新标题', '新正文', 2900, shown=old),
                 select('其他会话', '私有内容', 2900, shown=('', ''), pixels=False, owner='session-b')]
             + body_image('新正文') + [select('新标题', '新正文', 2990)])


def test_streaming_cannot_extend_timeout_and_legacy_client_never_waits(receiver):
    old = ('旧标题', '旧正文')
    receiver([dict(expectPixels=True)] + title_image(old[0]) + body_image(old[1]) + [select(*old),
        select('标题', '片段一', 100, shown=old), select('标题', '片段二', 2500, shown=old),
        select('标题', '片段三', 3100, pixels=False),
        dict(expectPixels=False), select('旧客户端', '立即显示', 3200, pixels=False)]
        + title_image('恢复') + body_image('像素') + [dict(expectPixels=True), select('恢复', '像素', 3300)])


def test_visible_old_images_survive_cache_pressure_while_waiting(receiver):
    old = ('旧标题', '旧正文')
    commands = [dict(expectPixels=True)] + title_image(old[0]) + body_image(old[1]) + [select(*old)]
    for index in range(40):
        commands += body_image(f'后台{index}')
    receiver(commands + [draw(text=old[0], style=1), draw(text=old[1], style=4),
                         select('新标题', '仍在等待', 100, shown=old)])


def test_replacement_that_exceeds_pinned_budget_preserves_the_old_image(receiver):
    noisy = bytes([0xf0, 0x00]) * (496 * 54 // 2)
    commands = [dict(expectPixels=True)]
    commands += send(transfer('保留', [runs([15] * (496 * 54))]))
    commands += [select('', '保留', owner='keep', style=2)]
    for index in range(9):
        text = f'大图{index}'
        commands += send(transfer(text, [noisy] * 4))
        commands += [select('', text, owner=text, style=2)]
    replacement = transfer('保留', [noisy] * 4)
    receiver(commands + send(replacement[:-1]) + [dict(payload=replacement[-1], ok=False),
              draw(text='保留', pages=1), select('', '保留', owner='keep', style=2)])
