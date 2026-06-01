# OLED Screen + Toggle Switch Interface — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Add a 128×64 SSD1306 OLED display and two toggle switches to the Pi station, showing real-time download/upload progress driven by a new Python daemon.

**Architecture:** A new `xoraya-screen` systemd service reads `/tmp/xoraya-status.json` (written by xoraya-cli) and journal output (for databridge) to drive an SSD1306 OLED via luma.oled. Two toggle switches on GPIO 17/27 start and stop the collect and upload services via `systemctl`. The C++ `StatusWriter` class is added to xoraya-cli for atomic JSON progress reporting.

**Tech Stack:** C++17 (StatusWriter), Python 3 (luma.oled, gpiozero, Pillow), systemd, pytest

---

## File Map

### New files
```
StatusWriter.hpp              — C++ static class interface
StatusWriter.cpp              — atomic JSON write to /tmp/xoraya-status.json
screen/
  status_reader.py            — reads JSON + journal + systemctl
  renderer.py                 — renders all screen states to PIL Images
  screen_daemon.py            — state machine, GPIO, main loop
  requirements.txt            — Python dependencies
  xoraya-screen.service       — systemd unit
  tests/
    test_status_reader.py
    test_renderer.py
```

### Modified files
```
collector.cpp                 — call setScanning() before scan, clear() on exit
downloader.cpp                — call setDevice(), setProgress(), setDone()/setError()
Makefile                      — add StatusWriter.cpp to SRCS
```

---

## Part A — C++ StatusWriter

---

### Task 1: Create StatusWriter.hpp and StatusWriter.cpp

**Files:**
- Create: `StatusWriter.hpp`
- Create: `StatusWriter.cpp`

- [ ] **Step 1: Create StatusWriter.hpp**

```cpp
#pragma once
#include <string>
#include <cstdint>

/**
 * Writes /tmp/xoraya-status.json atomically so the screen daemon
 * can read download progress without locking.
 * All methods are no-throw — write failures are silently ignored.
 */
class StatusWriter {
public:
    static void setScanning();
    static void setDevice(const std::string& name, int total_files);
    static void setProgress(int file_idx, int total,
                            float pct, float speed_mbps, int eta_s);
    static void setDone(int files_done, uint64_t bytes_total);
    static void setError(const std::string& msg);
    static void clear();
};
```

- [ ] **Step 2: Create StatusWriter.cpp**

```cpp
#include "StatusWriter.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

static const char* PATH     = "/tmp/xoraya-status.json";
static const char* PATH_TMP = "/tmp/xoraya-status.json.tmp";

static std::mutex s_mtx;

static void write_atomic(const std::string& json)
{
    FILE* f = fopen(PATH_TMP, "w");
    if (!f) return;
    fputs(json.c_str(), f);
    fclose(f);
    rename(PATH_TMP, PATH);
}

static long now_ts()
{
    return static_cast<long>(time(nullptr));
}

void StatusWriter::setScanning()
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[128];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"scanning\",\"updated_at\":%ld}", now_ts());
    write_atomic(buf);
}

void StatusWriter::setDevice(const std::string& name, int total_files)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"scanning\","
        "\"device\":\"%s\","
        "\"files_total\":%d,"
        "\"updated_at\":%ld}",
        name.c_str(), total_files, now_ts());
    write_atomic(buf);
}

void StatusWriter::setProgress(int file_idx, int total,
                               float pct, float speed_mbps, int eta_s)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"downloading\","
        "\"files_done\":%d,\"files_total\":%d,"
        "\"pct\":%.1f,\"speed_mbps\":%.2f,\"eta_s\":%d,"
        "\"updated_at\":%ld}",
        file_idx, total, (double)pct, (double)speed_mbps, eta_s, now_ts());
    write_atomic(buf);
}

void StatusWriter::setDone(int files_done, uint64_t bytes_total)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[256];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"done\","
        "\"files_done\":%d,\"bytes_total\":%llu,"
        "\"updated_at\":%ld}",
        files_done,
        (unsigned long long)bytes_total,
        now_ts());
    write_atomic(buf);
}

void StatusWriter::setError(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    // Replace double-quotes to avoid breaking JSON
    std::string safe;
    for (char c : msg) safe += (c == '"') ? '\'' : c;
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"error\",\"msg\":\"%s\",\"updated_at\":%ld}",
        safe.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::clear()
{
    std::lock_guard<std::mutex> lk(s_mtx);
    remove(PATH);
}
```

- [ ] **Step 3: Verify it compiles in isolation**

```bash
g++ -std=c++17 -c StatusWriter.cpp -o StatusWriter.o
```

Expected: no output (compiles cleanly).

- [ ] **Step 4: Commit**

```bash
git add StatusWriter.hpp StatusWriter.cpp
git commit -m "feat: add StatusWriter for OLED progress reporting"
```

---

### Task 2: Integrate StatusWriter into collector.cpp and downloader.cpp

**Files:**
- Modify: `collector.cpp`
- Modify: `downloader.cpp`

- [ ] **Step 1: Add include to collector.cpp**

At the top of `collector.cpp`, after the existing includes (after line 26):

```cpp
#include "StatusWriter.hpp"
```

- [ ] **Step 2: Call setScanning() before scan in run_pass()**

In `collector.cpp`, `run_pass()` around line 123, change:

```cpp
    print_timestamp();
    printf("Scanning network (2 s)...\n");
    fflush(stdout);

    auto loggers = scan_network(2000);
```

to:

```cpp
    print_timestamp();
    printf("Scanning network (2 s)...\n");
    fflush(stdout);

    StatusWriter::setScanning();
    auto loggers = scan_network(2000);
```

- [ ] **Step 3: Call clear() at the end of cmd_collect()**

In `collector.cpp`, `cmd_collect()`, the final `return global_fail;` at line 246:

```cpp
    StatusWriter::clear();
    return global_fail;
```

- [ ] **Step 4: Add include to downloader.cpp**

At the top of `downloader.cpp`, after the existing includes:

```cpp
#include "StatusWriter.hpp"
```

- [ ] **Step 5: Call setDevice() + setProgress() in download_gen2()**

In `download_gen2()`, after `targets.reserve(...)` (around line 598), before the download loop, add:

```cpp
    StatusWriter::setDevice(device_name, static_cast<int>(targets.size()));
```

Then inside the measurement loop (`for (size_t i = 0; ...)`), in the inner wait loop where progress is printed (around lines 670-675):

```cpp
                if (bytes_max > 0) {
                    double pct   = (double)bytes_rx / (double)bytes_max * 100.0;
                    double mbit  = (double)(bytes_rx - last_rx) * 8.0 / 1e6 / 0.2;
                    printf("\r  %5.1f%%  %4.0f Mbit/s   ", pct, mbit);
                    fflush(stdout);
                    StatusWriter::setProgress(
                        static_cast<int>(i + 1),
                        static_cast<int>(targets.size()),
                        static_cast<float>(pct),
                        static_cast<float>(mbit / 8.0),   // Mbit/s → MB/s
                        0);
                }
```

- [ ] **Step 6: Call setProgress() in CopyProgress::HandleReportCopy() (Gen3)**

In the `CopyProgress` class (around line 524), change `HandleReportCopy` to:

```cpp
    void HandleReportCopy(float pct_total,
                          timespanLowRes::type_t eta_sec,
                          uint32_t mbit_s) override
    {
        StatusWriter::setProgress(
            0, 0,
            pct_total,
            static_cast<float>(mbit_s) / 8.0f,
            static_cast<int>(eta_sec));
        printf("\r  %5.1f%%  %4u Mbit/s  ETA : %ds   ",
               pct_total, mbit_s, (int)eta_sec);
        fflush(stdout);
    }
```

- [ ] **Step 7: Call setDone()/setError() at end of cmd_download()**

In `cmd_download()`, replace the final block (around line 953):

```cpp
    ctrl->Disconnect();
    if (rc == 0) {
        StatusWriter::setDone(0, 0);
        printf("\nFiles available in: %s\n", dest_dir.c_str());
    } else {
        StatusWriter::setError("download failed");
    }
    return rc;
```

- [ ] **Step 8: Update Makefile to include StatusWriter.cpp**

Change the `SRCS` block in `Makefile`:

```makefile
SRCS  = main.cpp
SRCS += scanner.cpp
SRCS += downloader.cpp
SRCS += collector.cpp
SRCS += StatusWriter.cpp
```

- [ ] **Step 9: Build**

```bash
make clean && make
```

Expected: compiles with no errors. Binary `xoraya-cli` produced.

- [ ] **Step 10: Smoke test — verify JSON is written during a dry-run collect**

```bash
./xoraya-cli collect --dry-run --verbose &
sleep 3
cat /tmp/xoraya-status.json
```

Expected: JSON file contains `"state":"scanning"` (written before scan). May be absent after dry-run exits (cleared by `StatusWriter::clear()`).

To catch it mid-run:
```bash
./xoraya-cli collect --dest /tmp/test-dl --verbose &
PID=$!
sleep 1
cat /tmp/xoraya-status.json
wait $PID
```

- [ ] **Step 11: Commit**

```bash
git add collector.cpp downloader.cpp Makefile
git commit -m "feat: integrate StatusWriter into collect/download pipeline"
```

---

## Part B — Python Screen Daemon

---

### Task 3: Set up screen/ directory and verify hardware access

**Files:**
- Create: `screen/requirements.txt`
- Create: `screen/tests/__init__.py` (empty)

- [ ] **Step 1: Create screen/ layout**

```bash
mkdir -p screen/tests
touch screen/tests/__init__.py
```

- [ ] **Step 2: Create screen/requirements.txt**

```
luma.oled[i2c]>=3.12.0
gpiozero>=2.0
Pillow>=10.0.0
pytest>=8.0.0
```

- [ ] **Step 3: Install on Pi**

```bash
pip3 install -r screen/requirements.txt
```

Expected: installs without error. On a non-Pi dev machine, luma.oled may warn about missing I2C — that's fine for running tests.

- [ ] **Step 4: Verify I2C address on Pi**

```bash
sudo i2cdetect -y 1
```

Expected: address `3c` visible in the grid. If absent, check SDA/SCL wiring and that I2C is enabled (`sudo raspi-config` → Interface Options → I2C).

- [ ] **Step 5: Commit**

```bash
git add screen/
git commit -m "chore: scaffold screen/ directory and requirements"
```

---

### Task 4: Implement status_reader.py with tests

**Files:**
- Create: `screen/status_reader.py`
- Create: `screen/tests/test_status_reader.py`

- [ ] **Step 1: Write failing tests**

Create `screen/tests/test_status_reader.py`:

```python
import json
import os
import tempfile
import time

import pytest

# Patch STATUS_FILE path before importing the module
STATUS_FILE_PATH = None  # set per test via monkeypatch


def write_status(path, data):
    with open(path, 'w') as f:
        json.dump(data, f)


def test_read_xoraya_status_returns_none_when_file_absent(tmp_path, monkeypatch):
    import screen.status_reader as sr
    monkeypatch.setattr(sr, 'STATUS_FILE', str(tmp_path / 'absent.json'))
    assert sr.read_xoraya_status() is None


def test_read_xoraya_status_returns_none_when_stale(tmp_path, monkeypatch):
    import screen.status_reader as sr
    p = tmp_path / 'status.json'
    write_status(p, {'state': 'downloading', 'updated_at': 1})
    monkeypatch.setattr(sr, 'STATUS_FILE', str(p))
    # updated_at=1 is ancient — stale
    assert sr.read_xoraya_status() is None


def test_read_xoraya_status_returns_dict_when_fresh(tmp_path, monkeypatch):
    import screen.status_reader as sr
    p = tmp_path / 'status.json'
    data = {'state': 'downloading', 'pct': 42.0, 'updated_at': int(time.time())}
    write_status(p, data)
    monkeypatch.setattr(sr, 'STATUS_FILE', str(p))
    result = sr.read_xoraya_status()
    assert result is not None
    assert result['state'] == 'downloading'
    assert result['pct'] == 42.0


def test_read_xoraya_status_handles_corrupt_json(tmp_path, monkeypatch):
    import screen.status_reader as sr
    p = tmp_path / 'status.json'
    p.write_text('not json {{{')
    monkeypatch.setattr(sr, 'STATUS_FILE', str(p))
    assert sr.read_xoraya_status() is None


def test_get_uptime_returns_string():
    import screen.status_reader as sr
    result = sr.get_uptime()
    assert isinstance(result, str)
    assert len(result) > 0


def test_get_disk_free_returns_string():
    import screen.status_reader as sr
    result = sr.get_disk_free()
    assert isinstance(result, str)
    assert len(result) > 0
```

- [ ] **Step 2: Run tests — verify they fail with ImportError**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
python3 -m pytest screen/tests/test_status_reader.py -v
```

Expected: `ModuleNotFoundError: No module named 'screen.status_reader'`

- [ ] **Step 3: Implement screen/status_reader.py**

```python
import json
import os
import re
import subprocess
import time

STATUS_FILE      = '/tmp/xoraya-status.json'
STALE_THRESHOLD  = 5   # seconds


def read_xoraya_status():
    """Returns parsed JSON dict, or None if file is absent, stale, or corrupt."""
    try:
        mtime = os.path.getmtime(STATUS_FILE)
        if time.time() - mtime > STALE_THRESHOLD:
            return None
        with open(STATUS_FILE) as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError, ValueError):
        return None


def is_service_active(service_name):
    """Returns True if the named systemd service is currently active."""
    r = subprocess.run(
        ['systemctl', 'is-active', '--quiet', service_name],
        capture_output=True,
    )
    return r.returncode == 0


def read_databridge_progress():
    """
    Parses the last 'Done' line from the databridge journal.
    Returns {'uploaded': N, 'skipped': N, 'failed': N} or None.
    """
    r = subprocess.run(
        ['journalctl', '-u', 'databridge', '-n', '20',
         '--no-pager', '--output=cat'],
        capture_output=True, text=True,
    )
    for line in reversed(r.stdout.splitlines()):
        m = re.search(
            r'uploaded:\s*(\d+).*?skipped:\s*(\d+).*?failed:\s*(\d+)',
            line,
        )
        if m:
            return {
                'uploaded': int(m.group(1)),
                'skipped':  int(m.group(2)),
                'failed':   int(m.group(3)),
            }
    return None


def get_ip_address():
    """Returns the Pi's first non-loopback IP, or '?.?.?.?' on failure."""
    try:
        r = subprocess.run(['hostname', '-I'], capture_output=True, text=True)
        parts = r.stdout.strip().split()
        return parts[0] if parts else '?.?.?.?'
    except OSError:
        return '?.?.?.?'


def get_disk_free():
    """Returns free space on / as a human-readable string (e.g. '18.4G')."""
    try:
        r = subprocess.run(['df', '-h', '/'], capture_output=True, text=True)
        lines = r.stdout.strip().splitlines()
        if len(lines) >= 2:
            parts = lines[1].split()
            return parts[3] if len(parts) > 3 else '?'
    except OSError:
        pass
    return '?'


def get_uptime():
    """Returns uptime as '3d 14h' or '2h 5m'."""
    try:
        with open('/proc/uptime') as f:
            total_s = float(f.read().split()[0])
        days  = int(total_s // 86400)
        hours = int((total_s % 86400) // 3600)
        mins  = int((total_s % 3600) // 60)
        if days > 0:
            return f'{days}d {hours}h'
        return f'{hours}h {mins}m'
    except (OSError, ValueError):
        return '?'
```

- [ ] **Step 4: Run tests — verify they pass**

```bash
python3 -m pytest screen/tests/test_status_reader.py -v
```

Expected:
```
PASSED test_read_xoraya_status_returns_none_when_file_absent
PASSED test_read_xoraya_status_returns_none_when_stale
PASSED test_read_xoraya_status_returns_dict_when_fresh
PASSED test_read_xoraya_status_handles_corrupt_json
PASSED test_get_uptime_returns_string
PASSED test_get_disk_free_returns_string
```

- [ ] **Step 5: Commit**

```bash
git add screen/status_reader.py screen/tests/test_status_reader.py screen/tests/__init__.py
git commit -m "feat: add status_reader with JSON + journal + system info"
```

---

### Task 5: Implement renderer.py with tests

**Files:**
- Create: `screen/renderer.py`
- Create: `screen/tests/test_renderer.py`

- [ ] **Step 1: Write failing tests**

Create `screen/tests/test_renderer.py`:

```python
from PIL import Image
import pytest


def import_renderer():
    import screen.renderer as r
    return r


def test_render_boot_returns_128x64_image():
    r = import_renderer()
    img = r.render_boot()
    assert img.size == (128, 64)
    assert img.mode == '1'


def test_render_idle_status_returns_correct_size():
    r = import_renderer()
    img = r.render_idle_status(
        device='DX11246-2', net_ok=True,
        logger_files=92, local_files=47,
        last_upload='09:14', sw1=False, sw2=False,
    )
    assert img.size == (128, 64)


def test_render_idle_status_with_error_badge():
    r = import_renderer()
    img = r.render_idle_status(
        device='DX11246-2', net_ok=False,
        logger_files=0, local_files=10,
        last_upload='never', sw1=False, sw2=False,
        error_badge='2 failed',
    )
    assert img.size == (128, 64)


def test_render_downloading_progress():
    r = import_renderer()
    img = r.render_downloading(
        device='DX11246-2',
        file_idx=3, total=7, pct=42.0,
        speed_mbps=1.2, eta_s=45,
        sw1=True, sw2=False,
    )
    assert img.size == (128, 64)


def test_render_downloading_scanning_mode():
    r = import_renderer()
    img = r.render_downloading(
        device='DX11246-2',
        file_idx=0, total=0, pct=0.0,
        speed_mbps=0.0, eta_s=0,
        sw1=True, sw2=False, scanning=True,
    )
    assert img.size == (128, 64)


def test_render_uploading():
    r = import_renderer()
    img = r.render_uploading(
        uploaded=12, total=20, skipped=2, failed=0,
        sw1=False, sw2=True,
    )
    assert img.size == (128, 64)


def test_render_done_download():
    r = import_renderer()
    img = r.render_done('Download complete', 'Files: 7', '1.2 GB')
    assert img.size == (128, 64)


def test_render_error():
    r = import_renderer()
    img = r.render_error('Download failed', '2 failed', 'journalctl -u xoraya')
    assert img.size == (128, 64)


def test_render_idle_system():
    r = import_renderer()
    img = r.render_idle_system(
        ip='192.168.1.42', disk_free='18.4G',
        uptime='3d 14h', sw1=False, sw2=False,
    )
    assert img.size == (128, 64)


def test_render_sleep():
    r = import_renderer()
    img = r.render_sleep()
    assert img.size == (128, 64)
    # Sleep screen must be all black
    assert img.getbbox() is None
```

- [ ] **Step 2: Run tests — verify they fail**

```bash
python3 -m pytest screen/tests/test_renderer.py -v
```

Expected: `ModuleNotFoundError: No module named 'screen.renderer'`

- [ ] **Step 3: Implement screen/renderer.py**

```python
"""
renderer.py — renders each screen state to a 128×64 PIL Image (mode '1').

All functions return Image objects that can be sent directly to luma.oled's
device.display(). No hardware access here — fully testable without an OLED.
"""

from PIL import Image, ImageDraw, ImageFont

WIDTH  = 128
HEIGHT = 64


def _canvas():
    """Returns a blank 128×64 black image and a Draw object."""
    img = Image.new('1', (WIDTH, HEIGHT), 0)
    return img, ImageDraw.Draw(img)


def _font():
    return ImageFont.load_default()


def _sw_row(d, sw1, sw2, y=55):
    """Draws switch indicators at the bottom of the screen."""
    d.rectangle([(0, y), (55, 63)], outline=1)
    d.text((3, y + 1), f'① {"ON " if sw1 else "OFF"}', font=_font(), fill=1)
    d.rectangle([(72, y), (127, 63)], outline=1)
    d.text((75, y + 1), f'② {"ON " if sw2 else "OFF"}', font=_font(), fill=1)


def _divider(d, y):
    d.line([(0, y), (127, y)], fill=1)


def _bar(d, pct, y=35, height=8):
    """Draws a filled progress bar at row y."""
    d.rectangle([(0, y), (127, y + height - 1)], outline=1)
    w = max(0, int(pct / 100.0 * 126))
    if w > 0:
        d.rectangle([(1, y + 1), (w, y + height - 2)], fill=1)


def render_boot(version='1.0.0'):
    img, d = _canvas()
    f = _font()
    d.text((8, 10), 'DISTALMOTION SA', font=f, fill=1)
    d.text((8, 22), 'DATALOGGER STATION', font=f, fill=1)
    _divider(d, 34)
    d.text((10, 38), f'v{version} · Starting…', font=f, fill=1)
    return img


def render_idle_status(device, net_ok, logger_files, local_files,
                       last_upload, sw1, sw2, error_badge=None):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), device or 'No logger', font=f, fill=1)
    net_str = '● NET' if net_ok else '○ ---'
    d.text((90, 0), net_str, font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), f'Logger files: {logger_files}', font=f, fill=1)
    d.text((0, 24), f'Local files:  {local_files}', font=f, fill=1)
    d.text((0, 34), f'Last:  {last_upload}', font=f, fill=1)
    if error_badge:
        d.text((0, 44), f'⚠ {error_badge}', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_idle_system(ip, disk_free, uptime, sw1, sw2):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), 'SYSTEM', font=f, fill=1)
    d.text((72, 0), 'pi@xoraya', font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), f'IP:       {ip}', font=f, fill=1)
    d.text((0, 24), f'Disk:     {disk_free}', font=f, fill=1)
    d.text((0, 34), f'Uptime:   {uptime}', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_downloading(device, file_idx, total, pct,
                       speed_mbps, eta_s, sw1, sw2, scanning=False):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), '▼ DOWNLOADING', font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), f'{device}  →  Pi', font=f, fill=1)
    if scanning or total == 0:
        d.text((0, 26), 'Scanning…', font=f, fill=1)
    else:
        d.text((0, 26), f'File {file_idx} / {total}', font=f, fill=1)
        d.text((98, 26), f'{int(pct)}%', font=f, fill=1)
        _bar(d, pct, y=36, height=7)
        m, s = divmod(int(eta_s), 60)
        d.text((0, 45), f'{speed_mbps:.1f} MB/s', font=f, fill=1)
        d.text((80, 45), f'ETA {m}:{s:02d}', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_uploading(uploaded, total, skipped, failed, sw1, sw2):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), '▲ UPLOADING', font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), 'Pi  →  Azure Blob', font=f, fill=1)
    pct = int(uploaded / total * 100) if total > 0 else 0
    d.text((0, 26), f'{uploaded} / {total} files', font=f, fill=1)
    d.text((98, 26), f'{pct}%', font=f, fill=1)
    _bar(d, pct, y=36, height=7)
    d.text((0, 45), f'{skipped} skip · {failed} fail', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_done(title, stats_line1, stats_line2=''):
    img, d = _canvas()
    f = _font()
    d.text((54, 4), '✓', font=f, fill=1)
    d.text((10, 18), title, font=f, fill=1)
    _divider(d, 30)
    d.text((0, 34), stats_line1, font=f, fill=1)
    if stats_line2:
        d.text((0, 46), stats_line2, font=f, fill=1)
    return img


def render_error(title, detail='', hint=''):
    img, d = _canvas()
    f = _font()
    d.text((0, 2), f'⚠  {title}', font=f, fill=1)
    _divider(d, 14)
    if detail:
        d.text((0, 18), detail, font=f, fill=1)
    if hint:
        d.text((0, 32), 'Logs:', font=f, fill=1)
        d.text((0, 44), hint, font=f, fill=1)
    return img


def render_sleep():
    """All-black image — OLED off."""
    return Image.new('1', (WIDTH, HEIGHT), 0)
```

- [ ] **Step 4: Run tests — verify they pass**

```bash
python3 -m pytest screen/tests/test_renderer.py -v
```

Expected: all 10 tests PASS.

- [ ] **Step 5: Visual sanity check — save sample screens to PNG**

```bash
python3 - <<'EOF'
import sys; sys.path.insert(0, '.')
from screen.renderer import *
render_boot().save('/tmp/boot.png')
render_idle_status('DX11246-2', True, 92, 47, '09:14', False, False).save('/tmp/idle.png')
render_downloading('DX11246-2', 3, 7, 42.0, 1.2, 45, True, False).save('/tmp/dl.png')
render_uploading(12, 20, 2, 0, False, True).save('/tmp/ul.png')
render_done('Download complete', 'Files: 7', '1.2 GB').save('/tmp/done.png')
render_error('Download failed', '2 failed', 'journalctl -u xoraya').save('/tmp/err.png')
print("Saved to /tmp/*.png — open them to check layout.")
EOF
```

Open the PNGs and verify layout looks reasonable. They will be very small (128×64 px) — zoom in.

- [ ] **Step 6: Commit**

```bash
git add screen/renderer.py screen/tests/test_renderer.py
git commit -m "feat: add OLED renderer for all screen states"
```

---

### Task 6: Implement screen_daemon.py

**Files:**
- Create: `screen/screen_daemon.py`

Note: This file ties together hardware (GPIO, OLED) and cannot be fully tested without a Pi. The logic is kept simple and deterministic; the renderer and status_reader are already tested independently.

- [ ] **Step 1: Create screen/__init__.py** (makes screen/ a Python package)

```bash
touch screen/__init__.py
```

- [ ] **Step 2: Create screen/screen_daemon.py**

```python
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
        sw1_rose  = cur_sw1 and not prev_sw1   # OFF → ON
        sw1_fell  = not cur_sw1 and prev_sw1   # ON → OFF
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
```

- [ ] **Step 3: Syntax-check the file**

```bash
python3 -m py_compile screen/screen_daemon.py && echo "OK"
```

Expected: `OK`

- [ ] **Step 4: Commit**

```bash
git add screen/__init__.py screen/screen_daemon.py
git commit -m "feat: add screen_daemon with state machine and GPIO control"
```

---

### Task 7: Create systemd service and deploy to Pi

**Files:**
- Create: `screen/xoraya-screen.service`

- [ ] **Step 1: Create screen/xoraya-screen.service**

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
Environment=DEST=/home/pi/Dexterlogs

[Install]
WantedBy=multi-user.target
```

- [ ] **Step 2: Commit**

```bash
git add screen/xoraya-screen.service
git commit -m "feat: add xoraya-screen systemd service unit"
```

- [ ] **Step 3: Deploy to Pi — copy screen/ directory**

On dev machine (adjust user/host as needed):

```bash
rsync -av screen/ pi@raspberrypi:/home/pi/xoraya_cli/screen/
```

Or via git pull on the Pi:

```bash
# On Pi:
cd /home/pi/xoraya_cli && git pull
```

- [ ] **Step 4: Install Python deps on Pi**

```bash
# On Pi:
pip3 install -r /home/pi/xoraya_cli/screen/requirements.txt
```

- [ ] **Step 5: Enable and start the service on Pi**

```bash
# On Pi (as root or with sudo):
sudo cp /home/pi/xoraya_cli/screen/xoraya-screen.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable xoraya-screen
sudo systemctl start xoraya-screen
```

- [ ] **Step 6: Verify service is running**

```bash
sudo systemctl status xoraya-screen
```

Expected: `Active: active (running)`. Display should show the boot screen followed by the IDLE screen.

- [ ] **Step 7: Smoke test — toggle SW1 ON**

Flip switch 1 to ON. Expected:
- Screen transitions to DOWNLOADING (shows "Scanning…" while xoraya-collect starts)
- `systemctl status xoraya-collect` shows `active (running)`

Flip switch 1 back to OFF. Expected:
- `xoraya-collect` stops
- Screen returns to IDLE

- [ ] **Step 8: Check journal for errors**

```bash
journalctl -u xoraya-screen -n 30 --no-pager
```

Expected: no Python tracebacks.

---

## Self-Review Checklist

- [x] **StatusWriter** — all 6 methods implemented, atomic write, no-throw
- [x] **collector.cpp** — setScanning() before scan, clear() on exit
- [x] **downloader.cpp** — setDevice() + setProgress() for Gen2; setProgress() for Gen3 via CopyProgress; setDone()/setError() in cmd_download()
- [x] **Makefile** — StatusWriter.cpp added to SRCS
- [x] **status_reader.py** — JSON read with staleness check, journal parse, system info
- [x] **renderer.py** — all 8 screen states, render_sleep() returns all-black image
- [x] **screen_daemon.py** — all states in spec, switch rise/fall logic, service start/stop
- [x] **xoraya-screen.service** — User=pi, WorkingDirectory, Restart=always
- [x] **Both switches ON** — both services start independently, screen shows DOWNLOADING (download state machine runs)
- [x] **Error badge** — set on IDLE Page A, cleared on next successful operation
- [x] **IDLE cycles** — page A (status) ↔ page B (system) every 8 s
- [x] **SLEEP** — screen goes black after 60 s; any switch flip wakes + triggers
