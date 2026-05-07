# Newest-First Download Order + --last N Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Make `download` and `collect` download measurements newest-first by default, add `--last N` to both commands, and change the default destination to `/home/Dexterlogs`.

**Architecture:** Reverse the `targets` build loop in `download_gen2` and `copy_gen3`. Add a `last_n` parameter that propagates from CLI → `cmd_download` → the two inner functions. No new files needed.

**Tech Stack:** C++17, X2E Linux SDK (`libxorayasdk`), Makefile build

---

## File Map

| File | What changes |
|---|---|
| `downloader.cpp` | `download_gen2` and `copy_gen3`: reverse loop, apply `last_n` slice |
| `downloader.hpp` | Add `int last_n = -1` to `cmd_download` |
| `collector.hpp` | Add `int last_n = -1` to `CollectOptions`; change `dest_dir` default |
| `collector.cpp` | Pass `opts.last_n` to both `cmd_download` calls |
| `main.cpp` | Parse `--last N` for `download` and `collect`; update help text |

---

### Task 1: Reverse the targets loop in `download_gen2`

**Files:**
- Modify: `downloader.cpp` (around line 568 — `download_gen2` signature and targets block at lines 592–604)

- [ ] **Step 1: Add `last_n` parameter to `download_gen2` signature**

Change the function signature from:
```cpp
static int download_gen2(LoggerCtrl& ctrl,
                          const std::string& device_name,
                          const std::string& dest_dir,
                          int index,
                          bool delete_after)
```
to:
```cpp
static int download_gen2(LoggerCtrl& ctrl,
                          const std::string& device_name,
                          const std::string& dest_dir,
                          int index,
                          bool delete_after,
                          int last_n = -1)
```

- [ ] **Step 2: Replace the targets build loop for the "all" case**

Find this block (inside `download_gen2`, the `if (index < 0)` branch):
```cpp
    if (index < 0) {
        targets.reserve(count);
        for (size_t i = 0; i < count; ++i)
            targets.push_back(all.get(i));
        printf("Downloading %zu measurement(s)...\n", count);
    } else {
```

Replace with:
```cpp
    if (index < 0) {
        size_t start = (last_n > 0 && static_cast<size_t>(last_n) < count)
                       ? count - static_cast<size_t>(last_n)
                       : 0;
        targets.reserve(count - start);
        for (size_t i = count; i-- > start; )
            targets.push_back(all.get(i));
        printf("Downloading %zu measurement(s)...\n", targets.size());
    } else {
```

- [ ] **Step 3: Build and check for compile errors**

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make 2>&1 | head -30
```
Expected: compile succeeds (possibly with unused-parameter warning on `last_n` in `copy_gen3` — fix in Task 2).

- [ ] **Step 4: Commit**

```bash
git add downloader.cpp
git commit -m "feat: reverse download order and add last_n to download_gen2"
```

---

### Task 2: Reverse the targets loop in `copy_gen3`

**Files:**
- Modify: `downloader.cpp` (around line 761 — `copy_gen3` signature and targets block at lines 783–795)

- [ ] **Step 1: Add `last_n` parameter to `copy_gen3` signature**

Change:
```cpp
static int copy_gen3(LoggerCtrl& ctrl,
                      const std::string& dest_dir,
                      int index,
                      bool delete_after)
```
to:
```cpp
static int copy_gen3(LoggerCtrl& ctrl,
                      const std::string& dest_dir,
                      int index,
                      bool delete_after,
                      int last_n = -1)
```

- [ ] **Step 2: Replace the targets build loop for the "all" case**

Find this block (inside `copy_gen3`, the `if (index < 0)` branch):
```cpp
    if (index < 0) {
        targets.reserve(count);
        for (size_t i = 0; i < count; ++i)
            targets.push_back(all.get(i));
        printf("Copying %zu measurement(s)...\n", count);
    } else {
```

Replace with:
```cpp
    if (index < 0) {
        size_t start = (last_n > 0 && static_cast<size_t>(last_n) < count)
                       ? count - static_cast<size_t>(last_n)
                       : 0;
        targets.reserve(count - start);
        for (size_t i = count; i-- > start; )
            targets.push_back(all.get(i));
        printf("Copying %zu measurement(s)...\n", targets.size());
    } else {
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | head -30
```
Expected: clean compile.

- [ ] **Step 4: Commit**

```bash
git add downloader.cpp
git commit -m "feat: reverse download order and add last_n to copy_gen3"
```

---

### Task 3: Thread `last_n` through `cmd_download`

**Files:**
- Modify: `downloader.hpp` (line 39 — `cmd_download` signature)
- Modify: `downloader.cpp` (line 844 — `cmd_download` definition + dispatch calls at lines 918, 923)

- [ ] **Step 1: Add `last_n` to `cmd_download` declaration in `downloader.hpp`**

Change:
```cpp
int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index = -1,
                 bool delete_after = false,
                 bool stop_logging = false);
```
to:
```cpp
int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index = -1,
                 bool delete_after = false,
                 bool stop_logging = false,
                 int last_n = -1);
```

- [ ] **Step 2: Add `last_n` to `cmd_download` definition in `downloader.cpp`**

Change (around line 844):
```cpp
int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index,
                 bool delete_after,
                 bool stop_logging)
```
to:
```cpp
int cmd_download(const std::string& device,
                 const std::string& dest_dir,
                 int index,
                 bool delete_after,
                 bool stop_logging,
                 int last_n)
```

- [ ] **Step 3: Pass `last_n` to both dispatch calls**

Find the two dispatch lines inside `cmd_download` (around lines 918 and 923):
```cpp
            rc = download_gen2(ctrl, logger_name, dest_dir, index, delete_after);
```
and:
```cpp
            rc = copy_gen3(ctrl, dest_dir, index, delete_after);
```

Change to:
```cpp
            rc = download_gen2(ctrl, logger_name, dest_dir, index, delete_after, last_n);
```
and:
```cpp
            rc = copy_gen3(ctrl, dest_dir, index, delete_after, last_n);
```

- [ ] **Step 4: Build**

```bash
make 2>&1 | head -30
```
Expected: clean compile.

- [ ] **Step 5: Commit**

```bash
git add downloader.hpp downloader.cpp
git commit -m "feat: thread last_n through cmd_download to gen2/gen3 dispatch"
```

---

### Task 4: Add `last_n` to `CollectOptions` and pass it through `collector`

**Files:**
- Modify: `collector.hpp` (add `last_n` field, change `dest_dir` default)
- Modify: `collector.cpp` (pass `opts.last_n` to both `cmd_download` calls)

- [ ] **Step 1: Update `CollectOptions` in `collector.hpp`**

Change:
```cpp
struct CollectOptions {
    std::string dest_dir     = "./downloads";
    bool        delete_after  = false;
    int         interval_s    = -1;
    std::string device_filter;
    bool        dry_run       = false;
    bool        verbose       = false;
    bool        stop_logging  = false;
};
```
to:
```cpp
struct CollectOptions {
    std::string dest_dir     = "/home/Dexterlogs";
    bool        delete_after  = false;
    int         interval_s    = -1;
    std::string device_filter;
    bool        dry_run       = false;
    bool        verbose       = false;
    bool        stop_logging  = false;
    int         last_n        = -1;   // -1 = all; >0 = only the N most recent
};
```

- [ ] **Step 2: Pass `opts.last_n` to both `cmd_download` calls in `collector.cpp`**

Find the verbose path (around line 174):
```cpp
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging);
```
Change to:
```cpp
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging, opts.last_n);
```

Find the simple mode path (around line 186):
```cpp
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging);
```
Change to:
```cpp
            int rc = cmd_download(lg.name, opts.dest_dir, -1, opts.delete_after, opts.stop_logging, opts.last_n);
```

- [ ] **Step 3: Build**

```bash
make 2>&1 | head -30
```
Expected: clean compile.

- [ ] **Step 4: Commit**

```bash
git add collector.hpp collector.cpp
git commit -m "feat: add last_n to CollectOptions, change default dest to /home/Dexterlogs"
```

---

### Task 5: Parse `--last N` in `main.cpp` and update help text

**Files:**
- Modify: `main.cpp` (help text, `cmd_download` parser, `collect` parser)

- [ ] **Step 1: Update help text**

Find the `print_usage` function. Make these three changes:

Change the download usage line:
```cpp
    printf("  download <device> <dest_dir> [N] [--delete-after-download] [--stop-logging]\n");
```
to:
```cpp
    printf("  download <device> <dest_dir> [N] [--delete-after-download] [--stop-logging] [--last N]\n");
```

Change the collect `--dest` default:
```cpp
    printf("        --dest <dir>              Destination folder (default: ./downloads)\n");
```
to:
```cpp
    printf("        --dest <dir>              Destination folder (default: /home/Dexterlogs)\n");
```

Add `--last N` description under the collect options (after the `--stop-logging` line):
```cpp
    printf("        --last <N>                Download only the N most recent measurements (N >= 1)\n");
```

Also add `--last N` to the download description block:
```cpp
    printf("      With --last N: download only the N most recent measurements.\n");
```

- [ ] **Step 2: Add `--last N` parsing to the `download` command parser**

In the `static int cmd_download(int argc, char** argv)` function, add `last_n` variable and parsing. The tricky part: `--last N` and a specific index `N` are mutually exclusive.

Replace the full function:
```cpp
static int cmd_download(int argc, char** argv)
{
    // argv[0] = device, argv[1] = dest_dir
    // argv[2..] = optional: [N] [--delete-after-download] [--stop-logging] [--last N] (any order)
    const std::string device   = argv[0];
    const std::string dest_dir = argv[1];
    int  index        = -1;
    bool delete_after = false;
    bool stop_logging = false;
    int  last_n       = -1;

    for (int i = 2; i < argc; ++i) {
        if (strcmp(argv[i], "--delete-after-download") == 0) {
            delete_after = true;
        } else if (strcmp(argv[i], "--stop-logging") == 0) {
            stop_logging = true;
        } else if (strcmp(argv[i], "--last") == 0) {
            if (i + 1 >= argc) {
                fprintf(stderr, "Error: --last requires an argument.\n");
                return 1;
            }
            char* end = nullptr;
            long n = strtol(argv[++i], &end, 10);
            if (*end != '\0' || n < 1) {
                fprintf(stderr, "Error: --last must be an integer >= 1.\n");
                return 1;
            }
            last_n = static_cast<int>(n);
        } else {
            char* end = nullptr;
            long n = strtol(argv[i], &end, 10);
            if (*end != '\0' || n < 0) {
                fprintf(stderr, "Error: unrecognized argument '%s'.\n", argv[i]);
                fprintf(stderr, "Usage: download <device> <dest_dir> [N] [--delete-after-download] [--stop-logging] [--last N]\n");
                return 1;
            }
            index = static_cast<int>(n);
        }
    }

    if (index >= 0 && last_n >= 1) {
        fprintf(stderr, "Error: a specific index and --last are mutually exclusive.\n");
        return 1;
    }

    return ::cmd_download(device, dest_dir, index, delete_after, stop_logging, last_n);
}
```

- [ ] **Step 3: Add `--last N` parsing to the `collect` command parser**

Inside the `if (strcmp(cmd, "collect") == 0)` block, add after the `--device` block:
```cpp
            } else if (strcmp(argv[i], "--last") == 0) {
                if (i + 1 >= argc) {
                    fprintf(stderr, "Error: --last requires an argument.\n");
                    return 1;
                }
                char* end = nullptr;
                long n = strtol(argv[++i], &end, 10);
                if (*end != '\0' || n < 1) {
                    fprintf(stderr, "Error: --last must be an integer >= 1.\n");
                    return 1;
                }
                opts.last_n = static_cast<int>(n);
```

Also update the error message at the end of the collect parser to include `[--last N]`:
```cpp
                fprintf(stderr, "Usage: %s collect [--dest <dir>] [--delete-after-download] "
                                "[--interval <s>] [--device <ip>] [--dry-run] [--verbose] "
                                "[--stop-logging] [--last N]\n", argv[0]);
```

- [ ] **Step 4: Build**

```bash
make 2>&1 | head -30
```
Expected: clean compile, no warnings.

- [ ] **Step 5: Commit**

```bash
git add main.cpp
git commit -m "feat: add --last N flag to download and collect commands"
```

---

### Task 6: Manual verification

- [ ] **Step 1: Run a scan to find the logger**

```bash
./xoraya-cli scan
```
Expected: logger visible (e.g. `DX11252-1`).

- [ ] **Step 2: List measurements to know the count**

```bash
./xoraya-cli list DX11252-1
```
Expected: table with indices 0..N. Note which is the newest (highest index).

- [ ] **Step 3: Verify newest-first order on download**

```bash
./xoraya-cli download DX11252-1 /tmp/test-order --verbose 2>&1 | grep "Measurement"
```
Expected: measurement indices printed in descending order (e.g. `→ Measurement 4`, `→ Measurement 3`, ...) instead of ascending.

- [ ] **Step 4: Test `--last 2` on download**

```bash
./xoraya-cli download DX11252-1 /tmp/test-last --last 2
```
Expected: only 2 measurements downloaded, newest first. Verify with `ls -lh /tmp/test-last`.

- [ ] **Step 5: Test mutual exclusion on download**

```bash
./xoraya-cli download DX11252-1 /tmp/test-last 3 --last 2
```
Expected: `Error: a specific index and --last are mutually exclusive.`

- [ ] **Step 6: Test `--last 0` rejection**

```bash
./xoraya-cli download DX11252-1 /tmp/test-last --last 0
```
Expected: `Error: --last must be an integer >= 1.`

- [ ] **Step 7: Test `--last N > total` on download**

If the logger has 5 measurements:
```bash
./xoraya-cli download DX11252-1 /tmp/test-last --last 100
```
Expected: all measurements downloaded (no error).

- [ ] **Step 8: Test `--last` on collect**

```bash
./xoraya-cli collect --last 1 --dry-run --verbose
```
Expected: only 1 measurement per logger shown, the newest.

- [ ] **Step 9: Verify default dest changed**

```bash
./xoraya-cli --help | grep default
```
Expected: `default: /home/Dexterlogs` (not `./downloads`).

- [ ] **Step 10: Final commit if any last fixes, then tag**

```bash
git log --oneline -6
```
Expected: 5 commits from Tasks 1–5 visible cleanly.
