"""Actual current-source retired native ingress/appearance generation contracts.
System host C only; synthetic state/transport/storage boundaries, no SDK/device.
Whole entry prefix plus real screen-page branch and exact main early guard are
extracted; unrelated dispatcher topics/main debug implementations are not linked.
TemporaryDirectory removes all host compiler products after this module.
"""
from pathlib import Path
import hashlib,json,subprocess,tempfile
import pytest
from p4_control_integrity_test import _c_function as function
FIRMWARE=Path(__file__).resolve().parents[1]
FIXTURE=FIRMWARE/'tests/fixtures/appearance_route_host.c.in'
def ref(p):
 p=Path(p);return {'path':str(p),'bytes':p.stat().st_size,'sha256':hashlib.sha256(p.read_bytes()).hexdigest()}
def build(source,out):
 text=source.read_text();names=['json_string','copy_text','pet_p4_appearance_claim','pet_p4_appearance_owned','pet_p4_appearance_end','pet_p4_appearance_active','pet_p4_appearance_mutation_blocked','appearance_phase_try_begin','pet_p4_appearance_phase_begin','pet_p4_appearance_phase_end','appearance_io_observe','appearance_io_busy','asset_phase_send_current','deferrable_topic','flush_deferred_reply','send_topic_payload','send_asset_ack_indexed','send_asset_ack','pet_p4_protocol_set_deferred_reply','send_asset_activation_ack','handle_asset_activate','commit_owner_current','pet_p4_appearance_commit_slot']
 pieces=[function(text,n) for n in names]
 a=text.index('typedef struct {\n  char manifest[PET_P4_ASSET_MANIFEST_MAX]');b=text.index('} asset_activation_snapshot_t;',a)+len('} asset_activation_snapshot_t;')
 x=text.index('typedef struct {\n  bool asset_transfer,raw_receiving,appearance_owner,file_open,slot_pending;');z=text.index('} appearance_io_observation_t;',x)+len('} appearance_io_observation_t;')
 entry=function(text,'pet_p4_handle_line_with_reply');entry=entry[:entry.index('  root = cJSON_ParseWithOpts')].replace('pet_p4_handle_line_with_reply','entry_probe',1)
 tag='  } else if (strcmp(topic, "control/screen-page") == 0) {';a1=text.index(tag)+len(tag);b1=text.index('  } else if (strcmp(topic, "stats/update")',a1)
 entry+='  root=cJSON_ParseWithOpts(line,NULL,true);payload=cJSON_GetObjectItemCaseSensitive(root,"payload");topic=json_string(root,"topic");\n  if(!strcmp(topic,"control/screen-page")){'+text[a1:b1]+'  }\n  cJSON_Delete(root);return true;\n}\n'
 main=source.with_name('pet_p4_main.c').read_text();start='    /* Retired ordinary native JSON must not reach debug/trace/state dispatch. */'
 guard=main[main.index(start):main.index('#if PET_P4_QUERY_REPLY_TRACE\n    uint64_t query_begin',main.index(start))] if start in main else ''
 native=source.with_name('pet_p4_usb_native.c').read_text();frame=function(native,'handle_complete_frame');frame=frame[:frame.index('  if(kind==PET_P4_NATIVE_KIND_COMMIT)')]+'}\n'
 fallback=function(native,'handle_json_frame')+'\n'+frame
 c=FIXTURE.read_text().replace('@@SNAPSHOT@@',text[a:b]).replace('@@OBSERVATION@@',text[x:z]).replace('@@ACTUAL@@','\n'.join(pieces)).replace('@@ENTRY@@',entry).replace('@@QUEUE_GUARD@@',guard).replace('@@FALLBACK@@',fallback);out.mkdir(mode=0o700)
 src=out/'actual.c';src.write_text(c);exe=out/'actual'
 cmd=['/usr/bin/cc','-std=c11','-O1','-g','-Wall','-Wextra','-Werror','-Wno-unused-function','-Wno-unused-parameter','-Wno-deprecated-declarations','-fsanitize=address,undefined','-I',str(FIRMWARE/'main'),'-I',str(FIRMWARE/'tests/vendor/cjson'),str(src),str(FIRMWARE/'tests/vendor/cjson/cJSON.c'),str(FIRMWARE/'main/pet_p4_resources.c'),'-o',str(exe)]
 (out/'home').mkdir(mode=0o700);(out/'tmp').mkdir(mode=0o700)
 env={'PATH':'/usr/bin:/bin:/usr/sbin:/sbin','HOME':str(out/'home'),'TMPDIR':str(out/'tmp'),'LANG':'C','ASAN_OPTIONS':'detect_leaks=0'}
 r=subprocess.run(cmd,env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=60)
 (out/'compile.stdout').write_text(r.stdout);(out/'compile.stderr').write_text(r.stderr)
 (out/'compile.json').write_text(json.dumps({'argv':cmd,'exitCode':r.returncode,'source':ref(source),'extraction':[{ 'name':n,'sha256':hashlib.sha256(s.encode()).hexdigest()} for n,s in zip(names,pieces)]},indent=2))
 if r.returncode:raise RuntimeError('compile failed: '+r.stderr)
 return exe,env
CASES=['activate-current','activate-retired','activate-prepare','activate-begin2','activate-marker','activate-uart-prepare','commit-current','commit-retired','commit-prepare','commit-begin1','commit-begin2','commit-raw','commit-ready','commit-begin3','commit-marker','commit-uart-prepare','activate-retired-deferred','activate-current-deferred','commit-ready-deferred','entry-retired','entry-current','entry-uart-retired','queue-retired','queue-current','queue-uart-retired','phase-native-begin1','phase-uart-io-current-begin1','phase-current','fallback-current','fallback-retired-between','fallback-retired-at-frame','fallback-enqueue-current']
@pytest.fixture(scope='module')
def route_binary(tmp_path_factory):
 with tempfile.TemporaryDirectory(prefix='appearance-route-',dir=tmp_path_factory.getbasetemp()) as name:
  yield build(FIRMWARE/'main/pet_p4_protocol.c',Path(name)/'build')
@pytest.mark.parametrize('case',CASES)
def test_actual_native_ingress_and_appearance_route_boundaries(route_binary,case):
 exe,env=route_binary
 result=subprocess.run([str(exe),case],env=env,stdout=subprocess.PIPE,stderr=subprocess.PIPE,text=True,timeout=10)
 assert result.returncode==0,(case,result.returncode,result.stdout,result.stderr)
 assert result.stdout.startswith('PASS '+case+' ')
