#!/usr/bin/env python3
"""Create I/P-only content retaining a nonzero SPS reordering declaration."""
import csv
import json
import pathlib
import re
import subprocess
import sys
import tempfile


def probe(path):
    return json.loads(subprocess.run([
        "ffprobe", "-v", "error", "-show_packets", "-show_frames", "-show_streams",
        "-show_entries", "packet=pos,size:frame=pict_type:stream=has_b_frames",
        "-of", "json", str(path)], capture_output=True, text=True, check=True).stdout)


def write_capture(capture, packets):
    capture.mkdir()
    (capture / "video_stream_0.h264").write_bytes(b"".join(packets))
    with (capture / "video_packets.csv").open("w", newline="") as file:
        writer = csv.writer(file)
        writer.writerow(["packet_index", "stream_index", "device_timestamp", "host_receive_steady_ns", "file_offset", "size_bytes"])
        offset = 0
        for index, packet in enumerate(packets):
            writer.writerow([index, 0, index * 33, 1000000000 + index * 33366667, offset, len(packet)])
            offset += len(packet)


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    parent = pathlib.Path(sys.argv[2]).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    root = pathlib.Path(tempfile.mkdtemp(prefix="low-latency-", dir=parent))
    original = root / "bframes.h264"
    subprocess.run([
        "ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
        "-i", "testsrc2=size=160x80:rate=30", "-frames:v", "32", "-c:v", "libx264",
        "-threads", "1", "-pix_fmt", "yuv420p", "-x264-params",
        "bframes=2:b-adapt=0:b-pyramid=none:ref=1:keyint=32:scenecut=0",
        "-f", "h264", str(original)], check=True, timeout=30)
    info = probe(original)
    # Removing only non-reference B access units leaves the reference I/P chain
    # intact, while the SPS still advertises reordering. No private camera video.
    items = info["packets_and_frames"]
    packets = [p for p in items if p["type"] == "packet"]
    data = original.read_bytes()
    kept = []
    for packet in packets:
        part = data[int(packet["pos"]):int(packet["pos"]) + int(packet["size"])]
        starts = list(re.finditer(b"\x00\x00\x00?\x01", part))
        vcl_headers = [part[m.end()] for m in starts if part[m.end()] & 31 in (1, 5)]
        if vcl_headers and all((h >> 5) & 3 == 0 for h in vcl_headers):
            continue
        kept.append(part)
    capture = root / "capture"
    video = capture / "video_stream_0.h264"
    write_capture(capture, kept)
    filtered = probe(video)
    assert filtered["streams"][0]["has_b_frames"] > 0
    frame_types = [p["pict_type"] for p in filtered["packets_and_frames"] if p["type"] == "frame"]
    assert "B" not in frame_types and len(frame_types) == len(kept)
    b_capture = root / "b_capture"
    write_capture(b_capture, [data[int(p["pos"]):int(p["pos"]) + int(p["size"])] for p in packets])
    subprocess.run([str(executable), str(capture), str(b_capture)], check=True, timeout=30)


if __name__ == "__main__":
    main()
