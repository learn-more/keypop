# Generates app.ico. Run: uv run --with pillow make_icon.py
from PIL import Image, ImageDraw, ImageFont

SIZES = [16, 20, 24, 32, 48, 256]
BODY = (201, 143, 27, 255)  # keycap side
FACE = (255, 196, 64, 255)  # keycap top, kSpecial in main.cpp
INK = (32, 32, 32, 255)     # kBack in main.cpp
SS = 8                      # supersampling factor


def render(size: int) -> Image.Image:
    n = size * SS
    img = Image.new("RGBA", (n, n), (0, 0, 0, 0))
    d = ImageDraw.Draw(img)
    r = n * 0.19
    d.rounded_rectangle((0, 0, n - 1, n - 1), radius=r, fill=BODY)
    # The face sits high so the body shows as a thick bottom edge.
    side, top, bottom = n * 0.07, n * 0.04, n * 0.17
    d.rounded_rectangle((side, top, n - 1 - side, n - 1 - bottom), radius=r * 0.8, fill=FACE)
    font = ImageFont.truetype("segoeuib.ttf", int(n * 0.66))
    cx, cy = n / 2, (top + n - 1 - bottom) / 2
    d.text((cx, cy), "K", font=font, fill=INK, anchor="mm")
    return img.resize((size, size), Image.LANCZOS)


frames = [render(s) for s in SIZES]
frames[-1].save("app.ico", sizes=[(s, s) for s in SIZES], append_images=frames[:-1])
print("wrote app.ico:", ", ".join(str(s) for s in SIZES))
