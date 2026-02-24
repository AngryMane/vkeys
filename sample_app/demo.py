#!/usr/bin/env python3
"""
demo.py — Interactive ZMK IPC demo.

Connects to both sockets, watches ZMK events in a background thread,
then lets you trigger key presses interactively.

Usage:
    python demo.py

Press Ctrl-C to quit.
"""

import sys
import time
import threading

from zmk_client import ZmkIpcClient
from watch_events import format_event


COLUMNS = 12   # must match the DTS `columns` property


def pos_to_rc(pos: int) -> tuple[int, int]:
    return pos // COLUMNS, pos % COLUMNS


def print_help() -> None:
    print(
        "\nCommands:\n"
        "  <position>      press+release key at linear position (0-based)\n"
        "  r <row> <col>   press+release key at explicit row/col\n"
        "  h               show this help\n"
        "  q               quit\n"
    )


def main() -> None:
    client = ZmkIpcClient()

    print("Connecting to ZMK IPC sockets...")
    errors: list[str] = []
    try:
        client.connect_input()
    except FileNotFoundError as e:
        errors.append(f"  Input  ({e.filename}): not found")

    try:
        client.connect_output()
    except FileNotFoundError as e:
        errors.append(f"  Output ({e.filename}): not found")

    if errors:
        print("Connection error(s):")
        for msg in errors:
            print(msg)
        print("Make sure the ZMK native_sim process is running.")
        client.close()
        sys.exit(1)

    print("Connected to both sockets.")

    # ----------------------------------------------------------------
    # Background event watcher
    # ----------------------------------------------------------------
    event_lock = threading.Lock()

    def on_event(ev) -> None:
        with event_lock:
            # Move to beginning of line, print event, restore prompt
            print(f"\r{format_event(ev)}")
            print("> ", end="", flush=True)

    def on_error(exc: Exception) -> None:
        print(f"\nEvent stream closed: {exc}")

    client.watch_events(on_event, on_error=on_error)

    # ----------------------------------------------------------------
    # Interactive prompt
    # ----------------------------------------------------------------
    print_help()

    try:
        while True:
            try:
                line = input("> ").strip()
            except EOFError:
                break

            if not line:
                continue

            if line in ("q", "quit", "exit"):
                break

            if line in ("h", "help", "?"):
                print_help()
                continue

            parts = line.split()

            # r <row> <col>
            if parts[0] == "r" and len(parts) == 3:
                try:
                    row, col = int(parts[1]), int(parts[2])
                except ValueError:
                    print("Usage: r <row> <col>  (integers)")
                    continue
                print(f"  PRESS   row={row} col={col}")
                client.send_key_press_rc(row, col)
                time.sleep(0.05)
                print(f"  RELEASE row={row} col={col}")
                client.send_key_release_rc(row, col)
                continue

            # <position>
            try:
                pos = int(parts[0])
            except ValueError:
                print(f"Unknown command: {line!r}  (type 'h' for help)")
                continue

            row, col = pos_to_rc(pos)
            print(f"  PRESS   position={pos} (row={row}, col={col})")
            client.send_key_press(pos)
            time.sleep(0.05)
            print(f"  RELEASE position={pos}")
            client.send_key_release(pos)

    except KeyboardInterrupt:
        pass

    print("\nClosing connections.")
    client.close()


if __name__ == "__main__":
    main()
