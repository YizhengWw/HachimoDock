"""Release guardrails, distinct from a runtime free-heap measurement."""
import csv
import json
from pathlib import Path
import subprocess
import re

MIN_OTA_HEADROOM = 128 * 1024
MAX_INTERNAL_STATIC = 384 * 1024
MEMORY_ACCOUNTING = 'p4-allocated-ram-v2'
# ESP-IDF esp32p4/include/soc/soc.h: TCM, shared IRAM/DRAM, LP SRAM.
# Count every nonempty section in these ranges, not only a list of old names.
INTERNAL_REGIONS = ((0x30100000, 0x30102000), (0x4FF00000, 0x4FFC0000),
                    (0x50108000, 0x50110000))
REQUIRED = ('.iram0.text', '.dram0.data', '.dram0.bss')


def memory_sections(sections):
    if not isinstance(sections, dict) or not all(name in sections for name in REQUIRED):
        raise ValueError('Missing internal-memory sections in ELF size report')
    internal, external, spans = {}, 0, []
    for name, record in sections.items():
        if (not isinstance(name, str) or not re.fullmatch(r'\.[A-Za-z0-9_.-]{1,63}', name)
                or not isinstance(record, dict) or set(record) != {'size', 'address'}
                or any(type(record[k]) is not int for k in record)):
            raise ValueError('Invalid ELF section record')
        size, address = record['size'], record['address']
        if not 0 <= size <= 32*1024*1024 or not 0 <= address < 2**32 or address+size > 2**32:
            raise ValueError('Invalid ELF section bounds')
        fits = any(lo <= address < hi and address+size <= hi for lo, hi in INTERNAL_REGIONS)
        intersects = any(address < hi and lo < address+size for lo, hi in INTERNAL_REGIONS)
        if intersects and not fits or name in REQUIRED and not fits:
            raise ValueError('ELF section crosses or misses internal RAM region')
        if fits and size:
            internal[name] = size
            spans.append((address, address+size))
        if name.startswith('.ext_ram.') and name != '.ext_ram.dummy' and size:
            if not 0x48000000 <= address < 0x4C000000 or address+size > 0x4C000000:
                raise ValueError('External RAM section bounds invalid')
            external += size
    spans.sort()
    if any(right[0] < left[1] for left, right in zip(spans, spans[1:])):
        raise ValueError('Overlapping internal RAM sections')
    return dict(sorted(internal.items())), external


def evaluate(binary_bytes, ota_bytes, sections):
    if type(binary_bytes) is not int or type(ota_bytes) is not int or not 0 < binary_bytes <= 32*1024*1024 or not 0 < ota_bytes <= 32*1024*1024:
        raise ValueError('Invalid firmware or OTA size')
    accounted, external = memory_sections(sections)
    internal = sum(accounted.values())
    result = dict(appBytes=binary_bytes, otaBytes=ota_bytes,
                  otaHeadroomBytes=ota_bytes - binary_bytes,
                  internalStaticBytes=internal,
                  externalStaticBytes=external,
                  memoryAccounting=MEMORY_ACCOUNTING, internalSections=accounted,
                  minimumOtaHeadroomBytes=MIN_OTA_HEADROOM,
                  maximumInternalStaticBytes=MAX_INTERNAL_STATIC)
    if result['otaHeadroomBytes'] < MIN_OTA_HEADROOM:
        raise ValueError(f'OTA headroom below 128 KiB: {result}')
    if internal > MAX_INTERNAL_STATIC:
        raise ValueError(f'Internal static memory exceeds 384 KiB: {result}')
    return result


def check(binary, partitions, size_tool):
    binary = Path(binary).resolve()
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
        if fields and fields[0].startswith('.'):
            # GNU size prints the input filename before its rows. A relative
            # .pio/.../firmware.elf header is not a section, even with spaces.
            if re.fullmatch(r'.+\.elf\s*:\s*', line.strip()):
                continue
            if len(fields) != 3 or not fields[1].isdigit() or not fields[2].isdigit() or fields[0] in sections:
                raise ValueError('Malformed or duplicate ELF size row')
            sections[fields[0]] = {'size': int(fields[1]), 'address': int(fields[2])}
    result = evaluate(binary.stat().st_size, min(capacities), sections)
    binary.with_name('firmware-budget.json').write_text(json.dumps(result, indent=2) + '\n')
    print('Firmware budget:', json.dumps(result))
    return result
