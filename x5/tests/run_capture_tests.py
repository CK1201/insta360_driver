#!/usr/bin/env python3
"""Generate small real codec fixtures and audit persisted packet indexes."""
import csv
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    parent = pathlib.Path(sys.argv[2]).resolve()
    parent.mkdir(parents=True, exist_ok=True)
    run = pathlib.Path(tempfile.mkdtemp(prefix="capture-", dir=parent))
    for suffix, encoder in [("h264", "libx264"), ("h265", "libx265")]:
        command = ["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                   "-i", "color=c=red:size=160x80:rate=12", "-frames:v", "12",
                   "-c:v", encoder, "-threads", "1", "-pix_fmt", "yuv420p"]
        if suffix == "h265":
            command += ["-x265-params", "pools=none:frame-threads=1:log-level=error"]
        command += ["-f", "hevc" if suffix == "h265" else "h264", str(run / f"input.{suffix}")]
        subprocess.run(command, check=True, timeout=30)
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                    "-i", "color=c=0x4080c0:size=160x80:rate=12", "-frames:v", "12",
                    "-vf", "scale=in_color_matrix=bt601:out_color_matrix=bt709:in_range=tv:out_range=pc",
                    "-c:v", "libx264", "-threads", "1", "-pix_fmt", "yuvj420p",
                    "-color_range", "pc", "-colorspace", "bt709", "-f", "h264",
                    str(run / "full_range.h264")], check=True, timeout=30)
    encoded = (run / "input.h264").read_bytes()
    starts = list(re.finditer(b"\x00\x00\x00?\x01", encoded))
    first_interframe = next(encoded[m.start():starts[i + 1].start() if i + 1 < len(starts) else len(encoded)]
                            for i, m in enumerate(starts) if encoded[m.end()] & 31 == 1)
    (run / "prefixed.h264").write_bytes(first_interframe + encoded)
    result = subprocess.run([str(executable), str(run / "input.h264"), str(run / "input.h265"),
                             str(run / "full_range.h264"), str(run / "prefixed.h264"), str(run / "output")],
                            text=True, capture_output=True, timeout=30)
    print(result.stdout)
    if result.returncode:
        print(result.stderr, file=sys.stderr)
    result.check_returncode()
    assert "deprecated pixel format used" not in result.stderr, result.stderr
    for codec in ["h264", "h265"]:
        directory = run / "output" / codec
        video = (directory / f"video_stream_0.{codec}").read_bytes()
        with (directory / "video_packets.csv").open() as file:
            rows = list(csv.DictReader(file))
        cursor = 0
        for sequence, row in enumerate(rows):
            assert int(row["packet_index"]) == sequence
            assert int(row["stream_index"]) == 0
            assert int(row["device_timestamp"]) == 900000 + sequence
            assert int(row["host_receive_steady_ns"]) == 1000000 + sequence
            assert int(row["file_offset"]) == cursor
            cursor += int(row["size_bytes"])
        assert cursor == len(video)
        with (directory / "imu.csv").open() as file:
            imu = list(csv.DictReader(file))
        assert len(imu) == 2 and float(imu[1]["gz"]) == -6
        assert int(imu[0]["device_timestamp"]) == 91
        with (directory / "exposure.csv").open() as file:
            exposure = list(csv.DictReader(file))
        assert len(exposure) == 1 and float(exposure[0]["device_timestamp"]) == 93.5
        assert float(exposure[0]["exposure_time_s"]) == 0.008
        subprocess.run(["ffmpeg", "-v", "error", "-i", str(directory / f"video_stream_0.{codec}"),
                        "-f", "null", "-"], check=True, timeout=30)
    print("PASS: packet offsets, raw timestamps, IMU/exposure values, full video decode")


if __name__ == "__main__":
    main()
