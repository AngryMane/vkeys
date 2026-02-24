"""
ZMK IPC Client — Unix socket transport adapter.

Wire format (both directions):
    [4-byte big-endian length][protobuf-encoded bytes]

Two sockets:
    KSCAN_SOCK  (/tmp/zmk_kscan_ipc.sock)  client → ZMK  (ClientMessage)
    EVENTS_SOCK (/tmp/zmk_ipc.sock)        ZMK → client  (ZmkEvent)

Typical usage::

    client = ZmkIpcClient()
    client.connect()

    # background event loop
    client.watch_events(lambda ev: print(format_event(ev)))

    # send a key press / release
    client.send_key_press(position=0)
    time.sleep(0.05)
    client.send_key_release(position=0)

    client.close()
"""

import socket
import struct
import threading
from typing import Callable, Optional

from zmk_ipc_pb2 import ClientMessage, KeyEvent, ZmkEvent

KSCAN_SOCK = "/tmp/zmk_kscan_ipc.sock"
EVENTS_SOCK = "/tmp/zmk_ipc.sock"


# ---------------------------------------------------------------------------
# Low-level framing helpers
# ---------------------------------------------------------------------------

def _recv_exact(sock: socket.socket, n: int) -> bytes:
    """Read exactly *n* bytes from *sock*, blocking until done."""
    buf = bytearray()
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("socket closed before all bytes were received")
        buf.extend(chunk)
    return bytes(buf)


def _send_frame(sock: socket.socket, data: bytes) -> None:
    """Send *data* with a 4-byte big-endian length prefix."""
    sock.sendall(struct.pack(">I", len(data)) + data)


def _recv_frame(sock: socket.socket) -> bytes:
    """Receive one length-prefixed frame and return the payload bytes."""
    header = _recv_exact(sock, 4)
    length = struct.unpack(">I", header)[0]
    return _recv_exact(sock, length)


# ---------------------------------------------------------------------------
# High-level client
# ---------------------------------------------------------------------------

class ZmkIpcClient:
    """Client for the ZMK native_sim IPC interface.

    Parameters
    ----------
    kscan_path:
        Unix socket path for the kscan IPC driver (input to ZMK).
    events_path:
        Unix socket path for the IPC observer (output from ZMK).
    """

    def __init__(
        self,
        kscan_path: str = KSCAN_SOCK,
        events_path: str = EVENTS_SOCK,
    ) -> None:
        self._kscan_path = kscan_path
        self._events_path = events_path
        self._kscan_sock: Optional[socket.socket] = None
        self._events_sock: Optional[socket.socket] = None

    # ------------------------------------------------------------------
    # Connection management
    # ------------------------------------------------------------------

    def connect_input(self) -> None:
        """Connect to the kscan IPC socket (client → ZMK key events)."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self._kscan_path)
        self._kscan_sock = s

    def connect_output(self) -> None:
        """Connect to the IPC observer socket (ZMK → client events)."""
        s = socket.socket(socket.AF_UNIX, socket.SOCK_STREAM)
        s.connect(self._events_path)
        self._events_sock = s

    def connect(self) -> None:
        """Connect to both sockets."""
        self.connect_input()
        self.connect_output()

    def close(self) -> None:
        """Close all open sockets."""
        for sock in (self._kscan_sock, self._events_sock):
            if sock:
                try:
                    sock.close()
                except OSError:
                    pass
        self._kscan_sock = None
        self._events_sock = None

    def __enter__(self) -> "ZmkIpcClient":
        self.connect()
        return self

    def __exit__(self, *_) -> None:
        self.close()

    # ------------------------------------------------------------------
    # Sending key events  (client → ZMK)
    # ------------------------------------------------------------------

    def send_key_press(self, position: int) -> None:
        """Inject a key-press event at the given linear matrix position."""
        self._send_key_event(position, KeyEvent.PRESS)

    def send_key_release(self, position: int) -> None:
        """Inject a key-release event at the given linear matrix position."""
        self._send_key_event(position, KeyEvent.RELEASE)

    def send_key_press_rc(self, row: int, col: int) -> None:
        """Inject a key-press event using explicit row/col addressing."""
        self._send_key_event_rc(row, col, KeyEvent.PRESS)

    def send_key_release_rc(self, row: int, col: int) -> None:
        """Inject a key-release event using explicit row/col addressing."""
        self._send_key_event_rc(row, col, KeyEvent.RELEASE)

    def _send_key_event(self, position: int, action: int) -> None:
        if self._kscan_sock is None:
            raise RuntimeError("input socket not connected; call connect_input() first")
        msg = ClientMessage(key_event=KeyEvent(action=action, position=position))
        _send_frame(self._kscan_sock, msg.SerializeToString())

    def _send_key_event_rc(self, row: int, col: int, action: int) -> None:
        from zmk_ipc_pb2 import KeyPosition
        if self._kscan_sock is None:
            raise RuntimeError("input socket not connected; call connect_input() first")
        msg = ClientMessage(
            key_event=KeyEvent(action=action, key_pos=KeyPosition(row=row, col=col))
        )
        _send_frame(self._kscan_sock, msg.SerializeToString())

    # ------------------------------------------------------------------
    # Receiving ZMK events  (ZMK → client)
    # ------------------------------------------------------------------

    def recv_event(self) -> ZmkEvent:
        """Block until one ZmkEvent is received and return it."""
        if self._events_sock is None:
            raise RuntimeError("output socket not connected; call connect_output() first")
        data = _recv_frame(self._events_sock)
        ev = ZmkEvent()
        ev.ParseFromString(data)
        return ev

    def watch_events(
        self,
        callback: Callable[[ZmkEvent], None],
        *,
        on_error: Optional[Callable[[Exception], None]] = None,
    ) -> threading.Thread:
        """Start a daemon thread that calls *callback* for every ZmkEvent.

        Parameters
        ----------
        callback:
            Called in the background thread for each received event.
        on_error:
            Optional error handler called when the loop terminates.
            If omitted, errors are silently ignored.

        Returns
        -------
        The started :class:`threading.Thread`.
        """
        def _run() -> None:
            while True:
                try:
                    ev = self.recv_event()
                    callback(ev)
                except Exception as exc:
                    if on_error:
                        on_error(exc)
                    break

        t = threading.Thread(target=_run, daemon=True, name="zmk-event-watcher")
        t.start()
        return t
