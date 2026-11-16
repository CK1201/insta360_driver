"""Offline Python API acceptance through real wire, decoder and persistence paths."""
import csv
import json
import os
from pathlib import Path
import queue
import select
import socket
import struct
import subprocess
import sys
import threading
import time

import numpy as np
import pytest

from insta360_go3s import Frame, Reader


def until(predicate, timeout=5):
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return
        time.sleep(.005)
    raise AssertionError("condition did not become true")


def exact(conn, count):
    result = b""
    while len(result) < count:
        data = conn.recv(count - len(result))
        if not data:
            raise EOFError("fixture EOF")
        result += data
    return result


def receive(conn):
    h, p = struct.unpack("!II", exact(conn, 8))
    result = json.loads(exact(conn, h))
    assert not exact(conn, p)
    return result


@pytest.fixture(scope="session")
def videos(tmp_path_factory):
    root = tmp_path_factory.mktemp("go3s_codecs")
    result = {}
    for codec, encoder in (("h264", "libx264"), ("h265", "libx265")):
        path = root / ("video." + codec)
        options = ["-x264-params", "keyint=6:bframes=0"] if codec == "h264" else [
            "-x265-params", "pools=none:keyint=6:bframes=0:log-level=error"]
        subprocess.run(["ffmpeg", "-v", "error", "-f", "lavfi", "-i",
                        "testsrc2=size=160x96:rate=30", "-frames:v", "12", "-c:v", encoder,
                        "-pix_fmt", "yuv420p", "-threads", "1", *options, "-f",
                        "h264" if codec == "h264" else "hevc", str(path)], check=True)
        video = path.read_bytes()
        packets = json.loads(subprocess.check_output(["ffprobe", "-v", "error", "-show_packets",
            "-show_entries", "packet=pos,size", "-of", "json", str(path)]))["packets"]
        chunks = [video[int(p["pos"]):int(p["pos"]) + int(p["size"])] for p in packets]
        pixels = subprocess.check_output(["ffmpeg", "-v", "error", "-i", str(path), "-threads", "1",
            "-f", "rawvideo", "-pix_fmt", "bgr24", "-"])
        result[codec] = (video, chunks, np.frombuffer(pixels, np.uint8).reshape(12, 96, 160, 3))
    return result


class Bridge:
    def __init__(self, root, monkeypatch, videos, codec="h264", save=False, duration=0, sensor_delay_s=0):
        self.codec = codec
        self.video, self.chunks, self.pixels = videos[codec]
        self.commands = queue.Queue()
        self.ready = threading.Event()
        self.done = threading.Event()
        self.error = None
        self.transmitted = b""
        self.listener = socket.socket()
        self.listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        self.listener.bind(("127.0.0.1", 0))
        self.listener.listen(1)
        self.listener.settimeout(8)
        self.output = root / "output"
        self.marker = root / "adb-used"
        fakebin = root / "bin"
        fakebin.mkdir()
        adb = fakebin / "adb"
        adb.write_text(f"#!{sys.executable}\n" + '''import pathlib,sys
pathlib.Path(''' + repr(str(self.marker)) + ''').touch()
a = sys.argv[1:]
if a == ['devices']: print('List of devices attached\\nfixture-phone\\tdevice')
elif 'getprop' in a:
 print({'ro.product.manufacturer':'synthetic', 'ro.product.model':'offline phone',
 'ro.product.name':'fixture', 'ro.build.version.sdk':'28', 'ro.product.cpu.abilist':'arm64-v8a'}[a[-1]])
elif 'forward' in a: pass
else: sys.exit(99)
''')
        adb.chmod(0o755)
        monkeypatch.setenv("PATH", str(fakebin) + os.pathsep + os.environ["PATH"])
        self.config = root / "config.yaml"
        self.config.write_text(json.dumps({"bridge": {"port": self.listener.getsockname()[1]},
            "stream": {"imu": False}, "preview": {"enabled": True},
            "output": {"save": save, "directory": str(self.output)},
            "runtime": {"duration_s": duration, "status_interval_s": 60},
            "timing": {"sensor_delay_s": sensor_delay_s}}))
        self.thread = threading.Thread(target=self.run, daemon=True)
        self.thread.start()

    def run(self):
        try:
            conn, _ = self.listener.accept()
            with conn:
                conn.settimeout(5)
                seq = 0
                def send(kind, payload=b"", **fields):
                    nonlocal seq
                    header = json.dumps(dict(v=1, type=kind, seq=seq, **fields)).encode()
                    seq += 1
                    wire = struct.pack("!II", len(header), len(payload)) + header + payload
                    # Exercise real fragmented socket framing.
                    conn.sendall(wire[:3]); conn.sendall(wire[3:])
                send("hello", sdk_version="2.2.6", sdk_internal_loss_observable=False)
                start = receive(conn)
                assert start["type"] == "start" and start["stream"]["imu"] is False
                send("session", identity={"model": "synthetic GO3S"}, requested=start, readback={}, preview={})
                self.ready.set()
                offset = 0
                while True:
                    if select.select([conn], [], [], .005)[0]:
                        assert receive(conn)["type"] == "stop"
                        send("end", reason="stop")
                        break
                    try:
                        command, value = self.commands.get_nowait()
                    except queue.Empty:
                        continue
                    if command == "video":
                        for chunk in self.chunks[offset:offset + value]:
                            send("video", chunk, stream_index=0, codec=self.codec,
                                 camera_timestamp=123 + offset, phone_receive_ns=456 + offset)
                            self.transmitted += chunk
                            offset += 1
                    elif command == "error":
                        send("error", message=value)
                    elif command == "bad_video":
                        data = b"\x00\x00\x00\x01\x65\xff\xff\xff\xff\x00\x00\x01\x09\xf0"
                        send("video", data, stream_index=0, codec="h264", camera_timestamp=999, phone_receive_ns=999)
                        self.transmitted += data
                        send("exposure", camera_timestamp=1000, phone_receive_ns=1000, exposure_time_s=.004)
                    elif command == "end":
                        send("end", reason="stop")
                        break
        except BaseException as exc:
            self.error = exc
            self.ready.set()
        finally:
            self.listener.close()
            self.done.set()

    def send(self, count):
        self.commands.put(("video", count))

    def check(self):
        assert self.done.wait(8), "bridge thread did not finish"
        if self.error:
            raise self.error

    def session(self):
        return next(self.output.glob("go3s_*"))


def test_constructor_only_validates_and_no_hardware(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos, save=True)
    reader = Reader(config=str(bridge.config))
    assert not bridge.marker.exists() and not bridge.output.exists()
    assert reader.latest() is None and reader.running is False and reader.error is None
    with pytest.raises(StopIteration):
        reader.read(timeout=0)
    for timeout in (-1, float("inf"), float("nan")):
        with pytest.raises(ValueError):
            reader.read(timeout=timeout)
    for stream in (-1, 1):
        with pytest.raises(ValueError):
            reader.latest(stream_index=stream)
        with pytest.raises(ValueError):
            reader.read(timeout=0, stream_index=stream)
    reader.close(); reader.close()
    # Invalid config is rejected before touching ADB or making output directories.
    bridge.config.write_text('runtime:\n  duration_s: -1\n')
    with pytest.raises(RuntimeError, match="duration_s"):
        Reader(config=str(bridge.config))
    assert not bridge.marker.exists() and not bridge.output.exists()
    bridge.listener.close()


def test_latest_read_freshness_single_slot_and_ownership(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos)
    reader = Reader(str(bridge.config))
    assert reader.start() is reader and reader.start() is reader
    assert bridge.ready.wait(5)
    assert reader.latest() is None
    with pytest.raises(TimeoutError):
        reader.read(timeout=.02)
    bridge.send(1)
    first = reader.read(timeout=2)
    assert isinstance(first, Frame)
    assert first.index == 1 and first.stream_index == 0
    assert first.image.shape == (96, 160, 3) and first.image.dtype == np.uint8
    assert first.image.flags.writeable and first.image.flags.c_contiguous
    assert 0 < first.host_receive_ns <= first.host_decode_ns <= time.monotonic_ns()
    # The production scaler selects the same range/matrix as FFmpeg.
    assert np.max(np.abs(first.image.astype(int) - bridge.pixels[0].astype(int))) <= 4
    original = first.image.copy()
    first.image[:] = 0
    assert np.array_equal(reader.latest().image, original)
    with pytest.raises(TimeoutError):
        reader.read(timeout=0)
    bridge.send(5)
    until(lambda: reader.latest().index == 6)
    latest = reader.latest()
    assert latest.index == 6  # A slow reader skips the overwritten single slot.
    with pytest.raises(TimeoutError):
        reader.read(timeout=.02)  # latest advanced this Reader's shared cursor.
    bridge.send(1)
    assert reader.read(timeout=1).index == 7
    reader.stop(); reader.stop(); reader.close()
    assert not reader.running and reader.error is None
    assert np.all(first.image == 0) and np.array_equal(latest.image, bridge.pixels[5])
    with pytest.raises(StopIteration):
        reader.read(timeout=0)
    bridge.check()


@pytest.mark.parametrize("codec", ["h264", "h265"])
@pytest.mark.parametrize("sensor_delay_s", [0.0, 0.120])
def test_real_codecs_save_all_raw_data_and_normal_end(tmp_path, monkeypatch, videos, codec, sensor_delay_s):
    bridge = Bridge(tmp_path, monkeypatch, videos, codec=codec, save=True, sensor_delay_s=sensor_delay_s)
    reader = Reader(str(bridge.config)).start()
    assert bridge.ready.wait(5)
    bridge.send(12)
    bridge.commands.put(("end", None))
    until(lambda: not reader.running)
    final = reader.read(timeout=0)
    assert final.index == 12 and final.image.shape == (96, 160, 3)
    assert 0 <= final.host_receive_ns <= final.host_decode_ns <= time.monotonic_ns()
    expected = final.host_receive_ns - round(sensor_delay_s*1e9) if final.host_receive_ns else 0
    assert final.timestamp_ns == expected
    with pytest.raises(StopIteration):
        reader.read(timeout=0)
    assert reader.latest().index == 12 and reader.latest().timestamp_ns == expected
    reader.close()
    bridge.check()
    session = bridge.session()
    assert (session / ("video." + codec)).read_bytes() == bridge.video
    rows = list(csv.DictReader((session / "video_packets.csv").open()))
    assert len(rows) == 12 and sum(int(row["byte_length"]) for row in rows) == len(bridge.video)
    summary = json.loads((session / "summary.yaml").read_text())
    assert summary["complete"] and summary["decoded_frames"] == 12
    assert summary["accepted_events"] == summary["processed_events"]
    assert json.loads((session / "config_effective.yaml").read_text())["preview"]["enabled"] is False
    assert json.loads((session / "config_effective.yaml").read_text())["timing"]["sensor_delay_s"] == sensor_delay_s
    times = list(csv.DictReader((session / "frame_timing.csv").open()))
    assert len(times) == 12
    for row in times:
        received = int(row["host_receive_ns"])
        assert int(row["timestamp_ns"]) == (received - round(sensor_delay_s*1e9) if received else 0)


def test_wait_and_stop_release_gil_and_wake_reader(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos)
    reader = Reader(str(bridge.config)).start()
    assert bridge.ready.wait(5)
    bridge.send(1)
    reader.read(timeout=2)
    outcome = []
    entered = threading.Event()
    def waiting():
        entered.set()
        try:
            reader.read(timeout=None)
        except StopIteration:
            outcome.append("stopped")
    waiter = threading.Thread(target=waiting)
    waiter.start()
    assert entered.wait(1)
    # Main Python thread keeps running while read waits without the GIL.
    time.sleep(.03)
    reader.stop()  # Releases GIL so bridge Python thread can return end.
    waiter.join(1)
    assert outcome == ["stopped"] and not waiter.is_alive()
    bridge.check()


@pytest.mark.parametrize("timeout", [None, 1e30])
def test_threaded_read_receives_new_frame(tmp_path, monkeypatch, videos, timeout):
    bridge = Bridge(tmp_path, monkeypatch, videos)
    with Reader(str(bridge.config)) as reader:
        assert bridge.ready.wait(5)
        bridge.send(1)
        reader.read(timeout=2)
        received = []
        waiter = threading.Thread(target=lambda: received.append(reader.read(timeout=timeout)))
        waiter.start()
        time.sleep(.02)
        bridge.send(1)
        waiter.join(2)
        assert not waiter.is_alive() and received[0].index == 2
    bridge.check()


@pytest.mark.parametrize("fault", ["error", "bad_video"])
def test_background_error_surfaces_and_raw_drain(tmp_path, monkeypatch, videos, fault):
    bridge = Bridge(tmp_path, monkeypatch, videos, save=True)
    reader = Reader(str(bridge.config)).start()
    assert bridge.ready.wait(5)
    bridge.send(12)
    until(lambda: reader.latest() is not None and reader.latest().index == 12)
    bridge.commands.put((fault, "synthetic original failure"))
    until(lambda: not reader.running)
    expected = "synthetic original failure" if fault == "error" else "video decode failure"
    assert expected in reader.error
    with pytest.raises(RuntimeError, match=expected):
        reader.latest()
    with pytest.raises(RuntimeError, match=expected):
        reader.read(timeout=0)
    reader.stop(); reader.close()
    bridge.check()
    session = bridge.session()
    assert (session / "video.h264").read_bytes() == bridge.transmitted
    summary = json.loads((session / "summary.yaml").read_text())
    assert not summary["complete"] and summary["accepted_events"] == summary["processed_events"]
    if fault == "bad_video":
        assert summary["exposure_callbacks"] == 1


def test_duration_and_restart_reset_session(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos, duration=.3)
    reader = Reader(str(bridge.config))
    with reader as same:
        assert same is reader and bridge.ready.wait(5)
        bridge.send(12)
        until(lambda: not reader.running)
        assert reader.latest().index == 12
    bridge.check()
    # Reuse the identical config/port with a second mock phone session.
    listener = socket.socket()
    listener.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    listener.bind(("127.0.0.1", json.loads(bridge.config.read_text())["bridge"]["port"]))
    listener.listen(1)
    listener.settimeout(8)
    bridge.listener = listener
    bridge.ready.clear(); bridge.done.clear(); bridge.error = None
    bridge.thread = threading.Thread(target=bridge.run, daemon=True)
    bridge.thread.start()
    reader.start()
    assert reader.latest() is None and reader.error is None
    assert bridge.ready.wait(5)
    bridge.send(1)
    assert reader.read(timeout=2).index == 1
    reader.close()
    bridge.check()


def test_startup_error_and_restart_clear_error(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos)
    adb = tmp_path / "bin" / "adb"
    valid = adb.read_text()
    adb.write_text(f"#!{sys.executable}\nimport sys\nprint('fixture adb failed')\nsys.exit(1)\n")
    reader = Reader(str(bridge.config)).start()
    until(lambda: not reader.running)
    assert "fixture adb failed" in reader.error
    with pytest.raises(RuntimeError, match="fixture adb failed"):
        reader.latest()
    reader.stop()
    adb.write_text(valid)
    reader.start()
    assert reader.error is None and bridge.ready.wait(5)
    bridge.send(1)
    assert reader.read(timeout=2).index == 1
    reader.stop()
    bridge.check()


def test_stop_before_first_frame_is_clean_and_wakes_waiter(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos)
    reader = Reader(str(bridge.config)).start()
    assert bridge.ready.wait(5)
    reader.stop()
    assert reader.error is None and not reader.running
    with pytest.raises(StopIteration):
        reader.read(timeout=None)
    bridge.check()


def test_background_failure_wakes_blocked_read(tmp_path, monkeypatch, videos):
    bridge = Bridge(tmp_path, monkeypatch, videos)
    reader = Reader(str(bridge.config)).start()
    assert bridge.ready.wait(5)
    result = []
    def wait_for_error():
        try:
            reader.read(timeout=None)
        except RuntimeError as exc:
            result.append(str(exc))
    waiter = threading.Thread(target=wait_for_error)
    waiter.start()
    bridge.commands.put(("error", "failure while blocked"))
    waiter.join(3)
    assert not waiter.is_alive() and "failure while blocked" in result[0]
    reader.close()
    bridge.check()


def test_python_capture_preserves_stdin_and_signal_handlers(tmp_path, monkeypatch, videos):
    import signal
    bridge = Bridge(tmp_path, monkeypatch, videos)
    reader = Reader(str(bridge.config))
    previous = {kind: signal.getsignal(kind) for kind in (signal.SIGINT, signal.SIGTERM)}
    caught = []
    for kind in previous:
        signal.signal(kind, lambda number, _frame: caught.append(number))
    original_stdin = os.dup(0)
    pipe_read, pipe_write = os.pipe()
    try:
        reader.start()
        assert bridge.ready.wait(5)
        bridge.send(1)
        reader.read(timeout=2)
        os.write(pipe_write, b"q")
        os.dup2(pipe_read, 0)
        time.sleep(.1)
        assert reader.running
        assert select.select([pipe_read], [], [], 0)[0] and os.read(pipe_read, 1) == b"q"
        os.kill(os.getpid(), signal.SIGINT)
        os.kill(os.getpid(), signal.SIGTERM)
        assert caught == [signal.SIGINT, signal.SIGTERM]
    finally:
        os.dup2(original_stdin, 0)
        os.close(original_stdin); os.close(pipe_read); os.close(pipe_write)
        for kind, handler in previous.items():
            signal.signal(kind, handler)
        reader.close()
    bridge.check()


def test_destructor_stops_without_holding_gil(tmp_path, monkeypatch, videos):
    import gc
    bridge = Bridge(tmp_path, monkeypatch, videos)
    reader = Reader(str(bridge.config)).start()
    assert bridge.ready.wait(5)
    bridge.send(1)
    image = reader.read(timeout=2).image
    original = image.copy()
    del reader
    gc.collect()
    bridge.check()  # The Python mock bridge must be able to reply during deletion.
    assert np.array_equal(image, original)
