# xoraya-cli

Autonomous Linux CLI for Xoraya dataloggers (ML-N4000 and compatible).

No Qt dependency — pure C++17 with the X2E Linux SDK.

---

## Overview

`xoraya-cli` connects to Xoraya dataloggers over a local network, lists and downloads measurement files stored on their internal HDD, and optionally deletes them after a successful download.

The tool is designed for scripted or automated use on Linux systems. The `collect` command in particular is suited for periodic unattended data retrieval.

---

## Quick start

```bash
# Build
cd cli/
make

# Discover loggers on the network
./xoraya-cli scan

# List measurements on a logger
./xoraya-cli list DX11252-2

# Download all measurements
./xoraya-cli download DX11252-2 /tmp/logs

# Automated collect (all loggers, loop every 5 minutes)
./xoraya-cli collect --dest /data/logs --interval 300
```

---

## Documentation

| Document | Contents |
|---|---|
| [docs/build.md](docs/build.md) | Prerequisites, dependencies, build instructions |
| [docs/usage.md](docs/usage.md) | All commands, options, and examples |
| [docs/collect.md](docs/collect.md) | Collect workflow and automation |
| [docs/troubleshooting.md](docs/troubleshooting.md) | Common issues and diagnostic steps |

---

## Supported devices

| Device family | Type reported by scan | Download engine | Output format |
|---|---|---|---|
| ML-N4000 | Gen2 (N4000) | `engine::Download` + WriterMf4 | MF4 |
| DataCube | DataCube | `engine::Download` + WriterMf4 | MF4 |
| DLN cluster | DLNcluster | `engine::Download` + WriterMf4 | MF4 |
| Gen3 | Gen3 | `engine::Copy` (SelfOwned) | Native |
| DataCube N-Series | DataCubeNSeries | `engine::Copy` (SelfOwned) | Native |

> **Gen1 is not supported.** An explicit error message is displayed if a Gen1 device is detected.

---

## Return codes

| Code | Meaning |
|---|---|
| `0` | Success |
| `1` | Error (connection failure, invalid index, SDK error, failed deletion, …) |
