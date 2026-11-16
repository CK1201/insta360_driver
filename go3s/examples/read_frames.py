"""Consume fresh GO3S frames without a GUI; YAML controls save and duration."""
import argparse
from insta360_go3s import Reader


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--config", default="config/go3s.yaml")
    args = parser.parse_args()
    try:
        with Reader(config=args.config) as reader:
            while True:
                try:
                    frame = reader.read(timeout=1.0)
                except TimeoutError:
                    continue
                except StopIteration:
                    break
                print(frame.index, frame.image.shape, frame.timestamp_ns, frame.host_receive_ns, frame.host_decode_ns)
    except KeyboardInterrupt:
        pass  # The context manager stops and drains capture before exiting.


if __name__ == "__main__":
    main()
