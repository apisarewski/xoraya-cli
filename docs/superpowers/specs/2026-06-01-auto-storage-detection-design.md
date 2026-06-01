# Auto Storage Detection — Design Spec

**Date:** 2026-06-01
**Status:** Approved — ready for implementation

---

## Overview

Replace the hardcoded `--dest` default (`/home/Dexterlogs`) with automatic detection of a mounted external drive. The destination folder is always `<mount_point>/Dexterlogs/`, created if absent.

The detected path is propagated to the screen daemon via the existing status JSON so it can count local `.mf4` files correctly.

---

## Behaviour

| Situation | Result |
|-----------|--------|
| `--dest` explicitly passed | Use it as-is (existing behaviour, unchanged) |
| No `--dest`, 1 external drive mounted | Use `/media/<user>/<DRIVE>/Dexterlogs/` |
| No `--dest`, 0 external drives found | Exit 1 with `Error: no external drive found under /media/<user>/` |
| No `--dest`, 2+ external drives found | Exit 1 with `Error: multiple external drives found — plug in only one` |

The same rules apply to the `collect` command. The `download` command also benefits: when no `dest_dir` is given, it uses the detected drive instead of the hardcoded default.

---

## Architecture

```
main.cpp
  │
  ├─ --dest passed? → use it directly
  │
  └─ --dest absent? → detect_dest_dir()
                          │
                          ├─ 0 drives → error, exit 1
                          ├─ 2+ drives → error, exit 1
                          └─ 1 drive  → /media/<user>/<DRIVE>/Dexterlogs/
                                            │
                                            ▼
                                     collector.cpp
                                     (mkdir_p already handles creation)
                                            │
                                            ▼
                                     StatusWriter
                                     (writes dest_dir to JSON)
                                            │
                                            ▼
                                     screen_daemon.py
                                     (reads dest_dir from JSON to count .mf4 files)
```

---

## New Files

### `storage.hpp`

```cpp
#pragma once
#include <string>

/**
 * Detects the single mounted external drive and returns
 * "<mount>/Dexterlogs" as the destination directory.
 *
 * Returns empty string and sets err on failure (0 or 2+ drives found).
 */
std::string detect_dest_dir(std::string& err);
```

### `storage.cpp`

Detection logic:

1. Get the current username via `getpwuid(getuid())->pw_name`
2. Build base path `/media/<username>/`
3. Open with `opendir`; if the directory doesn't exist, treat as 0 drives found
4. For each directory entry (skip `.` and `..`):
   - Build candidate path `/media/<username>/<entry>/`
   - Check `/proc/mounts` to confirm it is a real mount point (the string appears as a mount target in `/proc/mounts`)
5. Count valid candidates:
   - 0 → `err = "no external drive found under /media/<username>/"`, return `""`
   - 2+ → `err = "multiple external drives found — plug in only one"`, return `""`
   - 1 → return `"/media/<username>/<entry>/Dexterlogs"`

**No subprocess calls.** All detection is done with POSIX APIs (`opendir`, `/proc/mounts`).

---

## Modified Files

### `main.cpp`

Two call sites where `dest_dir` / `opts.dest_dir` gets its default value. In both cases, introduce a `bool dest_explicit = false` flag that is set to `true` whenever `--dest` (or a positional dest argument) is parsed. Detection runs only when `dest_explicit` is `false` after arg parsing.

1. **`cmd_download` path** — after arg parsing, if `!dest_explicit`:
   ```cpp
   if (!dest_explicit) {
       std::string err;
       dest_dir = detect_dest_dir(err);
       if (dest_dir.empty()) {
           fprintf(stderr, "Error: %s\n", err.c_str());
           return 1;
       }
   }
   ```

2. **`cmd_collect` path** — same pattern: after arg parsing, if `!dest_explicit`, call `detect_dest_dir()` and assign the result to `opts.dest_dir`. Fail with exit 1 if detection returns empty.

`--dest` (or positional dest arg for `download`) sets `dest_explicit = true` and bypasses detection entirely.

### `StatusWriter.hpp` / `StatusWriter.cpp`

Add `dest_dir` parameter to `setScanning()` and `setDevice()`:

```cpp
static void setScanning(const std::string& dest_dir = "");
static void setDevice(const std::string& name, int total_files,
                      const std::string& dest_dir = "");
```

Both methods add `"dest_dir":"<path>"` to the JSON when `dest_dir` is non-empty. Existing callers that omit the parameter continue to work (default `""`).

### `collector.cpp`

Pass `opts.dest_dir` to the updated `StatusWriter::setScanning()` and `StatusWriter::setDevice()` calls.

### `screen_daemon.py`

`count_local_mf4()` currently reads `DEST_FOLDER` (env var). Update it to:
1. Read `dest_dir` from the current `read_xoraya_status()` result if present
2. Fall back to `DEST_FOLDER` env var if the JSON is absent or has no `dest_dir`

### `Makefile`

Add `storage.cpp` to `SRCS`.

---

## Error Messages

```
Error: no external drive found under /media/pi/
       Plug in a USB drive or SSD and try again.

Error: multiple external drives found — plug in only one.
       Found: /media/pi/SSD1, /media/pi/USB2
```

The second message lists the drives found to help the user identify them.

---

## Out of Scope

- Detection on non-Pi Linux machines (assumes `/media/<username>/` convention from udisks2)
- Drive health checks or filesystem type validation
- Hot-plug detection during a running `collect --interval` loop (drive must be present at start)
- Configuring a custom subdirectory name (always `Dexterlogs`)
