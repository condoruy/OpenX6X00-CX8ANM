"""CX8ANM: the glyphs of the callsign on the boot logo (bootanim.c), from Inter Bold.
Usage: python3 tools/logo/mkcallfont.py assets/fonts bootanim/callfont.h"""
import sys
from PIL import Image, ImageDraw, ImageFont

fontdir, out = sys.argv[1], sys.argv[2]
CHARS = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ/-"
SIZE = 44
font = ImageFont.truetype(fontdir + "/Inter-Bold.ttf", SIZE)
asc, desc = font.getmetrics()
H = asc + 2
glyphs = []
for c in CHARS:
    w = int(font.getlength(c)) + 2
    im = Image.new("L", (w, H), 0)
    ImageDraw.Draw(im).text((1, 0), c, font=font, fill=255)
    glyphs.append((c, w, list(im.getdata())))
with open(out, "w") as f:
    f.write("/* Made by tools/logo/mkcallfont.py from Inter Bold %dpx: do not edit. The callsign on the boot logo */\n" % SIZE)
    f.write("#define CALLFONT_H %d\n#define CALLFONT_SPACING 3\n" % H)
    for i, (c, w, px) in enumerate(glyphs):
        f.write("static const uint8_t callfont_%d[] = {%s};\n" % (i, ",".join(str(p) for p in px)))
    f.write("static const struct { char c; uint8_t w; const uint8_t *px; } callfont[] = {\n")
    for i, (c, w, px) in enumerate(glyphs):
        f.write("    {'%s', %d, callfont_%d},\n" % (c, w, i))
    f.write("};\n")
print(len(glyphs), "glyphs, height", H, "->", out)
