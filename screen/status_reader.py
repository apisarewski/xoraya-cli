import json
import os
import subprocess
import time

STATUS_FILE      = '/tmp/xoraya-status.json'
STALE_THRESHOLD  = 5   # seconds
XORAYA_CLI_BIN   = '/usr/local/bin/xoraya-cli'

# xoraya-cli is an x86-64 binary run under Box64 on the Pi (see the *.service
# units); invoking it directly without these, Box64 can't find libxorayasdk
# and the ICU libs and exits before even running detect-dest.
_BOX64_ENV = {
    **os.environ,
    'BOX64_LD_LIBRARY_PATH': '/lib/x2e',
    'BOX64_EMULATED_LIBS': 'libicuuc.so.74:libicudata.so.74:libicui18n.so.74',
}


def read_xoraya_status():
    """Returns parsed JSON dict, or None if file is absent, stale, or corrupt."""
    try:
        with open(STATUS_FILE) as f:
            data = json.load(f)
        if time.time() - data.get('updated_at', 0) > STALE_THRESHOLD:
            return None
        return data
    except (OSError, json.JSONDecodeError, ValueError, KeyError):
        return None


def is_service_active(service_name):
    """Returns True if the named systemd service is currently active."""
    r = subprocess.run(
        ['systemctl', 'is-active', '--quiet', service_name],
        capture_output=True,
    )
    return r.returncode == 0


def is_storage_detected():
    """Returns True if xoraya-cli can resolve a single external drive destination."""
    r = subprocess.run(
        [XORAYA_CLI_BIN, 'detect-dest'],
        capture_output=True,
        env=_BOX64_ENV,
    )
    return r.returncode == 0
