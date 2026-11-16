"""Exercise the production Reader/bindings with real codecs, without a USB camera."""
import threading
import time
import subprocess

import numpy as np
import pytest
import insta360_x5 as camera
from _x5_reader_fixture import Fixture


@pytest.fixture(scope="module", params=["h264", "h265"])
def encoded(request, tmp_path_factory):
    folder = tmp_path_factory.mktemp(request.param)
    path = folder / ("sample." + request.param)
    codec = "libx264" if request.param == "h264" else "libx265"
    args = ["ffmpeg", "-v", "error", "-f", "lavfi", "-i", "testsrc2=s=64x32:r=30",
            "-frames:v", "8", "-c:v", codec, "-threads", "1", "-pix_fmt", "yuv420p"]
    if request.param == "h265":
        args += ["-x265-params", "pools=none:frame-threads=1:log-level=error"]
    subprocess.run(args + ["-f", "hevc" if request.param == "h265" else "h264", str(path)], check=True)
    return request.param, path.read_bytes()


def wait_for(predicate):
    deadline = time.monotonic() + 3
    while not predicate():
        assert time.monotonic() < deadline
        time.sleep(0.005)


@pytest.mark.parametrize("sensor_delay_s", [0.0, 0.120])
def test_decoding_freshness_ownership_and_recording(encoded, tmp_path, sensor_delay_s):
    codec, data = encoded
    fixture = Fixture(codec, str(tmp_path), True, sensor_delay_s=sensor_delay_s)
    r = fixture.reader
    assert r.latest() is None and not r.running
    with r:
        # Fragmented byte stream feeds the actual CapturePipeline and FFmpeg.
        for offset in range(0, len(data), 137):
            fixture.push(data[offset:offset+137], 10_000 + offset, 0)
        frame = r.read(timeout=3)
        assert frame.image.dtype == np.uint8 and frame.image.shape == (32, 64, 3)
        assert frame.host_decode_ns > 0 and frame.host_decode_ns >= frame.host_receive_ns >= 0
        expected = frame.host_receive_ns - round(sensor_delay_s * 1e9) if frame.host_receive_ns else 0
        assert frame.timestamp_ns == expected
        retained = frame.image.copy()
        frame.image[:] = 0
        assert np.array_equal(r.latest().image, retained) or r.latest().index > frame.index
        wait_for(lambda: fixture.frames >= 5)
        latest = r.latest()
        with pytest.raises(TimeoutError):
            r.read(timeout=0.02)
        assert latest.index >= 5
        fixture.end()
        wait_for(lambda: not r.running)
        last = r.latest()
        assert last.index == 8 and last.stream_index == 0
        with pytest.raises(StopIteration):
            r.read(timeout=0)
    assert r.error is None
    assert not np.any(frame.image)  # The user's mutation survives stop; backing storage stays valid.
    assert np.any(last.image)
    assert r.latest().timestamp_ns == last.timestamp_ns
    assert (tmp_path / ("video_stream_0."+codec)).read_bytes() == data
    import csv
    rows = list(csv.DictReader((tmp_path / "video_packets.csv").open()))
    raw = (tmp_path / ("video_stream_0."+codec)).read_bytes()
    assert sum(int(row["size_bytes"]) for row in rows) == len(data)
    for row in rows:
        offset, size = int(row["file_offset"]), int(row["size_bytes"])
        assert raw[offset:offset+size] == data[offset:offset+size]
    times = list(csv.DictReader((tmp_path / "frame_timings.csv").open()))
    assert len(times) == 8
    for row in times:
        received = int(row["host_receive_steady_ns"])
        assert int(row["timestamp_ns"]) == (received - round(sensor_delay_s*1e9) if received else 0)


def test_gil_wait_stop_and_restart():
    f = Fixture("h264", "", False)
    r = f.reader.start()
    finished = threading.Event()
    outcomes = []
    def blocked():
        try:
            r.read(timeout=None)
        except StopIteration:
            outcomes.append("stopped")
        finally:
            finished.set()
    thread = threading.Thread(target=blocked)
    thread.start()
    # If read holds the GIL, neither this thread nor stop can progress.
    time.sleep(0.03)
    r.stop()
    assert finished.wait(1)
    thread.join()
    assert outcomes == ["stopped"]
    r.close()
    r.start()
    assert r.latest() is None and r.running
    r.stop()


def test_failure_and_validation():
    f = Fixture("h264", "", False)
    r = f.reader.start()
    for timeout in [-1, float("nan"), float("inf")]:
        with pytest.raises(ValueError, match="timeout"):
            r.read(timeout=timeout)
    with pytest.raises(ValueError, match="stream_index"):
        r.latest(stream_index=-1)
    f.fail("disk failed in test backend")
    with pytest.raises(RuntimeError, match="disk failed"):
        r.read(timeout=1)
    with pytest.raises(RuntimeError, match="disk failed"):
        r.latest()
    assert "disk failed" in r.error
    r.stop()


def test_constructor_is_offline_and_x5_remains_30fps(tmp_path):
    config = tmp_path / "config.yaml"
    config.write_text("stream: {resolution: 960, fps: 60}\n")
    with pytest.raises(RuntimeError, match="30"):
        camera.Reader(config=config)


def test_final_reference_destruction_releases_gil():
    fixture = Fixture("h264", "", False, shutdown_delay_ms=600)
    reader = fixture.reader.start()
    del fixture  # Only the Python Reader holder now owns the native Reader.
    active = threading.Event()
    active.set()
    stamps = []
    def heartbeat():
        while active.is_set():
            stamps.append(time.monotonic())
            time.sleep(0.005)
    thread = threading.Thread(target=heartbeat)
    thread.start()
    time.sleep(0.03)
    started = time.monotonic()
    del reader
    finished = time.monotonic()
    active.clear()
    thread.join()
    assert finished - started >= 0.5  # Native delayed shutdown actually ran.
    assert any(started + 0.05 < stamp < finished - 0.05 for stamp in stamps)
