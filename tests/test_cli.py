#!/usr/bin/env python3
"""Exercise the real command line without a camera or output side effects."""
import pathlib
import re
import subprocess
import sys
import tempfile


def main():
    executable = pathlib.Path(sys.argv[1]).resolve()
    config = pathlib.Path(sys.argv[2]).resolve()

    def run(*args):
        return subprocess.run([str(executable), *args], text=True, capture_output=True, timeout=10)

    result = run("--help")
    assert result.returncode == 0 and "--check-config" in result.stdout, result
    result = run("--config", str(config), "--check-config")
    assert result.returncode == 0 and re.search(r"(?m)^  resolution: \d+$", result.stdout), result
    assert "usb" not in result.stderr.lower(), result
    assert str(config.parent.parent / "outputs") in result.stdout, result
    with tempfile.TemporaryDirectory(prefix="x5-cli-") as directory:
        root = pathlib.Path(directory)
        bad = root / "bad.yaml"
        bad.write_text("camera:\n  exposure:\n    mode: manual\noutput:\n  directory: should-not-exist\n")
        result = run("--config", str(bad), "--check-config")
        assert result.returncode != 0 and "camera.exposure" in result.stderr, result
        assert not (root / "should-not-exist").exists()
        fixed_fps = root / "fixed-fps.yaml"
        for fps in [20, 24, 25, 60, 120]:
            fixed_fps.write_text(f"stream: {{resolution: 1280, fps: {fps}}}\noutput: {{directory: should-not-exist}}\n")
            result = run("--config", str(fixed_fps), "--check-config")
            assert result.returncode != 0 and "stream.fps" in result.stderr and "30" in result.stderr, result
            assert not (root / "should-not-exist").exists()
        result = run("--config", str(root / "absent.yaml"), "--check-config")
        assert result.returncode != 0 and "absent.yaml" in result.stderr, result
    for duration in ["-1", "nan", "1oops"]:
        result = run("--config", str(config), "--duration", duration, "--check-config")
        assert result.returncode != 0, (duration, result)
    result = run("--config")
    assert result.returncode != 0, result
    result = run("--unknown")
    assert result.returncode != 0, result
    result = run("--config", str(config), "--duration", "2.5", "--check-config")
    assert result.returncode == 0 and "2.5" in result.stdout, result
    print("PASS: help, offline config validation, relative paths, duration override and CLI errors")


if __name__ == "__main__":
    main()
