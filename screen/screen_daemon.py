#!/usr/bin/env python3
"""
screen_daemon.py — OLED display daemon for Xoraya datalogger station.

Reads /tmp/xoraya-status.json (written by xoraya-cli) and journal output
(for databridge) to drive a 128x64 SSD1306 OLED. Two toggle switches on
GPIO 17/27 start and stop the collect and upload services.
"""

import os
import subprocess
import sys
import time
from enum import Enum, auto

from gpiozero import Button
from luma.core.interface.serial import i2c
from luma.oled.device import ssd1306

from status_reader import (
    read_xoraya_status,
    read_databridge_progress,
    is_service_active,
    get_ip_address,
    get_disk_free,
    get_uptime,
)
from renderer import (
    render_boot,
    render_idle_status,
    render_idle_system,
    render_downloading,
    render_uploading,
    render_done,
    render_error,
    render_sleep,
)

# ── Hardware ──────────────────────────────────────────────────────────────────
GPIO_SW1   = 17    # Download toggle switch
GPIO_SW2   = 27    # Upload toggle switch
I2C_PORT   = 1
I2C_ADDR   = 0x3C

# ── Timing (seconds) ─────────────────────────────────────────────────────────
BOOT_SECS   = 3
DONE_SECS   = 5
ERROR_SECS  = 8
SLEEP_SECS  = 60
CYCLE_SECS  = 8
POLL_SECS   = 1.0
NET_CHECK_INTERVAL = 10

# ── Services ──────────────────────────────────────────────────────────────────
SVC_COLLECT  = 'xoraya-collect'
SVC_UPLOAD   = 'databridge'
DEST_FOLDER  = os.environ.get('DEST', '/home/pi/Dexterlogs')


class State(Enum):
    BOOT        = auto()
    IDLE        = auto()
    SLEEP       = auto()
    DOWNLOADING = auto()
    UPLOADING   = auto()
    DONE        = auto()
    ERROR       = auto()


def count_local_mf4():
    try:
        return sum(1 for f in os.listdir(DEST_FOLDER) if f.endswith('.mf4'))
    except OSError:
        return 0


def check_net():
    r = subprocess.run(
        ['ping', '-c', '1', '-W', '1', '8.8.8.8'],
        capture_output=True,
    )
    return r.returncode == 0


def svc_start(name):
    subprocess.run(['systemctl', 'start', name], capture_output=True)


def svc_stop(name):
    subprocess.run(['systemctl', 'stop', name], capture_output=True)


def main():
    serial = i2c(port=I2C_PORT, address=I2C_ADDR)
    oled   = ssd1306(serial)

    sw1 = Button(GPIO_SW1, pull_up=True, bounce_time=0.05)
    sw2 = Button(GPIO_SW2, pull_up=True, bounce_time=0.05)

    state       = State.BOOT
    t_state     = time.monotonic()
    idle_page   = 0
    t_page      = time.monotonic()
    error_badge = None
    done_info   = {}
    net_ok      = False
    t_net       = 0.0
    prev_sw1    = sw1.is_pressed
    prev_sw2    = sw2.is_pressed

    def transition(new_state):
        nonlocal state, t_state, idle_page, t_page
        state   = new_state
        t_state = time.monotonic()
        idle_page = 0
        t_page  = time.monotonic()

    while True:
        now     = time.monotonic()
        elapsed = now - t_state

        # ── Network (every 10 s) ──────────────────────────────────────────────
        if now - t_net > NET_CHECK_INTERVAL:
            net_ok = check_net()
            t_net  = now

        # ── Switch transitions ────────────────────────────────────────────────
        cur_sw1 = sw1.is_pressed
        cur_sw2 = sw2.is_pressed
        sw1_rose  = cur_sw1 and not prev_sw1   # OFF -> ON
        sw1_fell  = not cur_sw1 and prev_sw1   # ON -> OFF
        sw2_rose  = cur_sw2 and not prev_sw2
        sw2_fell  = not cur_sw2 and prev_sw2
        prev_sw1  = cur_sw1
        prev_sw2  = cur_sw2

        # ── State machine ─────────────────────────────────────────────────────

        if state == State.BOOT:
            oled.display(render_boot())
            if elapsed >= BOOT_SECS:
                transition(State.IDLE)

        elif state == State.SLEEP:
            oled.display(render_sleep())
            if sw1_rose:
                svc_start(SVC_COLLECT)
                transition(State.DOWNLOADING)
            elif sw1_fell or sw2_fell:
                transition(State.IDLE)
            elif sw2_rose:
                svc_start(SVC_UPLOAD)
                transition(State.UPLOADING)

        elif state == State.IDLE:
            # Auto-sleep
            if elapsed >= SLEEP_SECS:
                transition(State.SLEEP)
                continue

            # Cycle pages
            if now - t_page >= CYCLE_SECS:
                idle_page = 1 - idle_page
                t_page = now

            status = read_xoraya_status()
            device = status.get('device', '') if status else ''

            if idle_page == 0:
                oled.display(render_idle_status(
                    device=device,
                    net_ok=net_ok,
                    logger_files=status.get('files_total', 0) if status else 0,
                    local_files=count_local_mf4(),
                    last_upload=done_info.get('last_upload', 'never'),
                    sw1=cur_sw1,
                    sw2=cur_sw2,
                    error_badge=error_badge,
                ))
            else:
                oled.display(render_idle_system(
                    ip=get_ip_address(),
                    disk_free=get_disk_free(),
                    uptime=get_uptime(),
                    sw1=cur_sw1,
                    sw2=cur_sw2,
                ))

            if sw1_rose:
                svc_start(SVC_COLLECT)
                transition(State.DOWNLOADING)
                continue
            if sw2_rose:
                svc_start(SVC_UPLOAD)
                transition(State.UPLOADING)
                continue

        elif state == State.DOWNLOADING:
            status   = read_xoraya_status()
            scanning = (status is None) or (status.get('state') == 'scanning')

            oled.display(render_downloading(
                device=status.get('device', '') if status else '',
                file_idx=status.get('files_done', 0) if status else 0,
                total=status.get('files_total', 0) if status else 0,
                pct=status.get('pct', 0.0) if status else 0.0,
                speed_mbps=status.get('speed_mbps', 0.0) if status else 0.0,
                eta_s=status.get('eta_s', 0) if status else 0,
                sw1=cur_sw1,
                sw2=cur_sw2,
                scanning=scanning,
            ))

            if status and status.get('state') == 'done':
                done_info = {
                    'type': 'download',
                    'files': status.get('files_done', 0),
                    'bytes': status.get('bytes_total', 0),
                    'last_upload': done_info.get('last_upload', 'never'),
                }
                error_badge = None
                transition(State.DONE)
            elif status and status.get('state') == 'error':
                error_badge = status.get('msg', 'error')[:20]
                done_info['type'] = 'download'
                transition(State.ERROR)
            elif not is_service_active(SVC_COLLECT) and status is None:
                transition(State.IDLE)

            if sw1_fell:
                svc_stop(SVC_COLLECT)
                transition(State.IDLE)

        elif state == State.UPLOADING:
            db = read_databridge_progress()

            uploaded = db['uploaded'] if db else 0
            skipped  = db['skipped']  if db else 0
            failed   = db['failed']   if db else 0
            total    = uploaded + skipped + failed

            oled.display(render_uploading(
                uploaded=uploaded,
                total=total,
                skipped=skipped,
                failed=failed,
                sw1=cur_sw1,
                sw2=cur_sw2,
            ))

            if not is_service_active(SVC_UPLOAD):
                if db is not None and db['failed'] == 0:
                    done_info = {
                        'type': 'upload',
                        'uploaded': uploaded,
                        'failed': failed,
                        'last_upload': time.strftime('%H:%M'),
                    }
                    error_badge = None
                    transition(State.DONE)
                else:
                    if db:
                        error_badge = f"{failed} failed"
                    done_info['type'] = 'upload'
                    transition(State.ERROR)

            if sw2_fell:
                svc_stop(SVC_UPLOAD)
                transition(State.IDLE)

        elif state == State.DONE:
            if done_info.get('type') == 'download':
                files = done_info.get('files', 0)
                mb    = done_info.get('bytes', 0) / 1e6
                oled.display(render_done(
                    'Download complete',
                    f'Files: {files}',
                    f'{mb:.0f} MB' if mb > 0 else '',
                ))
            else:
                oled.display(render_done(
                    'Upload complete',
                    f"Uploaded: {done_info.get('uploaded', 0)}",
                    f"Failed: {done_info.get('failed', 0)}",
                ))
            if elapsed >= DONE_SECS:
                transition(State.IDLE)

        elif state == State.ERROR:
            svc = 'journalctl -u xoraya' if done_info.get('type') == 'download' \
                  else 'journalctl -u databridge'
            title = 'Download failed' if done_info.get('type') == 'download' \
                    else 'Upload failed'
            oled.display(render_error(title, error_badge or '', svc))
            if elapsed >= ERROR_SECS:
                transition(State.IDLE)

        time.sleep(POLL_SECS)


if __name__ == '__main__':
    main()
