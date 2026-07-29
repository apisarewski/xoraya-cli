# Deploying xoraya-cli on Raspberry Pi (Box64)

Since the X2E SDK is only available as an x86-64 binary, the recommended approach on a Raspberry Pi is to use **Box64** — an x86-64 emulator built for ARM64. Box64 translates x86-64 instructions to ARM64 at runtime, using native ARM versions of standard system libraries and emulating only the libraries it cannot wrap natively.

**Requirements:** 64-bit Raspberry Pi OS or Debian Bookworm (aarch64).

---

## Phase 1 — Check your Pi OS version

SSH into the Pi and verify you are running 64-bit:

```bash
uname -m && lsb_release -a
```

Expected output: `aarch64`. If you see `armv7l`, you need to reinstall with the 64-bit image.

---

## Phase 2 — Install Box64

Box64 requires its repository to be added manually:

```bash
sudo wget https://ryanfortner.github.io/box64-debs/box64.list -O /etc/apt/sources.list.d/box64.list

wget -qO- https://ryanfortner.github.io/box64-debs/KEY.gpg | sudo gpg --dearmor -o /etc/apt/trusted.gpg.d/box64-debs-archive-keyring.gpg

sudo apt update
sudo apt install box64-rpi4arm64
```

Verify:

```bash
box64 --version
```

---

## Phase 3 — Install ARM dependencies on the Pi

Box64 uses native ARM versions of all common system libraries. Install them:

```bash
sudo apt install \
  libxerces-c3.2 \
  libusb-1.0-0 \
  libsmbclient \
  libcurl4 \
  libgpgme11 \
  libssl3 \
  libkrb5-3 \
  libicu72
```

---

## Phase 4 — Copy the binary and x86-64 libraries from your AMD machine

Run these on your **AMD machine**. They copy the binary, the proprietary SDK library, and three additional x86-64 libraries that Box64 cannot wrap natively (xerces, lz4, and ICU):

```bash
# Binary
scp /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli/xoraya-cli \
    pi@192.168.1.51:~/xoraya_cli/

# Proprietary SDK (must be x86-64 — Box64 emulates this)
scp /lib/x2e/libxorayasdk.so.1 \
    pi@192.168.1.51:~/xoraya_cli/

# x86-64 libraries Box64 cannot wrap natively
scp /lib/x86_64-linux-gnu/libxerces-c-3.2.so \
    /lib/x86_64-linux-gnu/liblz4.so.1 \
    /lib/x86_64-linux-gnu/libicuuc.so.74 \
    /lib/x86_64-linux-gnu/libicudata.so.74 \
    /lib/x86_64-linux-gnu/libicui18n.so.74 \
    pi@192.168.1.51:~/xoraya_cli/
```

> **Why ICU must be x86-64:** ICU appends its version number to every symbol name (e.g. `u_toupper_74`). The ARM ICU 72 on Debian Bookworm exports `u_toupper_72`, so the symlink trick does not work. Box64 must use the matching x86-64 version.

---

## Phase 5 — Install the libraries on the Pi

SSH into the Pi:

```bash
sudo mkdir -p /lib/x2e

sudo cp ~/xoraya_cli/libxorayasdk.so.1 \
        ~/xoraya_cli/libxerces-c-3.2.so \
        ~/xoraya_cli/liblz4.so.1 \
        ~/xoraya_cli/libicuuc.so.74 \
        ~/xoraya_cli/libicudata.so.74 \
        ~/xoraya_cli/libicui18n.so.74 \
        /lib/x2e/

sudo ln -sf /lib/x2e/libxorayasdk.so.1 /lib/libxorayasdk.so.1
```

---

## Phase 6 — Test the emulation

```bash
BOX64_LD_LIBRARY_PATH=/lib/x2e \
BOX64_EMULATED_LIBS=libicuuc.so.74:libicudata.so.74:libicui18n.so.74 \
box64 /usr/local/bin/xoraya-cli --help
```

If the help text prints correctly, try a real scan:

```bash
BOX64_LD_LIBRARY_PATH=/lib/x2e \
BOX64_EMULATED_LIBS=libicuuc.so.74:libicudata.so.74:libicui18n.so.74 \
box64 /usr/local/bin/xoraya-cli scan
```

---

## Phase 7 — Set up the config file

```bash
cp ~/xoraya_cli/xoraya-collect.conf.example ~/xoraya_cli/xoraya-collect.conf
nano ~/xoraya_cli/xoraya-collect.conf
```

```
DEST=/home/pi/Dexterlogs
INTERVAL=10
```

```bash
mkdir -p /home/pi/Dexterlogs
```

---

## Phase 8 — Install the binary and set up the service

Install the binary:

```bash
sudo cp ~/xoraya_cli/xoraya-cli /usr/local/bin/xoraya-cli
```

Edit the service file to add the Box64 environment variables and prefix the command with `box64`:

```bash
nano ~/xoraya_cli/xoraya-collect.pi.service
```

The file should look like this:

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
  box64 /usr/local/bin/xoraya-cli collect --dest ${DEST} --interval ${INTERVAL} --delete-after-download --verbose
Restart=on-failure
RestartSec=5

[Install]
WantedBy=multi-user.target
```

Install and start:

```bash
sudo cp ~/xoraya_cli/xoraya-collect.pi.service /etc/systemd/system/xoraya-collect.service
sudo systemctl daemon-reload
sudo systemctl enable xoraya-collect
sudo systemctl start xoraya-collect
```

Verify:

```bash
sudo systemctl status xoraya-collect
journalctl -u xoraya-collect -f
```

---

## Day-to-day operations

| Task | Command |
|---|---|
| View live logs | `journalctl -u xoraya-collect -f` |
| Restart after config change | `sudo systemctl restart xoraya-collect` |
| Check status | `sudo systemctl status xoraya-collect` |
| Stop | `sudo systemctl stop xoraya-collect` |

---

## Updating the binary

On your AMD machine, rebuild and copy:

```bash
cd /home/alexander/Documents/DataloggerExtract/LogViewerLib/xoraya_cli
make
scp xoraya-cli pi@192.168.1.51:~/xoraya_cli/
```

On the Pi:

```bash
sudo cp ~/xoraya_cli/xoraya-cli /usr/local/bin/xoraya-cli
sudo systemctl restart xoraya-collect
```

---

## DataBridge upload service

Once DataBridgeCLI is deployed on the Pi, install the upload service so collected MF4 files are automatically uploaded to Azure.

### Prerequisites

1. Copy `DataBridgeCLI` and `libDataBridgeCore.so.2.0.0` to the Pi:

```bash
ssh pi@192.168.1.51 "mkdir -p ~/Documents/master/databridge/build/linux-x64/DataBridgeCLI"

scp /path/to/DataBridgeCLI \
    /path/to/libDataBridgeCore.so.2.0.0 \
    pi@192.168.1.51:~/Documents/master/databridge/build/linux-x64/DataBridgeCLI/
```

2. Create symlinks on the Pi:

```bash
ssh pi@192.168.1.51 "cd ~/Documents/master/databridge/build/linux-x64/DataBridgeCLI && \
  ln -sf libDataBridgeCore.so.2.0.0 libDataBridgeCore.so.2 && \
  ln -sf libDataBridgeCore.so.2.0.0 libDataBridgeCore.so"
```

3. Copy the required x86-64 OpenSSL libraries (Box64 needs these to emulate crypto):

```bash
scp /usr/lib/x86_64-linux-gnu/libssl.so.3 \
    /usr/lib/x86_64-linux-gnu/libcrypto.so.3 \
    pi@192.168.1.51:/tmp/
ssh pi@192.168.1.51 "sudo mv /tmp/libssl.so.3 /tmp/libcrypto.so.3 /lib/x2e/"
```

4. Create `/home/pi/xoraya_cli/databridge.conf` (chmod 600) with your credentials:

```bash
cp ~/xoraya_cli/databridge.conf.example /home/pi/xoraya_cli/databridge.conf
chmod 600 /home/pi/xoraya_cli/databridge.conf
# Edit the file and fill in DEST, DATABRIDGE_BLOB_SAS_TOKEN_EU, and all
# DATABRIDGE_MONITOR_* variables with real values.
```

> **Security:** `databridge.conf` contains secrets — it is git-ignored and must never be committed.

### Install the service

```bash
sudo cp ~/xoraya_cli/databridge.service /etc/systemd/system/databridge.service
sudo systemctl daemon-reload
sudo systemctl enable databridge
sudo systemctl start databridge
```

Check it is running:

```bash
sudo systemctl status databridge
journalctl -u databridge -f
```

Expected: DataBridgeCLI starts, connects to Azure Blob Storage, scans `/home/pi/Dexterlogs`, then exits with code 0. Systemd restarts it every 60 seconds.

### How it works

DataBridgeCLI is a one-shot uploader: it scans the logs folder, uploads any new MF4 files, and exits. The service uses `Restart=always` with `RestartSec=60` to poll every 60 seconds.

`BOX64_DYNAREC=0` is required — Box64's JIT recompiler mishandles x86-64 AES/SHA instructions used by the embedded OpenSSL, causing SSL connect errors. Interpreter mode is slower but correct.

### Known limitation

Azure Monitor telemetry (audit log upload) fails with a Qt SSL version mismatch under Box64. This is a Box64 issue with OpenSSL version reporting; it does not affect MF4 file uploads.

### Both services together

Check both are running:

```bash
sudo systemctl status xoraya-collect databridge
```

View combined logs:

```bash
journalctl -u xoraya-collect -u databridge --since "5 minutes ago"
```

### Day-to-day operations (DataBridge)

| Task | Command |
|---|---|
| View live logs | `journalctl -u databridge -f` |
| Restart after config change | `sudo systemctl restart databridge` |
| Check status | `sudo systemctl status databridge` |
| Stop | `sudo systemctl stop databridge` |

---

## OLED Screen and Toggle Switches

The station includes a 128×64 SSD1306 OLED display and two toggle flip switches wired directly to the Pi GPIO header.

### Hardware

| Component | Spec |
|-----------|------|
| Display | SSD1306 — 128×64 monochrome OLED |
| Interface | I2C bus 1, address `0x3C` |
| Switch 1 | GPIO 27 — Download (starts/stops `xoraya-collect`) |
| Switch 2 | GPIO 10 — Upload (starts/stops `databridge`) |

### Wiring

```
Pi Header Pin   Signal        Connect to
─────────────   ──────────    ──────────────────────────
Pin 1  (3.3V)   Power         VCC  (OLED)
Pin 3  (GPIO2)  I2C SDA       SDA  (OLED)
Pin 5  (GPIO3)  I2C SCL       SCL  (OLED)
Pin 6  (GND)    Ground        GND  (OLED)
Pin 13 (GPIO27) Switch 1 COM  COM terminal of SW1 (Download)
Pin 14 (GND)    Switch 1 GND  GND terminal of SW1
Pin 19 (GPIO10) Switch 2 COM  COM terminal of SW2 (Upload)
Pin 20 (GND)    Switch 2 GND  GND terminal of SW2
```

The toggle switches are **miniature SPDT ON-ON** (1CO, 3 terminals, soldering lugs). Both positions are always active — there is no centre-off position.

| Terminal | Connect to |
|----------|------------|
| **COM** (centre lug) | GPIO pin — Pin 11 (SW1) or Pin 13 (SW2) |
| **One end lug** | GND |
| **Other end lug** | Leave unconnected |

The firmware uses the Pi's internal pull-up resistors:
- Switch flipped toward the **GND lug** → COM pulled to GND → GPIO reads LOW → `is_pressed = True` → service starts
- Switch flipped toward the **open lug** → COM floats high via pull-up → GPIO reads HIGH → `is_pressed = False` → service stops

It does not matter which end lug you connect to GND — just be consistent: pick one side as "ON" and solder GND there.

### Verify wiring before starting the daemon

```bash
# Check the OLED is detected at address 0x3C
sudo i2cdetect -y 1
# Expected: "3c" appears in the grid

# Enable I2C if not already active
sudo raspi-config   # → Interface Options → I2C → Enable

# Read current switch positions
python3 -c "
from gpiozero import Button
sw1 = Button(27, pull_up=True)
sw2 = Button(10, pull_up=True)
print('SW1 (Download):', 'ON' if sw1.is_pressed else 'OFF')
print('SW2 (Upload)  :', 'ON' if sw2.is_pressed else 'OFF')
"
```

### Install Python dependencies

```bash
pip3 install -r /home/pi/xoraya_cli/screen/requirements.txt
```

### Install and start the screen service

```bash
sudo cp /home/pi/xoraya_cli/screen/xoraya-screen.service /etc/systemd/system/
sudo systemctl daemon-reload
sudo systemctl enable xoraya-screen
sudo systemctl start xoraya-screen
```

Verify it is running and the OLED shows the boot screen:

```bash
sudo systemctl status xoraya-screen
journalctl -u xoraya-screen -n 30 --no-pager
```

### Day-to-day operations (screen daemon)

| Task | Command |
|---|---|
| View live logs | `journalctl -u xoraya-screen -f` |
| Restart | `sudo systemctl restart xoraya-screen` |
| Check status | `sudo systemctl status xoraya-screen` |
| Stop | `sudo systemctl stop xoraya-screen` |
