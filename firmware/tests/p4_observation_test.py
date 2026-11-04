"""Real immutable-cache concurrency; no devices, clocks, providers or audio."""
import json
import os
import re
import subprocess

import pytest

from p4_program_test import ROOT, WARNINGS, INCLUDES, FREESTANDING, run, p4_compiler

CORE = ROOT / 'main/pet_p4_observation.c'
TEST = ROOT / 'tests/p4_observation_test.c'


@pytest.mark.parametrize('sanitizer', ['address,undefined', 'thread'])
def test_observation_pins_nonblocking_bounds_and_real_concurrency(tmp_path, sanitizer):
    if sanitizer == 'thread':
        # Do not turn unsupported host tooling into a race false-positive, or
        # skip a real failure of the cache. Probe ONLY an empty native program.
        probe, binary = tmp_path / 'tsan-probe.c', tmp_path / 'tsan-probe'
        probe.write_text('int main(void){return 0;}\n')
        result = subprocess.run([os.environ.get('CC', 'cc'), '-fsanitize=thread', str(probe), '-o', str(binary)],
                                capture_output=True, text=True, timeout=30)
        if result.returncode and any(s in result.stderr.lower() for s in (
                'unsupported option', 'not supported for this target', 'cannot find -ltsan')):
            pytest.skip('existing host compiler has no ThreadSanitizer runtime; no download')
        result.check_returncode()
        result = subprocess.run([str(binary)], capture_output=True, text=True, timeout=30)
        if result.returncode and any(s in result.stderr.lower() for s in (
                'unexpected memory mapping', 'failed to mmap', 'not supported on this platform')):
            pytest.skip('empty ThreadSanitizer probe cannot run in this host environment')
        result.check_returncode()
    executable = tmp_path / 'cache'
    run([os.environ.get('CC', 'cc'), *WARNINGS, '-O1', '-pthread', '-fsanitize='+sanitizer,
         '-fno-omit-frame-pointer', '-DPET_OBSERVATION_STRESS_ITERATIONS=20000',
         *INCLUDES, CORE, TEST, '-o', executable])
    execution = subprocess.run([str(executable)], check=True, text=True, capture_output=True, timeout=30)
    assert not execution.stderr.strip(), 'native sanitizer diagnostic must not be suppressed by exit status'
    result = json.loads(execution.stdout)
    assert result['coherent'] and result['writers'] == 2 and result['readers'] == 4
    assert result['attempts'] == 40000 and result['writes'] > 100 and result['reads'] > 100
    assert result['cacheBytes'] <= 224


def test_observation_p4_lockfree_object_no_hidden_libraries_or_dynamic_stack(tmp_path):
    compiler = p4_compiler()
    if not compiler:
        pytest.skip('requires existing compiler, never invokes PlatformIO or downloads')
    obj = tmp_path / 'cache.o'
    run([compiler, *WARNINGS, '-Os', '-march=rv32imafc_zicsr_zifencei_xesppie', '-mabi=ilp32f',
         *FREESTANDING, '-fstack-usage', *INCLUDES, '-c', CORE, '-o', obj])
    prefix = str(compiler).removesuffix('gcc')
    assert not run([prefix+'nm', '-u', obj])
    for line in obj.with_suffix('.su').read_text().splitlines():
        _, amount, kind = line.split('\t')
        assert kind == 'static' and int(amount) <= 160


def test_actual_input_writer_and_getter_publish_one_coherent_observation(tmp_path):
    source = (ROOT / 'main/pet_p4_input.c').read_text()
    header = (ROOT / 'main/pet_p4_input.h').read_text()

    def function(name):
        start = source.index(name+'(')
        start = source.rfind('\n', 0, start)+1
        opening = source.index('{', start)
        depth, end = 1, opening+1
        while depth:
            depth += (source[end] == '{') - (source[end] == '}')
            end += 1
        return source[start:end]

    declaration = re.search(r'typedef struct \{\s+bool ready;.*?\} pet_p4_input_joystick_snapshot_t;', header, re.S).group()
    state = source[source.index('static pet_p4_input_joystick_snapshot_t g_joystick_observation_model;'):source.index('static void joystick_observation_begin(')]
    writer = function('record_joystick_sample')
    needle = '(void)pet_observation_publish('
    assert writer.count(needle) == 1
    # The real writer is paused after updating private fields but BEFORE
    # publication, precisely where the previous atomic-field getter tore.
    writer = writer.replace(needle, 'pause_writer();\n  '+needle)
    code = r'''
#include "pet_p4_observation.h"
#include <stdbool.h>
#include <stdio.h>
#include <assert.h>
#include <limits.h>
#include <pthread.h>
static pthread_mutex_t mutex=PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t changed=PTHREAD_COND_INITIALIZER;
static bool paused,released;
static void pause_writer(void){pthread_mutex_lock(&mutex);paused=true;pthread_cond_signal(&changed);while(!released)pthread_cond_wait(&changed,&mutex);pthread_mutex_unlock(&mutex);}
'''+declaration+'\n'+state+function('joystick_observation_begin')+'\n'+writer+'\n'+function('pet_p4_input_get_joystick_snapshot')+r'''
static void *update(void *context){(void)context;record_joystick_sample(1000,1100);return NULL;}
int main(void){
  pet_p4_input_joystick_snapshot_t s;
  pet_p4_input_get_joystick_snapshot(&s);assert(!s.ready&&!s.current_x&&!s.minimum_x);
  joystick_observation_begin(true,2000,2000);
  pthread_t writer;assert(!pthread_create(&writer,NULL,update,NULL));
  pthread_mutex_lock(&mutex);while(!paused)pthread_cond_wait(&changed,&mutex);pthread_mutex_unlock(&mutex);
  pet_p4_input_get_joystick_snapshot(&s);
  assert(s.ready&&s.current_x==2000&&s.minimum_x==2000&&s.current_y==2000&&s.minimum_y==2000);
  pthread_mutex_lock(&mutex);released=true;pthread_cond_signal(&changed);pthread_mutex_unlock(&mutex);assert(!pthread_join(writer,NULL));
  pet_p4_input_get_joystick_snapshot(&s);
  assert(s.ready&&s.current_x==1000&&s.minimum_x==1000&&s.current_y==1100&&s.minimum_y==1100&&s.maximum_x==2000&&s.center_x==2000);
  unsigned current=atomic_load(&g_joystick_observation.published);
  atomic_store(&g_joystick_observation.slots[current].readers,UINT_MAX);
  pet_p4_input_get_joystick_snapshot(&s);assert(!s.ready&&!s.current_x&&!s.center_x);
  atomic_store(&g_joystick_observation.slots[current].readers,0);
  joystick_observation_begin(false,2048,2048);pet_p4_input_get_joystick_snapshot(&s);
  assert(!s.ready&&s.center_x==2048&&s.current_x==2048&&s.minimum_x==2048);
  puts("coherent input observation passed");return 0;
}
'''
    c, executable = tmp_path / 'input-observation.c', tmp_path / 'input-observation'
    c.write_text(code)
    subprocess.run(['cc', *WARNINGS, '-O1', '-pthread', '-fsanitize=address,undefined',
                    *INCLUDES, str(CORE), str(c), '-o', str(executable)], check=True)
    assert run([executable]) == 'coherent input observation passed'
