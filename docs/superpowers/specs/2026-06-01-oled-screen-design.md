# OLED Screen + Toggle Switch Interface

**Date:** 2026-06-01
**Status:** Approved — ready for implementation

---

## Overview

Add a 128×64 SSD1306 OLED display and two toggle flip switches to the Raspberry Pi datalogger station. The screen shows real-time status of the download and upload services. The switches directly start and stop those services.

This is a new standalone Python daemon (`xoraya-screen`) plus a small C++ addition to `xoraya-cli` for progress data. Existing services (`xoraya-collect`, `databridge`) are otherwise unchanged.

---

## Hardware

| Component | Spec |
|-----------|------|
| Display | SSD1306 — 128×64 monochrome OLED |
| Interface | I2C bus 1, address `0x3C` |
| SDA / SCL | GPIO 2 / GPIO 3 (Pi default I2C-1) |
| Switch 1 (DOWNLOAD) | GPIO 17 — internal pull-up |
| Switch 2 (UPLOAD) | GPIO 27 — internal pull-up |

Switch type: **toggle flip switches** (stay in position). ON = start service. OFF = stop service.

---

## Architecture

Three systemd services on the Pi. One shared status file. No complex IPC.

```
xoraya-collect ──writes──► /tmp/xoraya-status.json ──reads──► xoraya-screen
                                                                     │
databridge ──────journal──────────────────────────────────────────────┤
                                                                     │
GPIO 17, 27 ─────────────────────────────────────────────────────────┘
                                                                     │
                                                              SSD1306 OLED
```

**xoraya-collect** (existing, minor C++ addition):
- `StatusWriter` class writes `/tmp/xoraya-status.json` during scan and download.
- File is deleted on service exit.

**databridge** (existing, unmodified):
- Pre-built x86-64 binary — cannot be modified.
- Screen daemon reads upload progress from `journalctl -u databridge -n 20`.

**xoraya-screen** (new Python daemon):
- Reads status JSON every second.
- Polls journal and `systemctl is-active` for service state.
- Renders appropriate screen to SSD1306 via `luma.oled`.
- Watches GPIO 17 and 27 via `gpiozero` for switch state changes.
- Triggers `systemctl start/stop` on switch transitions.

---

## Status JSON Schema

Written by `StatusWriter` to `/tmp/xoraya-status.json` (atomic: write to `.tmp` then `rename`).

```json
{
  "state":         "downloading",
  "device":        "DX11246-2",
  "files_done":    3,
  "files_total":   7,
  "pct":           42,
  "speed_mbps":    1.2,
  "eta_s":         45,
  "updated_at":    1780322253
}
```

`state` values: `idle` | `scanning` | `downloading` | `done` | `error`

If the file is absent or stale (>5 s old), the screen daemon treats xoraya-cli as inactive.

The `scanning` state maps to the DOWNLOADING screen with "Scanning…" as the subtitle instead of a file progress bar.

---

## C++ StatusWriter Interface

New files at project root: `StatusWriter.hpp` + `StatusWriter.cpp`.

```cpp
// Called from scanner when a logger is found
StatusWriter::setScanning();
StatusWriter::setDevice(const std::string& name, int total_files);

// Called from downloader progress callback
StatusWriter::setProgress(int file_idx, int total,
                          float pct, float speed_mbps, int eta_s);

// Called on completion
StatusWriter::setDone(int files_done, uint64_t bytes_total);
StatusWriter::setError(const std::string& msg);

// Called on exit (clears the file)
StatusWriter::clear();
```

Implementation: single static mutex, writes to `/tmp/xoraya-status.json.tmp` then `rename()`. No exceptions thrown — write failures are silently ignored to avoid disrupting the CLI.

Call sites:
- `scanner.cpp` — `setScanning()` at scan start, `setDevice()` when logger found
- `downloader.cpp` — `setProgress()` inside `HandleReportCopy`, `setDone()`/`setError()` at end
- `collector.cpp` — `clear()` in exit path

---

## Screen States

| State | Triggered by | Auto-exit |
|-------|-------------|-----------|
| BOOT | Daemon starts | 3 s → IDLE |
| IDLE | Default / after any op | 60 s → SLEEP; cycles pages every 8 s |
| SLEEP | 60 s idle | Any switch flip → IDLE |
| DOWNLOADING | SW1 flipped ON | Service exits → DONE or ERROR |
| UPLOADING | SW2 flipped ON | Service exits → DONE or ERROR |
| DONE | Operation completes cleanly | 5 s → IDLE |
| ERROR | Operation exits with failures | 8 s → IDLE (error badge persists on IDLE) |

---

## Switch Behaviour

Each switch controls its service independently. No blocking logic.

| Event | Action |
|-------|--------|
| SW1 OFF → ON | `systemctl start xoraya-collect` |
| SW1 ON → OFF | `systemctl stop xoraya-collect` |
| SW2 OFF → ON | `systemctl start databridge` |
| SW2 ON → OFF | `systemctl stop databridge` |
| Any flip while SLEEP | Wake to IDLE first, then apply action above |
| Both ON simultaneously | Both services run; screen shows DOWNLOADING (download has display priority) |

---

## Screen Designs

All screens are white-on-black monochrome, rendered with Pillow into a `luma.oled` canvas.

### BOOT
```
        XORAYA
  DATALOGGER STATION
  ─────────────────
  v1.2.0 · Starting…
```

### IDLE — Page A (Status, shown first)
```
DX11246-2          ● NET
─────────────────────────
Logger files:           92
Local files:            47
Last upload:  09:14 · 3h ago
─────────────────────────
  [① OFF]        [② OFF]
```

### IDLE — Page B (System, shown after 8 s)
```
SYSTEM              pi@xoraya
─────────────────────────
IP:          192.168.1.42
Disk free:        18.4 GB
Uptime:           3d 14h
─────────────────────────
  [① OFF]        [② OFF]
```

### DOWNLOADING
```
▼ DOWNLOADING         ◌
DX11246-2  →  Pi
File 3 / 7              42%
████████░░░░░░░░░░░░░░░░░
1.2 MB/s            ETA 0:45
  [① ON ]        [② OFF]
```

### UPLOADING
```
▲ UPLOADING           ◌
Pi  →  Azure Blob
12 / 20 files           60%
████████████░░░░░░░░░░░░░
2 skipped · 0 failed
  [① OFF]        [② ON ]
```

### DONE
```


         ✓
  Download complete
  ─────────────────
  Files: 7 · 1.2 GB
```
The title and stats adapt: "Download complete / Files: N · X GB" or "Upload complete / Uploaded: N · Failed: 0". Auto-returns to IDLE after 5 s.

### ERROR
```
  ⚠  Download failed
  ─────────────────
  3 files · 2 failed
  Check logs:
  journalctl -u xoraya
```
Auto-returns to IDLE after 8 s. Error badge `⚠ 2 failed` shown on IDLE Page A until next successful operation.

---

## Files to Create / Modify

### New files
```
screen/
  screen_daemon.py        — main loop, state machine, GPIO, rendering
  requirements.txt        — luma.oled[i2c], gpiozero, Pillow
  xoraya-screen.service   — systemd unit (User=pi)

StatusWriter.hpp           — C++ header
StatusWriter.cpp           — C++ implementation
```

### Modified files
```
scanner.cpp     — call StatusWriter::setScanning(), setDevice()
downloader.cpp  — call StatusWriter::setProgress(), setDone(), setError()
collector.cpp   — call StatusWriter::clear() on exit
Makefile        — add StatusWriter.cpp to build
```

---

## Python Dependencies

```
luma.oled[i2c]   — SSD1306 driver
gpiozero         — GPIO switch detection (works on all Pi models)
Pillow           — text/graphics rendering into OLED canvas
```

Install on Pi: `pip3 install luma.oled gpiozero Pillow`

The `pi` user must be in the `i2c` and `gpio` groups (standard Pi OS setup — already satisfied).

---

## systemd Service

```ini
[Unit]
Description=Xoraya OLED screen daemon
After=multi-user.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/xoraya_cli/screen
ExecStart=/usr/bin/python3 screen_daemon.py
Restart=always
RestartSec=5

[Install]
WantedBy=multi-user.target
```

No `EnvironmentFile` needed — the daemon reads `/tmp/xoraya-status.json` directly and calls `systemctl` via subprocess.

---

## Out of Scope

- Battery / UPS indicator (no UPS hardware assumed)
- Network SSID display
- Azure upload count (not queryable offline without additional API call)
- Multi-logger simultaneous download progress (single progress bar shows active logger)
