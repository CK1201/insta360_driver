#!/usr/bin/env python3
"""Real encoded geometry transitions, including a correct final/latest frame."""
import pathlib
import subprocess
import sys
import tempfile

root = pathlib.Path(sys.argv[2]).resolve()
root.mkdir(parents=True, exist_ok=True)
run = pathlib.Path(tempfile.mkdtemp(prefix="geometry-", dir=root))
for name, size in [("good", "160x80"), ("wrong", "128x64")]:
    subprocess.run(["ffmpeg", "-hide_banner", "-loglevel", "error", "-f", "lavfi",
                    "-i", f"color=c=red:size={size}:rate=30", "-frames:v", "1", "-c:v", "libx264",
                    "-bf", "0", "-threads", "1", "-pix_fmt", "yuv420p", "-f", "h264",
                    str(run / f"{name}.h264")], check=True, timeout=20)
subprocess.run([str(pathlib.Path(sys.argv[1]).resolve()), str(run / "good.h264"),
                str(run / "wrong.h264"), str(run / "output")], check=True, timeout=20)
