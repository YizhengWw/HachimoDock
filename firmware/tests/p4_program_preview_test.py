"""Product authoring ABI matches native C for ten generic logic workloads.

All files contain synthetic vectors. No device/credentials/audio/network. A host
result is not a P4 timing or production execution acceptance claim.
"""
from pathlib import Path
import base64
import json
import os
import struct
import subprocess
import zlib

ROOT = Path(__file__).resolve().parents[2]


def test_product_authoring_abi_matches_native_c_for_ten_cases_and_all_quanta(tmp_path):
    fixture = ROOT / 'pc/skills/petui/contracts/program-preview-v1.json'
    node = os.environ.get('PET_FRAMEWORK_NODE', 'node')
    script = r'''
import fs from 'node:fs';
import {encodeProgram,createProgramPreview} from './pc/src/component-center/program-core.js';
const suite=JSON.parse(fs.readFileSync(process.argv[1]));const output=[];
for(const c of suite.cases)for(const quantum of [1,7,64]){
 const bytes=encodeProgram({engine:suite.engine,schema:37,cells:c.cells,fuel:512,code:c.code});
 const seed={schema:37,revision:0,cells:c.cells,values:c.initial,outputs:Array(8).fill(0)};
 const p=createProgramPreview(bytes,{seed,quantum}),reports=[];
 for(const [i,event]of c.events.entries()){
  if(p.enqueue(i+1,event)!=='ok')throw Error('enqueue');let done=false;
  for(let n=0;n<515;n++){const r=p.pump();if(r==='complete'){reports.push(p.takeReport());done=true;break;}if(r!=='yielded')throw Error('pump');}
  if(!done)throw Error('fuel bound');
 }
 output.push({name:c.name,quantum,seed,events:c.events,image:Buffer.from(bytes).toString('base64'),reports,snapshot:p.snapshot()});
}
for(let port=0;port<8;port++)for(const quantum of [1,7,64]){
 const bytes=encodeProgram({engine:suite.engine,schema:9,cells:1,fuel:32,code:[['CONST',0,0,0,0],['EVENT',1,0,0,0],['STORE',0,1,0,0],['OUTPUT',port,1,0,0],['HALT',0,0,0,0]]});
 const seed={schema:9,revision:0,cells:1,values:[0],outputs:Array(8).fill(0)},limits=Array.from({length:8},()=>({min:-2147483648,max:2147483647}));limits[port]={min:-10,max:10};
 const events=[[-10],[10],[-11],[11]],p=createProgramPreview(bytes,{seed,quantum,outputLimits:limits}),reports=[];
 for(const [i,event]of events.entries()){
  if(p.enqueue(i+1,event)!=='ok')throw Error('enqueue');let done=false;
  for(let n=0;n<35;n++){const r=p.pump();if(r==='complete'){reports.push(p.takeReport());done=true;break;}if(r!=='yielded')throw Error('pump');}
  if(!done)throw Error('bound');
 }
 output.push({name:`bounded-output-${port}`,quantum,seed,events,limits,image:Buffer.from(bytes).toString('base64'),reports,snapshot:p.snapshot()});
}
console.log(JSON.stringify(output));
'''
    result = subprocess.run([node, '--input-type=module', '-e', script, str(fixture)],
                            cwd=ROOT, check=True, capture_output=True, text=True, timeout=30)
    cases = json.loads(result.stdout)
    assert len(cases) == 54 and len(set(c['name'] for c in cases)) == 18
    code = r'''
#include "pet_p4_program_runtime.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
uint8_t *pp_buffer(void);int pp_configure(unsigned);void pp_seed(unsigned,unsigned,unsigned);
void pp_seed_cell(unsigned,int32_t);void pp_seed_output(unsigned,int32_t);int pp_start(unsigned,int);
int pp_limit(unsigned,int32_t,int32_t);
int pp_enqueue(unsigned,unsigned,unsigned,int32_t,int32_t,int32_t,int32_t);int pp_pump(void);int pp_report(void);
unsigned pp_report_result(void),pp_report_revision(void),pp_report_fuel(void),pp_report_low(void),pp_report_high(void);
int pp_snapshot(void);unsigned pp_revision(void);int32_t pp_cell(unsigned),pp_output(unsigned);
static uint32_t word(FILE *f){unsigned char bytes[4];assert(fread(bytes,1,4,f)==4);return (uint32_t)bytes[0]|((uint32_t)bytes[1]<<8)|((uint32_t)bytes[2]<<16)|((uint32_t)bytes[3]<<24);}
static int32_t signed_word(FILE *f){uint32_t bits=word(f);int32_t out;memcpy(&out,&bits,4);return out;}
int main(int argc,char **argv){
 assert(argc==2);FILE *f=fopen(argv[1],"rb");assert(f);unsigned length=word(f),quantum=word(f);
 assert(length<=PET_PROGRAM_IMAGE_MAX_BYTES&&quantum>=1&&quantum<=64);
 assert(fread(pp_buffer(),1,length,f)==length);assert(pp_configure(length)==PET_PROGRAM_IMAGE_OK);
 unsigned schema=word(f),revision=word(f),cells=word(f);assert(cells<=64);pp_seed(schema,revision,cells);
 for(unsigned i=0;i<64;i++)pp_seed_cell(i,signed_word(f));
 for(unsigned i=0;i<8;i++)pp_seed_output(i,signed_word(f));
 unsigned limited=word(f);assert(limited<=1);if(limited)for(unsigned i=0;i<8;i++){int32_t lo=signed_word(f),hi=signed_word(f);assert(pp_limit(i,lo,hi)==1);}
 assert(pp_start(quantum,1)==PET_RUNTIME_OK);unsigned events=word(f);assert(events>=1&&events<=32);printf("{\"reports\":[");
 for(unsigned i=0;i<events;i++){
  unsigned low=word(f),high=word(f),count=word(f);assert(count<=4);int32_t words[4];for(unsigned j=0;j<4;j++)words[j]=signed_word(f);
  assert(pp_enqueue(low,high,count,words[0],words[1],words[2],words[3])==PET_RUNTIME_OK);bool done=false;
  for(unsigned n=0;n<515;n++){int r=pp_pump();if(r==PET_RUNTIME_COMPLETE){done=true;break;}assert(r==PET_RUNTIME_YIELDED);}
  assert(done&&pp_report()==PET_RUNTIME_OK);unsigned result=pp_report_result();assert(result==PET_PROGRAM_COMMITTED||result==PET_PROGRAM_BOUNDS);
  printf("%s{\"sequence\":%llu,\"result\":\"%s\",\"revision\":%u,\"usedFuel\":%u}",i?",":"",(unsigned long long)(((uint64_t)pp_report_high()<<32)|pp_report_low()),result==PET_PROGRAM_COMMITTED?"committed":"bounds",pp_report_revision(),pp_report_fuel());
 }
 assert(fgetc(f)==EOF);fclose(f);assert(pp_snapshot()==PET_RUNTIME_OK);
 printf("],\"snapshot\":{\"schema\":%u,\"revision\":%u,\"cells\":%u,\"values\":[",schema,pp_revision(),cells);
 for(unsigned i=0;i<cells;i++)printf("%s%d",i?",":"",pp_cell(i));
 printf("],\"outputs\":[");
 for(unsigned i=0;i<8;i++)printf("%s%d",i?",":"",pp_output(i));
 printf("]}}\n");
 return 0;
}
'''
    source, executable = tmp_path / 'preview.c', tmp_path / 'preview'
    source.write_text(code)
    sources = [ROOT / 'firmware/main' / name for name in (
        'pet_p4_program_core.c', 'pet_p4_program_image.c', 'pet_p4_program_runtime.c', 'pet_p4_program_timers.c')]
    subprocess.run(['cc', '-std=c11', '-O1', '-Wall', '-Wextra', '-Werror',
                    '-fsanitize=address,undefined', '-I', str(ROOT / 'firmware/main'),
                    *map(str, sources), str(ROOT / 'pc/scripts/game-wasm/program_bridge.c'),
                    str(source), '-o', str(executable)], check=True)
    for c in cases:
        image = base64.b64decode(c['image'], validate=True)
        assert image[:4] == b'P4P1' and len(image) == 24 + 8 * struct.unpack_from('<H', image, 8)[0]
        assert struct.unpack_from('<I', image, 20)[0] == zlib.crc32(image[:20] + image[24:])
        s = c['seed']
        raw = struct.pack('<II', len(image), c['quantum']) + image
        raw += struct.pack('<III', s['schema'], s['revision'], s['cells'])
        raw += struct.pack('<64i8i', *(s['values'] + [0] * (64 - s['cells'])), *s['outputs'])
        raw += struct.pack('<I', int('limits' in c))
        for limit in c.get('limits', []):
            raw += struct.pack('<ii', limit['min'], limit['max'])
        raw += struct.pack('<I', len(c['events']))
        for i, event in enumerate(c['events']):
            raw += struct.pack('<III4i', i + 1, 0, len(event), *(event + [0] * (4 - len(event))))
        path = tmp_path / 'synthetic-input.bin'
        path.write_bytes(raw)
        native = subprocess.run([str(executable), str(path)], check=True, capture_output=True, text=True, timeout=10)
        assert json.loads(native.stdout) == {'reports': c['reports'], 'snapshot': c['snapshot']}, (c['name'], c['quantum'])


def test_product_program_artifacts_are_reproducible_and_do_not_enable_device_execution():
    subprocess.run([os.environ.get('PET_FRAMEWORK_NODE', 'node'),
                    str(ROOT / 'pc/scripts/build-program-runtime.mjs'), '--check'], check=True, cwd=ROOT, timeout=30)
    native = (ROOT / 'firmware/main/pet_p4_resources.c').read_text()
    assert '[PET_RESOURCE_PROGRAM]={0,0}' in native and '[PET_RESOURCE_PERIPHERAL]={0,0}' in native
    bridge = (ROOT / 'pc/scripts/game-wasm/program_bridge.c').read_text()
    for forbidden in ('malloc(', 'fopen(', 'esp_', 'pet_app_store_', 'pet_program_host_', 'pet_resources_'):
        assert forbidden not in bridge


def test_explicit_stdin_preview_cli_is_bounded_local_and_never_replays_after_trap():
    c = json.loads((ROOT / 'pc/skills/petui/contracts/program-preview-v1.json').read_text())['cases'][0]
    payload = {'program': {'engine': 'p4-integer-program-v1', 'schema': 37, 'cells': c['cells'],
                           'fuel': 512, 'code': c['code']}, 'events': c['events']}
    cli = [os.environ.get('PET_FRAMEWORK_NODE', 'node'), str(ROOT / 'pc/skills/petui/scripts/program-preview.mjs')]
    out = subprocess.run(cli, input=json.dumps(payload), text=True, capture_output=True, check=True, timeout=10)
    result = json.loads(out.stdout)
    assert result['snapshot']['values'] == c['values'] and not result['deviceExecutionEnabled'] and not result['hardwareVerified']
    payload['program']['code'] = [['JUMP',0,0,0,0]]
    payload['program']['fuel'] = 4
    result = json.loads(subprocess.run(cli, input=json.dumps(payload), text=True, capture_output=True, check=True, timeout=10).stdout)
    assert result['stopped'] and len(result['reports']) == 1 and result['reports'][0]['result'] == 'fuel_exhausted'
    payload['program'] = {'engine': 'p4-integer-program-v1', 'schema': 9, 'cells': 1, 'fuel': 32,
                          'code': [['EVENT',0,0,0,0],['OUTPUT',0,0,0,0],['HALT',0,0,0,0]]}
    payload['events'] = [[11], [5]]
    payload['outputLimits'] = [{'min':-10, 'max':10}] + [{'min':0, 'max':0}] * 7
    result = json.loads(subprocess.run(cli, input=json.dumps(payload), text=True, capture_output=True, check=True, timeout=10).stdout)
    assert result['stopped'] and len(result['reports']) == 1 and result['reports'][0]['result'] == 'bounds'
    assert result['snapshot']['revision'] == 0 and result['snapshot']['outputs'] == [0] * 8
    for invalid in ('not json', 'x' * 65537, json.dumps({**payload,'events':[[]]*33}), json.dumps({**payload,'file':'private-path'}),
                    json.dumps({**payload,'outputLimits':[{'min':1,'max':0}]*8})):
        bad = subprocess.run(cli, input=invalid, text=True, capture_output=True, timeout=10)
        assert bad.returncode == 1 and json.loads(bad.stdout) == {'error':'program_preview_input_or_execution_invalid','deviceExecutionEnabled':False}
