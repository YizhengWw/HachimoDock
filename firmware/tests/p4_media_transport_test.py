"""Media ACKs must use the same queued path as realtime audio."""
from pathlib import Path
import subprocess

def test_media_ack_does_not_drain_before_newline():
    source = (Path(__file__).resolve().parents[1] / "main/pet_p4_main.c").read_text()
    body = source.split("static void transport_send_line(const char *line, void *ctx) {", 1)[1].split("\n}", 1)[0]
    assert 'media_prefix[] = "{\\\"topic\\\":\\\"media/"' in body
    assert "strncmp(line, media_prefix" in body
    assert "transport_send_line_impl(line, ctx, !audio)" in body


def test_real_transport_queues_logs_behind_complete_frames(tmp_path):
    """Compile the production TX functions; simulate asynchronous driver queues.

    Holding a task mutex does not drain those queues. The old console callback
    bypassed them when capture was off and deterministically corrupts this test.
    """
    source = (Path(__file__).resolve().parents[1] / "main/pet_p4_main.c").read_text()
    functions = source.split("static void usb_queue_all", 1)[1].split("static void send_screenshot_chunk", 1)[0]
    harness = tmp_path / "transport.c"
    harness.write_text(r'''
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <pthread.h>
#include <sched.h>
#define PET_P4_UART_NUM 0
#define portMAX_DELAY 0
#define pdMS_TO_TICKS(ms) (ms)
#define ESP_OK 0
typedef pthread_mutex_t *SemaphoreHandle_t;
static pthread_mutex_t mutex = PTHREAD_MUTEX_INITIALIZER;
static SemaphoreHandle_t g_transport_mutex = &mutex;
static bool g_transport_ready = true;
static char g_transport_log_buffer[1024];
typedef enum {PET_P4_TRANSPORT_BROADCAST, PET_P4_TRANSPORT_UART, PET_P4_TRANSPORT_USB_SERIAL_JTAG, PET_P4_TRANSPORT_NETWORK} pet_p4_transport_route_t;
static char net_queue[8192];
static size_t net_used;
static bool net_client = false;
static bool pet_p4_network_client_ready(void) {return net_client;}
static bool pet_p4_network_send_line(const char *line) {
  size_t n=strlen(line);assert(net_used+n+1<sizeof(net_queue));
  memcpy(net_queue+net_used,line,n);net_used+=n;net_queue[net_used++]='\n';return true;
}
static bool pet_p4_net_line_topic(const char *line,char *out,size_t size) {
  const char *p=strstr(line,"\"topic\":\"");if(!p)return false;p+=9;
  const char *q=strchr(p,'"');if(!q||(size_t)(q-p)>=size)return false;memcpy(out,p,(size_t)(q-p));out[q-p]=0;return true;
}
static bool pet_p4_net_broadcast_allowed(const char *topic) {return !strncmp(topic,"input/",6);}
static char usb_queue[262144], uart_queue[262144];
static size_t usb_used, uart_used;
static int drains, console_writes;
static bool usb_seen = true;
static void xSemaphoreTake(SemaphoreHandle_t m, int wait) {(void)wait;assert(!pthread_mutex_lock(m));}
static void xSemaphoreGive(SemaphoreHandle_t m) {assert(!pthread_mutex_unlock(m));}
static bool usb_protocol_seen(void) {return usb_seen;}
static int usb_serial_jtag_write_bytes(const char *p,size_t n,int timeout) {
  (void)timeout;if(n>7)n=7;assert(usb_used+n<sizeof(usb_queue));
  memcpy(usb_queue+usb_used,p,n);usb_used+=n;sched_yield();return (int)n;
}
static int uart_write_bytes(int port,const char *p,size_t n) {
  (void)port;if(n>11)n=11;assert(uart_used+n<sizeof(uart_queue));
  memcpy(uart_queue+uart_used,p,n);uart_used+=n;sched_yield();return (int)n;
}
static int usb_serial_jtag_wait_tx_done(int timeout) {(void)timeout;drains++;return 0;}
static int uart_wait_tx_done(int port,int timeout) {(void)port;(void)timeout;drains++;return 0;}
static void pet_p4_native_usb_send_json_line(const char *line,void *ctx) {(void)line;(void)ctx;}
static int test_vprintf(const char *fmt,va_list args) {(void)fmt;(void)args;console_writes++;return 1;}
#define vprintf test_vprintf
'''+"static void usb_queue_all"+functions+r'''
static int log_line(const char *fmt,...) {va_list ap;va_start(ap,fmt);int n=transport_log_vprintf(fmt,ap);va_end(ap);return n;}
static const char *frame="{\"topic\":\"media/status\",\"payload\":{\"ok\":true}}";
static void *frames(void *arg) {(void)arg;for(int i=0;i<500;i++)transport_send_line(frame,NULL);return NULL;}
static void *logs(void *arg) {(void)arg;for(int i=0;i<500;i++)log_line("I render perf=%d\n",i);return NULL;}
int main(void) {
  // Boot fallback only before protocol drivers/tasks are ready.
  g_transport_ready=false;log_line("boot\n");assert(console_writes==1);g_transport_ready=true;
  transport_send_audio_line(frame,NULL);log_line("I playback started\n");
  assert(!strcmp(usb_queue,"{\"topic\":\"media/status\",\"payload\":{\"ok\":true}}\nI playback started\n"));
  assert(!strcmp(usb_queue,uart_queue));assert(drains==0&&console_writes==1);
  usb_used=uart_used=0;memset(usb_queue,0,sizeof(usb_queue));memset(uart_queue,0,sizeof(uart_queue));
  pthread_t a,b;assert(!pthread_create(&a,NULL,frames,NULL));assert(!pthread_create(&b,NULL,logs,NULL));
  pthread_join(a,NULL);pthread_join(b,NULL);assert(!strcmp(usb_queue,uart_queue));
  char *save;int frame_count=0,log_count=0;
  for(char *line=strtok_r(usb_queue,"\n",&save);line;line=strtok_r(NULL,"\n",&save)) {
    if(!strcmp(line,frame))frame_count++;
    else {int i,n=0;assert(sscanf(line,"I render perf=%d%n",&i,&n)==1&&line[n]==0);log_count++;}
  }
  assert(frame_count==500&&log_count==500&&drains==0&&console_writes==1);
  usb_used=uart_used=0;memset(usb_queue,0,sizeof(usb_queue));memset(uart_queue,0,sizeof(uart_queue));
  char long_log[4096];memset(long_log,'X',sizeof(long_log)-1);long_log[sizeof(long_log)-1]=0;
  assert(log_line("%s",long_log)==4095);assert(usb_used==1023&&usb_queue[1022]=='\n');
  transport_send_line(frame,NULL);assert(!strcmp(usb_queue+1023,"{\"topic\":\"media/status\",\"payload\":{\"ok\":true}}\n"));
  usb_seen=false;size_t prior=usb_used;log_line("uart only");assert(usb_used==prior);
  assert(drains==0&&console_writes==1);
  // Wi-Fi delivery: only allowed broadcast topics reach an attached client; replies to a LAN request
  // stay on the LAN; nothing is sent while no client is authenticated; logs never go to the network.
  usb_used=uart_used=0;memset(usb_queue,0,sizeof(usb_queue));memset(uart_queue,0,sizeof(uart_queue));
  const char *key="{\"topic\":\"input/event\",\"payload\":{}}",*pcm="{\"topic\":\"audio/chunk\",\"payload\":{}}";
  transport_send_line(key,NULL);assert(net_used==0);
  net_client=true;
  transport_send_line(key,NULL);transport_send_line(pcm,NULL);
  assert(!strcmp(net_queue,"{\"topic\":\"input/event\",\"payload\":{}}\n"));
  net_used=0;memset(net_queue,0,sizeof(net_queue));usb_used=uart_used=0;
  static const pet_p4_transport_route_t lan=PET_P4_TRANSPORT_NETWORK;
  transport_send_line(frame,(void *)&lan);
  assert(!strcmp(net_queue,"{\"topic\":\"media/status\",\"payload\":{\"ok\":true}}\n"));
  assert(usb_used==0&&uart_used==0);
  log_line("I never on the lan\n");assert(strstr(net_queue,"never")==NULL);
  puts("PASS actual TX: partial writes, 500 concurrent frames/logs, bounded diagnostics, no console bypass/drain, Wi-Fi routing");
}
''')
    exe = tmp_path / "transport"
    subprocess.run(["cc", "-std=c11", "-D_POSIX_C_SOURCE=200809L", "-Wall", "-Wextra", "-Werror", "-fsanitize=address,undefined", "-pthread", str(harness), "-o", str(exe)], check=True)
    subprocess.run([str(exe)], check=True)
