"""R137/R140 public program candidate: explicit profile quota table; the candidate rides the verified chain and
undefines only the switches the admission matrix does not admit; guards distinguish 'ordinary builds stay zero'
from 'the public candidate is open'. Offline only; no lease, device or sound."""
from pathlib import Path
import re
import configparser
import subprocess

ROOT=Path(__file__).resolve().parents[1]
PUBLIC='env:esp32_p4_program_public_candidate'
PUBLIC_V3=PUBLIC+'_v3'
UNDEFINED=('-UPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE','-UPET_VARIABLE_OWNERS_ENABLED','-UPET_COMPONENT_PROGRAM_RAM_RESUME','-UPET_P4_UART_PACED_REFERENCE','-UPET_P4_UART_PACED_BYTES')


def _limits(tmp_path,flags):
    code=r'''
#include <stdio.h>
#include <assert.h>
#include "pet_p4_resources.h"
#include "pet_p4_program_core.h"
int main(void){
 pet_resource_limit_t p=pet_resources_limit(PET_RESOURCE_PROGRAM),q=pet_resources_limit(PET_RESOURCE_PERIPHERAL);
#if PET_P4_PUBLIC_PROGRAM_PROFILE
 assert(PET_PROGRAM_FUEL==4096&&p.leases==1&&p.units==4096);
 pet_resource_broker_t broker={0};pet_resource_owner_t first={0},second={0};pet_resource_lease_t lease={0},refused={0};
 assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM),&first)==PET_RESOURCE_OK);
 assert(pet_resources_owner_open(&broker,PET_RESOURCE_MASK(PET_RESOURCE_PROGRAM),&second)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,first,PET_RESOURCE_PROGRAM,4097,&refused)!=PET_RESOURCE_OK&&!refused.serial);
 assert(pet_resources_try_acquire(&broker,first,PET_RESOURCE_PROGRAM,4096,&lease)==PET_RESOURCE_OK);
 assert(pet_resources_try_acquire(&broker,second,PET_RESOURCE_PROGRAM,1,&refused)!=PET_RESOURCE_OK&&!refused.serial);
 pet_resource_usage_t usage={0};pet_resources_usage(&broker,&usage);assert(usage.leases[PET_RESOURCE_PROGRAM]==1&&usage.units[PET_RESOURCE_PROGRAM]==4096);
#endif
 printf("%u %u %u %u\n",(unsigned)p.leases,(unsigned)p.units,(unsigned)q.leases,(unsigned)q.units);return 0;
}
'''
    src=tmp_path/'limits.c';exe=src.with_suffix('');src.write_text(code)
    subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror',*flags,'-I',str(ROOT/'main'),str(src),str(ROOT/'main/pet_p4_resources.c'),'-o',str(exe)],check=True)
    return tuple(int(x) for x in subprocess.run([str(exe)],capture_output=True,text=True,check=True).stdout.split())


def test_profile_table_is_explicit_and_ordinary_builds_stay_zero(tmp_path):
    base=['-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1']
    assert _limits(tmp_path,[])==(0,0,0,0)                           # ordinary: {0,0}, PERIPHERAL {0,0}
    assert _limits(tmp_path,base)==(2,4096,0,0)                      # private unified parent: {2,4096}
    assert _limits(tmp_path,base+['-DPET_P4_PUBLIC_PROGRAM_PROFILE=1'])==(1,4096,0,0)  # public: ONE lease, PERIPHERAL still zero
    assert _limits(tmp_path,base+['-DPET_P4_PUBLIC_PROGRAM_PROFILE=1','-DPET_P4_PROGRAM_V2=1','-DPET_P4_TEXT_TABLE=1','-DPET_P4_APP_DIRTY_CACHE=1'])==(1,4096,0,0)
    # The -U list of the public env really undefines after the inherited -D (GCC processes in order).
    assert _limits(tmp_path,base+['-DPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1','-UPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE','-DPET_VARIABLE_OWNERS_ENABLED=1','-UPET_VARIABLE_OWNERS_ENABLED','-DPET_P4_PUBLIC_PROGRAM_PROFILE=1'])==(1,4096,0,0)
    limits=(ROOT/'main/pet_p4_resources.c').read_text()
    assert '[PET_RESOURCE_PROGRAM]={0,0}' in limits and '[PET_RESOURCE_PERIPHERAL]={0,0}' in limits
    assert limits.index('#if PET_P4_PUBLIC_PROGRAM_PROFILE')<limits.index('[PET_RESOURCE_PROGRAM]={1,4096},')<limits.index('#elif PET_COMPONENT_PROGRAM_ENABLED && PET_COMPONENT_PROGRAM_EXECUTION_REVIEWED')


def test_public_v2_descriptor_and_default_fuel_stay_4096(tmp_path):
    import copy,json
    from p4_program_v2_test import SUITE,SOURCES,V2_FLAGS
    vendor=ROOT/'tests/vendor/cjson'
    flags=['-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1',
           '-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1',*V2_FLAGS]
    # The same production descriptor parser rejects >4096 only in the public
    # profile; private v2 retains its wider budgets and existing hello/default
    # callers all consume this same PET_PROGRAM_FUEL constant.
    for public,ceiling in ((False,16384),(True,4096)):
        binary=tmp_path/f'parser-{public}'
        subprocess.run(['cc','-std=c11','-Wno-deprecated-declarations','-Wno-misleading-indentation',
                        *flags,*(['-DPET_P4_PUBLIC_PROGRAM_PROFILE=1'] if public else []),
                        '-I',str(ROOT/'main'),'-I',str(vendor),str(ROOT/'tests/p4_program_v2_test.c'),
                        *[str(ROOT/'main'/f'pet_p4_{n}.c') for n in SOURCES],str(vendor/'cJSON.c'),'-lm','-o',str(binary)],check=True)
        for fuel in (4096,4097,16384):
            runtime=copy.deepcopy(SUITE['runtime']);runtime['program']['image']['fuel']=fuel
            result=subprocess.run([str(binary),'parse'],input=json.dumps(runtime),text=True,capture_output=True,check=True)
            assert json.loads(result.stdout)['ok']==(fuel<=ceiling),(public,fuel,result.stdout)
    protocol=(ROOT/'main/pet_p4_protocol.c').read_text()
    assert '"widgetProgramMaxFuel",PET_PROGRAM_FUEL' in protocol
    assert '"widgetProgramAggregateFuel",pet_resources_limit(PET_RESOURCE_PROGRAM).units' in protocol


def test_public_profile_requires_reviewed_switches_and_excludes_private_grants(tmp_path):
    src=tmp_path/'guard.c';src.write_text('#include "pet_p4_component_features.h"\nint main(void){return PET_P4_PUBLIC_PROGRAM_PROFILE;}\n')
    def run(flags):return subprocess.run(['cc','-std=c11','-fsyntax-only',*flags,'-I',str(ROOT/'main'),str(src)],capture_output=True,text=True)
    assert run([]).returncode==0
    r=run(['-DPET_P4_PUBLIC_PROGRAM_PROFILE=1']);assert r.returncode!=0 and 'requires the reviewed program' in r.stderr
    full=['-DPET_P4_PUBLIC_PROGRAM_PROFILE=1','-DPET_COMPONENT_PROGRAM_ENABLED=1','-DPET_COMPONENT_PROGRAM_EXECUTION_REVIEWED=1','-DPET_COMPONENT_BACKGROUND_ENABLED=1','-DPET_COMPONENT_PROGRAM_PARENT_UNIFIED=1']
    assert run(full).returncode==0
    for extra in ('-DPET_VARIABLE_OWNERS_ENABLED=1','-DPET_PRIVATE_PARENT_CAPACITY_ACCEPTANCE=1','-DPET_COMPONENT_PROGRAM_RAM_RESUME=1'):
        r=run(full+[extra]);assert r.returncode!=0 and 'excludes private capacity acceptance' in r.stderr,extra
        assert run(full+[extra,extra.replace('-D','-U').split('=')[0]]).returncode==0  # the public env's -U form clears it


def test_public_candidate_rides_the_verified_chain_and_undefines_only_the_unadmitted_switches():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini')
    env=config[PUBLIC];flags=env['build_flags']
    release=re.search(r'set\(PROJECT_VER "([^"]+)"\)',(ROOT/'CMakeLists.txt').read_text()).group(1)
    assert env['extends']=='env:esp32_p4_capability_parent' and env['custom_p4_project_version']==release
    lines=[l.strip() for l in flags.split('\n') if l.strip()]
    lines=[l for l in lines if not l.startswith(';')]
    assert lines[0]=='${env:esp32_p4_capability_parent.build_flags}'
    assert tuple(lines[1:6])==UNDEFINED
    # R159 adds bounded service presentation only in the reviewed candidate;
    # the exact allowlist cannot admit resources, priorities or clock changes.
    # R162 adds only the separately tested bounded UART control pacing switch.
    assert lines[6:]==['-DPET_P4_PUBLIC_PROGRAM_PROFILE=1','-DPET_P4_CONVERSATION_VIDEO=1','-DPET_P4_UART_CONTROL_PACING=1','-DPET_SERVICE_BRIDGE_ENABLED=1','-DPET_HOST_COLLECTION_ENABLED=1']
    assert not any(v in flags for v in ('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY'))
    for name in config.sections():
        if name not in (PUBLIC,PUBLIC_V3,'env:esp32_p4_service_bridge_candidate'):
            assert 'PET_P4_PUBLIC_PROGRAM_PROFILE=1' not in config[name].get('build_flags','') and PUBLIC not in config[name].get('extends','')
    service=config['env:esp32_p4_service_bridge_candidate']
    assert service['extends']==PUBLIC
    assert service['build_flags'].split()==['${env:esp32_p4_program_public_candidate.build_flags}','-DPET_SERVICE_BRIDGE_ENABLED=1','-DPET_HOST_COLLECTION_ENABLED=1']
    ordinary=config['env:esp32_p4_evboard'].get('build_flags','')
    assert '-DPET_SERVICE_BRIDGE_ENABLED=1' not in ordinary
    # The v3 chip-family twin only changes platform / chip family / clock (as esp32_p4_evboard_v3 does); same source, flags, label.
    v3=config[PUBLIC_V3];assert v3['extends']==PUBLIC and v3['custom_p4_chip_family']=='v3' and '55.03.38-1' in v3['platform'] and v3['board_build.f_cpu']=='400000000L'
    assert set(v3.keys())-set(config['DEFAULT'].keys())<={'extends','platform','custom_p4_chip_family','board_build.f_cpu'}
    assert (ROOT/'sdkconfig.esp32_p4_program_public_candidate_v3').read_bytes()==(ROOT/'sdkconfig.esp32_p4_evboard_v3').read_bytes()
    # The bundled desktop firmware must carry this label: CMake PROJECT_VER and the evboard label equal the candidate's.
    cmake=(ROOT/'CMakeLists.txt').read_text();assert 'set(PROJECT_VER "'+release+'")' in cmake
    assert config['env:esp32_p4_evboard']['custom_p4_project_version']==release
    assert config['platformio']['default_envs']=='esp32_p4_evboard'
    assert (ROOT/'sdkconfig.esp32_p4_program_public_candidate').read_bytes()==(ROOT/'sdkconfig.esp32_p4_large_reply_parent').read_bytes()


def test_build_evidence_targets_include_the_public_candidate():
    text=(ROOT.parent/'pc/scripts/build-framework-evidence.mjs').read_text()
    assert "program:'esp32_p4_program_public_candidate'" in text and "report.targets.length===Object.keys(TARGETS).length" in text
