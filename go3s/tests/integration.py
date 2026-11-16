"""Synthetic localhost phone fixtures, real FFmpeg H264/HEVC. No ADB or hardware."""
import csv
import json
import os
from pathlib import Path
import socket
import struct
import subprocess
import sys
import tempfile
import threading
import time

FIXTURE, CLI, FFMPEG = sys.argv[1:]


def framed(header, payload=b''):
    header = json.dumps(dict(v=1, **header), separators=(',', ':')).encode()
    return struct.pack('!II', len(header), len(payload)) + header + payload


def exact(conn, size):
    result = b''
    while len(result) < size:
        data = conn.recv(size - len(result))
        if not data:
            raise EOFError('fixture EOF')
        result += data
    return result


def receive(conn):
    h, p = struct.unpack('!II', exact(conn, 8))
    header = json.loads(exact(conn, h))
    assert not exact(conn, p)
    return header


def encode(root, codec):
    output = root / ('fixture.' + codec)
    encoder = 'libx264' if codec == 'h264' else 'libx265'
    args = [FFMPEG, '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=30', '-frames:v', '12',
            '-c:v', encoder, '-pix_fmt', 'yuv420p', '-threads', '1']
    args += ['-x264-params', 'keyint=6:bframes=0'] if codec == 'h264' else ['-x265-params', 'pools=none:keyint=6:bframes=0:log-level=error']
    subprocess.run(args + ['-f', 'h264' if codec == 'h264' else 'hevc', str(output)], check=True, capture_output=True)
    return output.read_bytes()


def run_case(root, name, video, codec='h264', fault=None, imu=True, complete=False):
    output = root / name
    output.mkdir()
    listener = socket.socket()
    listener.bind(('127.0.0.1', 0))
    listener.listen(1)
    process = subprocess.Popen([FIXTURE, str(listener.getsockname()[1]), str(output), '0.25', str(imu).lower()] + (['complete'] if complete else []), stdout=subprocess.PIPE, stderr=subprocess.PIPE)
    conn, _ = listener.accept()
    conn.settimeout(8)
    seq = 0
    transmitted_video = b''
    raw_gyro = bytes(range(83))
    raw_gyro_tail = b'gyro-invalid-final-packet'
    def send(kind, payload=b'', **values):
        nonlocal seq
        data = framed(dict(type=kind, seq=seq, **values), payload)
        seq += 1
        # Fragment both fixed framing and JSON deliberately.
        cuts = (1, 3, 7, 11, 19)
        begin = 0
        for end in cuts:
            conn.sendall(data[begin:end]); begin = end
        conn.sendall(data[begin:])
    try:
        send('hello', phone={'model': 'synthetic', 'android_sdk': 28, 'abis': ['arm64-v8a']}, sdk_version='2.2.6', sdk_internal_loss_observable=False)
        start = receive(conn)
        assert start['type'] == 'start' and start['camera']['exposure'] == {'mode': 'auto', 'iso': None, 'shutter_s': None, 'ev': 0.0}
        assert start['stream']['imu'] == imu
        send('session', identity={'model': 'synthetic GO3S', 'serial': 'synthetic', 'firmware': 'test', 'sdk_version': '2.2.6'}, requested=start,
             readback={'exposure': start['camera']['exposure'], 'white_balance_kelvin': 0}, preview={}, sdk_internal_loss_observable=False)
        if fault == 'write':
            session = next(output.glob('go3s_*'))
            (session / 'video.h264').symlink_to('/dev/full')
        if complete:
            raw_fixture = root / (name + '.' + codec)
            raw_fixture.write_bytes(video)
            packets = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_packets',
                '-show_entries', 'packet=pos,size', '-of', 'json', str(raw_fixture)], text=True))['packets']
            chunks = [video[int(p['pos']):int(p['pos']) + int(p['size'])] for p in packets]
        else:
            chunks = [video[i:i + 397] for i in range(0, len(video), 397)]
        for index, chunk in enumerate(chunks):
            if fault == 'gap' and index == len(chunks) - 1:
                seq += 1
            send('video', chunk, camera_timestamp=100 + index, phone_receive_ns=10_000 + index, stream_index=0, codec=codec)
            transmitted_video += chunk
        if fault == 'decode':
            # Invalid IDR forces FFmpeg decode failure; later raw metadata must drain.
            bad = b'\x00\x00\x00\x01\x65\xff\xff\xff\xff\x00\x00\x01\x09\xf0'
            send('video', bad, camera_timestamp=999, phone_receive_ns=20_000, stream_index=0, codec=codec)
            transmitted_video += bad
        if imu and fault != 'missing_imu':
            send('gyro', raw_gyro, camera_timestamp=888, phone_receive_ns=30_000,
                 samples=[[123456789, 1.25, -2.5, 3.75, -4.125, 5.5, -6.875], [123456789, 9, 8, 7, 6, 5, 4]])
        if fault != 'missing_exposure':
            send('exposure', camera_timestamp=777, phone_receive_ns=40_000, exposure_time_s=0.004)
        send('status', phone_receive_ns=50_000, connected=True, battery_level=75, battery_scale=100,
             power='synthetic', storage=[{'freeSpace': 12345}], preview={'codec': codec, 'width': 160, 'height': 96, 'fps': 30})
        if fault == 'gyro_parse':
            send('gyro', raw_gyro_tail, camera_timestamp=889, phone_receive_ns=60_000,
                 samples=[], parse_error='synthetic final gyro decode failure')
        if fault == 'eof':
            conn.close()
        elif fault == 'partial':
            conn.sendall(b'\x00\x00\x00')
            conn.close()
        elif fault == 'gap':
            # Protocol failure may close before stop; no new accepted events.
            conn.shutdown(socket.SHUT_WR)
        else:
            stop = receive(conn)
            assert stop['type'] == 'stop'
            if fault == 'end_timeout':
                # Stay connected past the host's bounded end wait.
                time.sleep(5.3)
            else:
                send('end', reason='stop', sdk_internal_loss_observable=False)
            conn.close()
        stdout, stderr = process.communicate(timeout=10)
    finally:
        conn.close(); listener.close()
        if process.poll() is None:
            process.kill(); process.wait()
    session = next(output.glob('go3s_*'))
    summary = json.loads((session / 'summary.yaml').read_text())
    assert summary['sdk_internal_loss_verification'] == 'unverified'
    assert summary['accepted_events'] == summary['processed_events'], summary
    assert summary['hardware_verified'] is False
    assert summary['preview_decode_mode'] == ('complete_h264_low_delay' if complete and codec == 'h264' else 'byte_stream')
    assert (process.returncode == 0) == (fault in (None, 'missing_exposure')), (name, process.returncode, stdout, stderr, summary)
    if fault in (None, 'missing_exposure'):
        assert summary['complete'] and summary['decoded_frames'] == 12, summary
        assert b'fps=' in stderr and b'imu_samples_per_s=' in stderr
        assert b'size=160x96' in stderr and b'battery=75/100 power="synthetic" storage=' in stderr
        assert b'exposure_s=0.004' in stderr if fault is None else b'exposure_s=unknown' in stderr
        assert (session / ('video.' + codec)).read_bytes() == transmitted_video
        index = list(csv.DictReader((session / 'video_packets.csv').open()))
        offset = 0
        for row in index:
            assert int(row['byte_offset']) == offset
            assert int(row['phone_receive_ns']) != int(row['host_receive_ns'])
            offset += int(row['byte_length'])
        assert offset == len(transmitted_video)
        timing = list(csv.DictReader((session / 'frame_timing.csv').open()))
        assert len(timing) == 12 and all(int(row['width']) == 160 and int(row['height']) == 96 for row in timing)
    if imu and fault not in ('gap', 'missing_imu'):
        expected_gyro = raw_gyro + (raw_gyro_tail if fault == 'gyro_parse' else b'')
        assert (session / 'gyro.bin').read_bytes() == expected_gyro
        parsed = list(csv.DictReader((session / 'imu.csv').open()))
        assert len(parsed) == 2 and parsed[0]['device_timestamp_raw'] == '123456789'
        assert [float(parsed[0][key]) for key in ('ax_raw', 'ay_raw', 'az_raw', 'gx_raw', 'gy_raw', 'gz_raw')] == [1.25, -2.5, 3.75, -4.125, 5.5, -6.875]
        assert parsed[0]['device_timestamp_raw'] == parsed[1]['device_timestamp_raw'], 'never deduplicate SDK samples'
    if fault == 'decode':
        assert (session / 'video.h264').read_bytes() == transmitted_video
        assert summary['imu_samples'] == 2 and summary['exposure_callbacks'] == 1
        assert 'video decode failure' in summary['error']
    if fault == 'missing_imu':
        assert 'IMU enabled but no' in summary['error']
    if fault == 'missing_exposure':
        assert summary['exposure_warning'] and b'no camera exposure callbacks' in stderr
    if fault == 'end_timeout':
        assert not summary['end_received'] and 'deadline' in summary['error']
    if fault == 'gyro_parse':
        assert summary['end_received'] and summary['end_reason'] == 'stop'
        assert not summary['complete'] and 'camera gyro parse failure' in summary['error']
        assert summary['imu_samples'] == 2 and summary['gyro_packets'] == 2
        gyro_index = list(csv.DictReader((session / 'gyro_packets.csv').open()))
        assert len(gyro_index) == 2
        assert int(gyro_index[1]['byte_offset']) == len(raw_gyro)
        assert int(gyro_index[1]['byte_length']) == len(raw_gyro_tail)
        assert gyro_index[1]['sample_count'] == '0'
    print(name, 'passed:', summary['error'] or 'normal stop/end')


with tempfile.TemporaryDirectory(prefix='go3s_synthetic_') as temp:
    root = Path(temp)
    # Poison ADB proves offline config checking never launches it or creates output.
    fake_bin = root / 'bin'; fake_bin.mkdir()
    marker = root / 'adb-used'
    (fake_bin / 'adb').write_text('#!/bin/sh\ntouch "' + str(marker) + '"\nexit 99\n')
    (fake_bin / 'adb').chmod(0o755)
    env = dict(os.environ, PATH=str(fake_bin) + ':' + os.environ['PATH'])
    config = root / 'config.yaml'
    config.write_text('output:\n  save: true\n  directory: ./must-not-exist\n')
    checked = subprocess.run([CLI, '--config', str(config), '--duration', '0.5', '--check-config'], env=env, capture_output=True, text=True)
    assert checked.returncode == 0, checked.stderr
    assert json.loads(checked.stdout)['runtime']['duration_s'] == .5
    assert not marker.exists() and not (root / 'must-not-exist').exists()
    assert subprocess.run([CLI, '--help'], capture_output=True).returncode == 0
    for invalid in ['camera:\n  resolution: 1920\n', 'camera:\n  exposure:\n    mode: manual\n', 'runtime:\n  duration_s: -1\n', 'stream:\n  imu: true\n  imu: false\n']:
        config.write_text(invalid)
        assert subprocess.run([CLI, '--config', str(config), '--check-config'], env=env, capture_output=True).returncode != 0
    assert not marker.exists()
    for delay in (0, 0.120):
        config.write_text('timing:\n  sensor_delay_s: ' + str(delay) + '\n')
        checked = subprocess.run([CLI, '--config', str(config), '--check-config'], env=env, capture_output=True, text=True)
        assert checked.returncode == 0, checked.stderr
        assert json.loads(checked.stdout)['timing']['sensor_delay_s'] == delay
    for delay in ('-0.1', '.nan', '.inf', '1e30', 'null', 'oops'):
        config.write_text('timing:\n  sensor_delay_s: ' + delay + '\n')
        rejected = subprocess.run([CLI, '--config', str(config), '--check-config'], env=env, capture_output=True, text=True)
        assert rejected.returncode != 0 and 'timing.sensor_delay_s' in rejected.stderr, rejected.stderr
    assert not marker.exists()
    for interval in (0.05, 61):
        config.write_text('runtime:\n  status_interval_s: ' + str(interval) + '\n')
        assert subprocess.run([CLI, '--config', str(config), '--check-config'], env=env, capture_output=True).returncode != 0
    for interval in (0.1, 60):
        config.write_text('runtime:\n  status_interval_s: ' + str(interval) + '\n')
        accepted = subprocess.run([CLI, '--config', str(config), '--check-config'], env=env, capture_output=True, text=True)
        assert accepted.returncode == 0, accepted.stderr
        assert json.loads(accepted.stdout)['runtime']['status_interval_s'] == interval
    assert not marker.exists()
    h264 = encode(root, 'h264')
    h265 = encode(root, 'h265')
    run_case(root, 'normal_h264', h264)
    run_case(root, 'normal_fast_h264', h264, complete=True)
    run_case(root, 'decode_fast_h264', h264, fault='decode', complete=True)
    run_case(root, 'normal_h265', h265, 'h265')
    run_case(root, 'normal_live_h265', h265, 'h265', complete=True)
    run_case(root, 'imu_disabled', h264, imu=False)
    for fault in ('eof', 'partial', 'gap', 'write', 'decode', 'missing_imu', 'missing_exposure', 'end_timeout', 'gyro_parse'):
        run_case(root, fault, h264, fault=fault)
print('All synthetic integration checks passed; no phone/camera acceptance implied.')
