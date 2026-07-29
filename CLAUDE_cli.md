# Claude Code Instructions — xoraya-cli

## Context

We have a functional C++17 CLI under Linux for communicating with Xoraya dataloggers (ML-N4000 and compatible).

Existing commands:
- scan: network discovery
- list <device>: list measurements
- download <device> <dest_dir> [N] [--delete-after-download] [--stop-logging]
- delete <device> <N>
- collect: scan + download all loggers, with options

Constraints:
- Pure C++17 (no Qt)
- X2E Linux SDK
- no system()
- no subprocesses
- no console output parsing
- reuse existing internal C++ functions directly

---

## Goal

Add new commands or features by:
1. Implementing the function in `downloader.cpp` (if SDK-related) or a new `.cpp` file
2. Declaring it in the corresponding `.hpp`
3. Adding parsing + dispatch in `main.cpp` following the existing pattern
4. Adding the new `.cpp` to `SRCS` in `Makefile`

---

## Expected commands

    ./xoraya-cli collect
    ./xoraya-cli collect --dest ./downloads
    ./xoraya-cli collect --dest ./downloads --interval 60
    ./xoraya-cli collect --delete-after-download
    ./xoraya-cli collect --device 192.168.1.10
    ./xoraya-cli collect --dry-run
    ./xoraya-cli collect --verbose
    ./xoraya-cli collect --stop-logging

---

## Options

### --dest <dir>
- optional
- default: ./downloads

### --delete-after-download
- optional
- non-destructive by default

### --interval <seconds>
- optional
- if absent: single pass
- if present: infinite loop, re-scanning every N seconds

### --device <id>
- optional
- filters a specific device (exact IP)

### --dry-run
- downloads nothing
- deletes nothing
- only prints what would be done

### --verbose
- prints full details (reuses download output)
- default: simple mode

### --stop-logging
- stops logging before download, restarts it afterwards
- default: logging is not interrupted

---

## Detailed behavior

- use the existing network scan
- if no logger found:
  - print an informational message
  - do not return an error
  - if --interval: wait for next cycle
  - otherwise: exit

- if multiple loggers:
  - sequential processing

- if a logger fails:
  - print the error
  - continue with others

- download:
  - use the existing download logic
  - all files go into the same destination folder
  - no additional subfolders

- deletion:
  - only if --delete-after-download

- deduplication:
  - none (the same measurement can be re-downloaded)

---

## Technical constraints

- no use of system()
- no CLI sub-command execution
- no stdout reading/parsing
- direct reuse of existing internal functions
- minimal modifications
- simple and maintainable code

---

## Expected architecture

```
struct CollectOptions {
    std::string dest_dir     = "./downloads";
    bool        delete_after  = false;
    int         interval_s    = -1;
    std::string device_filter;
    bool        dry_run       = false;
    bool        verbose       = false;
    bool        stop_logging  = false;
};

int cmd_collect(const CollectOptions& opts);
```

---

## Key points

- do not duplicate existing download logic
- do not introduce unnecessary complexity
- properly handle loop mode (--interval)
- Ctrl+C must exit cleanly
- "0 logger found" is not an error

---

## Current Status (as of 2026-07-13)

### Raspberry Pi deployment

- Pi IP: `192.168.1.51`, user `pi`
- Binary deployed at `/usr/local/bin/xoraya-cli` (Box64 emulation)
- Service: `xoraya-collect.service` (running)
- Upload service: `databridge.service`

### OLED screen daemon

Files in `screen/`:
- `screen_daemon.py` — state machine (BOOT→IDLE↔DOWNLOADING/UPLOADING→DONE/ERROR)
- `renderer.py` — all rendering functions, returns 128×64 PIL Image mode '1'
- `status_reader.py` — reads `/tmp/xoraya-status.json`, journalctl, system info
- `boot_splash.py` — minimal oneshot script, shows static boot screen early at boot
- `xoraya-screen.service` — full daemon, `After=sysinit.target`, `Restart=always`
- `xoraya-screen-splash.service` — oneshot splash, `DefaultDependencies=no`, `WantedBy=sysinit.target`

Both services installed and enabled on the Pi.

### Hardware wiring

```
Pin 1  (3.3V)   → VCC  (OLED SSD1306)
Pin 3  (GPIO2)  → SDA  (OLED)
Pin 5  (GPIO3)  → SCL  (OLED)
Pin 6  (GND)    → GND  (OLED)
Pin 13 (GPIO27) → COM  of SW1 (Download toggle)
Pin 14 (GND)    → GND terminal of SW1
Pin 19 (GPIO10) → COM  of SW2 (Upload toggle)
Pin 20 (GND)    → GND terminal of SW2
```

Switch type: miniature SPDT ON-ON, soldering lugs. COM→GPIO, one end→GND, other end→unconnected.
gpiozero: `Button(27, pull_up=True)` and `Button(10, pull_up=True)`.
I2C enabled via `raspi-config nonint do_i2c 0`. `i2cdetect -y 1` shows `3c`.

### Open issue: OLED not displaying anything

The OLED is detected at address `0x3C` on `/dev/i2c-1`. The Python scripts run without errors (exit 0). `luma.oled` sends commands successfully. But nothing appears on the physical screen.

Diagnosis done so far:
- `i2cdetect -y 1` → shows `3c` ✓
- `pi` is in the `i2c` group ✓
- `ImageFont.load_default(size=8)` works (Pillow 12.3) ✓
- Direct render test (daemon stopped, 10s hold) → script says "displayed" but user sees nothing

**Suspicion: hardware issue** — either contrast register not being set, or the OLED panel is defective/wired incorrectly (VCC/GND swapped, or SDA/SCL swapped).

Next steps to try:
1. Try `oled.contrast(255)` explicitly before `oled.display()`
2. Try `i2cset -y 1 0x3c 0x00 0x8d 0x14 0xaf` (manual SSD1306 charge pump + display on)
3. Check physical wiring: confirm VCC=3.3V, GND=GND, SDA=Pin3, SCL=Pin5
4. Try a different I2C address (some SSD1306 use `0x3D`)

### StatusWriter (C++)

`StatusWriter.cpp` writes atomic JSON to `/tmp/xoraya-status.json`. Includes `dest_dir` field in all states. `setScanning(dest_dir)` stores it as a static for subsequent writes.

### Auto storage detection

`storage.cpp` / `storage.hpp`: `detect_dest_dir(err)` scans `/media/<username>/`, checks `/proc/mounts`, returns `<mount>/Dexterlogs` (exactly 1 drive) or error (0 or 2+ drives). Used in `main.cpp` when `--dest` is not explicit.
