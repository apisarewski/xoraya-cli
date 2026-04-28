# Developer notes

## Source layout

```
cli/
├── main.cpp           — Entry point, CLI argument parsing, command dispatch
├── scanner.hpp/cpp    — Network scan (CScansCommand)
├── downloader.hpp/cpp — list / download / delete / patch logic
├── collector.hpp/cpp  — collect command (scan loop + cmd_download calls)
├── mf4_patch_dlc.py   — Standalone Python validation tool for the DLC bug
└── Makefile
```

---

## Architecture

The CLI is structured as a thin dispatcher in `main.cpp` over three independent modules:

```
main.cpp
  ├── scanner.hpp    → scan_network()
  ├── downloader.hpp → cmd_list(), cmd_download(), cmd_delete(), abort_downloads()
  └── collector.hpp  → CollectOptions, cmd_collect()
       └── calls scan_network() and cmd_download() internally
```

There is no shared state between commands beyond the SDK's own globals. Each command creates its own `LoggerCtrl` and disconnects when done.

---

## Key entry points

### `scanner.cpp` — `scan_network(int timeout_ms)`

Wraps `CScansCommand::Start()`. Blocks for `timeout_ms` milliseconds, then returns a `std::vector<LoggerInfo>`. The scan timeout is hardcoded to 2000 ms in all call sites.

### `downloader.cpp` — `cmd_download()`

Public entry point for downloads. Does:
1. `LoggerClient::CreateCtrl(LCT_Universal)` → `ctrl->Connect(device)`
2. `ctrl->GetName()` → retrieves the real alias name for filename construction
3. Dispatches to `download_gen2()` or `copy_gen3()` based on `ctrl->ConnectionType()`
4. `ctrl->Disconnect()`

### `downloader.cpp` — `download_gen2()`

Handles the full Gen2 download loop. The complete sequence per measurement:

1. `HddDirMeasurement` → builds target list (`enableShadow()` called to merge stream/default queues)
2. Creates a single `MsgFilterFactory::WriterMf4` filter before the measurement loop with properties:
   - `MaxFileSize`: `"52428800"` (50 MB split threshold)
   - `SplitOnMaxSize`: `"true"`
   - `Extension`: `"mf4"` (WriterMf4 appends this + a 4-digit split counter)
3. Per-measurement: updates `Filename` on the existing filter, then runs `HddGetMeasurement`
4. Inner polling loop (200 ms intervals) exits when both are false:
   - `ctrl->IsReceiving()` — data is still arriving from the logger
   - `ctrl->GetCnt(CId_IsDataConsuming) != 0` — `FileWriterMdf` is still writing to disk
5. `cmdGet.callAgain()` — returns true if more data chunks are pending (shadow measurements); loops back to step 3 if so
6. `cmdGet.finalizeShadow()` — called once when `callAgain()` returns false and `HasShadow()` is true
7. **DLC patch** — after the full `callAgain()` loop, `patch_mf4_dlc()` is called on every split file produced for that measurement (see DLC patch section)
8. `downloaded.push_back(m)` — measurement recorded as done
9. Optional `delete_gen2()` if `--delete-after-download`

**Abort:** `g_abort` (set by `abort_downloads()`) is checked inside both the per-measurement loop and the inner polling loop. When set, the inner loop breaks and `cmd::HddStop` is sent to the logger to cleanly terminate the transfer.

### `downloader.cpp` — `copy_gen3()`

Handles Gen3 devices via `engine::Copy` (SelfOwned mode):
- `cp.setMeasurements(targets)` + `cp.SetTargetPath(dest_dir)` + `cp.Run()`
- `cp.WaitForEndOfCopy()` — blocks until complete
- Progress reported via `CopyProgress` callback (implements `engine::Copy::ICallBackHandler`)

No DLC patch is applied to Gen3 files (the bug is specific to `libxorayasdk`'s `X2eToMdfConverter`, used only in the Gen2 path).

### `downloader.cpp` — `abort_downloads()`

Called from the SIGINT handler in `collector.cpp`. Sets the `g_abort` volatile flag to `1`. The polling loop inside `download_gen2()` checks this flag every 200 ms and issues `cmd::HddStop` when it detects it.

### `collector.cpp` — `run_pass()`

Executes one collect pass. In simple (non-verbose) mode, `dup2` is used to redirect `cmd_download`'s stdout/stderr to `/dev/null`. The `FdGuard` struct saves and restores the file descriptors around the call, ensuring the per-logger `OK`/`ERREUR` line is printed on the real stdout.

---

## DLC patch — `patch_mf4_dlc()`

### Background

`libxorayasdk 1.00.0046` contains a bug in `X2eToMdfConverter::` (internal class in `FileWriterMdf`): for every fixed bus-event CG record written to MF4, byte 4 of `CAN_DataFrame` (the `DLC_field` used by the CAN viewer) is written as `0x00` instead of the actual DLC. Byte 5 (`flag2`) is written correctly by the same SDK. The bug affects 100% of fixed records in every produced file.

This SDK version cannot be upgraded: `1.00.0049` produces files ~3× too large (split threshold bug).

**Confirmed record layout (fixed CG, 30 bytes total):**

```
offset  0– 1 : rec_id         (uint16 LE, one of {1, 3, 5, 7})
offset  2– 9 : timestamp      (uint64, 100 ns since Unix epoch)
offset 10    : Asynchronous   (uint8 flag)
offset 11–29 : CAN_DataFrame  (19 bytes)
  [byte 4] = offset 15 : DLC_field  ← 0x00 due to SDK bug (PATCH TARGET)
  [byte 5] = offset 16 : flag2      ← correct DLC (PATCH SOURCE)
```

### Fix

`patch_mf4_dlc(const std::string& path)` in `downloader.cpp`:

1. Loads the file entirely into a `std::vector<uint8_t>`
2. Validates the MF4 file header: first 8 bytes must be `"MDF     "` (the ID block; unlike all other MF4 blocks it does **not** carry a `##` prefix)
3. Parses the block chain: `##HD` (always at offset 64) → `##DG` chain → CG chain → `##DL`/`##DT`
4. Builds a table mapping `rec_id → {is_vlsd, data_bytes}` from the CG chain
5. Skips DG blocks without fixed CG groups (rec_ids 1, 3, 5, 7)
6. For each fixed record: if `buf[offset+15] == 0` and `buf[offset+16] != 0`, sets `buf[offset+15] = buf[offset+16]`
7. Rewrites the buffer to disk only if patches were made

The condition `byte4 == 0 && byte5 != 0` ensures:
- Already-correct files (e.g. from the Windows SDK) are detected and skipped (no write)
- CAN error frames where both bytes are 0 are left untouched

**Integration:** called inside `download_gen2()` immediately after the `callAgain()` loop finishes for each measurement, once `CId_IsDataConsuming` has returned to zero (guaranteeing `FileWriterMdf` has closed all files). Applied to every `<base_fname>_NNNN.mf4` split file in `dest_dir`.

### Python validation tool

`mf4_patch_dlc.py` is the standalone validation script used to confirm the hypothesis before C++ integration. It produces a patched copy (original untouched), with full MF4 structure validation and 500-record spot-check. Use it to test a file outside of the normal download flow:

```bash
python3 mf4_patch_dlc.py input.mf4 output_patched.mf4
```

---

## Filename construction

Filenames are built manually in `download_gen2()`:

```cpp
// format_ts() converts timestampHiResLG_t (100 ns since Unix epoch)
// to "YYYYMMDD_HHmmss" using gmtime_r + strftime
std::string base_path = dest_dir + "/" + device_name
                        + "_" + format_ts(m.GetTimeStartHiRes())
                        + "-" + format_ts(m.GetTimeEndHiRes())
                        + (m.IsMainMeasurement() ? "" : "_snapshot");
filter->SetProperty("Filename", base_path);
```

`BuildFilename()` is not used because:
- `%index%` is not resolved by the engine in isolation
- `%FileStartDateTime%` produces a non-standard format (`2026-03-04[14.58.46]`)
- Combining `.mf4` in the path and `Extension=mf4` would produce `.mf4.mf4`

The extension is passed via `SetProperty("Extension", "mf4")`. WriterMf4 appends it plus a 4-digit split counter (`_0001`).

---

## SDK types and conventions

| SDK type | Used for |
|---|---|
| `LoggerCtrl` | Smart pointer to the logger controller interface |
| `LoggerDataRecv` | Message receiver — holds the filter chain |
| `LoggerDataRecvFilter` | A single output filter (e.g. WriterMf4) |
| `hdd::MeasurementList` | List of Gen2 measurements from `HddDirMeasurement` |
| `hdd::FinalMeasurementList` | List of Gen3 measurements from `HddDirFinalMeasurement` |
| `HiResDateTime` | Timestamp wrapper; `getTime()` returns 100 ns since Unix epoch |
| `x2e::X2Error` | Error type returned by `ctrl->GetName()`; compare with `X2Error::NoError()` |
| `x2e::Util::ErrorHdl` | Error type returned by `ctrl->DoCmd()` and `ctrl->Connect()`; check with `.IsNone()` |
| `timestampHiResLG_t` | `uint64_t`, 100 ns ticks since Unix epoch (not Windows epoch) |

Note: `X2Error` and `ErrorHdl` are **different types** with different APIs.

---

## Adding a new command

1. Implement the function in `downloader.cpp` (if SDK-related) or a new `.cpp` file.
2. Declare it in the corresponding `.hpp`.
3. Add parsing + dispatch in `main.cpp` following the existing pattern.
4. Add the new `.cpp` to `SRCS` in `Makefile`.

---

## Known limitations

| Limitation | Detail |
|---|---|
| Gen1 not supported | Explicit error message on connection |
| Scan timeout fixed | 2 seconds, not configurable via CLI |
| No deduplication in collect | The same measurement is downloaded again on each pass unless `--delete-after-download` is used |
| `--device` in collect uses IP only | Does not accept a logger name |
| Gen3 filename scheme | `engine::Copy` (SelfOwned) uses its own internal naming; filenames are not controlled by the CLI |
| No index range in download | Cannot specify a range; only all or a single measurement |
| Ctrl+C leaves partial file | The incomplete file for the aborted measurement is not deleted |
| No parallel downloads | Loggers are downloaded sequentially in `collect` |
| DLC patch is Gen2-only | `patch_mf4_dlc()` is not called for Gen3 files |
| Extra AT blocks in MF4 | `libxorayasdk 1.00.0046` inserts two extra AT blocks (`*.default_queue.first/last`) not present in Windows SDK output. These do not prevent the CAN viewer from opening the file once the DLC patch is applied. |

---

## Port notes (Windows → Linux)

This CLI is a port of logic originally in `logviewerlib/XorayaUtils/XorayaConnection.cpp` (Qt/Windows):

| Original (Qt) | CLI equivalent |
|---|---|
| `QString` / `QVector` | `std::string` / `std::vector` |
| `QDateTime::fromMSecsSinceEpoch(...).toString("yyyyMMdd_hhmmss")` | `gmtime_r` + `strftime("%Y%m%d_%H%M%S")` |
| `filter->SetProperty("Extension", "mf4")` | Identical |
| `filter->SetProperty("MaxFileSize", "52428800")` | Identical |
| `emit signal(...)` | Direct function call / return value |
| `qDebug()` | `fprintf(stderr, ...)` |
| `X2E_XorayaWin32_DYN_LINKING_AUTO` | Not defined; `-lxorayasdk` in Makefile instead |
| `HddGetMeasurement` + `callAgain()` loop | Same API, identical sequence |
| `MsgFilterFactory::WriterMf4` | Same class name and properties |
