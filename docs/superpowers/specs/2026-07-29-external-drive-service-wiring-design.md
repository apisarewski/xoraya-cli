# Wire External-Drive Auto-Detection into the Pi Services — Design Spec

**Date:** 2026-07-29
**Status:** Approved — ready for implementation

---

## Overview

`detect_dest_dir()` (added in [2026-06-01-auto-storage-detection-design.md](2026-06-01-auto-storage-detection-design.md)) already scans `/media/<user>/` and resolves the destination to `<mount>/Dexterlogs` when exactly one external drive is present. On the Raspberry Pi station, however, this logic is dead code in production: `xoraya-collect.service` passes `--dest ${DEST}` (a static path on the internal SD card), and `databridge.service` (a separate proprietary binary, `DataBridgeCLI`, not part of this repo) reads `DEST` from its own static conf file pointing at the same internal folder.

This spec wires the existing detection logic into both services so that:
- **Download** only writes to an auto-detected external drive — never to internal storage.
- **Upload** reads from the same auto-detected drive.
- If no drive (or more than one) is present, the corresponding operation fails outright and the OLED screen shows an error. There is no fallback to internal storage.

---

## Behaviour

| Situation | Download (`xoraya-collect.service`) | Upload (`databridge.service`) |
|---|---|---|
| Exactly 1 external drive mounted | Uses `<mount>/Dexterlogs` (existing `detect_dest_dir()`) | Uses the same `<mount>/Dexterlogs`, resolved via `xoraya-cli detect-dest` |
| 0 drives mounted | Fails immediately, no download attempted, OLED shows error | Service fails to start, OLED shows error |
| 2+ drives mounted | Fails immediately (ambiguous), OLED shows error | Service fails to start, OLED shows error |
| Drive removed while a service is already running | Not handled (see Out of Scope) | Not handled (see Out of Scope) |

Retry policy: no in-app polling/retry loop. Existing systemd `Restart=on-failure` (`xoraya-collect.service`, `RestartSec=5`) and `Restart=always` (`databridge.service`, `RestartSec=60`) already retry the whole unit periodically, which is sufficient — as soon as a single drive is plugged in, the next restart succeeds.

Folder name stays **`Dexterlogs`** (plural) — matches existing code, conf files, and the folder already used on Pi deployments; not renamed.

---

## Architecture

```
xoraya-collect.service                     databridge.service
        │                                          │
        │ ExecStart (no --dest arg)                │ ExecStartPre:
        ▼                                           │   xoraya-cli detect-dest
   xoraya-cli collect --interval ...                │   → writes /run/xoraya-dest.env
        │                                            │        (DEST=<mount>/Dexterlogs)
        │ !dest_explicit                             ▼
        ▼                                     EnvironmentFile=/run/xoraya-dest.env
   detect_dest_dir()  ◄────────── same function ────┘
        │                         also called by detect-dest
        ├─ 0 or 2+ drives → StatusWriter::setError(err); exit 1
        │                         │
        │                         ▼
        │                  /tmp/xoraya-status.json {"state":"error",...}
        │                         │
        └─ 1 drive → proceed              screen_daemon.py (DOWNLOADING state)
                                            already reads status=="error" → render_error
                                            (no Python change needed)

                                   ExecStartPre failure → databridge.service never starts
                                   → is_service_active('databridge') == False
                                   → screen_daemon.py (UPLOADING state) already treats
                                     "not active" as failure → render_error
                                     (no Python change needed)
```

Both services ultimately resolve the drive through the exact same `detect_dest_dir()` C++ function — no duplicated detection logic.

---

## New/Modified Files

### `main.cpp` (modified)

At the two existing `detect_dest_dir()` failure sites (the `download` command parsing, and the `collect` command parsing), add a call to `StatusWriter::setError(err)` before `return 1`. Today these sites only `fprintf(stderr, ...)`, so a failure here is invisible to the OLED daemon — the service just goes inactive and the screen silently falls back to IDLE.

```cpp
if (!dest_explicit) {
    std::string err;
    dest_dir = detect_dest_dir(err);
    if (dest_dir.empty()) {
        fprintf(stderr, "Error: %s\n", err.c_str());
        StatusWriter::setError(err);   // new
        return 1;
    }
}
```

Same change at the `collect` call site (`opts.dest_dir`).

Requires adding `#include "StatusWriter.hpp"` to `main.cpp` (not currently included there; `StatusWriter` is otherwise only used inside `collector.cpp`/`downloader.cpp`, but the dest-resolution failure happens in `main.cpp` before those functions are ever called, so this is the correct/only place to report it).

### New subcommand: `xoraya-cli detect-dest`

Added to `main.cpp`'s command dispatch, next to `scan`/`list`/`download`/etc. Behaviour:
- Calls `detect_dest_dir(err)` directly (already linked into the binary via `storage.cpp`/`storage.hpp`; no new source file, no `Makefile` change).
- Success → print the resolved path to stdout, exit 0.
- Failure → print `err` to stderr, exit 1.

No `StatusWriter` call needed here — the OLED's `UPLOADING` error path is driven by `is_service_active('databridge')`, not the status JSON (see `screen_daemon.py`'s existing logic), so a plain non-zero exit is sufficient to surface the failure.

Documented in `print_help()`.

### `xoraya-collect.service` / `xoraya-collect.pi.service` (modified)

Remove `--dest ${DEST}` from `ExecStart`. The binary's own `detect_dest_dir()` then runs automatically (`dest_explicit == false`).

```
ExecStart=systemd-inhibit --what=sleep --who=xoraya-collect --why="Download in progress" \
  /usr/local/bin/xoraya-cli collect --interval ${INTERVAL} --delete-after-download --verbose
```

### `xoraya-collect.conf` / `xoraya-collect.conf.example` (modified)

Remove the `DEST=` line (no longer read). Keep `INTERVAL=`.

### `databridge.service` (modified)

Add an `ExecStartPre` that resolves the drive via the new subcommand and writes it to a runtime env file, then load that file after the static conf so it overrides `DEST`:

```
ExecStartPre=/bin/sh -c 'echo "DEST=$(/usr/local/bin/xoraya-cli detect-dest)" > /run/xoraya-dest.env'
EnvironmentFile=/home/pi/xoraya_cli/databridge.conf
EnvironmentFile=/run/xoraya-dest.env
```

If `detect-dest` exits non-zero, the `sh -c` pipeline fails, `ExecStartPre` fails, and systemd never runs `ExecStart` — the unit fails to start. Its stderr (the `detect_dest_dir()` error message) lands in `journalctl -u databridge`, which is exactly the hint `screen_daemon.py`'s `render_error` already points to.

### `databridge.conf.example` (modified)

Remove the `DEST=` line (now generated at runtime, not user-edited).

### `docs/deploy-raspberry-pi.md` (modified)

- Replace the `mkdir -p /home/pi/Dexterlogs` / static-path instructions with an explanation of the auto-detected external-drive flow.
- Document the `xoraya-cli detect-dest` subcommand and the `databridge.service` `ExecStartPre` wiring.

### `CLAUDE_cli.md` (modified)

Note that `detect_dest_dir()` is now actually exercised in production for both download (directly) and upload (via the `detect-dest` bridge subcommand), closing the gap left by the 2026-06-01 spec's "wire into systemd" omission.

---

## Error Messages

Unchanged from the existing `detect_dest_dir()` implementation:

```
Error: no external drive found under /media/pi/
       Plug in a USB drive or SSD and try again.

Error: multiple external drives found — plug in only one.
       Found: /media/pi/SSD1, /media/pi/USB2
```

For download, this message now also lands in `/tmp/xoraya-status.json` (`msg` field, truncated to 20 chars by `screen_daemon.py` for the OLED badge). For upload, it's visible via `journalctl -u databridge` per the existing error screen's hint text.

---

## Out of Scope

- **Hot-unplug during a running operation.** The drive is resolved once per process start (`collect`'s own start, or `detect-dest`'s single invocation before `databridge` starts). If the drive is removed while a `--interval` download loop or an upload is already in progress, this is not detected mid-run; the next service restart will pick up the new state correctly.
- **Renaming `Dexterlogs`.** Stays plural, matches all existing references.
- **Modifying `DataBridgeCLI` itself.** It remains an unmodified external binary; all drive-resolution logic stays in `xoraya-cli`.
- **Any change to the "0 loggers found" network-scan behaviour** (that remains "not an error," per existing `collect` semantics) — this spec only concerns the destination-drive resolution, not the datalogger network scan.
