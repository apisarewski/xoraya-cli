# Pi collect + upload services — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Deploy two independent systemd services on the Raspberry Pi — one that downloads MF4 files from Xoraya loggers, one that uploads them to the cloud via DataBridgeCLI.

**Architecture:** `xoraya-collect.service` (already partially set up) and `databridge.service` (new) share `/home/pi/Dexterlogs`. Each service manages its own retry/detection logic internally. No orchestration code needed.

**Tech Stack:** systemd, Box64 (x86-64 emulation on ARM64), xoraya-cli, DataBridgeCLI + libDataBridgeCore.so

---

## File map

| File | Action | Purpose |
|---|---|---|
| `xoraya_cli/databridge.service` | Create | systemd unit for DataBridgeCLI |
| `xoraya_cli/databridge.conf.example` | Create | Example DataBridge config with Pi paths |

`xoraya-collect.service` and `xoraya-collect.pi.service` already exist — no changes needed.

---

## Task 1: Create the databridge service file

**Files:**
- Create: `xoraya_cli/databridge.service`

- [ ] **Step 1: Create the service file**

Create `xoraya_cli/databridge.service` with this exact content:

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

- [ ] **Step 2: Verify syntax locally**

```bash
systemd-analyze verify xoraya_cli/databridge.service
```

Expected: no output (no errors). Warnings about missing units are acceptable.

- [ ] **Step 3: Create the example config file**

Create `xoraya_cli/databridge.conf.example` with this exact content:

```ini
[General]
BlobRootPath=
LogsFolder=/home/pi/Dexterlogs
PrioritizeSnapshots=true
RetryMaxAttempts=5
RetryMaxDelaySec=60
```

- [ ] **Step 4: Commit**

```bash
git add xoraya_cli/databridge.service xoraya_cli/databridge.conf.example
git commit -m "feat: add databridge systemd service and example config for Pi"
```

---

## Task 2: Copy DataBridgeCLI to the Pi

**Run all commands on the AMD machine.**

- [ ] **Step 1: Create the target directory on the Pi**

```bash
ssh pi@192.168.1.51 "mkdir -p ~/Documents/master/databridge/build/linux-x64/DataBridgeCLI"
```

Expected: no output.

- [ ] **Step 2: Copy the binary and shared library**

```bash
scp /home/alexander/Documents/master/databridge/build/linux-x64/DataBridgeCLI/DataBridgeCLI \
    /home/alexander/Documents/master/databridge/build/linux-x64/DataBridgeCLI/libDataBridgeCore.so.2.0.0 \
    pi@192.168.1.51:~/Documents/master/databridge/build/linux-x64/DataBridgeCLI/
```

Expected: no output (silent success).

- [ ] **Step 3: Create the versioned symlink on the Pi**

```bash
ssh pi@192.168.1.51 "cd ~/Documents/master/databridge/build/linux-x64/DataBridgeCLI && \
  ln -sf libDataBridgeCore.so.2.0.0 libDataBridgeCore.so.2 && \
  ln -sf libDataBridgeCore.so.2.0.0 libDataBridgeCore.so"
```

Expected: no output.

- [ ] **Step 4: Verify the binary exists**

```bash
ssh pi@192.168.1.51 "ls -la ~/Documents/master/databridge/build/linux-x64/DataBridgeCLI/"
```

Expected: `DataBridgeCLI`, `libDataBridgeCore.so.2.0.0`, and two symlinks.

---

## Task 3: Deploy DataBridge config to the Pi

**Run all commands on the AMD machine.**

- [ ] **Step 1: Create the config directory on the Pi**

```bash
ssh pi@192.168.1.51 "mkdir -p ~/.config/Distalmotion"
```

- [ ] **Step 2: Copy the config file**

```bash
scp /home/alexander/.config/Distalmotion/DataBridge.conf \
    pi@192.168.1.51:~/.config/Distalmotion/DataBridge.conf
```

- [ ] **Step 3: Fix the LogsFolder path on the Pi**

```bash
ssh pi@192.168.1.51 "sed -i 's|LogsFolder=.*|LogsFolder=/home/pi/Dexterlogs|' \
  ~/.config/Distalmotion/DataBridge.conf"
```

- [ ] **Step 4: Verify the config**

```bash
ssh pi@192.168.1.51 "cat ~/.config/Distalmotion/DataBridge.conf"
```

Expected output:
```ini
[General]
BlobRootPath=
LogsFolder=/home/pi/Dexterlogs
PrioritizeSnapshots=true
RetryMaxAttempts=5
RetryMaxDelaySec=60
```

---

## Task 4: Test DataBridgeCLI manually on the Pi

Before installing the service, verify Box64 can run DataBridgeCLI.

**SSH into the Pi for all steps.**

- [ ] **Step 1: Run DataBridgeCLI manually**

```bash
cd ~/Documents/master/databridge/build/linux-x64/DataBridgeCLI && \
BOX64_LD_LIBRARY_PATH=/lib/x2e \
BOX64_EMULATED_LIBS=libicuuc.so.74:libicudata.so.74:libicui18n.so.74 \
LD_LIBRARY_PATH=. \
box64 ./DataBridgeCLI --logs-folder /home/pi/Dexterlogs
```

Expected: DataBridgeCLI starts, prints startup logs, and either begins watching the folder or reports an auth/connectivity error. A connectivity error is acceptable — it means the binary runs correctly but can't reach the cloud yet. Press Ctrl+C after a few seconds.

- [ ] **Step 2: If Box64 reports missing libraries**

Follow the same pattern as xoraya-cli: copy the missing x86-64 `.so` from the AMD machine to `/lib/x2e/` on the Pi, then retry Step 1.

---

## Task 5: Install and start the databridge service

**SSH into the Pi for all steps.**

- [ ] **Step 1: Copy the service file**

```bash
sudo cp ~/xoraya_cli/databridge.service /etc/systemd/system/databridge.service
```

- [ ] **Step 2: Reload systemd**

```bash
sudo systemctl daemon-reload
```

- [ ] **Step 3: Enable and start**

```bash
sudo systemctl enable databridge
sudo systemctl start databridge
```

- [ ] **Step 4: Verify it is running**

```bash
sudo systemctl status databridge
```

Expected: `Active: active (running)`.

- [ ] **Step 5: Check live logs**

```bash
journalctl -u databridge -f
```

Expected: DataBridgeCLI startup messages. Auth/connectivity errors are acceptable if cloud credentials are not yet configured on the Pi. Press Ctrl+C to exit.

---

## Task 6: Verify both services together

**SSH into the Pi.**

- [ ] **Step 1: Check status of both services**

```bash
sudo systemctl status xoraya-collect databridge
```

Expected: both show `Active: active (running)`.

- [ ] **Step 2: Confirm shared folder exists**

```bash
ls /home/pi/Dexterlogs
```

Expected: directory exists (may be empty if no logger has connected yet).

- [ ] **Step 3: Check both service logs side by side**

```bash
journalctl -u xoraya-collect -u databridge --since "5 minutes ago"
```

Expected: logs from both services interleaved, no fatal errors.

- [ ] **Step 4: Push the service file to GitHub**

On the AMD machine:

```bash
git push origin main
```

---

## Task 7: Update documentation

- [ ] **Step 1: Update deploy-raspberry-pi.md**

Add a section at the end of `docs/deploy-raspberry-pi.md`:

```markdown
## DataBridge upload service

Once DataBridgeCLI is deployed (see Task 2–3 of the implementation plan), install its service:

\`\`\`bash
sudo cp ~/xoraya_cli/databridge.service /etc/systemd/system/databridge.service
sudo systemctl daemon-reload
sudo systemctl enable databridge
sudo systemctl start databridge
\`\`\`

Both services share `/home/pi/Dexterlogs`. Check both are running:

\`\`\`bash
sudo systemctl status xoraya-collect databridge
\`\`\`
```

- [ ] **Step 2: Commit**

```bash
git add docs/deploy-raspberry-pi.md
git commit -m "docs: add DataBridge service deployment section to Pi guide"
git push origin main
```
