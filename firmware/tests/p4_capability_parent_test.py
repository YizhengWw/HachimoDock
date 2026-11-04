"""R150 shared capability parent: the four parallel capability tracks enable their default-off package flags ONLY in
`esp32_p4_capability_parent`, which rides the verified main-frame chain and carries the public candidate. Env chain
pins, default-off declarations with #error dependency gates, a consistent resolved flag set, and a byte-identical
sdkconfig. Offline only: no device, no pio run."""
from pathlib import Path
import configparser
import re
import subprocess

ROOT=Path(__file__).resolve().parents[1]
ENV='env:esp32_p4_capability_parent'
PARENT='env:esp32_p4_main_frame_parent'
PUBLIC='env:esp32_p4_program_public_candidate'
FLAG_HEADERS=('pet_p4_render_policy.h','pet_p4_form_v2.h','pet_p4_form_v3.h','pet_p4_component_features.h','pet_p4_instrument_tone_config.h','pet_p4_font_metrics_columns_config.h')
INDEPENDENT_TRACK_FLAGS={'PET_P4_DATA_ROWS','PET_P4_LONGTEXT','PET_P4_NOTIFY','PET_P4_SEQUENCE','PET_P4_SAMPLES','PET_P4_BIG_IMAGE','PET_P4_EVENTS'}
RTOS_FORBIDDEN=('TASK_PRIORITY','F_CPU','BUDGET_US','WATCHDOG','AFFINITY','CONFIG_')


def _config():
    config=configparser.ConfigParser(interpolation=None);config.read(ROOT/'platformio.ini');return config


def _lines(section):
    return [l.strip() for l in section.get('build_flags','').split('\n') if l.strip()]


def _resolved_defines(config,name):
    """Walk extends from the root down and apply -D/-U in order, like the compiler sees them."""
    chain=[];cursor=name
    while cursor:
        chain.append(cursor);cursor=config[cursor].get('extends','')
    defines={}
    for env in reversed(chain):
        for line in _lines(config[env]):
            if line.startswith('${'):continue
            if line.startswith('-D'):
                key,_,value=line[2:].partition('=');defines[key]=value or '1'
            elif line.startswith('-U'):defines.pop(line[2:],None)
    return defines


def _flags(config):
    return [l[2:-2] for l in _lines(config[ENV]) if re.fullmatch(r'-DPET_[A-Z0-9_]+=1',l)]


def _headers():
    return {name:(ROOT/'main'/name).read_text() for name in FLAG_HEADERS}


def _syntax(defines,source):
    return subprocess.run(['cc','-std=c11','-Wall','-Wextra','-Werror','-fsyntax-only','-I',str(ROOT/'main'),*[f'-D{k}={v}' for k,v in defines.items()],str(source)],capture_output=True,text=True)


def test_env_chain_capability_parent_rides_main_frame_and_carries_the_public_candidate():
    config=_config();env=config[ENV]
    assert env['extends']==PARENT and env['custom_p4_project_version']=='0.7.103-p4-r150-capability'
    lines=_lines(env)
    assert lines[0]=='${env:esp32_p4_main_frame_parent.build_flags}'
    # Only package-capability switches, each exactly -DPET_P4_<FLAG>=1; no -U, no RTOS/clock/priority knobs.
    assert lines[1:] and all(re.fullmatch(r'-DPET_[A-Z0-9_]+=1',l) for l in lines[1:]),lines
    assert len(set(lines[1:]))==len(lines[1:])
    assert not any(v in env['build_flags'] for v in RTOS_FORBIDDEN)
    assert set(env.keys())-set(config['DEFAULT'].keys())=={'extends','custom_p4_project_version','build_flags'}
    versions=[config[s]['custom_p4_project_version'] for s in config.sections() if 'custom_p4_project_version' in config[s]]
    assert versions.count('0.7.103-p4-r150-capability')==1
    candidate=config[PUBLIC]
    assert candidate['extends']==ENV and _lines(candidate)[0]=='${env:esp32_p4_capability_parent.build_flags}'
    # Nobody else rides the capability parent or enables the switches it introduces
    # (a switch the verified chain already carried, e.g. a reviewed form engine, is not new here).
    inherited=_resolved_defines(config,PARENT)
    for name in config.sections():
        if name in (ENV,PUBLIC):continue
        text=config[name].get('extends','')+config[name].get('build_flags','')
        assert 'esp32_p4_capability_parent' not in text or config[name].get('extends','')==PUBLIC,name
        for flag in _flags(config):
            if flag in inherited or config[name].get('extends','')==PUBLIC:continue
            assert f'{flag}=1' not in text,(name,flag)
    assert (ROOT/'sdkconfig.esp32_p4_capability_parent').read_bytes()==(ROOT/'sdkconfig.esp32_p4_main_frame_parent').read_bytes()


def test_every_track_flag_is_declared_default_off_and_gated_by_an_error_dependency(tmp_path):
    config=_config();headers=_headers();declared='\n'.join(headers.values());flags=_flags(config)
    # Dependency gates may live next to the feature they guard (any main header); declarations stay in the flag headers.
    joined='\n'.join(h.read_text() for h in sorted((ROOT/'main').glob('*.h')))
    assert flags
    src=tmp_path/'guard.c'
    src.write_text(''.join(f'#include "{h}"\n' for h in FLAG_HEADERS)+'#include "pet_p4_catalog_profile.h"\nint main(void){return 0;}\n')
    gated=[]
    for flag in flags:
        assert re.search(r'#ifndef '+flag+r'\n#define '+flag+r' 0\n#endif',declared),flag
        # Either the flag's own dependency gate names it first (a lone -D fails with #error),
        # or the flag is the parent another track flag's gate depends on (a chain root).
        if re.search(r'#if\s+'+flag+r'\s*&&[^\n]*\n#error',joined):gated.append(flag)
        elif flag not in INDEPENDENT_TRACK_FLAGS:assert re.search(r'#if\s+PET_[A-Z0-9_]+\s*&&[^\n]*!\(?'+flag+r'\b[^\n]*\n#error',joined),flag
    # These three data-only features have no parent feature dependency. Prove
    # standalone compilation succeeds, instead of inventing a fake #error gate.
    for flag in INDEPENDENT_TRACK_FLAGS & set(flags):
        alone=_syntax({flag:'1'},src)
        assert alone.returncode==0,alone.stderr
    assert gated
    # Ordinary builds compile the headers with everything off.
    assert _syntax({},src).returncode==0
    # Standalone enabling of a gated track flag is refused by its dependency gate.
    for flag in gated:
        context={flag:'1'}
        if flag=='PET_P4_CELLS_V1':context['PET_COMPONENT_PROGRAM_ENABLED']='1'
        alone=_syntax(context,src)
        assert alone.returncode!=0 and 'error' in alone.stderr,flag
    # The resolved capability parent (whole chain, -U applied) is a consistent combination.
    resolved=_resolved_defines(config,ENV)
    for flag in flags:assert resolved.get(flag)=='1',flag
    assert _syntax(resolved,src).returncode==0,_syntax(resolved,src).stderr
    # The public candidate still resolves (it only undefines unadmitted private switches).
    public=_resolved_defines(config,PUBLIC)
    for flag in flags:assert public.get(flag)=='1',flag
    assert public.get('PET_P4_PUBLIC_PROGRAM_PROFILE')=='1'
    assert _syntax(public,src).returncode==0,_syntax(public,src).stderr
