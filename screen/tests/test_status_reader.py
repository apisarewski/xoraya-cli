import json
import os
import tempfile
import time

import pytest


def write_status(path, data):
    with open(path, 'w') as f:
        json.dump(data, f)


def test_read_xoraya_status_returns_none_when_file_absent(tmp_path, monkeypatch):
    import screen.status_reader as sr
    monkeypatch.setattr(sr, 'STATUS_FILE', str(tmp_path / 'absent.json'))
    assert sr.read_xoraya_status() is None


def test_read_xoraya_status_returns_none_when_stale(tmp_path, monkeypatch):
    import screen.status_reader as sr
    p = tmp_path / 'status.json'
    write_status(p, {'state': 'downloading', 'updated_at': 1})
    monkeypatch.setattr(sr, 'STATUS_FILE', str(p))
    # updated_at=1 is ancient — stale
    assert sr.read_xoraya_status() is None


def test_read_xoraya_status_returns_dict_when_fresh(tmp_path, monkeypatch):
    import screen.status_reader as sr
    p = tmp_path / 'status.json'
    data = {'state': 'downloading', 'pct': 42.0, 'updated_at': int(time.time())}
    write_status(p, data)
    monkeypatch.setattr(sr, 'STATUS_FILE', str(p))
    result = sr.read_xoraya_status()
    assert result is not None
    assert result['state'] == 'downloading'
    assert result['pct'] == 42.0


def test_read_xoraya_status_handles_corrupt_json(tmp_path, monkeypatch):
    import screen.status_reader as sr
    p = tmp_path / 'status.json'
    p.write_text('not json {{{')
    monkeypatch.setattr(sr, 'STATUS_FILE', str(p))
    assert sr.read_xoraya_status() is None


def test_get_uptime_returns_string():
    import screen.status_reader as sr
    result = sr.get_uptime()
    assert isinstance(result, str)
    assert len(result) > 0


def test_get_disk_free_returns_string():
    import screen.status_reader as sr
    result = sr.get_disk_free()
    assert isinstance(result, str)
    assert len(result) > 0
