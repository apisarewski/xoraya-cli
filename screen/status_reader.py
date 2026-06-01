import json
import os
import re
import subprocess
import time

STATUS_FILE      = '/tmp/xoraya-status.json'
STALE_THRESHOLD  = 5   # seconds


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


def read_databridge_progress():
    """
    Parses the last matching line from the databridge journal.
    Returns {'uploaded': N, 'skipped': N, 'failed': N} or None.
    """
    r = subprocess.run(
        ['journalctl', '-u', 'databridge', '-n', '20',
         '--no-pager', '--output=cat'],
        capture_output=True, text=True,
    )
    for line in reversed(r.stdout.splitlines()):
        m = re.search(
            r'uploaded:\s*(\d+).*?skipped:\s*(\d+).*?failed:\s*(\d+)',
            line,
        )
        if m:
            return {
                'uploaded': int(m.group(1)),
                'skipped':  int(m.group(2)),
                'failed':   int(m.group(3)),
            }
    return None


def get_ip_address():
    """Returns the Pi's first non-loopback IP, or '?.?.?.?' on failure."""
    try:
        r = subprocess.run(['hostname', '-I'], capture_output=True, text=True)
        parts = r.stdout.strip().split()
        return parts[0] if parts else '?.?.?.?'
    except OSError:
        return '?.?.?.?'


def get_disk_free():
    """Returns free space on / as a human-readable string (e.g. '18.4G')."""
    try:
        r = subprocess.run(['df', '-h', '/'], capture_output=True, text=True)
        lines = r.stdout.strip().splitlines()
        if len(lines) >= 2:
            parts = lines[1].split()
            return parts[3] if len(parts) > 3 else '?'
    except OSError:
        pass
    return '?'


def get_uptime():
    """Returns uptime as '3d 14h' or '2h 5m'."""
    try:
        with open('/proc/uptime') as f:
            total_s = float(f.read().split()[0])
        days  = int(total_s // 86400)
        hours = int((total_s % 86400) // 3600)
        mins  = int((total_s % 3600) // 60)
        if days > 0:
            return f'{days}d {hours}h'
        return f'{hours}h {mins}m'
    except (OSError, ValueError):
        return '?'
