# Usage

## Synopsis

```
xoraya-cli <command> [options]
xoraya-cli --help
```

A `<device>` argument accepts either the logger's **alias name** (e.g. `DX11252-2`) or its **IP address** (e.g. `192.168.2.1`). Use `scan` to discover names and IPs.

---

## Commands

### `scan` — Network discovery

```bash
./xoraya-cli scan
```

Broadcasts on the local network for 2 seconds and lists all Xoraya loggers that respond.

**Output:**

```
Name                      IP               Firmware             State              Type
-------------------------  ----------------  --------------------  ------------------  ----------
DX11252-2                 192.168.2.1      4.0b.0022            Multi-Logging       Gen2 (N4000)

1 logger(s) found.
```

**Fields:**

| Field | Description |
|---|---|
| Name | Alias name of the logger (used as `<device>` in other commands) |
| IP | IPv4 address |
| Firmware | Firmware version string |
| State | Current logging state (e.g. `Multi-Logging`, `Stopped`) |
| Type | Device family: Gen2 (N4000), DataCube, DLNcluster, Gen3, DataCubeNSeries |

**Notes:**
- The scan timeout is fixed at 2 seconds. Loggers on slow or routed networks may not respond in time.
- Gen1 devices appear in the scan output but are not supported for download or delete.

---

### `list` — List measurements on a logger

```bash
./xoraya-cli list <device>
```

Connects to the logger, temporarily stops logging if necessary, lists all measurements stored on the HDD, then restarts logging.

**Example:**

```bash
./xoraya-cli list DX11252-2
./xoraya-cli list 192.168.2.1
```

**Output:**

```
Logger: DX11252-2  (3 measurement(s))

   #  Start (UTC)          End (UTC)            Messages       Size  Type
 ----------------------------------------------------------------------------------
   0  2025-12-16 09:54:05  2025-12-16 09:54:45      24524   852.2 KB  Main
   1  2025-12-16 13:25:01  2025-12-16 13:25:28      11966   448.7 KB  Snapshot
   2  2025-12-16 13:27:47  2025-12-16 13:31:04      85637     2.7 MB  Main
```

**Fields:**

| Field | Description |
|---|---|
| # | Zero-based index, used as `N` in `download` and `delete` |
| Start / End | UTC timestamps of the measurement |
| Messages | Number of log messages recorded |
| Size | Uncompressed data size on the HDD |
| Type | `Main` (continuous recording) or `Snapshot` (triggered capture) |

**Notes:**
- All timestamps are UTC.
- If the logger is currently recording, it is stopped temporarily and restarted after the listing. This interruption is brief and handled automatically.

---

### `download` — Download measurements

```bash
./xoraya-cli download <device> <dest_dir> [N] [--delete-after-download]
```

Downloads measurements from the logger's HDD to a local directory.

**Arguments:**

| Argument | Required | Description |
|---|---|---|
| `<device>` | Yes | Logger alias name or IP address |
| `<dest_dir>` | Yes | Local destination directory (created automatically if absent) |
| `N` | No | Index of the specific measurement to download. Omit to download all. |
| `--delete-after-download` | No | Delete measurements from the logger after a successful download |

**Examples:**

```bash
# Download all measurements
./xoraya-cli download DX11252-2 /tmp/logs

# Download measurement at index 7
./xoraya-cli download DX11252-2 /tmp/logs 7

# Download all and delete afterwards
./xoraya-cli download DX11252-2 /tmp/logs --delete-after-download

# Download measurement 7 and delete it afterwards
./xoraya-cli download DX11252-2 /tmp/logs 7 --delete-after-download
```

**Behavior:**

- By default the command is **non-destructive**: no measurement is deleted from the logger.
- `--delete-after-download` only deletes after a fully successful download. If the download fails, nothing is deleted.
- The `N` and `--delete-after-download` arguments may appear in any order after `<dest_dir>`.
- There is **no deduplication**: downloading the same measurement twice will overwrite the existing file.

**Output (Gen2 example):**

```
Connecting to 'DX11252-2'...
Connected. Type: Gen2 (engine::Download + MF4)
Downloading 3 measurement(s)...

  → Measurement 0
   45.2%   12 Mbit/s  ETA: 32s
  ✓  47185920 / 47185920 bytes  11 Mbit/s avg.

  → Measurement 1
  ✓  33685504 / 33685504 bytes   9 Mbit/s avg.

  Overall average speed: 10 Mbit/s

Files available in: /tmp/logs
```

**Notes:**
- Progress information (`%`, `Mbit/s`, `ETA`) is printed to stdout, connection messages to stderr.
- For very small measurements (under ~1 MB) the throughput display shows `0 Mbit/s` — this is a display rounding artefact, not an error. The data is transferred correctly.

---

### `delete` — Delete a measurement

```bash
./xoraya-cli delete <device> <N>
```

Deletes measurement at index `N` from the logger's HDD. This operation is **irreversible**.

**Example:**

```bash
./xoraya-cli delete DX11252-2 2
```

**Output:**

```
⚠  Deleting measurement 2 from 'DX11252-2'...
   Measurement 2 deleted.
```

**Notes:**
- The index `N` must exist. Use `list` to verify available indices beforehand.
- On Gen3 devices, deletion is performed by time range (begin/end timestamps of the measurement), not by index. If a Gen3 deletion fails for one measurement, the error is reported and the command returns `1`, but subsequent measurements are still processed.

---

### `collect` — Automated multi-logger collection

See [collect.md](collect.md) for the full workflow description.

```bash
./xoraya-cli collect [--dest <dir>] [--delete-after-download] \
                     [--interval <s>] [--device <ip>] \
                     [--dry-run] [--verbose]
```

**Options:**

| Option | Default | Description |
|---|---|---|
| `--dest <dir>` | `./downloads` | Destination directory (created automatically if absent) |
| `--delete-after-download` | off | Delete measurements from logger after successful download |
| `--interval <s>` | off | Loop indefinitely, re-scanning every `s` seconds (`s` must be > 0) |
| `--device <ip>` | all loggers | Restrict to one logger by **exact IP match** |
| `--dry-run` | off | Show what would happen without performing any download |
| `--verbose` | off | Print full per-measurement progress (same output as `download`) |

**Examples:**

```bash
# One-shot collect of all loggers, default destination
./xoraya-cli collect

# Collect to a specific directory
./xoraya-cli collect --dest /data/xoraya

# Collect one specific logger
./xoraya-cli collect --device 192.168.2.1 --dest /data/xoraya

# Loop every 5 minutes, destructive
./xoraya-cli collect --dest /data/xoraya --interval 300 --delete-after-download

# Preview what would be downloaded (no actual transfer)
./xoraya-cli collect --dry-run

# Detailed output for debugging
./xoraya-cli collect --dest /tmp/test --verbose
```

---

## Output files and filename convention

Files produced by `download` and `collect` follow this naming scheme:

```
{logger_name}_{start_date}_{start_time}-{end_date}_{end_time}[_snapshot]_{split_index}.mf4
```

| Part | Format | Example |
|---|---|---|
| `logger_name` | Alias returned by `GetName()` after connection | `DX11252-2` |
| `start_date` | `YYYYMMDD` (UTC) | `20251216` |
| `start_time` | `HHmmss` (UTC) | `095405` |
| `end_date` | `YYYYMMDD` (UTC) | `20251216` |
| `end_time` | `HHmmss` (UTC) | `095445` |
| `_snapshot` | Present only for Snapshot measurements | `_snapshot` |
| `split_index` | 4-digit counter added by the WriterMf4 filter | `_0001` |
| extension | Always `.mf4` for Gen2/DataCube/DLNcluster | `.mf4` |

**Full example:**

```
DX11252-2_20251216_095405-20251216_095445_0001.mf4
DX11252-2_20251216_132501-20251216_132528_snapshot_0001.mf4
```

**Split files (measurements > 50 MB):**

Measurements exceeding 50 MB are split into multiple files. The split index increments from `_0001`:

```
DX11252-2_20251216_095405-20251216_151210_0001.mf4
DX11252-2_20251216_095405-20251216_151210_0002.mf4
DX11252-2_20251216_095405-20251216_151210_0003.mf4
```

The split index `_0001` is always present, even when no split occurs (measurement fits in a single file). This is standard WriterMf4 filter behavior.

**Gen3 / DataCubeNSeries:**

Gen3 devices use `engine::Copy` in SelfOwned mode, which applies its own internal naming scheme to the output files. The file format is native (not MF4) and the file names are determined by the SDK, not by the CLI.

**Note on the logger name vs the device argument:**

The filename always uses the **alias name** returned by `ctrl->GetName()` after connection, regardless of whether the device was addressed by name or IP. This ensures consistent filenames even when connecting by IP.
