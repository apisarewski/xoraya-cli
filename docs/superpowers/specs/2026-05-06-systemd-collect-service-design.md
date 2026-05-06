# Design: systemd service for `xoraya-cli collect`

**Date:** 2026-05-06
**Status:** approved

---

## Goal

Automate `xoraya-cli collect` to run continuously on Linux startup, re-scanning for Xoraya loggers every 10 seconds, downloading and deleting measurements, without interrupting active logging.

---

## Collect command invoked

```bash
/usr/local/bin/xoraya-cli collect \
    --dest ${DEST} \
    --interval ${INTERVAL} \
    --delete-after-download
```

- `--interval ${INTERVAL}`: loop indefinitely, configurable (default 10 s)
- `--delete-after-download`: avoid re-downloading measurements on the next pass
- No `--stop-logging`: logging is never interrupted
- No `--device`: all discovered loggers are processed

---

## Components

### 1. Config file

**Path:** `/home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli/xoraya-collect.conf`

Plain text, `key=value` format, no `sudo` required to edit.

```
INTERVAL=10
DEST=/home/Dexterlogs
```

Changing a value takes effect after `sudo systemctl restart xoraya-collect`.

### 2. systemd unit

**Path:** `/etc/systemd/system/xoraya-collect.service`

```ini
[Unit]
Description=Xoraya data collector
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
User=alexander
EnvironmentFile=/home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli/xoraya-collect.conf
ExecStart=/usr/local/bin/xoraya-cli collect --dest ${DEST} --interval ${INTERVAL} --delete-after-download
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Key directives:
- `After=network-online.target`: waits for network before starting
- `Restart=on-failure`: restarts automatically if the process crashes (non-zero exit)
- `RestartSec=5`: 5-second delay before restart
- `User=alexander`: runs as the user, not root

### 3. Binary installation

The binary is copied from the build directory to a stable system path:

```bash
sudo cp xoraya-cli /usr/local/bin/xoraya-cli
```

This decouples the running service from the development tree.

---

## Installation steps

```bash
# 1. Build and install binary
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make
sudo cp xoraya-cli /usr/local/bin/xoraya-cli

# 2. Create config file (no sudo)
cp xoraya-collect.conf.example xoraya-collect.conf
# edit INTERVAL and DEST as needed

# 3. Install service file
sudo cp xoraya-collect.service /etc/systemd/system/xoraya-collect.service

# 4. Enable and start
sudo systemctl daemon-reload
sudo systemctl enable xoraya-collect
sudo systemctl start xoraya-collect
```

---

## Day-to-day operations

| Task | Command |
|---|---|
| View live logs | `journalctl -u xoraya-collect -f` |
| Stop the service | `sudo systemctl stop xoraya-collect` |
| Start the service | `sudo systemctl start xoraya-collect` |
| Restart after config change | `sudo systemctl restart xoraya-collect` |
| Disable auto-start | `sudo systemctl disable xoraya-collect` |
| Update binary after rebuild | `make && sudo cp xoraya-cli /usr/local/bin/xoraya-cli && sudo systemctl restart xoraya-collect` |

---

## Deliverables

- `xoraya-collect.conf.example` — example config file, committed to the repo
- `xoraya-collect.service` — systemd unit file, committed to the repo
- `docs/install-service.md` — installation instructions

---

## What is NOT in scope

- Log rotation (journal handles it)
- Multiple service instances
- `--device` filter (all loggers are collected)
- `--stop-logging` (logging is never interrupted)
