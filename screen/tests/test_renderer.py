from PIL import Image
import pytest


def import_renderer():
    import screen.renderer as r
    return r


def test_render_boot_returns_128x64_image():
    r = import_renderer()
    img = r.render_boot()
    assert img.size == (128, 64)
    assert img.mode == '1'


def test_render_idle_status_returns_correct_size():
    r = import_renderer()
    img = r.render_idle_status(
        device='DX11246-2', net_ok=True,
        logger_files=92, local_files=47,
        last_upload='09:14', sw1=False, sw2=False,
    )
    assert img.size == (128, 64)


def test_render_idle_status_with_error_badge():
    r = import_renderer()
    img = r.render_idle_status(
        device='DX11246-2', net_ok=False,
        logger_files=0, local_files=10,
        last_upload='never', sw1=False, sw2=False,
        error_badge='2 failed',
    )
    assert img.size == (128, 64)


def test_render_downloading_progress():
    r = import_renderer()
    img = r.render_downloading(
        device='DX11246-2',
        file_idx=3, total=7, pct=42.0,
        speed_mbps=1.2, eta_s=45,
        sw1=True, sw2=False,
    )
    assert img.size == (128, 64)


def test_render_downloading_scanning_mode():
    r = import_renderer()
    img = r.render_downloading(
        device='DX11246-2',
        file_idx=0, total=0, pct=0.0,
        speed_mbps=0.0, eta_s=0,
        sw1=True, sw2=False, scanning=True,
    )
    assert img.size == (128, 64)


def test_render_uploading():
    r = import_renderer()
    img = r.render_uploading(
        uploaded=12, total=20, skipped=2, failed=0,
        sw1=False, sw2=True,
    )
    assert img.size == (128, 64)


def test_render_done_download():
    r = import_renderer()
    img = r.render_done('Download complete', 'Files: 7', '1.2 GB')
    assert img.size == (128, 64)


def test_render_error():
    r = import_renderer()
    img = r.render_error('Download failed', '2 failed', 'journalctl -u xoraya')
    assert img.size == (128, 64)


def test_render_idle_system():
    r = import_renderer()
    img = r.render_idle_system(
        ip='192.168.1.42', disk_free='18.4G',
        uptime='3d 14h', sw1=False, sw2=False,
    )
    assert img.size == (128, 64)


def test_render_sleep():
    r = import_renderer()
    img = r.render_sleep()
    assert img.size == (128, 64)
    # Sleep screen must be all black
    assert img.getbbox() is None
