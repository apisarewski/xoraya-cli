import json
import subprocess
import time


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


def test_is_storage_detected_true_on_zero_exit(monkeypatch):
    import screen.status_reader as sr

    def fake_run(*args, **kwargs):
        return subprocess.CompletedProcess(args, returncode=0)

    monkeypatch.setattr(sr.subprocess, 'run', fake_run)
    assert sr.is_storage_detected() is True


def test_is_storage_detected_false_on_nonzero_exit(monkeypatch):
    import screen.status_reader as sr

    def fake_run(*args, **kwargs):
        return subprocess.CompletedProcess(args, returncode=1)

    monkeypatch.setattr(sr.subprocess, 'run', fake_run)
    assert sr.is_storage_detected() is False
