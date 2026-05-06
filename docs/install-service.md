# Installing the xoraya-collect systemd service

This sets up `xoraya-cli collect` to run automatically at boot, re-scanning every 10 seconds and downloading measurements from all discovered loggers.

---

## Prerequisites

- `xoraya-cli` built and ready
- systemd (standard on Ubuntu/Debian/Fedora)
- `network-manager` or equivalent so `network-online.target` is reachable

---

## Installation

### 1. Build and install the binary

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make
sudo cp xoraya-cli /usr/local/bin/xoraya-cli
```

### 2. Create the config file

```bash
cp xoraya-collect.conf.example xoraya-collect.conf
```

Edit `xoraya-collect.conf` to set your destination directory and interval:

```
DEST=/home/Dexterlogs
INTERVAL=10
```

### 3. Install the service file

```bash
sudo cp xoraya-collect.service /etc/systemd/system/xoraya-collect.service
sudo systemctl daemon-reload
```

### 4. Enable and start

```bash
sudo systemctl enable xoraya-collect   # auto-start at boot
sudo systemctl start xoraya-collect    # start now
```

---

## Day-to-day operations

| Task | Command |
|---|---|
| View live logs | `journalctl -u xoraya-collect -f` |
| Stop | `sudo systemctl stop xoraya-collect` |
| Start | `sudo systemctl start xoraya-collect` |
| Restart after config change | `sudo systemctl restart xoraya-collect` |
| Disable auto-start | `sudo systemctl disable xoraya-collect` |
| Check status | `sudo systemctl status xoraya-collect` |

---

## Updating the binary after a rebuild

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make && sudo cp xoraya-cli /usr/local/bin/xoraya-cli && sudo systemctl restart xoraya-collect
```

---

## Changing the interval or destination

Edit `xoraya-collect.conf` (no sudo required), then restart the service:

```bash
nano /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli/xoraya-collect.conf
sudo systemctl restart xoraya-collect
```
