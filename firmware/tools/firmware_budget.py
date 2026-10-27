"""Release guardrails, distinct from a runtime free-heap measurement."""
import csv
import json
from pathlib import Path
import subprocess

MIN_OTA_HEADROOM = 128 * 1024
MAX_INTERNAL_STATIC = 384 * 1024


def evaluate(binary_bytes, ota_bytes, sections):
    internal = sum(sections.get(name, 0) for name in
                   ('.iram0.text', '.dram0.data', '.dram0.bss'))
    result = dict(appBytes=binary_bytes, otaBytes=ota_bytes,
                  otaHeadroomBytes=ota_bytes - binary_bytes,
                  internalStaticBytes=internal,
                  externalStaticBytes=sections.get('.ext_ram.bss', 0),
                  minimumOtaHeadroomBytes=MIN_OTA_HEADROOM,
                  maximumInternalStaticBytes=MAX_INTERNAL_STATIC)
    if result['otaHeadroomBytes'] < MIN_OTA_HEADROOM:
        raise ValueError(f'OTA headroom below 128 KiB: {result}')
    if internal > MAX_INTERNAL_STATIC:
        raise ValueError(f'Internal static memory exceeds 384 KiB: {result}')
    return result


def check(binary, partitions, size_tool):
    binary = Path(binary)
    capacities = []
    for row in csv.reader(Path(partitions).read_text().splitlines()):
        if len(row) > 4 and row[1].strip() == 'app':
            capacities.append(int(row[4].strip(), 0))
    if not capacities:
        raise ValueError('No OTA app partition found')
    output = subprocess.check_output([str(size_tool), '-A', str(binary.with_suffix('.elf'))], text=True)
    sections = {}
    for line in output.splitlines():
        fields = line.split()
        if len(fields) >= 2 and fields[0].startswith('.') and fields[1].isdigit():
            sections[fields[0]] = int(fields[1])
    if not all(name in sections for name in ('.iram0.text', '.dram0.data', '.dram0.bss')):
        raise ValueError('Missing internal-memory sections in ELF size report')
    result = evaluate(binary.stat().st_size, min(capacities), sections)
    binary.with_name('firmware-budget.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Firmware budget:', json.dumps(result))
    return result
