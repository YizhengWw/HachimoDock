"""Fixed public reply pacing, independent from the private comparison profile."""
from pathlib import Path
import configparser
import subprocess

ROOT = Path(__file__).resolve().parents[1]


def test_public_control_pacing_has_fixed_burst_and_keeps_no_drain_path(tmp_path):
    config = configparser.ConfigParser(interpolation=None)
    config.read(ROOT / 'platformio.ini')
    enabled = [name for name in config.sections()
               if '-DPET_P4_UART_CONTROL_PACING=1' in config[name].get('build_flags', '')]
    assert enabled == ['env:esp32_p4_program_public_candidate']
    assert config['env:esp32_p4_evboard']['custom_p4_uart_baud'] == '4000000'
    source = (ROOT / 'main/pet_p4_main.c').read_text()
    queued = source.split('static void uart_queue_all(', 1)[1].split('static void uart_write_all(', 1)[0]
    assert 'vTaskDelay(' not in queued and 'uart_wait_tx_done(' not in queued
    no_drain = source.split('if (route == PET_P4_TRANSPORT_BROADCAST || route == PET_P4_TRANSPORT_UART)', 1)[1]
    assert 'uart_queue_all(line,strlen(line),NULL);' in ''.join(no_drain.split())
    assert 'uart_queue_all("\\n",1,NULL);' in ''.join(no_drain.split())
    program = tmp_path / 'policy.c'
    program.write_text('#include "pet_p4_uart_pacing.h"\n'
                       '_Static_assert(PET_P4_UART_PACING_ACTIVE == EXPECTED, "admission");\n'
                       '_Static_assert(PET_P4_UART_TX_BURST_BYTES == BURST, "fixed bound");\n')
    for production, expected, burst in ((0, 0, 64), (1, 1, 256)):
        subprocess.run(['cc', '-std=c11', '-fsyntax-only', '-I', str(ROOT / 'main'),
                        f'-DPET_P4_UART_CONTROL_PACING={production}',
                        f'-DEXPECTED={expected}', f'-DBURST={burst}', str(program)], check=True)
    rejected = subprocess.run(['cc', '-std=c11', '-fsyntax-only', '-I', str(ROOT / 'main'),
                               '-DPET_P4_UART_CONTROL_PACING=2', str(program)], capture_output=True)
    assert rejected.returncode != 0 and b'Invalid control UART pacing profile' in rejected.stderr
