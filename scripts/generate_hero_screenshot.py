#!/usr/bin/env python3
# Copyright 2026 SoEasy Team. All rights reserved.
# Use of this source code is governed by an Apache-2.0 license.

"""
Automated Product Hero Showcase Generator for SoEasy.
1. Invokes the C++ 'render_hero_shot' binary under QT_QPA_PLATFORM=offscreen.
2. Applies multi-layer gaussian ambient shadow, subtle 1px border, and 10px rounded corners.
3. Outputs pristine PNG to docs/screenshot.png.
"""

import os
import subprocess
from PIL import Image, ImageDraw, ImageFilter

def main():
    root = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
    raw_path = "/tmp/soeasy_real_window_raw.png"
    out_path = os.path.join(root, "docs", "screenshot.png")
    bin_path = os.path.join(root, "build", "render_hero_shot")

    if not os.path.exists(bin_path):
        print(f"Building render_hero_shot first...")
        subprocess.check_call(["cmake", "--build", os.path.join(root, "build"), "--target", "render_hero_shot", "-j4"])

    print("Running render_hero_shot (offscreen)...")
    env = os.environ.copy()
    env["QT_QPA_PLATFORM"] = "offscreen"
    subprocess.check_call([bin_path, raw_path], env=env)

    print("Post-processing screenshot with multi-layer shadow and rounded corners...")
    raw = Image.open(raw_path).convert("RGBA")
    w, h = raw.size

    radius = 10
    mask = Image.new("L", (w, h), 0)
    draw = ImageDraw.Draw(mask)
    draw.rounded_rectangle((0, 0, w, h), radius=radius, fill=255)

    rounded_win = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    rounded_win.paste(raw, (0, 0), mask)

    border_draw = ImageDraw.Draw(rounded_win)
    border_draw.rounded_rectangle((0, 0, w - 1, h - 1), radius=radius, outline=(203, 213, 225, 220), width=1)

    padding = 48
    canvas_w = w + padding * 2
    canvas_h = h + padding * 2

    # Layer 1: Ambient deep shadow
    s1 = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    s1_draw = ImageDraw.Draw(s1)
    s1_draw.rounded_rectangle((padding, padding + 16, padding + w, padding + h + 16), radius=radius + 2, fill=(15, 23, 42, 55))
    s1 = s1.filter(ImageFilter.GaussianBlur(radius=26))

    # Layer 2: Soft spread shadow
    s2 = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    s2_draw = ImageDraw.Draw(s2)
    s2_draw.rounded_rectangle((padding, padding + 6, padding + w, padding + h + 6), radius=radius + 1, fill=(15, 23, 42, 45))
    s2 = s2.filter(ImageFilter.GaussianBlur(radius=10))

    # Layer 3: Crisp contact shadow
    s3 = Image.new("RGBA", (canvas_w, canvas_h), (0, 0, 0, 0))
    s3_draw = ImageDraw.Draw(s3)
    s3_draw.rounded_rectangle((padding, padding + 2, padding + w, padding + h + 2), radius=radius, fill=(15, 23, 42, 35))
    s3 = s3.filter(ImageFilter.GaussianBlur(radius=3))

    final_img = Image.alpha_composite(s1, s2)
    final_img = Image.alpha_composite(final_img, s3)
    final_img.paste(rounded_win, (padding, padding), rounded_win)

    os.makedirs(os.path.dirname(out_path), exist_ok=True)
    final_img.save(out_path, "PNG")
    print(f"Done! Final polished screenshot generated at: {out_path} ({final_img.size[0]}x{final_img.size[1]})")

if __name__ == "__main__":
    main()
