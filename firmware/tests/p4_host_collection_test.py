"""Real native parser/standard surface resolver plus strict authoring contract."""
import copy
import importlib.util
import json
from pathlib import Path
import subprocess
import sys

import pytest

ROOT=Path(__file__).resolve().parents[1]
PUBLIC=ROOT.parent
SKILL=PUBLIC/'pc/skills/petui'
SUITE=json.loads((SKILL/'contracts/host-collection-v1.json').read_text())

def changed(value,patch):
    out=copy.deepcopy(value);parent=out;parts=patch['path'].strip('/').split('/')
    for part in parts[:-1]:parent=parent[int(part)] if isinstance(parent,list) else parent[part]
    key=int(parts[-1]) if isinstance(parent,list) else parts[-1];parent[key]=patch['value'];return out

@pytest.fixture(scope='module')
def native(tmp_path_factory):
    directory=tmp_path_factory.mktemp('host-collection');vendor=ROOT/'tests/vendor/cjson';binaries={}
    for enabled in (0,1):
        binary=directory/f'collection-{enabled}'
        subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation','-fsanitize=address,undefined',
            f'-DPET_HOST_COLLECTION_ENABLED={enabled}','-I',str(ROOT/'main'),'-I',str(vendor),
            str(ROOT/'tests/p4_host_collection_test.c'),str(ROOT/'main/pet_p4_host_collection.c'),
            str(ROOT/'main/pet_p4_collection.c'),str(ROOT/'main/pet_p4_surface.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
        binaries[enabled]=binary
    def run(mode,value,enabled=1):
        result=subprocess.run([str(binaries[enabled]),mode],input=json.dumps(value,ensure_ascii=False),text=True,capture_output=True,check=True)
        assert not result.stderr,result.stderr
        return json.loads(result.stdout)
    return run

@pytest.fixture(scope='module')
def authoring():
    spec=importlib.util.spec_from_file_location('host_collection',SKILL/'scripts/host_collection.py')
    module=importlib.util.module_from_spec(spec);spec.loader.exec_module(module);return module

@pytest.mark.parametrize('kind',['config','snapshot'])
def test_shared_native_and_authoring_valid_and_refused_fields(native,authoring,kind):
    value=SUITE[kind];getattr(authoring,kind)(copy.deepcopy(value));assert native(kind,value)['ok']
    for patch in SUITE[kind+'Reject']:
        bad=changed(value,patch)
        with pytest.raises(ValueError):getattr(authoring,kind)(bad)
        assert not native(kind,bad)['ok'],patch

def test_integral_json_numbers_and_unicode_have_one_semantics(native,authoring):
    config=copy.deepcopy(SUITE['config']);config['visible']=2.0;config['x']=20.0;config['colors'][0]=65535.0
    assert authoring.config(config)['visible']==2;assert native('config',config)['ok']
    model=copy.deepcopy(SUITE['snapshot']);model['count']=3.0;model['rows'][0]['index']=0.0;model['rows'][0]['label']='🙂'*15
    assert native('snapshot',model)['ok'];authoring.snapshot(model)
    for count in (63,64):
        candidate=copy.deepcopy(SUITE['snapshot']);candidate['rows'][0]['key']='x'*count
        assert native('snapshot',candidate)['ok']==(count==63)
    for text in ('x'*64,'\u0085','\x7f'):
        candidate=copy.deepcopy(SUITE['snapshot']);candidate['rows'][0]['label']=text
        assert not native('snapshot',candidate)['ok']

def surface():
    return {'engine':'p4-app-surface-v1','graphics':'p4-app-graphics-v1','background':0,'elements':[
        {'type':'panel','x':0,'y':0,'w':640,'h':480,'color':0},
        {'type':'text','x':20,'y':20,'w':300,'h':24,'color':65535,'text':'Collection'}],
        'collection_view':copy.deepcopy(SUITE['config'])}

def test_real_surface_expands_resolves_and_keeps_old_firmware_closed(native,authoring):
    s=surface();authoring.config(s['collection_view'],s)
    assert not native('surface',{'surface':s,'snapshot':SUITE['snapshot']},0)['ok']
    result=native('surface',{'surface':s,'snapshot':SUITE['snapshot']})
    assert result['ok'] and len(result['nodes'])==8
    a,b=result['nodes'][2:5],result['nodes'][5:8]
    assert a[0]['action']=='host.pick_a' and b[0]['action']=='host.pick_b'
    assert a[1]['text']=='第一项' and b[1]['text']=='第二项' and b[0]['color']==65504
    assert a[0]['x']==20 and a[0]['y']==100 and b[0]['y']==164
    assert all(not n['hidden'] for n in a+b)
    other=copy.deepcopy(SUITE['snapshot']);other['source']='other'
    unavailable=native('surface',{'surface':s,'snapshot':other})['nodes'][2:]
    assert [n['text'] for n in unavailable if not n['hidden']]==['等待电脑数据']
    assert all(not n['action'] for n in unavailable)

def test_empty_and_trailing_rows_never_invent_targets(native):
    model=copy.deepcopy(SUITE['snapshot']);model.update(count=0,offset=0,selectionKey='hidden')
    empty={'present':False,'key':'','label':'','detail':'','index':-1,'selected':False,'truncated':False}
    model['rows']=[copy.deepcopy(empty),copy.deepcopy(empty)]
    assert native('snapshot',model)['ok']
    result=native('surface',{'surface':surface(),'snapshot':model})
    assert [n['text'] for n in result['nodes'][2:] if not n['hidden']]==['暂无条目']
    assert all(not n['action'] for n in result['nodes'][2:])
    model['count']=1;model['rows'][0]=copy.deepcopy(SUITE['snapshot']['rows'][0])
    assert native('snapshot',model)['ok']
    nodes=native('surface',{'surface':surface(),'snapshot':model})['nodes']
    assert nodes[2]['action']=='host.pick_a' and all(n['hidden'] for n in nodes[5:])

def test_total_node_budget_and_overlapping_controls_are_refused(native,authoring):
    for count in (26,27):
        s=surface();s['elements']=[copy.deepcopy(s['elements'][0]) for _ in range(count)]
        assert native('surface',{'surface':s})['ok']==(count==26)
        if count==27:
            with pytest.raises(ValueError):authoring.config(s['collection_view'],s)
    s=surface();s['elements'].append({'type':'button','x':30,'y':110,'w':80,'h':48,'color':0,'text':'conflict','action':'host.other'})
    assert not native('surface',{'surface':s})['ok']
    with pytest.raises(ValueError):authoring.config(s['collection_view'],s)

def test_real_host_publication_lifetime_observation_and_guarded_enqueue(tmp_path):
    (tmp_path/'freertos').mkdir()
    (tmp_path/'freertos/FreeRTOS.h').write_text('typedef int portMUX_TYPE;\n#define portMUX_INITIALIZER_UNLOCKED 0\n#define portENTER_CRITICAL(x) ((void)(x))\n#define portEXIT_CRITICAL(x) ((void)(x))\n')
    (tmp_path/'esp_random.h').write_text('#include <stdint.h>\nstatic uint32_t esp_random(void){return 1234;}\n')
    (tmp_path/'esp_attr.h').write_text('#define EXT_RAM_BSS_ATTR\n')
    header='''#include <stdint.h>
#include <stdbool.h>
#include "cJSON.h"
#define PET_P4_DEVICE_ID_MAX 64
typedef void (*pet_p4_send_line_fn)(const char*,void*);
typedef struct {char widget_id[48],eyebrow[96],headline[160],metric_value[64],metric_label[96],metric_unit[32],badge[16],note[160],footer[160];uint32_t revision;} pet_p4_miniapp_view_t;
bool pet_app_host_enabled(const char*,const char*,uint64_t);
'''
    code=(ROOT/'main/pet_p4_app_host.c').read_text().replace('#include "pet_p4_app_host.h"',header)
    code+='\nstatic const char *fixture_config='+json.dumps(json.dumps(SUITE['config'],ensure_ascii=True))+';\n'
    code+='static const char *fixture_snapshot='+json.dumps(json.dumps(SUITE['snapshot'],ensure_ascii=True))+';\n'
    code+=r'''
#include <assert.h>
static unsigned sent;
static void send(const char *line,void *ctx){(void)line;(void)ctx;sent++;}
static cJSON *packet(void){
  cJSON *p=cJSON_CreateObject();cJSON_AddStringToObject(p,"componentId","component");cJSON_AddStringToObject(p,"instance",instance);
  cJSON_AddNumberToObject(p,"sequence",sequence);cJSON_AddObjectToObject(p,"values");
  cJSON *ui=cJSON_AddObjectToObject(p,"ui");cJSON_AddNumberToObject(ui,"mode",0);cJSON_AddStringToObject(ui,"context","abc");
  cJSON *actions=cJSON_AddArrayToObject(ui,"actions");cJSON_AddItemToArray(actions,cJSON_CreateString("host.pick_a"));cJSON_AddItemToArray(actions,cJSON_CreateString("host.pick_b"));
  cJSON_AddItemToObject(p,"collectionView",cJSON_Parse(fixture_snapshot));return p;
}
static bool next(cJSON *p,uint64_t now){cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p,"sequence"),++sequence);return pet_app_host_apply(p,now);}
int main(void){
  pet_app_host_transport(send,NULL,"board");pet_app_host_tick("component",1,0,true,0);assert(sent==1);
  cJSON *definition=cJSON_Parse(fixture_config);pet_host_collection_config_t config;
  assert(pet_host_collection_parse_config(definition,&config));cJSON_Delete(definition);
  cJSON *p=packet();assert(pet_app_host_apply(p,1)&&!collection_preparing);
  pet_host_render_t render;pet_app_host_render_capture("component",&render,NULL,2);
  assert(render.current&&render.fresh&&render.collection.valid&&render.collection.count==3);
  assert(!strcmp(render.collection.rows[0].label,"第一项"));
  pet_host_observation_t old;assert(pet_app_host_collection_observe("component",&config,0,2,&old));
  uint64_t epoch=render.collection_epoch;assert(next(p,3));assert(pet_app_host_variable_epoch("component")==epoch);
  cJSON *model=cJSON_GetObjectItemCaseSensitive(p,"collectionView"),*rows=cJSON_GetObjectItemCaseSensitive(model,"rows");
  cJSON *a=cJSON_GetArrayItem(rows,0);
  cJSON_ReplaceItemInObjectCaseSensitive(a,"label",cJSON_CreateString("replacement"));assert(next(p,4));
  cJSON_ReplaceItemInObjectCaseSensitive(a,"label",cJSON_CreateString("第一项"));assert(next(p,5));
  assert(pet_app_host_variable_epoch("component")==epoch+2);
  unsigned before=sent;assert(pet_app_host_observed_action("component",1,0,"host.pick_a",&old,200));assert(sent==before);
  pet_host_observation_t current;assert(pet_app_host_collection_observe("component",&config,0,201,&current));
  assert(pet_app_host_observed_action("component",1,0,"host.pick_a",&current,202));assert(sent==before+1);
  assert(!pet_app_host_collection_observe("component",&config,0,203,&current)); // pending cannot retarget
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(p,"sequence"),sequence);assert(pet_app_host_apply(p,204));
  epoch=pet_app_host_variable_epoch("component");
  cJSON_ReplaceItemInObjectCaseSensitive(model,"source",cJSON_CreateString("wrong"));assert(next(p,205));
  assert(!pet_app_host_collection_observe("component",&config,0,206,&current));
  cJSON_ReplaceItemInObjectCaseSensitive(model,"source",cJSON_CreateString("items"));assert(next(p,207));
  // Bad publication leaves the prior complete reply untouched, then releases
  // its private parse scratch on every early-return path.
  epoch=pet_app_host_variable_epoch("component");cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(model,"count"),129);
  assert(!next(p,208)&&!collection_preparing);assert(pet_app_host_variable_epoch("component")==epoch);
  pet_app_host_render_capture("component",&render,NULL,209);assert(render.collection.count==3);
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(model,"count"),3);assert(next(p,210));
  assert(pet_app_host_collection_observe("component",&config,0,211,&old));
  cJSON_DeleteItemFromObjectCaseSensitive(p,"collectionView");assert(next(p,212));
  pet_app_host_render_capture("component",&render,NULL,213);assert(!render.collection.valid);
  assert(!pet_app_host_collection_observe("component",&config,0,213,&current));
  cJSON_AddItemToObject(p,"collectionView",cJSON_Parse(fixture_snapshot));assert(next(p,214));
  assert(!pet_app_host_collection_observe("component",&config,0,10214,&current));
  pet_app_host_render_capture("component",&render,NULL,10214);assert(!render.fresh&&!render.collection.valid);
  pet_app_host_tick("",0,0,false,215);pet_app_host_tick("component",1,0,true,216);
  assert(!pet_app_host_apply(p,217));before=sent;
  assert(pet_app_host_observed_action("component",1,0,"host.pick_a",&old,400));assert(sent==before);
  cJSON_Delete(p);p=packet();assert(pet_app_host_apply(p,218));
  assert(pet_app_host_collection_observe("component",&config,0,219,&current));
  assert(!pet_app_host_collection_observe("component",&config,2,219,&current)); // out-of-range never targets memory
  model=cJSON_GetObjectItemCaseSensitive(p,"collectionView");rows=cJSON_GetObjectItemCaseSensitive(model,"rows");
  cJSON_SetNumberValue(cJSON_GetObjectItemCaseSensitive(model,"count"),1);
  cJSON *empty=cJSON_Parse("{\"present\":false,\"key\":\"\",\"label\":\"\",\"detail\":\"\",\"index\":-1,\"selected\":false,\"truncated\":false}");
  cJSON_ReplaceItemInArray(rows,1,empty);assert(next(p,220));
  // Even an inconsistent host actions array cannot expose an empty row.
  assert(!pet_app_host_collection_observe("component",&config,1,221,&current));
  pet_app_host_transport(send,(void*)1,"board");assert(!pet_app_host_apply(p,222));
  assert(!pet_app_host_collection_observe("component",&config,0,223,&current));
  cJSON_Delete(p);pet_resource_usage_t usage;pet_resources_device_usage(&usage);assert(usage.live_owners==0);
  return 0;
}
'''
    source=tmp_path/'host.c';source.write_text(code);binary=tmp_path/'host';vendor=ROOT/'tests/vendor/cjson'
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-Wno-error=deprecated-declarations','-Wno-misleading-indentation',
        '-fsanitize=address,undefined','-DPET_HOST_COLLECTION_ENABLED=1','-I',str(tmp_path),'-I',str(vendor),'-I',str(ROOT/'main'),str(source),
        str(ROOT/'main/pet_p4_host_collection.c'),str(ROOT/'main/pet_p4_collection.c'),str(ROOT/'main/pet_p4_resources.c'),
        str(ROOT/'main/pet_p4_resources_device.c'),str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
    result=subprocess.run([str(binary)],capture_output=True,text=True)
    assert result.returncode==0 and not result.stderr,result.stderr

def test_full_authoring_runtime_composition_and_capability_path():
    sys.path.insert(0,str(SKILL/'scripts'))
    from validate_generated_widget import validate_runtime
    from app_services import application_errors
    runtime=copy.deepcopy(SUITE['runtimeBase']);runtime['surface']['collection_view']=copy.deepcopy(SUITE['config'])
    runtime['dashboard'].update(visualStyle='clean',visualPalette='mint',visualLayout='tool',visualSprite='star')
    declaration=copy.deepcopy(SUITE['declarationBase']);declaration['content'][0]['sources']['records']['initial']=copy.deepcopy(SUITE['records'])
    assert application_errors(declaration,runtime)==[]
    # The normal capability baseline must NOT silently opt into a candidate.
    caps={'widgetHostCollection':'p4-host-collection-v1','widgetRuntime':'p4-bounded-runtime-v4','widgetInput':'p4-input-v1',
          'widgetAppHost':'p4-app-host-v1','widgetAppSurface':'p4-app-surface-v1',
          'widgetAppGraphics':'p4-app-graphics-v1','widgetVariableBindings':'p4-variable-bindings-v1','widgetHostContext':'p4-host-context-v1'}
    # Full validation also checks physical bindings separately at package level;
    # this in-memory runtime test covers exactly the runtime/capability gate.
    for cap in ({},caps):
        errors=[];validate_runtime(runtime,'tool',Path('/nonexistent-test-assets'),cap,errors)
        if not cap:assert any('虚拟列表' in e for e in errors)
        else:assert not errors,errors
    for pointer,value in [('page_size',3),('page_size',True)]:
        bad=copy.deepcopy(declaration);bad['content'][0]['sources']['items']['view'][pointer]=value
        assert application_errors(bad,runtime)

def test_package_buttons_admit_only_real_validated_collection_row_controls():
    sys.path.insert(0,str(SKILL/'scripts'))
    from validate_generated_widget import validate_buttons
    runtime=copy.deepcopy(SUITE['runtimeBase'])
    runtime['surface']['collection_view']=copy.deepcopy(SUITE['config'])
    caps={'widgetHostCollection':'p4-host-collection-v1'}
    def check(value,capabilities=caps):
        errors=[]
        validate_buttons(copy.deepcopy(SUITE['buttons']),{t['on'] for t in value['transitions']},capabilities,value,errors)
        return errors
    assert check(runtime)==[]
    # A declared row expands into a real standard button; an unrelated host
    # command does not gain an exemption merely by sharing its prefix.
    extra=copy.deepcopy(runtime);extra['transitions'].append({'from':'*','on':'host.unreachable'})
    assert any('host.unreachable' in error for error in check(extra))
    assert any('host.pick_a' in error for error in check(runtime,{}))
    for patch in ({'visible':3},{'actions':['host.pick_a','host.pick_a']},{'source':'bad\nsource'}):
        bad=copy.deepcopy(runtime);bad['surface']['collection_view'].update(patch)
        assert any('host.pick_' in error for error in check(bad)),patch

def test_host_smoke_counts_only_validated_generated_collection_controls():
    sys.path.insert(0,str(SKILL/'scripts'))
    from bounded_app import smoke
    runtime=copy.deepcopy(SUITE['runtimeBase'])
    runtime['surface']['collection_view']=copy.deepcopy(SUITE['config'])
    result=smoke(runtime,copy.deepcopy(SUITE['buttons']))
    assert result['ok'] and set(SUITE['config']['actions'])<=set(result['actions'])
    assert result['hostServicesVerified'] is False and result['deviceVerified'] is False
    unknown=copy.deepcopy(runtime);unknown['transitions'].append({'from':'*','on':'host.unreachable'})
    assert not smoke(unknown,copy.deepcopy(SUITE['buttons']))['ok']
    for patch in ({'visible':3},{'actions':['host.pick_a','host.pick_a']},{'source':'bad\nsource'}):
        invalid=copy.deepcopy(runtime);invalid['surface']['collection_view'].update(patch)
        assert not smoke(invalid,copy.deepcopy(SUITE['buttons']))['ok'],patch
