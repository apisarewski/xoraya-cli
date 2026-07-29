#!/usr/bin/env python3
from luma.core.interface.serial import i2c
from luma.oled.device import ssd1306
from PIL import Image, ImageDraw, ImageFont

serial = i2c(port=1, address=0x3C)
oled   = ssd1306(serial)

img = Image.new('1', (128, 64), 0)
d   = ImageDraw.Draw(img)
f   = ImageFont.load_default(size=8)
d.text((10, 22), 'DISTALMOTION SA',    font=f, fill=1)
d.text((10, 34), 'DATALOGGER STATION', font=f, fill=1)
oled.display(img)
