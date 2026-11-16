"""Real I/P content retaining SPS reorder declarations; no private camera input."""
import csv
import json
from pathlib import Path
import re
import subprocess
import sys
import tempfile

executable = Path(sys.argv[1]).resolve()
with tempfile.TemporaryDirectory(prefix='go3s_latency_') as temporary:
    root = Path(temporary)
    encoded = root / 'bframes.h264'
    subprocess.run(['ffmpeg', '-v', 'error', '-f', 'lavfi', '-i', 'testsrc2=size=160x96:rate=30',
                    '-frames:v', '32', '-c:v', 'libx264', '-threads', '1', '-pix_fmt', 'yuv420p',
                    '-x264-params', 'bframes=2:b-adapt=0:b-pyramid=none:ref=1:keyint=32:scenecut=0',
                    '-f', 'h264', str(encoded)], check=True, timeout=20)
    info = json.loads(subprocess.check_output(['ffprobe', '-v', 'error', '-show_packets',
        '-show_entries', 'packet=pos,size', '-of', 'json', str(encoded)], text=True))
    data = encoded.read_bytes()
    packets = [data[int(p['pos']):int(p['pos'])+int(p['size'])] for p in info['packets']]
    kept = []
    for packet in packets:
        starts = list(re.finditer(b'\x00\x00\x00?\x01', packet))
        headers = [packet[m.end()] for m in starts if packet[m.end()] & 31 in (1, 5)]
        if headers and all((header >> 5) & 3 == 0 for header in headers):
            continue  # Remove only non-reference B access units, keeping the I/P chain.
        kept.append(packet)
    for name, content in [('ip', kept), ('b', packets)]:
        directory = root / name
        directory.mkdir()
        (directory/'video.h264').write_bytes(b''.join(content))
        with (directory/'video_packets.csv').open('w', newline='') as f:
            writer = csv.writer(f)
            writer.writerow(['seq','stream_index','codec','byte_offset','byte_length',
                             'camera_timestamp_raw','phone_receive_ns','host_receive_ns'])
            offset = 0
            for i, packet in enumerate(content):
                writer.writerow([i,0,'h264',offset,len(packet),i*33,100+i,1_000_000_000+i*33_366_667])
                offset += len(packet)
    subprocess.run([str(executable),str(root/'ip'),str(root/'b')],check=True,timeout=20)
