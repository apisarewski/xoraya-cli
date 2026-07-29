from PIL import Image, ImageDraw, ImageFont

WIDTH  = 128
HEIGHT = 64


def _canvas():
    img = Image.new('1', (WIDTH, HEIGHT), 0)
    return img, ImageDraw.Draw(img)


def _font(size=8):
    return ImageFont.load_default(size=size)


def _sw_row(d, sw1, sw2, y=53):
    f = _font()
    d.rectangle([(0, y), (57, 63)], outline=1)
    d.text((3, y + 2), f'DL {"ON " if sw1 else "off"}', font=f, fill=1)
    d.rectangle([(70, y), (127, 63)], outline=1)
    d.text((73, y + 2), f'UP {"ON " if sw2 else "off"}', font=f, fill=1)


def _divider(d, y):
    d.line([(0, y), (127, y)], fill=1)


def _bar(d, pct, y=35, height=8):
    d.rectangle([(0, y), (127, y + height - 1)], outline=1)
    w = max(0, int(pct / 100.0 * 126))
    if w > 0:
        d.rectangle([(1, y + 1), (w, y + height - 2)], fill=1)


def render_boot(version='1.0.0'):
    img, d = _canvas()
    f = _font()
    d.text((8, 10), 'DISTALMOTION SA', font=f, fill=1)
    d.text((8, 22), 'DATALOGGER STATION', font=f, fill=1)
    _divider(d, 34)
    d.text((10, 38), f'v{version} · Starting...', font=f, fill=1)
    return img


def render_idle_status(device, net_ok, logger_files, local_files,
                       last_upload, sw1, sw2, error_badge=None):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), device or 'No logger', font=f, fill=1)
    net_str = '* NET' if net_ok else 'o ---'
    d.text((90, 0), net_str, font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), f'Logger files: {logger_files}', font=f, fill=1)
    d.text((0, 24), f'Local files:  {local_files}', font=f, fill=1)
    d.text((0, 34), f'Last:  {last_upload}', font=f, fill=1)
    if error_badge:
        d.text((0, 44), f'! {error_badge}', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_idle_system(ip, disk_free, uptime, sw1, sw2):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), 'SYSTEM', font=f, fill=1)
    d.text((72, 0), 'pi@xoraya', font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), f'IP:       {ip}', font=f, fill=1)
    d.text((0, 24), f'Disk:     {disk_free}', font=f, fill=1)
    d.text((0, 34), f'Uptime:   {uptime}', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_downloading(device, file_idx, total, pct,
                       speed_mbps, eta_s, sw1, sw2, scanning=False):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), 'v DOWNLOADING', font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), f'{device}  ->  Pi', font=f, fill=1)
    if scanning or total == 0:
        d.text((0, 26), 'Scanning...', font=f, fill=1)
    else:
        d.text((0, 26), f'File {file_idx} / {total}', font=f, fill=1)
        d.text((98, 26), f'{int(pct)}%', font=f, fill=1)
        _bar(d, pct, y=36, height=7)
        m, s = divmod(int(eta_s), 60)
        d.text((0, 45), f'{speed_mbps:.1f} MB/s', font=f, fill=1)
        d.text((80, 45), f'ETA {m}:{s:02d}', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_uploading(uploaded, total, skipped, failed, sw1, sw2):
    img, d = _canvas()
    f = _font()
    d.text((0, 0), '^ UPLOADING', font=f, fill=1)
    _divider(d, 11)
    d.text((0, 14), 'Pi  ->  Azure Blob', font=f, fill=1)
    pct = int(uploaded / total * 100) if total > 0 else 0
    d.text((0, 26), f'{uploaded} / {total} files', font=f, fill=1)
    d.text((98, 26), f'{pct}%', font=f, fill=1)
    _bar(d, pct, y=36, height=7)
    d.text((0, 45), f'{skipped} skip · {failed} fail', font=f, fill=1)
    _divider(d, 53)
    _sw_row(d, sw1, sw2)
    return img


def render_done(title, stats_line1, stats_line2=''):
    img, d = _canvas()
    f = _font()
    d.text((54, 4), 'OK', font=f, fill=1)
    d.text((10, 18), title, font=f, fill=1)
    _divider(d, 30)
    d.text((0, 34), stats_line1, font=f, fill=1)
    if stats_line2:
        d.text((0, 46), stats_line2, font=f, fill=1)
    return img


def render_error(title, detail='', hint=''):
    img, d = _canvas()
    f = _font()
    d.text((0, 2), f'!  {title}', font=f, fill=1)
    _divider(d, 14)
    if detail:
        d.text((0, 18), detail, font=f, fill=1)
    if hint:
        d.text((0, 32), 'Logs:', font=f, fill=1)
        d.text((0, 44), hint, font=f, fill=1)
    return img


def render_sleep():
    return Image.new('1', (WIDTH, HEIGHT), 0)
