from __future__ import annotations

import glob
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Callable, Optional


USB_BAUD = 115200
USB_PUSH_INTERVAL_SEC = 1.5
USB_HEARTBEAT_SEC = 5.0
USB_RETRY_SEC = 2.0
USB_PREFIX = "SS1 "
# Touch this file to make the pusher release the serial port (e.g. during firmware upload).
USB_PAUSE_FLAG = Path.home() / ".cursor" / "subscreen" / "usb_pause"

# termios baud map (macOS / BSD)
_BAUD_MAP = {
    9600: None,
    115200: None,
}


def _usb_paused() -> bool:
    return USB_PAUSE_FLAG.exists()


def _find_usbmodem_ports() -> list[str]:
    ports = sorted(glob.glob("/dev/cu.usbmodem*"))
    return [p for p in ports if "Bluetooth" not in p]


def _configure_raw_serial(fd: int, baud: int = USB_BAUD) -> None:
    """
    Configure the USB-Serial/JTAG device WITHOUT touching DTR/RTS.

    pyserial's open() asserts modem lines and puts ESP32-C6 into download mode
    (boot:0x67 waiting for download) — which leaves the LCD on snow with no UI.
    """
    import termios

    attrs = termios.tcgetattr(fd)
    iflag, oflag, cflag, lflag, ispeed, ospeed, cc = attrs

    # Raw mode.
    iflag = 0
    oflag = 0
    lflag = 0

    cflag |= termios.CREAD | termios.CLOCAL
    cflag &= ~termios.CSIZE
    cflag |= termios.CS8
    cflag &= ~(termios.PARENB | termios.CSTOPB)
    # Critical: do not drop lines / toggle modem control on close.
    cflag &= ~getattr(termios, "HUPCL", 0)
    cflag &= ~getattr(termios, "CRTSCTS", 0)

    try:
        import termios as t

        speed = getattr(t, f"B{baud}")
        ispeed = speed
        ospeed = speed
    except AttributeError:
        # Fall back — 115200 is always present on macOS.
        ispeed = termios.B115200
        ospeed = termios.B115200

    cc[termios.VMIN] = 0
    cc[termios.VTIME] = 0

    termios.tcsetattr(fd, termios.TCSANOW, [iflag, oflag, cflag, lflag, ispeed, ospeed, cc])
    try:
        termios.tcflush(fd, termios.TCIOFLUSH)
    except Exception:
        pass


def _open_serial_fd(port: str) -> int:
    fd = os.open(port, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    try:
        _configure_raw_serial(fd, USB_BAUD)
    except Exception:
        os.close(fd)
        raise
    return fd


def _write_all(fd: int, data: bytes) -> None:
    view = memoryview(data)
    while len(view):
        try:
            n = os.write(fd, view)
        except BlockingIOError:
            time.sleep(0.01)
            continue
        if n <= 0:
            raise OSError("serial write returned 0")
        view = view[n:]


class UsbStatusPusher:
    """Push Bridge /status snapshots to the ESP32 over Type-C USB serial."""

    def __init__(self, snapshot: Callable[[], dict[str, Any]]) -> None:
        self._snapshot = snapshot
        self._stop = threading.Event()
        self._thread: Optional[threading.Thread] = None
        self._port: Optional[str] = None

    def start(self) -> None:
        if self._thread and self._thread.is_alive():
            return
        self._stop.clear()
        self._thread = threading.Thread(target=self._run, name="subscreen-usb", daemon=True)
        self._thread.start()

    def stop(self) -> None:
        self._stop.set()

    def _run(self) -> None:
        while not self._stop.is_set():
            if _usb_paused():
                self._port = None
                self._stop.wait(USB_RETRY_SEC)
                continue

            ports = _find_usbmodem_ports()
            if not ports:
                self._port = None
                self._stop.wait(USB_RETRY_SEC)
                continue

            port = ports[0]
            fd = -1
            try:
                fd = _open_serial_fd(port)
            except Exception:
                self._port = None
                self._stop.wait(USB_RETRY_SEC)
                continue

            self._port = port
            last_payload = ""
            last_sent_at = 0.0
            try:
                while not self._stop.is_set():
                    if _usb_paused():
                        break
                    if not glob.glob(port):
                        break
                    try:
                        payload = json.dumps(
                            self._snapshot(), separators=(",", ":"), ensure_ascii=False
                        )
                        now = time.monotonic()
                        changed = payload != last_payload
                        due_heartbeat = (now - last_sent_at) >= USB_HEARTBEAT_SEC
                        if changed or due_heartbeat or last_sent_at == 0.0:
                            _write_all(fd, (USB_PREFIX + payload + "\n").encode("utf-8"))
                            last_payload = payload
                            last_sent_at = now
                    except OSError:
                        break
                    self._stop.wait(USB_PUSH_INTERVAL_SEC)
            finally:
                if fd >= 0:
                    try:
                        os.close(fd)
                    except Exception:
                        pass
                self._port = None
                self._stop.wait(USB_RETRY_SEC)
