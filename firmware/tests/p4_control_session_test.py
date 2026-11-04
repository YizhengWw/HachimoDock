"""Actual negotiated C writer/session, legacy/native isolation, no hardware IO."""
from pathlib import Path
import configparser
import subprocess
from p4_control_integrity_test import actual_serial_tx_source

ROOT=Path(__file__).resolve().parents[1]

def test_actual_writer_negotiates_each_route_and_old_client_reconnects_without_reboot(tmp_path):
    source=actual_serial_tx_source().split('int main(void){',1)[0]
    marker='static int g_transport_mutex=1;'
    assert source.count(marker)==1
    source=source.replace(marker,'''#include "pet_p4_control_session.h"
#include "cJSON.h"
static const pet_p4_transport_route_t g_uart_route=PET_P4_TRANSPORT_UART;
static const pet_p4_transport_route_t g_usb_serial_jtag_route=PET_P4_TRANSPORT_USB_SERIAL_JTAG;
static unsigned mode_reads;
static bool switch_after_observation;
static bool observed_mode(const void *route){
 bool result=pet_control_session_framed(route);mode_reads++;
 if(switch_after_observation){
  switch_after_observation=false;
  assert(pet_control_session_negotiate(route,true,PET_CONTROL_INTEGRITY_ENGINE,"0123456789abcdef0123456789abcdef"));
 }
 return result;
}
#define pet_control_session_framed observed_mode
'''+marker)
    source+=r'''
static const char nonce[]="0123456789abcdef0123456789abcdef";
static bool proposal(const void *route,const char *json){
 cJSON *p=cJSON_Parse(json);assert(p);bool ok=pet_control_session_payload(route,p);cJSON_Delete(p);return ok;
}
static void plain(const void *route,unsigned index){
 reset();transport_send_line("{}",(void*)route);assert(!strcmp(wire[index],"{}\n")&&!strchr(wire[index],'!'));
}
static void framed(const void *route,unsigned index){
 reset();transport_send_line("{}",(void*)route);assert(!strncmp(wire[index],"!P4C1:00002:",12));
 assert(!strcmp(wire[index]+21,"{}\n"));
}
static void caps(const void *route,bool supported,bool active){
 cJSON *p=cJSON_CreateObject();assert(p);pet_control_session_capabilities(route,p);
 assert((cJSON_GetObjectItemCaseSensitive(p,"serialControlOffer")!=NULL)==supported);
 assert((cJSON_GetObjectItemCaseSensitive(p,"serialControlIntegrity")!=NULL)==active);
 const cJSON *n=cJSON_GetObjectItemCaseSensitive(p,"serialControlNonce");
 assert((n!=NULL)==active);if(n)assert(!strcmp(n->valuestring,nonce));cJSON_Delete(p);
}
int main(void){
 int native=17;pet_control_session_snapshot_t snapshot={0};
 assert(!pet_control_sessions_init(NULL,&g_usb_serial_jtag_route));
 assert(!pet_control_sessions_init(&g_uart_route,&g_uart_route));
 assert(pet_control_sessions_init(&g_uart_route,&g_usb_serial_jtag_route));
 assert(!pet_control_sessions_init(&g_uart_route,&g_usb_serial_jtag_route));
 plain(&g_uart_route,0);plain(&g_usb_serial_jtag_route,1);
 caps(&g_uart_route,true,false);caps(&native,false,false);caps(NULL,false,false);
 assert(pet_control_session_negotiate(&g_uart_route,true,PET_CONTROL_INTEGRITY_ENGINE,nonce));
 framed(&g_uart_route,0);plain(&g_usb_serial_jtag_route,1);caps(&g_uart_route,true,true);
 assert(pet_control_session_snapshot(&g_uart_route,&snapshot)&&snapshot.supported&&snapshot.framed&&!strcmp(snapshot.nonce,nonce));
 const char *bad[]={
  "{\"serialControl\":null}","{\"serialControl\":true}","{\"serialControl\":{}}",
  "{\"serialControl\":{\"engine\":\"bad\",\"nonce\":\"0123456789abcdef0123456789abcdef\"}}",
  "{\"serialControl\":{\"engine\":\"p4-control-fnv1a32-v1\",\"nonce\":\"short\"}}",
  "{\"serialControl\":{\"engine\":\"p4-control-fnv1a32-v1\",\"nonce\":\"0123456789abcdef0123456789abcdeF\"}}",
  "{\"serialControl\":{\"engine\":\"p4-control-fnv1a32-v1\",\"nonce\":\"0123456789abcdef0123456789abcdef\",\"extra\":1}}",
  "{\"serialControl\":{\"engine\":\"p4-control-fnv1a32-v1\",\"engine\":\"p4-control-fnv1a32-v1\",\"nonce\":\"0123456789abcdef0123456789abcdef\"}}",
  "{\"serialControl\":{},\"serialControl\":{}}"
 };
 for(unsigned i=0;i<sizeof(bad)/sizeof(bad[0]);i++){assert(!proposal(&g_uart_route,bad[i]));framed(&g_uart_route,0);caps(&g_uart_route,true,true);}
 assert(pet_control_session_negotiate(&g_usb_serial_jtag_route,true,PET_CONTROL_INTEGRITY_ENGINE,nonce));
 framed(&g_usb_serial_jtag_route,1);
 assert(proposal(&g_uart_route,"{\"desktopDeviceId\":\"old-client\"}"));
 plain(&g_uart_route,0);framed(&g_usb_serial_jtag_route,1);caps(&g_uart_route,true,false);
 assert(pet_control_session_negotiate(&native,true,PET_CONTROL_INTEGRITY_ENGINE,nonce));
 assert(pet_control_session_negotiate(NULL,false,NULL,NULL));caps(&native,false,false);
 plain(&g_uart_route,0);framed(&g_usb_serial_jtag_route,1);
 reset();transport_send_line("{}",NULL);assert(!strcmp(wire[0],"{}\n")&&!strncmp(wire[1],"!P4C1:",6)&&native_calls==1);
 reset();transport_send_audio_line("{}",(void*)&g_usb_serial_jtag_route);
 assert(!strcmp(wire[1],"{}\n")&&!drains[1]);
 unsigned before=mode_reads;reset();transport_send_audio_line("{}",(void*)&g_usb_serial_jtag_route);
 assert(mode_reads==before); /* No policy/atomic work added to audio. */
 assert(pet_control_session_negotiate(&g_uart_route,false,NULL,NULL));
 before=mode_reads;switch_after_observation=true;plain(&g_uart_route,0);
 assert(mode_reads==before+1);framed(&g_uart_route,0); /* Next line uses the new mode, never this line's body. */
 assert(!pet_control_session_snapshot(&g_uart_route,NULL));return 0;
}
'''
    path=tmp_path/'session.c';path.write_text(source);exe=tmp_path/'session'
    # Keep warnings fatal for our runtime/harness. The retained third-party
    # source uses sprintf; suppress only Apple's deprecation warning in that
    # vendor translation unit, not in any project code or firmware build.
    vendor=tmp_path/'cjson.o'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-deprecated-declarations',
      '-fsanitize=address,undefined','-I',str(ROOT/'tests/vendor/cjson'),'-c',
      str(ROOT/'tests/vendor/cjson/cJSON.c'),'-o',str(vendor)],check=True)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsanitize=address,undefined',
      '-DPET_P4_SERIAL_CONTROL_INTEGRITY=1','-DPET_P4_SERIAL_CONTROL_NEGOTIATED=1','-DPET_P4_HELLO_ASYNC=1',
      '-I',str(ROOT/'main'),'-I',str(ROOT/'tests/vendor/cjson'),str(path),
      str(ROOT/'main/pet_p4_control_session.c'),str(ROOT/'main/pet_p4_transport_audit.c'),
      str(vendor),'-lm','-o',str(exe)],check=True)
    subprocess.run([str(exe)],check=True,timeout=10)

def test_negotiation_is_private_and_requires_serialized_hello(tmp_path):
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    for section in config.sections():
        assert ('PET_P4_SERIAL_CONTROL_NEGOTIATED=1' in config[section].get('build_flags',''))==(section=='env:esp32_p4_negotiated_parent')
    header=tmp_path/'guard.c';header.write_text('#include "pet_p4_control_session.h"\n')
    result=subprocess.run(['cc','-std=c11','-fsyntax-only','-DPET_P4_SERIAL_CONTROL_NEGOTIATED=1','-I',str(ROOT/'main'),str(header)],capture_output=True,text=True)
    assert result.returncode and 'requires control integrity and serialized async HELLO' in result.stderr
    obj=tmp_path/'disabled.o'
    subprocess.run(['cc','-std=c11','-c','-I',str(ROOT/'main'),str(ROOT/'main/pet_p4_control_session.c'),'-o',str(obj)],check=True)
    symbols=subprocess.run(['nm',str(obj)],capture_output=True,text=True,check=True).stdout
    assert 'sessions' not in symbols and 'pet_control_session' not in symbols
