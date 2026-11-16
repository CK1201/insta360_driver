"""Synthetic ADB inventory only. The executable on PATH never contacts real ADB."""
import json
import os
from pathlib import Path
import socket
import subprocess
import sys
import tempfile

cli = sys.argv[1]
with tempfile.TemporaryDirectory(prefix='go3s_synthetic_adb_') as temp:
    root = Path(temp)
    fake = root / 'adb'
    fake.write_text('''#!/usr/bin/python3
import json, os, sys
from pathlib import Path
args=sys.argv[1:]
with open(os.environ['MOCK_LOG'],'a') as log: log.write(json.dumps(args)+'\\n')
inventory=json.loads(Path(os.environ['MOCK_INVENTORY']).read_text())
if args==['devices']:
 print('List of devices attached')
 for serial, phone in inventory.items(): print(serial+'\\t'+phone.get('state','device'))
elif len(args)>=5 and args[0]=='-s' and args[2:4]==['shell','getprop']:
 print(inventory[args[1]].get(args[4],''))
elif len(args)>=3 and args[2]=='forward': pass
else: sys.exit(99)
''')
    fake.chmod(0o755)
    inventory = root / 'inventory.json'
    log = root / 'calls.jsonl'
    env = dict(os.environ, PATH=str(root) + ':' + os.environ['PATH'], MOCK_INVENTORY=str(inventory), MOCK_LOG=str(log))
    good = {'ro.product.manufacturer': 'Xiaomi', 'ro.product.model': 'MI 6', 'ro.product.name': 'sagit', 'ro.build.version.sdk': '28', 'ro.product.cpu.abilist': 'arm64-v8a,armeabi-v7a'}
    camera = dict(good, **{'ro.product.manufacturer': 'Insta360', 'ro.product.model': 'GO3S'})
    # A closed local port exercises connect failure without an actual bridge.
    sock = socket.socket(); sock.bind(('127.0.0.1', 0)); port = sock.getsockname()[1]; sock.close()
    def run(devices, requested='', selected=None):
        inventory.write_text(json.dumps(devices)); log.write_text('')
        config = root / 'config.yaml'
        config.write_text(json.dumps({'bridge': {'adb_serial': requested, 'port': port}, 'preview': {'enabled': False}}))
        result = subprocess.run([cli, '--config', str(config), '--duration', '.1'], env=env, capture_output=True, text=True, timeout=5)
        assert result.returncode != 0
        calls = [json.loads(line) for line in log.read_text().splitlines()]
        forwards = [a for a in calls if 'forward' in a]
        if selected:
            assert forwards == [['-s', selected, 'forward', '--no-rebind', 'tcp:' + str(port), 'tcp:18765'], ['-s', selected, 'forward', '--remove', 'tcp:' + str(port)]], calls
            assert 'cannot connect phone bridge' in result.stderr
        else:
            assert not forwards, calls
            assert 'exactly one authorized Android' in result.stderr
    run({'phone': good, 'camera': camera}, selected='phone')
    run({'phone': good, 'camera': camera}, requested='camera')
    run({'one': good, 'two': good})
    run({'one': good, 'two': good}, requested='two', selected='two')
    run({'old': dict(good, **{'ro.build.version.sdk': '27'})})
    run({'x86': dict(good, **{'ro.product.cpu.abilist': 'x86_64'})})
    run({'unauthorized': dict(good, state='unauthorized')})
    run({'camera_named': dict(good, **{'ro.product.name': 'GO3S'})})
    run({})
print('Synthetic ADB selection checks passed; real ADB never invoked.')
