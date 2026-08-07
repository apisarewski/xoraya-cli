from PIL import Image, ImageDraw, ImageFont

WIDTH  = 128
HEIGHT = 64

# This SSD1306 panel is physically two-color: any pixel lit within the top
# 16 rows renders yellow, everything below renders blue. Keep all content
# below this line so the whole screen reads as a single color.
SAFE_TOP = 16


def _canvas():
    img = Image.new('1', (WIDTH, HEIGHT), 0)
    return img, ImageDraw.Draw(img)


def _font(size):
    return ImageFont.load_default(size=size)


_HEADER_FONT = _font(14)   # brand line, drawn in the yellow band
_STATUS_FONT = _font(11)   # combined storage/download line
_BODY_FONT   = _font(12)   # file/pct, speed/eta, done/error text, idle status lines
_VALUE_FONT  = _font(18)   # big hero values (SCANNING, done stats)
_VALUE_FONT_SM = _font(14) # big hero values when two lines must fit
_SMALL_FONT  = _font(9)    # error/detail lines (may hold longer free text)


def _bar(d, pct, y, height=10):
    d.rectangle([(0, y), (127, y + height - 1)], outline=1)
    w = max(0, int(pct / 100.0 * 126))
    if w > 0:
        d.rectangle([(1, y + 1), (w, y + height - 2)], fill=1)


def _centered_text(d, y, text, font):
    w = d.textbbox((0, 0), text, font=font)[2]
    d.text(((WIDTH - w) // 2, y), text, font=font, fill=1)


def _right_text(d, y, text, font):
    w = d.textbbox((0, 0), text, font=font)[2]
    d.text((WIDTH - w, y), text, font=font, fill=1)


def _status_line(d, y, storage_ok, download_on):
    d.text((0, y), f'STORAGE: {"YES" if storage_ok else "NO"}', font=_STATUS_FONT, fill=1)
    _right_text(d, y, f'DL: {"ON" if download_on else "OFF"}', _STATUS_FONT)


def _header(d):
    _centered_text(d, 0, 'DISTALMOTION SA', _HEADER_FONT)


def render_idle(storage_ok):
    img, d = _canvas()
    _header(d)
    _centered_text(d, 23, f'Storage: {"DETECTED" if storage_ok else "NOT FOUND"}', _BODY_FONT)
    _centered_text(d, 43, 'Download: OFF', _BODY_FONT)
    return img


def render_downloading(storage_ok, file_idx, total, pct,
                        speed_mbps, eta_s, scanning=False):
    img, d = _canvas()
    _header(d)
    _status_line(d, SAFE_TOP, storage_ok, download_on=True)

    if scanning or total == 0:
        _centered_text(d, 38, 'SCANNING', _VALUE_FONT)
    else:
        d.text((0, 28), f'File {file_idx} / {total}', font=_BODY_FONT, fill=1)
        d.text((94, 28), f'{int(pct)}%', font=_BODY_FONT, fill=1)
        _bar(d, pct, y=41, height=9)
        m, s = divmod(int(eta_s), 60)
        d.text((0, 51), f'{speed_mbps:.1f} MB/s', font=_BODY_FONT, fill=1)
        d.text((70, 51), f'ETA {m}:{s:02d}', font=_BODY_FONT, fill=1)
    return img


def render_done(title, stats_line1, stats_line2=''):
    img, d = _canvas()
    _header(d)
    _centered_text(d, SAFE_TOP, title, _BODY_FONT)
    if stats_line2:
        _centered_text(d, 32, stats_line1, _VALUE_FONT_SM)
        _centered_text(d, 48, stats_line2, _VALUE_FONT_SM)
    else:
        _centered_text(d, 38, stats_line1, _VALUE_FONT)
    return img


def render_error(title, detail='', hint=''):
    img, d = _canvas()
    _header(d)
    d.text((0, SAFE_TOP), f'!  {title}', font=_BODY_FONT, fill=1)
    if detail:
        d.text((0, 30), detail, font=_SMALL_FONT, fill=1)
    if hint:
        d.text((0, 43), 'Logs:', font=_SMALL_FONT, fill=1)
        d.text((0, 54), hint, font=_SMALL_FONT, fill=1)
    return img
