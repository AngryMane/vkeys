#!/usr/bin/env python3
"""
send_keys.py — Send a sequence of key press/release events to ZMK.

Usage:
    python send_keys.py [position ...]

Each positional argument is a linear key-matrix position (integer).
Each key is pressed for 50 ms, with 200 ms between keys.

Examples:
    # Press position 0 (row=0, col=0) once
    python send_keys.py 0

    # Type three keys at positions 0, 1, 2
    python send_keys.py 0 1 2

The DTS for native_sim/native/zmk_ipc defines a 4-row × 12-column matrix,
so positions run from 0 to 47 (position = row * 12 + col).
"""

import sys
import time

from zmk_client import ZmkIpcClient, KSCAN_SOCK

PRESS_DURATION = 0.05   # seconds a key is held
KEY_INTERVAL   = 0.20   # seconds between successive keys


def main() -> None:
    if len(sys.argv) < 2:
        print(__doc__)
        sys.exit(0)

    try:
        positions = [int(a) for a in sys.argv[1:]]
    except ValueError:
        print("Error: all arguments must be integers (key matrix positions)")
        sys.exit(1)

    client = ZmkIpcClient(kscan_path=KSCAN_SOCK)

    print(f"Connecting to {KSCAN_SOCK} ...")
    try:
        client.connect_input()
    except FileNotFoundError:
        print(
            f"Error: socket '{KSCAN_SOCK}' not found.\n"
            "Make sure the ZMK native_sim process is running."
        )
        sys.exit(1)

    print(f"Connected. Sending {len(positions)} key event(s).\n")

    try:
        for pos in positions:
            print(f"  PRESS   position={pos}")
            client.send_key_press(pos)
            time.sleep(PRESS_DURATION)

            print(f"  RELEASE position={pos}")
            client.send_key_release(pos)
            time.sleep(KEY_INTERVAL)
    except KeyboardInterrupt:
        pass
    finally:
        client.close()

    print("Done.")


if __name__ == "__main__":
    main()
