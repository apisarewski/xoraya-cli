# Collect workflow

## Purpose

The `collect` command is designed for **automated, unattended data retrieval** from one or more Xoraya loggers. It combines the scan, connect, and download steps into a single command, with optional looping.

---

## Basic workflow

Each execution of `collect` (one "pass") follows this sequence:

```
1. Scan the network for 2 seconds
2. Filter results if --device <ip> is specified
3. For each logger found:
   a. Connect
   b. Retrieve alias name via GetName()
   c. Read the HDD measurement list
   d. Download all measurements (index 0 to N)
   e. Optionally delete measurements if --delete-after-download
   f. Disconnect
4. Print a summary (total loggers, successes, failures)
5. If --interval is set: wait, then repeat from step 1
```

If a logger fails (connection error, SDK error), the error is printed and the command continues with the next logger. The final return code is `1` if **any** logger failed.

---

## Output modes

### Simple mode (default)

One line per logger, printed after the download completes:

```
[2026-04-16 13:00:34 UTC] Scanning network (2 s)...
  DX11252-2                 → OK
  DX11242-1                 → ERROR

  2 logger(s): 1 succeeded, 1 failed.
```

The per-measurement progress output of `cmd_download` is suppressed in this mode (redirected to `/dev/null` via `dup2`). Only the final OK/ERREUR status is shown.

### Verbose mode (`--verbose`)

Full download output for each logger, including per-measurement progress bars:

```
[2026-04-16 13:00:34 UTC] Scanning network (2 s)...

--- DX11252-2 (192.168.2.1) ---
Connecting to 'DX11252-2'...
Connected. Type: Gen2 (engine::Download + MF4)
Downloading 12 measurement(s)...

  → Measurement 0
   45.2%   12 Mbit/s  ETA: 32s
  ✓  47185920 / 47185920 bytes  11 Mbit/s avg.
  ...

  1 logger(s): 1 succeeded.
```

### Dry-run mode (`--dry-run`)

Prints what would happen without making any connection or transfer:

```
[2026-04-16 12:59:50 UTC] Scanning network (2 s)...
  [DRY-RUN] 1 logger(s) that would be processed:
    DX11252-2                 192.168.2.1      → download to /data/logs
  No action taken.
```

---

## Loop mode (`--interval`)

When `--interval <s>` is specified, `collect` runs indefinitely, re-scanning and re-downloading every `s` seconds (minimum 1):

```bash
./xoraya-cli collect --dest /data/logs --interval 300
```

Between passes:

```
  1 logger(s): 1 succeeded.

Next scan in 300 second(s). Ctrl+C to stop.
```

**Interruption:** press Ctrl+C at any time. If a download is in progress, it is cancelled cleanly (via `ForceCancel()`) and the process exits after the current measurement finishes aborting. The partial file for the interrupted measurement should be considered incomplete.

**No deduplication:** measurements already downloaded in a previous pass are downloaded again. If you want to avoid re-downloading already-collected data, use `--delete-after-download`.

---

## Filtering with `--device`

`--device` accepts an **exact IP address**. Only the logger whose IP matches is processed; all others discovered by the scan are ignored.

```bash
./xoraya-cli collect --device 192.168.2.1 --dest /data/logs
```

> Note: `--device` does **not** accept a logger name, only an IP address. Use `scan` to find the IP for a given logger name.

---

## Combining `--interval` and `--delete-after-download`

This combination is the standard pattern for a recurring collector that does not re-download already-collected measurements:

```bash
./xoraya-cli collect \
    --dest /data/logs \
    --interval 300 \
    --delete-after-download
```

**Deletion safety rules:**
1. Deletion only occurs after a fully successful download of a measurement.
2. If the download fails for any reason, the measurement is **not** deleted.
3. If the deletion itself fails (after a successful download), the command reports an error and returns `1` for that logger.

---

## No-logger case

If no logger is found on the network (or none matches the `--device` filter), `collect` prints an informational message and returns `0` — this is not an error:

```
[2026-04-16 13:00:34 UTC] Scanning network (2 s)...
  No logger found.
```

In loop mode, the next scan is scheduled normally.

---

## Scripting and cron

`collect` is designed to be called from a script or cron job. Use the built-in `--interval` for daemon-like operation, or call it from `cron` for external scheduling:

```cron
# Download every 10 minutes, keep a log
*/10 * * * * /path/to/xoraya-cli collect --dest /data/logs --delete-after-download >> /var/log/xoraya-collect.log 2>&1
```

Return code `0` means all loggers succeeded (or no loggers were found). Return code `1` means at least one logger failed — useful for alerting in scripts.
