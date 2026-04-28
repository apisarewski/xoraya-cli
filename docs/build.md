# Build

## Prerequisites

| Dependency | Expected location | Check |
|---|---|---|
| X2E Linux SDK — headers | `/usr/include/x2e/` | `ls /usr/include/x2e/loggerclient.h` |
| X2E Linux SDK — shared library | `/usr/lib/libxorayasdk.so` | `ls /usr/lib/libxorayasdk.so` |
| C++ compiler | `g++` ≥ 9 (C++17) | `g++ --version` |
| POSIX threads | `libpthread` (standard) | included with glibc |

The SDK is installed as a system dependency on the target machine. It is not bundled in the repository.

---

## Build

```bash
cd cli/
make
```

This produces the `xoraya-cli` binary in `cli/`.

```bash
# Clean build artifacts
make clean
```

---

## Makefile details

```makefile
CXX      ?= g++
CXXFLAGS  = -std=c++17 -Wall -Wextra -fPIC -O2
LDFLAGS   = -lxorayasdk -lpthread

SRCS = main.cpp scanner.cpp downloader.cpp collector.cpp
```

The SDK is linked via `-lxorayasdk`. No `X2E_XorayaWin32_DYN_LINKING_AUTO` macro is defined — that macro is a Windows-only auto-linking pragma, ignored on Linux. Dependencies are tracked automatically with `-MMD -MP`.

---

## Verifying the SDK installation

```bash
ls /usr/include/x2e/loggerclient.h
ls /usr/include/x2e/Engine_Download.h
ls /usr/lib/libxorayasdk.so
```

If any of these are missing, install the X2E Linux SDK package for the target platform before building.
