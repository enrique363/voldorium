"""Side-by-side comparison of 4:3 and anamorphic-16:9 screenshots.

usage: python -I ws_compare.py [shots_dir]
For each shotN.png in normal/ and widescreen/: left = 4:3 as captured (640x480),
right = widescreen capture stretched to 853x480 (how a 16:9 display shows it).
Writes compare_N.png into shots_dir.
"""
import os
import sys

from PIL import Image, ImageDraw

SC = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..")


def main():
    d = sys.argv[1] if len(sys.argv) > 1 else os.path.join(SC, "traces", "shots")
    n = 1
    while os.path.exists(os.path.join(d, "normal", f"shot{n}.png")):
        a = Image.open(os.path.join(d, "normal", f"shot{n}.png")).convert("RGB")
        b = Image.open(os.path.join(d, "widescreen", f"shot{n}.png")).convert("RGB").resize((853, 480), Image.LANCZOS)
        out = Image.new("RGB", (640 + 16 + 853, 480 + 24), (24, 24, 24))
        out.paste(a, (0, 24))
        out.paste(b, (656, 24))
        dr = ImageDraw.Draw(out)
        dr.text((4, 4), "4:3 original (640x480)", fill=(230, 230, 230))
        dr.text((660, 4), "16:9 Hor+: aspect_scale_x 0.75, stretched to 853x480", fill=(230, 230, 230))
        path = os.path.join(d, f"compare_{n}.png")
        out.save(path)
        print(path)
        n += 1


if __name__ == "__main__":
    main()
