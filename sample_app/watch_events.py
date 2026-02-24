#!/usr/bin/env python3
"""
watch_events.py — Subscribe to ZMK events and print them.

Usage:
    python watch_events.py [socket_path]

Default socket path: /tmp/zmk_ipc.sock
Press Ctrl-C to stop.
"""

import sys
import time

from zmk_client import ZmkIpcClient, EVENTS_SOCK
from zmk_ipc_pb2 import ZmkEvent, TransportType


def transport_name(transport_value: int) -> str:
    try:
        return TransportType.Name(transport_value)
    except ValueError:
        return f"TRANSPORT({transport_value})"


def format_event(ev: ZmkEvent) -> str:
    which = ev.WhichOneof("payload")

    if which == "kscan_event":
        k = ev.kscan_event
        state = "PRESS  " if k.pressed else "RELEASE"
        return (
            f"[kscan   ] {state}  pos={k.position:<4}  "
            f"source={k.source}  ts={k.timestamp} ms"
        )

    if which == "keyboard":
        kb = ev.keyboard
        transport = transport_name(kb.endpoint.transport)
        key_bytes = list(kb.keys)
        pressed = [f"0x{b:02x}" for b in key_bytes if b != 0]
        return (
            f"[keyboard] transport={transport:<15}  "
            f"modifiers=0x{kb.modifiers:02x}  "
            f"keys=[{', '.join(pressed) if pressed else '-'}]"
        )

    if which == "consumer":
        cr = ev.consumer
        transport = transport_name(cr.endpoint.transport)
        pressed = [f"0x{b:02x}" for b in cr.keys if b != 0]
        return (
            f"[consumer] transport={transport:<15}  "
            f"keys=[{', '.join(pressed) if pressed else '-'}]"
        )

    if which == "mouse":
        mr = ev.mouse
        transport = transport_name(mr.endpoint.transport)
        return (
            f"[mouse   ] transport={transport:<15}  "
            f"buttons={mr.buttons}  dx={mr.dx}  dy={mr.dy}  "
            f"scroll_x={mr.scroll_x}  scroll_y={mr.scroll_y}"
        )

    return f"[unknown ] {ev}"


def main() -> None:
    events_path = sys.argv[1] if len(sys.argv) > 1 else EVENTS_SOCK

    client = ZmkIpcClient(events_path=events_path)

    print(f"Connecting to {events_path} ...")
    try:
        client.connect_output()
    except FileNotFoundError:
        print(
            f"Error: socket '{events_path}' not found.\n"
            "Make sure the ZMK native_sim process is running."
        )
        sys.exit(1)

    print("Connected. Watching for ZMK events (Ctrl-C to stop).\n")

    def on_error(exc: Exception) -> None:
        print(f"\nConnection closed: {exc}")

    watcher = client.watch_events(
        lambda ev: print(format_event(ev), flush=True),
        on_error=on_error,
    )

    try:
        watcher.join()
    except KeyboardInterrupt:
        print("\nStopped.")
    finally:
        client.close()


if __name__ == "__main__":
    main()
