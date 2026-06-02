# Auto Storage Detection — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Automatically detect the single mounted external drive under `/media/<user>/` and use `<drive>/Dexterlogs/` as the destination, replacing the hardcoded `/home/Dexterlogs` default.

**Architecture:** A new `storage.cpp/hpp` module provides `detect_dest_dir()`. It is called in `main.cpp` when `--dest` is absent. The detected path is stored as a static inside `StatusWriter` (set by `setScanning()`) so every subsequent JSON write propagates `dest_dir` automatically — no changes needed in `downloader.cpp`. The screen daemon reads `dest_dir` from the JSON to count local `.mf4` files.

**Tech Stack:** C++17 (POSIX: `opendir`, `getpwuid`, `/proc/mounts`), Python 3

---

## File Map

### New files
```
storage.hpp     — declares detect_dest_dir()
storage.cpp     — implements detection via /media/<user>/ + /proc/mounts
```

### Modified files
```
Makefile                — add storage.cpp to SRCS
StatusWriter.hpp        — setScanning() gains optional dest_dir parameter
StatusWriter.cpp        — add s_dest_dir static; include dest_dir in all JSON writes
collector.cpp           — pass opts.dest_dir to setScanning()
main.cpp                — add dest_explicit flag + call detect_dest_dir() in both paths
screen/screen_daemon.py — count_local_mf4() reads dest from JSON
```

---

## Task 1: Create storage.hpp and storage.cpp

**Files:**
- Create: `storage.hpp`
- Create: `storage.cpp`
- Modify: `Makefile`

- [ ] **Step 1: Create storage.hpp**

```cpp
#pragma once
#include <string>

/**
 * Scans /media/<username>/ for mounted external drives.
 *
 * Returns "<mount>/Dexterlogs" when exactly one drive is found.
 * Returns "" and sets err when 0 or 2+ drives are found.
 */
std::string detect_dest_dir(std::string& err);
```

- [ ] **Step 2: Create storage.cpp**

```cpp
#include "storage.hpp"

#include <cstdio>
#include <cstring>
#include <dirent.h>
#include <pwd.h>
#include <string>
#include <unistd.h>
#include <vector>

static bool is_mount_point(const std::string& path)
{
    FILE* f = fopen("/proc/mounts", "r");
    if (!f) return false;
    char line[512];
    bool found = false;
    while (fgets(line, sizeof(line), f)) {
        char dev[256], mnt[256];
        if (sscanf(line, "%255s %255s", dev, mnt) == 2) {
            if (path == mnt) {
                found = true;
                break;
            }
        }
    }
    fclose(f);
    return found;
}

std::string detect_dest_dir(std::string& err)
{
    const passwd* pw = getpwuid(getuid());
    if (!pw) {
        err = "could not determine current username";
        return "";
    }
    std::string base = std::string("/media/") + pw->pw_name + "/";

    DIR* dir = opendir(base.c_str());
    if (!dir) {
        err = "no external drive found (could not open " + base + ")";
        return "";
    }

    std::vector<std::string> found;
    dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (entry->d_name[0] == '.') continue;
        std::string candidate = base + entry->d_name;
        if (is_mount_point(candidate)) {
            found.push_back(candidate);
        }
    }
    closedir(dir);

    if (found.empty()) {
        err = "no external drive found under " + base +
              "\n       Plug in a USB drive or SSD and try again.";
        return "";
    }
    if (found.size() > 1) {
        err = "multiple external drives found — plug in only one.";
        for (const auto& p : found) err += "\n       Found: " + p;
        return "";
    }
    return found[0] + "/Dexterlogs";
}
```

- [ ] **Step 3: Add storage.cpp to Makefile**

In `Makefile`, find the SRCS block (currently ends with `SRCS += StatusWriter.cpp`) and add:

```makefile
SRCS += storage.cpp
```

- [ ] **Step 4: Compile storage.cpp in isolation**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
g++ -std=c++17 -c storage.cpp -o storage.o
```

Expected: no output (compiles cleanly). Remove the object file after:

```bash
rm storage.o
```

- [ ] **Step 5: Commit**

```bash
git add storage.hpp storage.cpp Makefile
git commit -m "feat: add storage detector for external drive auto-detection"
```

---

## Task 2: Update StatusWriter to propagate dest_dir in all JSON writes

The strategy: store the detected path in a static `s_dest_dir` variable inside `StatusWriter.cpp`. `setScanning()` sets it once; every other write method includes it automatically.

**Files:**
- Modify: `StatusWriter.hpp`
- Modify: `StatusWriter.cpp`

- [ ] **Step 1: Update StatusWriter.hpp**

Replace the current `setScanning()` declaration with one that accepts an optional `dest_dir`:

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
    static void setScanning(const std::string& dest_dir = "");
    static void setDevice(const std::string& name, int total_files);
    static void setProgress(int file_idx, int total,
                            float pct, float speed_mbps, int eta_s);
    static void setDone(int files_done, uint64_t bytes_total);
    static void setError(const std::string& msg);
    static void clear();
};
```

- [ ] **Step 2: Rewrite StatusWriter.cpp**

Replace the entire file with this updated version that adds `s_dest_dir` and includes `dest_dir` in every JSON write:

```cpp
#include "StatusWriter.hpp"

#include <cstdio>
#include <ctime>
#include <mutex>
#include <string>

static const char* PATH     = "/tmp/xoraya-status.json";
static const char* PATH_TMP = "/tmp/xoraya-status.json.tmp";

static std::mutex s_mtx;
static std::string s_dest_dir;   // set by setScanning(), included in all writes

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

static std::string sanitise(const std::string& s)
{
    std::string out;
    for (char c : s) out += (c == '"') ? '\'' : c;
    return out;
}

void StatusWriter::setScanning(const std::string& dest_dir)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_dest_dir = sanitise(dest_dir);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"scanning\","
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::setDevice(const std::string& name, int total_files)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    std::string safe = sanitise(name);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"scanning\","
        "\"device\":\"%s\","
        "\"files_total\":%d,"
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        safe.c_str(), total_files, s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::setProgress(int file_idx, int total,
                               float pct, float speed_mbps, int eta_s)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"downloading\","
        "\"files_done\":%d,\"files_total\":%d,"
        "\"pct\":%.1f,\"speed_mbps\":%.2f,\"eta_s\":%d,"
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        file_idx, total, (double)pct, (double)speed_mbps, eta_s,
        s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::setDone(int files_done, uint64_t bytes_total)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"done\","
        "\"files_done\":%d,\"bytes_total\":%llu,"
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        files_done,
        (unsigned long long)bytes_total,
        s_dest_dir.c_str(),
        now_ts());
    write_atomic(buf);
}

void StatusWriter::setError(const std::string& msg)
{
    std::lock_guard<std::mutex> lk(s_mtx);
    std::string safe = sanitise(msg);
    char buf[512];
    snprintf(buf, sizeof(buf),
        "{\"state\":\"error\","
        "\"msg\":\"%s\","
        "\"dest_dir\":\"%s\","
        "\"updated_at\":%ld}",
        safe.c_str(), s_dest_dir.c_str(), now_ts());
    write_atomic(buf);
}

void StatusWriter::clear()
{
    std::lock_guard<std::mutex> lk(s_mtx);
    s_dest_dir.clear();
    remove(PATH);
}
```

- [ ] **Step 3: Compile StatusWriter.cpp in isolation**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
g++ -std=c++17 -c StatusWriter.cpp -o StatusWriter.o
```

Expected: no output. Then:

```bash
rm StatusWriter.o
```

- [ ] **Step 4: Commit**

```bash
git add StatusWriter.hpp StatusWriter.cpp
git commit -m "feat: propagate dest_dir through all StatusWriter JSON writes"
```

---

## Task 3: Integrate detect_dest_dir() into main.cpp

**Files:**
- Modify: `main.cpp`

Both the `cmd_download` static function and the `collect` block need a `dest_explicit` flag that skips detection when `--dest` (or a positional dest arg) is provided.

- [ ] **Step 1: Update cmd_download() in main.cpp**

Find the `static int cmd_download(int argc, char** argv)` function (currently around line 119). Replace its opening block:

```cpp
static int cmd_download(int argc, char** argv)
{
    // argv[0] = device
    // argv[1] (optional): dest_dir if it is not a flag and not a bare integer,
    //                     otherwise defaults to /home/Dexterlogs
    // remaining: [N] [--delete-after-download] [--stop-logging] [--last N] (any order)
    const std::string device = argv[0];
    std::string dest_dir = "/home/Dexterlogs";
    int  start        = 1;
    int  index        = -1;
    bool delete_after = false;
    bool stop_logging = false;
    int  last_n       = -1;

    if (argc > 1 && argv[1][0] != '-') {
        char* end = nullptr;
        strtol(argv[1], &end, 10);
        if (*end != '\0') {
            // Not a pure integer → treat as destination directory
            dest_dir = argv[1];
            start = 2;
        }
    }
```

with:

```cpp
static int cmd_download(int argc, char** argv)
{
    // argv[0] = device
    // argv[1] (optional): dest_dir if it is not a flag and not a bare integer
    // remaining: [N] [--delete-after-download] [--stop-logging] [--last N] (any order)
    const std::string device = argv[0];
    std::string dest_dir;
    bool dest_explicit    = false;
    int  start        = 1;
    int  index        = -1;
    bool delete_after = false;
    bool stop_logging = false;
    int  last_n       = -1;

    if (argc > 1 && argv[1][0] != '-') {
        char* end = nullptr;
        strtol(argv[1], &end, 10);
        if (*end != '\0') {
            // Not a pure integer → treat as destination directory
            dest_dir = argv[1];
            dest_explicit = true;
            start = 2;
        }
    }
```

Then, immediately before the final `return ::cmd_download(...)` at the end of that function, add the detection block:

```cpp
    if (!dest_explicit) {
        std::string err;
        dest_dir = detect_dest_dir(err);
        if (dest_dir.empty()) {
            fprintf(stderr, "Error: %s\n", err.c_str());
            return 1;
        }
    }

    return ::cmd_download(device, dest_dir, index, delete_after, stop_logging, last_n);
```

- [ ] **Step 2: Update the collect block in main()**

Find the `if (strcmp(cmd, "collect") == 0)` block. Add `bool dest_explicit = false;` at the start, and set it to `true` in the `--dest` branch:

```cpp
    if (strcmp(cmd, "collect") == 0) {
        CollectOptions opts;
        bool dest_explicit = false;
        for (int i = 2; i < argc; ++i) {
            if (strcmp(argv[i], "--delete-after-download") == 0) {
                opts.delete_after = true;
            } else if (strcmp(argv[i], "--dry-run") == 0) {
                opts.dry_run = true;
            } else if (strcmp(argv[i], "--verbose") == 0) {
                opts.verbose = true;
            } else if (strcmp(argv[i], "--stop-logging") == 0) {
                opts.stop_logging = true;
            } else if (strcmp(argv[i], "--dest") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --dest requires an argument.\n");
                    return 1;
                }
                opts.dest_dir = argv[++i];
                dest_explicit = true;
            } else if (strcmp(argv[i], "--interval") == 0) {
```

(The rest of the loop body is unchanged.) Then, between the closing `}` of the for-loop and `return cmd_collect(opts);`, add:

```cpp
        if (!dest_explicit) {
            std::string err;
            opts.dest_dir = detect_dest_dir(err);
            if (opts.dest_dir.empty()) {
                fprintf(stderr, "Error: %s\n", err.c_str());
                return 1;
            }
        }
        return cmd_collect(opts);
```

- [ ] **Step 3: Add #include "storage.hpp" to main.cpp**

At the top of `main.cpp`, after the existing includes:

```cpp
#include "storage.hpp"
```

- [ ] **Step 4: Update help text in print_help()**

Find the two lines that reference the hardcoded default and update them:

```cpp
    printf("      dest_dir defaults to auto-detected external drive when omitted.\n");
```

and:

```cpp
    printf("        --dest <dir>              Destination folder (default: auto-detect external drive)\n");
```

- [ ] **Step 5: Build**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make clean && make
```

Expected: compiles with no errors. Binary `xoraya-cli` produced.

- [ ] **Step 6: Smoke test — no drive plugged in**

```bash
./xoraya-cli collect --dry-run
```

Expected (on a machine with no drive under `/media/<user>/`):

```
Error: no external drive found under /media/alexander/
       Plug in a USB drive or SSD and try again.
```

- [ ] **Step 7: Smoke test — explicit --dest still works**

```bash
./xoraya-cli collect --dest /tmp/test-dl --dry-run --verbose
```

Expected: scan proceeds, dest is `/tmp/test-dl` (no auto-detection triggered).

- [ ] **Step 8: Commit**

```bash
git add main.cpp
git commit -m "feat: auto-detect external drive when --dest is omitted"
```

---

## Task 4: Pass dest_dir to StatusWriter::setScanning() in collector.cpp

**Files:**
- Modify: `collector.cpp`

- [ ] **Step 1: Update the setScanning() call in run_pass()**

In `collector.cpp`, find the `run_pass()` function. The current call (around line 128):

```cpp
    StatusWriter::setScanning();
```

Change to:

```cpp
    StatusWriter::setScanning(opts.dest_dir);
```

- [ ] **Step 2: Build**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make
```

Expected: no errors.

- [ ] **Step 3: Commit**

```bash
git add collector.cpp
git commit -m "feat: pass dest_dir to StatusWriter::setScanning for JSON propagation"
```

---

## Task 5: Update screen_daemon.py to read dest_dir from JSON

**Files:**
- Modify: `screen/screen_daemon.py`

- [ ] **Step 1: Update count_local_mf4() to accept a folder parameter**

Find the `count_local_mf4()` function (currently around line 70):

```python
def count_local_mf4():
    try:
        return sum(1 for f in os.listdir(DEST_FOLDER) if f.endswith('.mf4'))
    except OSError:
        return 0
```

Replace with:

```python
def count_local_mf4(folder=None):
    path = folder or DEST_FOLDER
    try:
        return sum(1 for f in os.listdir(path) if f.endswith('.mf4'))
    except OSError:
        return 0
```

- [ ] **Step 2: Pass dest from JSON to count_local_mf4() in the IDLE state**

In the `elif state == State.IDLE:` block, find the section where `status = read_xoraya_status()` is called and `render_idle_status(...)` is built. Currently it calls `count_local_mf4()` with no arguments. Update the surrounding code to:

```python
            status = read_xoraya_status()
            device = status.get('device', '') if status else ''
            dest   = status.get('dest_dir', None) if status else None

            if idle_page == 0:
                oled.display(render_idle_status(
                    device=device,
                    net_ok=net_ok,
                    logger_files=status.get('files_total', 0) if status else 0,
                    local_files=count_local_mf4(dest),
                    last_upload=done_info.get('last_upload', 'never'),
                    sw1=cur_sw1,
                    sw2=cur_sw2,
                    error_badge=error_badge,
                ))
```

- [ ] **Step 3: Syntax-check the file**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
python3 -c "
import ast
with open('screen/screen_daemon.py') as f: src = f.read()
ast.parse(src)
print('Syntax OK')
"
```

Expected: `Syntax OK`

- [ ] **Step 4: Commit**

```bash
git add screen/screen_daemon.py
git commit -m "feat: read dest_dir from status JSON in screen daemon"
```

---

## Self-Review Checklist

- [x] `detect_dest_dir()` — scans `/media/<user>/`, checks `/proc/mounts`, returns error on 0 or 2+ drives
- [x] Error messages include the base path and list of found drives (multiple case)
- [x] `dest_explicit` flag in both `cmd_download` and collect block — `--dest` bypasses detection
- [x] `s_dest_dir` static in StatusWriter — set by `setScanning()`, included in all JSON writes
- [x] `setScanning(dest_dir = "")` — default keeps existing callers that omit the arg working
- [x] `collector.cpp` passes `opts.dest_dir` to `setScanning()` — no changes needed in `downloader.cpp`
- [x] `count_local_mf4(folder=None)` — falls back to `DEST_FOLDER` env var when JSON absent
- [x] `clear()` resets `s_dest_dir` — clean state for next run
- [x] Makefile updated with `storage.cpp`
- [x] Help text updated to reflect auto-detection
