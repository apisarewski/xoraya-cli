#!/usr/bin/env python3
"""
screen_daemon.py — OLED display daemon for Xoraya datalogger station.

Reads /tmp/xoraya-status.json (written by xoraya-cli) to drive a 128x64
SSD1306 OLED. One toggle switch on GPIO 27 starts and stops the collect
service.
"""

import subprocess
import time
from enum import Enum, auto

from gpiozero import Button
from luma.core.interface.serial import i2c
from luma.oled.device import ssd1306

from status_reader import (
    read_xoraya_status,
    is_service_active,
    is_storage_detected,
)
from renderer import (
    render_idle,
    render_downloading,
    render_done,
    render_error,
)

# ── Hardware ──────────────────────────────────────────────────────────────────
GPIO_SW1   = 27    # Download toggle switch (physical pin 13)
I2C_PORT   = 1
I2C_ADDR   = 0x3C

# ── Timing (seconds) ─────────────────────────────────────────────────────────
DONE_SECS   = 5
ERROR_SECS  = 8
POLL_SECS   = 1.0
STORAGE_CHECK_INTERVAL_ABSENT  = 1   # poll fast so a newly-plugged drive shows up quickly
STORAGE_CHECK_INTERVAL_PRESENT = 3   # once detected, no need to hammer it every tick

# ── Services ──────────────────────────────────────────────────────────────────
SVC_COLLECT  = 'xoraya-collect'


class State(Enum):
    IDLE        = auto()
    DOWNLOADING = auto()
    DONE        = auto()
    ERROR       = auto()


def svc_start(name):
    # Managing units over D-Bus needs polkit authorization, which is only
    # granted for free outside an active graphical session with pi's
    # passwordless sudo — plain `systemctl start/stop` here silently no-ops
    # with "Interactive authentication required" and capture_output hides it.
    subprocess.run(['sudo', 'systemctl', 'start', name], capture_output=True)


def svc_stop(name):
    subprocess.run(['sudo', 'systemctl', 'stop', name], capture_output=True)


def main():
    serial = i2c(port=I2C_PORT, address=I2C_ADDR)
    oled   = ssd1306(serial)

    sw1 = Button(GPIO_SW1, pull_up=True, bounce_time=0.05)

    error_badge = None
    done_info   = {}
    storage_ok  = False
    t_storage   = 0.0

    # Sync to the physical switch instead of always assuming IDLE: a daemon
    # restart must not leave collect running unsupervised (switch OFF but a
    # previous run's process still active) or leave a download unattended
    # (switch already ON at startup).
    prev_sw1 = sw1.is_pressed
    if prev_sw1:
        svc_start(SVC_COLLECT)
        state = State.DOWNLOADING
    else:
        svc_stop(SVC_COLLECT)
        state = State.IDLE
    t_state = time.monotonic()

    def transition(new_state):
        nonlocal state, t_state
        state   = new_state
        t_state = time.monotonic()

    while True:
        now     = time.monotonic()
        elapsed = now - t_state

        # ── Storage ────────────────────────────────────────────────────────────
        interval = STORAGE_CHECK_INTERVAL_PRESENT if storage_ok else STORAGE_CHECK_INTERVAL_ABSENT
        if now - t_storage > interval:
            storage_ok = is_storage_detected()
            t_storage  = now

        # ── Switch transitions ────────────────────────────────────────────────
        cur_sw1  = sw1.is_pressed
        sw1_rose = cur_sw1 and not prev_sw1   # OFF -> ON
        sw1_fell = not cur_sw1 and prev_sw1   # ON -> OFF
        prev_sw1 = cur_sw1

        # ── State machine ─────────────────────────────────────────────────────

        if state == State.IDLE:
            oled.display(render_idle(storage_ok))

            if sw1_rose:
                svc_start(SVC_COLLECT)
                transition(State.DOWNLOADING)
                continue

        elif state == State.DOWNLOADING:
            status   = read_xoraya_status()
            scanning = (status is None) or (status.get('state') == 'scanning')

            oled.display(render_downloading(
                storage_ok=storage_ok,
                file_idx=status.get('files_done', 0) if status else 0,
                total=status.get('files_total', 0) if status else 0,
                pct=status.get('pct', 0.0) if status else 0.0,
                speed_mbps=status.get('speed_mbps', 0.0) if status else 0.0,
                eta_s=status.get('eta_s', 0) if status else 0,
                scanning=scanning,
            ))

            if status and status.get('state') == 'done':
                done_info = {
                    'files': status.get('files_done', 0),
                    'bytes': status.get('bytes_total', 0),
                }
                error_badge = None
                transition(State.DONE)
            elif status and status.get('state') == 'error':
                error_badge = status.get('msg', 'error')[:20]
                transition(State.ERROR)
            elif not storage_ok:
                # collect's --interval loop resolves dest_dir once at startup and
                # never re-checks it — it would happily keep scanning forever
                # even after the drive is pulled. Catch that here instead.
                svc_stop(SVC_COLLECT)
                error_badge = 'drive removed'
                transition(State.ERROR)
            elif not is_service_active(SVC_COLLECT) and status is None:
                transition(State.IDLE)

            if sw1_fell:
                svc_stop(SVC_COLLECT)
                transition(State.IDLE)

        elif state == State.DONE:
            files = done_info.get('files', 0)
            mb    = done_info.get('bytes', 0) / 1e6
            oled.display(render_done(
                'Download complete',
                f'Files: {files}',
                f'{mb:.0f} MB' if mb > 0 else '',
            ))
            if elapsed >= DONE_SECS:
                transition(State.IDLE)

        elif state == State.ERROR:
            oled.display(render_error(
                'Download failed', error_badge or '', 'journalctl -u xoraya-collect',
            ))
            if elapsed >= ERROR_SECS:
                transition(State.IDLE)

        time.sleep(POLL_SECS)


if __name__ == '__main__':
    main()
