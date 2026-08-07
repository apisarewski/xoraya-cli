def import_renderer():
    import screen.renderer as r
    return r


def test_render_idle_detected():
    r = import_renderer()
    img = r.render_idle(storage_ok=True)
    assert img.size == (128, 64)


def test_render_idle_not_detected():
    r = import_renderer()
    img = r.render_idle(storage_ok=False)
    assert img.size == (128, 64)


def test_render_downloading_progress():
    r = import_renderer()
    img = r.render_downloading(
        storage_ok=True,
        file_idx=3, total=7, pct=42.0,
        speed_mbps=1.2, eta_s=45,
    )
    assert img.size == (128, 64)


def test_render_downloading_scanning_mode():
    r = import_renderer()
    img = r.render_downloading(
        storage_ok=True,
        file_idx=0, total=0, pct=0.0,
        speed_mbps=0.0, eta_s=0, scanning=True,
    )
    assert img.size == (128, 64)


def test_render_done_download():
    r = import_renderer()
    img = r.render_done('Download complete', 'Files: 7', '1.2 GB')
    assert img.size == (128, 64)


def test_render_error():
    r = import_renderer()
    img = r.render_error('Download failed', '2 failed', 'journalctl -u xoraya-collect')
    assert img.size == (128, 64)
