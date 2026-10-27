"""Preserving OTA and metadata-only diagnostics for the native touch bench.

Never enters ROM flashing mode, resets GPIO, erases user partitions, aborts
another transfer, or sends uncorrelated retries of a committed update.
"""
import argparse
import base64
import hashlib
import json
import struct
import time
from pathlib import Path
import serial


def run(args):
    transfer = 'touch-bench-' + str(time.time_ns())
    s = serial.Serial(port=None, baudrate=4000000, timeout=.15, write_timeout=2)
    s.dtr = False
    s.rts = False
    s.exclusive = True
    s.port = args.port

    def send(topic, payload):
        raw = (json.dumps({'topic': topic, 'payload': payload}, separators=(',', ':')) + '\n').encode()
        for offset in range(0, len(raw), 64):
            s.write(raw[offset:offset+64])
            time.sleep(.0005)

    def request(topic, payload, reply, phase=None, timeout=10):
        request_id = 'touch-' + str(time.time_ns())
        payload = dict(payload, requestId=request_id)
        # Chunks/begin are protocol-idempotent; never blindly repeat commit.
        for attempt in range(1 if phase == 'commit' else 3):
            send(topic, payload)
            deadline = time.monotonic() + timeout
            while time.monotonic() < deadline:
                raw = s.readline().replace(b'\0', b'')
                start = raw.find(b'{')
                if start < 0:
                    continue
                try:
                    message = json.loads(raw[start:])
                except ValueError:
                    continue
                p = message.get('payload', {})
                if message.get('topic') != reply:
                    continue
                if phase:
                    if p.get('transferId') != transfer or p.get('phase') != phase:
                        continue
                    if phase == 'chunk' and p.get('seq') != payload['seq']:
                        continue
                elif p.get('requestId') != request_id:
                    continue
                return p
        raise TimeoutError(topic)

    def display(p):
        runtime = p.get('runtime', {})
        keys = ['firmware', 'chipRevision', 'buildId', 'touchReady', 'touchHealth',
                'touchDroppedEvents', 'screenPage', 'runningPartition', 'imageState']
        print(json.dumps({'runtime': {k: runtime.get(k) for k in keys},
                          'memory': p.get('memory')}, ensure_ascii=False), flush=True)

    begun = committed = commit_sent = False
    try:
        s.open()
        before = request('diagnostics/query', {}, 'diagnostics/status')
        display(before)
        if args.image:
            image = args.image.read_bytes()
            assert len(image) > 80 and image[0] == 0xe9
            assert struct.unpack_from('<H', image, 12)[0] == 18
            minimum, maximum = struct.unpack_from('<HH', image, 15)
            assert minimum <= before['runtime']['chipRevision'] <= maximum, 'chip mismatch'
            version = image[48:80].split(b'\0')[0].decode()
            assert version == '0.7.75-p4-touch', 'not a touch diagnostic image'
            status = request('firmware/query', {}, 'firmware/status')
            assert status['supported'] and not status['active'] and not status['pendingVerify'], 'OTA busy'
            digest = hashlib.sha256(image).hexdigest()
            ack = request('firmware/begin', dict(transferId=transfer, size=len(image), sha256=digest,
                          chipRevisionMin=minimum, chipRevisionMax=maximum), 'firmware/ack', 'begin', 30)
            assert ack['ok'], ack
            begun = True
            for seq, offset in enumerate(range(0, len(image), 2048)):
                part = image[offset:offset+2048]
                ack = request('firmware/chunk', dict(transferId=transfer, seq=seq,
                              decodedSize=len(part), data=base64.b64encode(part).decode()), 'firmware/ack', 'chunk')
                assert ack['ok'] and ack['receivedBytes'] == offset + len(part), ack
                if seq % 200 == 0:
                    print('OTA', offset+len(part), '/', len(image), flush=True)
            commit_sent = True
            ack = request('firmware/commit', {'transferId': transfer}, 'firmware/ack', 'commit', 20)
            assert ack['ok'] and ack['sha256'] == digest and ack['version'] == version, ack
            committed = True
            print('COMMITTED; waiting for stable boot', flush=True)
            time.sleep(8)
            deadline = time.monotonic() + 60
            while True:
                final = request('diagnostics/query', {}, 'diagnostics/status', timeout=15)
                if (final['runtime']['firmware'] == version
                        and final['runtime']['imageState'] == 'valid'
                        and final['runtime']['runningPartition'] != before['runtime']['runningPartition']):
                    break
                if time.monotonic() >= deadline:
                    raise RuntimeError('boot validity not confirmed; inspect device before retrying')
                time.sleep(2)
            display(final)
            assert final['runtime']['firmware'] == version and final['runtime']['imageState'] == 'valid'
            assert final['storage']['activeAppearanceSlot'] == before['storage']['activeAppearanceSlot']
            print('BOOT VERIFIED; appearance slot unchanged; no user partitions erased', flush=True)
        if args.action:
            ack = request('diagnostics/touch-test', {'action': args.action}, 'diagnostics/action')
            print(json.dumps(ack), flush=True)
        deadline = time.monotonic() + args.watch_seconds
        while time.monotonic() < deadline:
            time.sleep(2)
            display(request('diagnostics/query', {}, 'diagnostics/status'))
    finally:
        if s.is_open:
            if begun and not committed and not commit_sent:
                try:
                    send('firmware/abort', {'transferId': transfer})
                except Exception:
                    pass
            s.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--image', type=Path)
    parser.add_argument('--action', choices=['start', 'stop'])
    parser.add_argument('--watch-seconds', type=int, default=0)
    run(parser.parse_args())
