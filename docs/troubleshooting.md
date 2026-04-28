# Troubleshooting

## Connection errors

### `Error Connection/Invalid 'Connection failed'`

The SDK cannot establish a TCP connection to the logger.

**Probable causes:**

| Cause | Check |
|---|---|
| Logger not reachable on the network | `ping <ip>` |
| Previous connection not properly closed | A prior process was killed with SIGKILL (not Ctrl+C), leaving the logger's connection slot occupied. Wait 30–60 seconds for the SDK timeout to expire, then retry. |
| Logger addressed by name after a failed session | Try connecting by IP address instead of name: `./xoraya-cli download 192.168.2.1 ...` |
| Logger in an incompatible state | Check the `État` field from `scan`. Some states may prevent connection. |

**Workaround for stuck connections:**

```bash
# If connecting by name fails, try by IP
./xoraya-cli download 192.168.2.1 /tmp/logs
./xoraya-cli collect --device 192.168.2.1 --dest /tmp/logs
```

---

### Logger not found by `scan`

```
Aucun logger trouvé.
```

**Probable causes:**

| Cause | Check |
|---|---|
| Logger powered off or not connected | Physical check |
| Logger on a different network segment | The scan uses UDP broadcast; it does not cross routers. The host and logger must be on the same L2 subnet. |
| Scan timeout too short | The timeout is fixed at 2 seconds. On congested networks a logger may respond after this window. |
| Firewall blocking UDP broadcast | Check host firewall rules |

---

## Download errors

### `Erreur : HddDirMeasurement échoué`

The SDK failed to read the measurement list from the HDD.

**Probable causes:**
- Logger is recording and `stop_logging_if_needed` failed to stop it.
- HDD not accessible (logger initializing, HDD error).

---

### `Erreur : engine::Download::Run() échoué`

The download engine failed to start.

**Probable causes:**
- Filter not properly attached to the receiver (`LoggerDataRecv`).
- Invalid `Filename` property (path too long, filesystem not writable).

---

### `Erreur : download terminé avec état N`

The download completed but with a non-`Done` state.

| State value | Meaning |
|---|---|
| 3 (Abort) | Download was cancelled — normal when Ctrl+C is pressed |
| Other | SDK-level error during transfer |

If state 3 appears without a Ctrl+C, it may indicate a network interruption or logger reset during transfer.

---

## File issues

### Double `.mf4.mf4` extension (historical)

This was a bug in an earlier version where `.mf4` was included in both the `Filename` property and set again via `SetProperty("Extension", "mf4")`. It is fixed in the current version.

If you see `.mf4.mf4` files, rebuild from the latest source.

---

### `%index%` appearing literally in the filename (historical)

This was a bug where `%index%` was passed as a SDK placeholder to `BuildFilename()`, but the SDK does not resolve it via that path. It is fixed in the current version — filenames are now built manually from measurement timestamps.

---

### Files named with IP address instead of logger name

`GetName()` failed after connection and the fallback value (the connection string) was used. Look for this warning in the output:

```
Avertissement : GetName() échoué (code N), utilisation de '192.168.2.1'
```

This should not occur in normal operation. If it does, the files are still valid — only the name prefix differs.

---

### Split index `_0001` always present

This is **expected behavior**. The WriterMf4 filter always appends a 4-digit split counter, even when no split occurs. A single-file measurement is named `..._0001.mf4`. This matches the behavior of the original logviewerlib.

---

## Ctrl+C / interruption

### `collect` does not stop immediately on Ctrl+C

After Ctrl+C, the current measurement download is aborted via `ForceCancel()`. The process exits after the abort completes, which may take up to a few hundred milliseconds. This is normal.

If the process was **killed with SIGKILL** (not Ctrl+C / SIGINT), the SDK connection cleanup did not run. The logger may reject new connections for 30–60 seconds.

---

## Gen1 device

```
Erreur : type de device non supporté pour le téléchargement.
```

Gen1 devices appear in `scan` output but are not supported for `download`, `list`, or `delete`. No workaround exists within this tool.

---

## Speed display shows `0 Mbit/s`

For small measurements (under ~1 MB), the download completes in under 1 second and the throughput calculation rounds to zero. The transfer is correct — check the byte count (`✓ N / N octets`) to confirm.

---

## Build errors

### `fatal error: x2e/loggerclient.h: No such file or directory`

The X2E Linux SDK headers are not installed. Install the SDK package and verify:

```bash
ls /usr/include/x2e/loggerclient.h
```

### `/usr/bin/ld: cannot find -lxorayasdk`

The SDK shared library is not installed or not in the default library search path. Verify:

```bash
ls /usr/lib/libxorayasdk.so
ldconfig -p | grep xorayasdk
```
