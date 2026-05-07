# Design: newest-first download order + --last N option

**Date:** 2026-05-07
**Status:** approved

---

## Goal

1. Download measurements newest-first (highest index → lowest) by default, in both `download` and `collect`. This ensures the most recent data is secured first if a download is interrupted.
2. Add `--last N` option to both commands to limit the download to the N most recent measurements.
3. Change the default destination directory from `./downloads` to `/home/Dexterlogs`.

---

## Approach

Reverse the `targets` build loop in `download_gen2` and `copy_gen3`. When `last_n > 0`, start the loop at `count - last_n` instead of 0.

### Core loop change (both Gen2 and Gen3)

```cpp
// Before (oldest first, all):
for (size_t i = 0; i < count; ++i)
    targets.push_back(all.get(i));

// After (newest first, optionally limited):
size_t start = (last_n > 0) ? std::max<size_t>(0, count - last_n) : 0;
for (size_t i = count; i-- > start; )
    targets.push_back(all.get(i));
```

---

## Files changed

| File | Change |
|---|---|
| `downloader.cpp` | Reverse loop in `download_gen2` and `copy_gen3`; apply `last_n` slice |
| `downloader.hpp` | Add `int last_n = -1` to `cmd_download` signature |
| `collector.hpp` | Add `int last_n = -1` to `CollectOptions`; change `dest_dir` default to `/home/Dexterlogs` |
| `collector.cpp` | Pass `opts.last_n` to both `cmd_download` calls |
| `main.cpp` | Parse `--last N` for `download` and `collect`; update default dest in help text |

---

## Command-line interface

### `download`

```bash
./xoraya-cli download DX11252-2 /tmp/logs           # all, newest first
./xoraya-cli download DX11252-2 /tmp/logs --last 3  # last 3 only, newest first
./xoraya-cli download DX11252-2 /tmp/logs 7         # specific index (unchanged)
```

### `collect`

```bash
./xoraya-cli collect                    # all loggers, all measurements, newest first
./xoraya-cli collect --last 5           # last 5 measurements per logger, newest first
./xoraya-cli collect --dest /data/logs  # explicit destination
```

---

## Edge cases

| Case | Behaviour |
|---|---|
| `--last 0` | Rejected: error "–-last must be ≥ 1" |
| `--last N` where N > total measurements | Downloads all (no error) |
| `--last N` combined with specific index on `download` | Rejected: mutually exclusive |
| Single measurement on logger | Downloads that one measurement (no change) |

---

## Default destination

`CollectOptions::dest_dir` default changes from `"./downloads"` to `"/home/Dexterlogs"`. Help text in `main.cpp` updated accordingly.

---

## What is NOT in scope

- Changing download order for a specific index (`download <device> <dest> 7` is unchanged)
- Deduplication or tracking of already-downloaded measurements
- Any change to the deletion logic (deletion still follows download order)
