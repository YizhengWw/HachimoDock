"""Install a validated touch component without erasing existing components.

Uses the same JSON/P4S transaction as the desktop client. No commit retries.
Only exports non-identifying device capabilities, never the full hello payload.
"""
import argparse
import base64
import json
import sys
import time
from pathlib import Path
import serial
from p4_component_sprites import compile_component_sprites


def run(args):
    root = Path(__file__).resolve().parents[2]
    sys.path.insert(0, str(root / 'pc/skills/petui/scripts'))
    from validate_generated_widget import validate_widget
    transfer = 'touch-install-' + str(time.time_ns())
    connection = serial.Serial(port=None, baudrate=4000000, timeout=.2, write_timeout=3)
    connection.dtr = connection.rts = False
    connection.exclusive = True
    connection.port = args.port

    def read(topic, matches=lambda p: True, timeout=15):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            raw = connection.readline().replace(b'\0', b'')
            start = raw.find(b'{')
            if start < 0:
                continue
            try:
                message = json.loads(raw[start:])
            except ValueError:
                continue
            payload = message.get('payload', {})
            if message.get('topic') == topic and matches(payload):
                return payload
        raise TimeoutError(topic + '; inspect actual device state before retrying')

    def send(topic, payload):
        raw = (json.dumps({'topic': topic, 'payload': payload}, separators=(',', ':')) + '\n').encode()
        for offset in range(0, len(raw), 64):
            connection.write(raw[offset:offset+64])
            time.sleep(.0005)

    def query(topic, reply):
        request = 'touch-' + str(time.time_ns())
        send(topic, {'requestId': request})
        return read(reply, lambda p: p.get('requestId') == request)

    def phase(name, payload):
        send('widget/' + name, dict(payload, transferId=transfer))
        result = read('widget-install-ack', lambda p: p.get('transferId') == transfer
                      and p.get('phase') == name
                      and (name != 'chunk' or (p.get('path') == payload['path'] and p.get('index') == payload['index'])))
        if not result.get('ok'):
            raise RuntimeError(result.get('msg', 'widget transaction rejected'))

    def ids(inventory):
        if not inventory.get('ok') or not inventory.get('complete'):
            raise RuntimeError('incomplete inventory; refusing install')
        return {item['id'] for item in inventory['items']}

    try:
        connection.open()
        hello = read('hello', timeout=20)
        caps = hello['capabilities']
        safe_keys = ['widgetRuntime', 'widgetRuntimes', 'widgetScene', 'widgetScenes',
                     'widgetPointer', 'widgetSliceRevision', 'widgetSprites', 'widgetLimits', 'touchInput']
        safe_caps = {key: caps[key] for key in safe_keys if key in caps}
        if args.capabilities_out:
            args.capabilities_out.write_text(json.dumps(safe_caps, indent=2) + '\n')
        errors = validate_widget(args.package, safe_caps)
        if errors:
            raise RuntimeError(errors)
        print('Device capability and package validation passed', flush=True)
        before = query('widget/list', 'widget/inventory')
        before_ids = ids(before)
        meta = json.loads((args.package / 'component.json').read_text())
        widget = json.loads((args.package / 'runtime/widget.json').read_text())
        if meta['id'] not in before_ids and len(before_ids) >= before['maxInstalled']:
            raise RuntimeError('component catalog full; existing packages untouched')
        files = [('runtime/widget.json', json.dumps(widget, ensure_ascii=False, separators=(',', ':')).encode()),
                 ('buttons.json', json.dumps(json.loads((args.package/'buttons.json').read_text()), ensure_ascii=False, separators=(',', ':')).encode())]
        files += [('runtime/sprites/' + s['id'] + '.p4s', s['data']) for s in compile_component_sprites(args.package, widget)]
        phase('begin', {'widgetId': meta['id']})
        for path, data in files:
            checksum = 0xcbf29ce484222325
            for byte in data:
                checksum = ((checksum ^ byte) * 0x100000001b3) & 0xffffffffffffffff
            phase('chunk', {'path': path, 'index': 0, 'decodedSize': len(data),
                           'checksum': f'{checksum:016x}', 'data': base64.b64encode(data).decode()})
        phase('commit', {'widgetId': meta['id']})
        after = query('widget/list', 'widget/inventory')
        assert ids(after) == before_ids | {meta['id']}, 'catalog changed unexpectedly'
        assert any(i['id'] == meta['id'] and i['active'] for i in after['items'])
        print('INSTALLED ' + meta['id'] + '; previous catalog preserved; component is active', flush=True)
        deadline = time.monotonic() + args.watch_seconds
        while time.monotonic() < deadline:
            time.sleep(3)
            status = query('diagnostics/query', 'diagnostics/status')
            r = status['runtime']
            print(json.dumps({k: r.get(k) for k in ['touchReady', 'touchHealth', 'touchDroppedEvents', 'screenPage', 'imageState']}) , flush=True)
    finally:
        connection.close()


if __name__ == '__main__':
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--port', required=True)
    parser.add_argument('--package', type=Path, required=True)
    parser.add_argument('--capabilities-out', type=Path)
    parser.add_argument('--watch-seconds', type=int, default=0)
    run(parser.parse_args())
