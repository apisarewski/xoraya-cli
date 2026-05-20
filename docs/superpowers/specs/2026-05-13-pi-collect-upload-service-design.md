# Design: Raspberry Pi collect + upload services

**Date:** 2026-05-13
**Status:** Approved

---

## Goal

Run continuous data collection and upload on a Raspberry Pi using two independent systemd services sharing a single folder.

---

## Architecture

Two services, one shared folder. No orchestration code.

```
xoraya-collect.service          databridge.service
        │                               │
        ▼                               ▼
box64 xoraya-cli collect     box64 DataBridgeCLI
  --dest /home/pi/Dexterlogs    --logs-folder /home/pi/Dexterlogs
        │                               │
        └──────────┬────────────────────┘
                   ▼
         /home/pi/Dexterlogs/
              *.mf4 files
```

Each service manages its own condition internally:
- **xoraya-cli** scans the network every N seconds; does nothing if no logger is found
- **DataBridgeCLI** watches the folder and retries upload when internet is unavailable

---

## Services

### `xoraya-collect.service` (existing)

Already configured on the Pi. No changes needed.

```ini
[Unit]
Description=Xoraya data collector
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=pi
EnvironmentFile=/home/pi/xoraya_cli/xoraya-collect.conf
Environment=BOX64_LD_LIBRARY_PATH=/lib/x2e
Environment=BOX64_EMULATED_LIBS=libicuuc.so.74:libicudata.so.74:libicui18n.so.74
ExecStart=systemd-inhibit --what=sleep --who=xoraya-collect --why="Download in progress" \
  box64 /usr/local/bin/xoraya-cli collect \
  --dest ${DEST} --interval ${INTERVAL} --delete-after-download --verbose
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Config file `/home/pi/xoraya_cli/xoraya-collect.conf`:
```
DEST=/home/pi/Dexterlogs
INTERVAL=10
```

### `databridge.service` (new)

```ini
[Unit]
Description=DataBridge uploader
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=pi
WorkingDirectory=/home/pi/Documents/master/databridge/build/linux-x64/DataBridgeCLI
Environment=LD_LIBRARY_PATH=.
Environment=BOX64_LD_LIBRARY_PATH=/lib/x2e
Environment=BOX64_EMULATED_LIBS=libicuuc.so.74:libicudata.so.74:libicui18n.so.74
ExecStart=box64 ./DataBridgeCLI --logs-folder /home/pi/Dexterlogs
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

> Paths in `WorkingDirectory` and `--logs-folder` are adjusted by the user at deployment time.

---

## DataBridge config on the Pi

Copy from the AMD machine and adjust `LogsFolder`:

```
/home/pi/.config/Distalmotion/DataBridge.conf
```

```ini
[General]
LogsFolder=/home/pi/Dexterlogs
PrioritizeSnapshots=true
RetryMaxAttempts=5
RetryMaxDelaySec=60
```

---

## Deployment steps

1. Copy `DataBridgeCLI` directory to the Pi (same `scp` pattern as xoraya-cli)
2. Copy DataBridge config to `/home/pi/.config/Distalmotion/DataBridge.conf`
3. Install `databridge.service` to `/etc/systemd/system/`
4. `sudo systemctl daemon-reload`
5. `sudo systemctl enable databridge`
6. `sudo systemctl start databridge`
7. Verify both services running: `sudo systemctl status xoraya-collect databridge`

---

## Error handling

| Condition | Handled by |
|---|---|
| No logger on network | xoraya-cli: scan returns empty, sleeps, retries |
| Logger disconnects mid-download | xoraya-cli: Restart=on-failure restarts the service |
| No internet | DataBridgeCLI: built-in retry (RetryMaxAttempts=5, RetryMaxDelaySec=60) |
| DataBridgeCLI crashes | systemd: Restart=on-failure |
| Partial MF4 file uploaded | DataBridgeCLI handles this internally |

---

## Out of scope

- Coordination between download and upload (not needed — DataBridgeCLI skips files in use)
- Internet detection before starting DataBridgeCLI (handled by its retry logic)
- Alerting or monitoring
