#!/usr/bin/env python3
"""Prepare ONLY offline 20->18 MiB appearance layout migration artifacts.

Requires a complete pre-migration 32 MiB backup and the new partitions.bin.
Never accesses hardware, changes inputs, runs an SDK tool, or emits erase/flash
commands. OTA does not update the partition table. These overlapping regions
must NEVER be moved in place: keep the complete original backup independently.
Preparation is not a mounted-filesystem, boot, device, or migration acceptance.
"""
import argparse
import hashlib
import json
import os
from pathlib import Path
import struct

FLASH_BYTES = 32 * 1024 * 1024
TABLE_OFFSET = 0x8000
SECTOR = 4096
PAGE = 512
MD5_MARKER = b'\xeb\xeb' + b'\xff' * 14
OLD_LAYOUT = {
    'nvs': (1, 2, 0x9000, 0x6000, 0),
    'phy_init': (1, 1, 0xF000, 0x1000, 0),
    'ota_0': (0, 0x10, 0x10000, 0x280000, 0),
    'ota_1': (0, 0x11, 0x290000, 0x280000, 0),
    'otadata': (1, 0, 0x510000, 0x2000, 0),
    'storage': (1, 0x82, 0x520000, 0x6E0000, 0),
    'appearance0': (1, 0x40, 0xC00000, 0xA00000, 0),
    'appearance1': (1, 0x41, 0x1600000, 0xA00000, 0),
}
NEW_LAYOUT = {
    **OLD_LAYOUT,
    'storage': (1, 0x82, 0x520000, 0x8E0000, 0),
    'appearance0': (1, 0x40, 0xE00000, 0x900000, 0),
    'appearance1': (1, 0x41, 0x1700000, 0x900000, 0),
}


def require(condition, reason):
    if not condition:
        raise ValueError(reason)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def read_input(path, sizes):
    require(not path.is_symlink() and path.is_file(), f'input must be a regular non-symlink file: {path}')
    with path.open('rb') as stream:
        before = os.fstat(stream.fileno())
        require(before.st_size in sizes, f'wrong input size: {path} ({before.st_size})')
        data = stream.read(before.st_size + 1)
        after = os.fstat(stream.fileno())
    identity = lambda s: (s.st_dev, s.st_ino, s.st_size, s.st_mtime_ns, s.st_ctime_ns)
    require(identity(before) == identity(after) and len(data) == before.st_size,
            f'input changed while reading: {path}')
    return data


def parse_table(data, expected, description):
    require(len(data) in (0xC00, 0x1000), f'{description}: invalid table length')
    entries = {}
    payload = bytearray()
    checksum = None
    for offset in range(0, len(data), 32):
        row = data[offset:offset + 32]
        if row[:2] == b'\xeb\xeb':
            require(row[:16] == MD5_MARKER and checksum is None and entries,
                    f'{description}: malformed/duplicate MD5 record')
            checksum = hashlib.md5(payload).digest()
            require(row[16:] == checksum, f'{description}: partition MD5 mismatch')
            require(data[offset + 32:] == b'\xff' * (len(data) - offset - 32),
                    f'{description}: missing erased terminator or data after MD5')
            break
        require(row != b'\xff' * 32 and row[:2] == b'\xaa\x50',
                f'{description}: missing MD5 or invalid entry at {offset:#x}')
        magic, kind, subtype, start, size, label, flags = struct.unpack('<2sBBII16sI', row)
        name_bytes, zero, padding = label.partition(b'\0')
        require(bool(zero) and padding == b'\0' * len(padding),
                f'{description}: noncanonical label')
        name = name_bytes.decode('ascii')
        require(name not in entries and start >= 0x9000 and start % SECTOR == 0
                and size > 0 and size % SECTOR == 0 and start + size <= FLASH_BYTES,
                f'{description}: duplicate or out-of-range partition')
        entries[name] = (kind, subtype, start, size, flags)
        payload.extend(row)
    require(checksum is not None, f'{description}: mandatory MD5 missing')
    require(entries == expected, f'{description}: not the exact authorized layout (including labels/types/flags)')
    ordered = sorted((v[2], v[2] + v[3]) for v in entries.values())
    require(all(a[1] <= b[0] for a, b in zip(ordered, ordered[1:])),
            f'{description}: overlapping partitions')
    return {'md5': checksum.hex(), 'sha256': sha(data), 'bytes': len(data),
            'entries': {k: {'type': v[0], 'subtype': v[1], 'offset': v[2],
                            'size': v[3], 'flags': v[4]} for k, v in entries.items()}}


def region(backup, layout, name):
    start, size = layout[name][2:4]
    return backup[start:start + size]


def spiffs_magic(blocks, index):
    return (0x20140529 ^ PAGE ^ (blocks - index)) & 0xFFFF


def expand_spiffs(old):
    old_blocks = len(old) // SECTOR
    new_size = NEW_LAYOUT['storage'][3]
    new_blocks = new_size // SECTOR
    require(len(old) == OLD_LAYOUT['storage'][3] and new_size % SECTOR == 0,
            'SPIFFS size/alignment mismatch')
    # Verify ALL old blocks before changing even an in-memory byte. Do not
    # repair an erased/unformatted/corrupt old filesystem or infer its format.
    for index in range(old_blocks):
        actual = struct.unpack_from('<H', old, index * SECTOR + 508)[0]
        require(actual == spiffs_magic(old_blocks, index),
                f'old SPIFFS block {index}: invalid magic {actual:#06x}; refusing repair')
    expanded = bytearray(old)
    expanded.extend(b'\xff' * (new_size - len(old)))
    for index in range(new_blocks):
        base = index * SECTOR
        struct.pack_into('<H', expanded, base + 508, spiffs_magic(new_blocks, index))
        if index >= old_blocks:
            struct.pack_into('<H', expanded, base + 510, 0)
    changed = 0
    for index in range(old_blocks):
        base = index * SECTOR
        # Exact byte comparisons, not merely a filesystem-level hash: the
        # erase count at 510 and every other original byte MUST remain intact.
        require(expanded[base:base + 508] == old[base:base + 508]
                and expanded[base + 510:base + SECTOR] == old[base + 510:base + SECTOR],
                f'old SPIFFS block {index}: unexpected byte change')
        changed += sum(expanded[base + pos] != old[base + pos] for pos in (508, 509))
    for index in range(old_blocks, new_blocks):
        base = index * SECTOR
        require(expanded[base:base + 508] == b'\xff' * 508
                and expanded[base + 512:base + SECTOR] == b'\xff' * (SECTOR - 512)
                and expanded[base + 510:base + 512] == b'\0\0',
                f'new SPIFFS block {index}: invalid initialization')
    return bytes(expanded), {'blockSize': SECTOR, 'pageSize': PAGE, 'objectIdBits': 16,
                            'useMagicLength': True, 'oldBlocks': old_blocks,
                            'newBlocks': new_blocks, 'magicOffset': 508,
                            'eraseCountOffset': 510, 'changedOriginalBytes': changed,
                            'allOtherOriginalBytesIdentical': True,
                            'newBlockEraseCount': 0}


def fnv64(data):
    result = 0xCBF29CE484222325
    for byte in data:
        result = ((result ^ byte) * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return result


def prepare_raw(old, name):
    new_size = NEW_LAYOUT[name][3]
    if old == b'\xff' * len(old):
        return old[:new_size], {'empty': True, 'allOriginalSlotBytesFF': True,
                                'preservedPrefixBytes': new_size}
    require(old[:8] == b'P4RAW01\0', f'{name}: nonempty slot lacks P4RAW01 header')
    version, count, used, reserved = struct.unpack_from('<IIII', old, 8)
    require(version == 1 and reserved == 0 and 1 <= count <= 32,
            f'{name}: invalid version/reserved/count')
    require(4096 <= used <= new_size,
            f'{name}: used={used} exceeds new slot or invalid header; NEVER truncate live data')
    pack_id = old[24:88]
    require(len(pack_id) == 64 and all(b in b'0123456789abcdef' for b in pack_id),
            f'{name}: invalid pack id')
    ranges = []
    hashes = set()
    entry_proof = []
    for index in range(count):
        path_hash, offset, size, expected = struct.unpack_from('<QIIQ', old, 88 + index * 24)
        require(path_hash not in hashes and offset >= 4096 and offset % 4 == 0
                and size > 0 and offset + size <= used,
                f'{name}: entry {index} duplicate or outside committed data')
        hashes.add(path_hash)
        ranges.append((offset, offset + size))
        actual = fnv64(memoryview(old)[offset:offset + size])
        require(actual == expected, f'{name}: entry {index} FNV64 mismatch')
        entry_proof.append({'index': index, 'offset': offset, 'size': size,
                            'fnv64': f'{actual:016x}'})
    ranges.sort()
    require(all(a[1] <= b[0] for a, b in zip(ranges, ranges[1:])),
            f'{name}: overlapping committed entries')
    output = old[:new_size]
    require(output == old[:new_size], f'{name}: prefix changed')
    tail = old[new_size:]
    return output, {'empty': False, 'headerVersion': version, 'entryCount': count,
                    'usedBytes': used, 'entries': entry_proof,
                    'preservedPrefixBytes': new_size, 'prefixByteIdentical': True,
                    'omittedTailBytes': len(tail), 'omittedTailSha256': sha(tail),
                    'omittedTailNonFFBytes': sum(b != 255 for b in tail),
                    'omittedTailOutsideCommittedUsed': True}


def prepare(backup_path, table_path, output):
    backup = read_input(backup_path, {FLASH_BYTES})
    table = read_input(table_path, {0xC00, 0x1000})
    old_table = backup[TABLE_OFFSET:TABLE_OFFSET + SECTOR]
    old_proof = parse_table(old_table, OLD_LAYOUT, 'backup old table')
    new_proof = parse_table(table, NEW_LAYOUT, 'new table')
    storage, storage_proof = expand_spiffs(region(backup, OLD_LAYOUT, 'storage'))
    raw0, raw0_proof = prepare_raw(region(backup, OLD_LAYOUT, 'appearance0'), 'appearance0')
    raw1, raw1_proof = prepare_raw(region(backup, OLD_LAYOUT, 'appearance1'), 'appearance1')
    # Check the original files still match before admitting any output.
    require(read_input(backup_path, {FLASH_BYTES}) == backup, 'backup changed during preparation')
    require(read_input(table_path, {len(table)}) == table, 'new table changed during preparation')
    output.mkdir(mode=0o700, parents=False, exist_ok=False)
    artifacts = []
    for name, data, address in (
            ('storage.bin', storage, NEW_LAYOUT['storage'][2]),
            ('appearance0.bin', raw0, NEW_LAYOUT['appearance0'][2]),
            ('appearance1.bin', raw1, NEW_LAYOUT['appearance1'][2]),
            ('partitions.bin', table, TABLE_OFFSET)):
        path = output / name
        with path.open('xb') as stream:
            stream.write(data)
            stream.flush()
            os.fsync(stream.fileno())
        require(path.read_bytes() == data, f'output readback mismatch: {path}')
        artifacts.append({'name': name, 'offset': address, 'bytes': len(data), 'sha256': sha(data)})
    require(read_input(backup_path, {FLASH_BYTES}) == backup, 'backup changed while writing outputs')
    require(read_input(table_path, {len(table)}) == table, 'new table changed while writing outputs')
    manifest = {
        'schema': 1, 'status': 'OFFLINE_PREPARED_NOT_DEVICE_VERIFIED',
        'backup': {'path': str(backup_path), 'bytes': len(backup), 'sha256': sha(backup),
                   'retainedUnmodified': True},
        'newTableInput': {'path': str(table_path), 'sha256': sha(table)},
        'oldTable': old_proof, 'newTable': new_proof, 'artifacts': artifacts,
        'spiffs': storage_proof, 'appearance0': raw0_proof, 'appearance1': raw1_proof,
        'unchangedPartitions': ['nvs', 'phy_init', 'ota_0', 'ota_1', 'otadata'],
        'bootloaderAndUnallocatedBytes': 'retained only in the complete original backup',
        'overlappingOldNewRegions': True, 'completeBackupRequiredBeforeMigration': True,
        'otaUpdatesPartitionTable': False, 'hardwareUsed': False,
        'mountedFilesystemVerified': False, 'deviceBootVerified': False,
        'migrationVerified': False, 'releaseReady': False,
        'failurePolicy': 'No overwrite or deletion. Partial outputs lack a complete manifest; retain for diagnosis.'}
    with (output / 'manifest.json').open('x') as stream:
        json.dump(manifest, stream, ensure_ascii=False, indent=2)
        stream.write('\n')
        stream.flush()
        os.fsync(stream.fileno())
    return output / 'manifest.json'


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--backup', required=True, type=Path, help='complete original 32 MiB flash backup')
    parser.add_argument('--new-partitions', required=True, type=Path, help='new MD5-bearing partitions.bin')
    parser.add_argument('--output', required=True, type=Path, help='NEW directory; parent must already exist')
    args = parser.parse_args()
    try:
        manifest = prepare(args.backup.absolute(), args.new_partitions.absolute(), args.output.absolute())
    except (OSError, ValueError, UnicodeError, struct.error) as error:
        parser.exit(1, f'OFFLINE PREPARATION REFUSED: {error}\n')
    print(json.dumps({'status': 'OFFLINE_PREPARED_NOT_DEVICE_VERIFIED', 'manifest': str(manifest),
                      'hardwareUsed': False, 'migrationVerified': False}))


if __name__ == '__main__':
    main()
