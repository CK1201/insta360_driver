"""Pull native dual-fisheye BGR frames; see commands.md for installation and use."""
import argparse
from insta360_x5 import Reader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="config/x5.yaml")
    parser.add_argument("--frames", type=int, default=300)
    args = parser.parse_args()
    if args.frames <= 0:
        parser.error("--frames must be > 0")
    with Reader(config=args.config) as camera:
        count = 0
        while count < args.frames:
            try:
                frame = camera.read(timeout=1.0)
            except TimeoutError:
                continue
            except StopIteration:
                break
            count += 1
            if count == 1 or count % 30 == 0:
                print(frame.index, frame.stream_index, frame.image.shape,
                      frame.timestamp_ns, frame.host_receive_ns, frame.host_decode_ns)
            # frame.image is writable uint8 BGR. Each half preserves one lens.
            # RGB consumers can use frame.image[:, :, ::-1].copy().
    if camera.error:
        raise RuntimeError(camera.error)


if __name__ == "__main__":
    try:
        main()
    except KeyboardInterrupt:
        pass
