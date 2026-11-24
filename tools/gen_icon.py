#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Procedural copyright-free icon generator for Psi-Ops: The Mindgate Conspiracy.

Generates original geometric vector/raster artwork (deep midnight-blue Mindgate
bezel with glowing cyan psionic rings and stylized Psi emblem):
  - resources/psiops.png (256x256 RGBA8 PNG compatible with Linux SDL3 loader)
  - resources/psiops.ico (multi-size Windows icon: 16, 24, 32, 48, 64, 128, 256 px)
"""

from __future__ import annotations

import argparse
import math
from pathlib import Path

from PIL import Image, ImageDraw, ImageFilter

ICON_SIZES = [16, 24, 32, 48, 64, 128, 256]


def render_emblem(size: int = 1024) -> Image.Image:
    """Render an original procedural Mindgate / Psi emblem at supersampled resolution."""
    img = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    cx = cy = size / 2.0
    outer_r = size * 0.465

    # 1. Deep radial background disc + metallic rim (pixel-accurate math for crisp gradient)
    bg = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    px = bg.load()
    bezel_inner = outer_r * 0.905
    glow_ring_r = outer_r * 0.805
    for y in range(size):
        dy = y - cy
        dy2 = dy * dy
        for x in range(size):
            dx = x - cx
            dist = math.sqrt(dx * dx + dy2)
            if dist > outer_r + 1.5:
                continue
            # Smooth outer anti-aliased edge
            edge_a = max(0.0, min(1.0, (outer_r + 1.0 - dist) / 1.8))
            if dist >= bezel_inner:
                # Brushed cool titanium metallic bezel with directional top-left specular
                t = (dist - bezel_inner) / max(1.0, outer_r - bezel_inner)
                angle = math.atan2(dy, dx)
                spec = 0.5 + 0.5 * math.cos(angle + math.pi * 0.75)
                ridge = math.sin(t * math.pi)
                r = int(45 + 95 * ridge + 70 * spec * ridge)
                g = int(68 + 115 * ridge + 75 * spec * ridge)
                b = int(95 + 125 * ridge + 60 * spec * ridge)
                px[x, y] = (min(255, r), min(255, g), min(255, b), int(255 * edge_a))
            else:
                # Deep obsidian-indigo core with radial psionic bloom
                rn = dist / bezel_inner
                core_glow = math.exp(-((rn * 2.15) ** 2))
                ring_band = math.exp(-(((dist - glow_ring_r) / (size * 0.032)) ** 2))
                r = int(7 + 16 * core_glow + 14 * ring_band)
                g = int(16 + 78 * core_glow + 135 * ring_band)
                b = int(32 + 125 * core_glow + 185 * ring_band)
                px[x, y] = (min(255, r), min(255, g), min(255, b), int(255 * edge_a))

    img.alpha_composite(bg)

    # 2. Glowing energy rings + tactical Mindgate reticle ticks (blurred aura layer)
    glow_layer = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    gd = ImageDraw.Draw(glow_layer)

    ring_w = max(2, int(size * 0.018))
    gd.ellipse(
        [cx - glow_ring_r, cy - glow_ring_r, cx + glow_ring_r, cy + glow_ring_r],
        outline=(0, 224, 255, 220),
        width=ring_w,
    )
    inner_ring_r = outer_r * 0.66
    gd.ellipse(
        [cx - inner_ring_r, cy - inner_ring_r, cx + inner_ring_r, cy + inner_ring_r],
        outline=(56, 189, 248, 120),
        width=max(2, int(size * 0.008)),
    )

    # Cardinal reticle ticks
    tick_w = max(2, int(size * 0.014))
    for deg in (0, 90, 180, 270):
        rad = math.radians(deg)
        r0 = outer_r * 0.74
        r1 = outer_r * 0.87
        x0, y0 = cx + r0 * math.cos(rad), cy + r0 * math.sin(rad)
        x1, y1 = cx + r1 * math.cos(rad), cy + r1 * math.sin(rad)
        gd.line([(x0, y0), (x1, y1)], fill=(125, 240, 255, 235), width=tick_w)

    # 3. Stylized geometric Greek Psi (Ψ) trident emblem
    def draw_psi(draw: ImageDraw.ImageDraw, stroke_color: tuple[int, int, int, int], scale_w: float) -> None:
        sw = max(2, int(size * 0.056 * scale_w))
        # Central vertical shaft with tapered spear tip
        top_y = cy - size * 0.255
        bot_y = cy + size * 0.260
        draw.line([(cx, top_y), (cx, bot_y)], fill=stroke_color, width=sw)

        # Top spear diamond cap
        tip_w = sw * 0.85
        tip_h = size * 0.060
        draw.polygon(
            [
                (cx, top_y - tip_h * 0.85),
                (cx + tip_w, top_y + tip_h * 0.35),
                (cx, top_y + tip_h * 0.65),
                (cx - tip_w, top_y + tip_h * 0.35),
            ],
            fill=stroke_color,
        )

        # Base serif bar
        base_hw = size * 0.088
        draw.line([(cx - base_hw, bot_y), (cx + base_hw, bot_y)], fill=stroke_color, width=max(2, int(sw * 0.72)))

        # Left and right curved psionic arms (symmetric quadratic Bezier sampled cleanly)
        for sign in (-1.0, 1.0):
            pts: list[tuple[float, float]] = []
            p0 = (cx, cy + size * 0.115)
            p1 = (cx + sign * size * 0.225, cy + size * 0.095)
            p2 = (cx + sign * size * 0.215, cy - size * 0.165)
            steps = 32
            for i in range(steps + 1):
                t = i / steps
                omt = 1.0 - t
                x = omt * omt * p0[0] + 2.0 * omt * t * p1[0] + t * t * p2[0]
                y = omt * omt * p0[1] + 2.0 * omt * t * p1[1] + t * t * p2[1]
                pts.append((x, y))
            draw.line(pts, fill=stroke_color, width=max(2, int(sw * 0.86)), joint="curve")
            # Flared upper wing tip
            tx, ty = p2
            draw.polygon(
                [
                    (tx + sign * size * 0.032, ty - size * 0.042),
                    (tx + sign * size * 0.012, ty + size * 0.032),
                    (tx - sign * size * 0.034, ty + size * 0.012),
                ],
                fill=stroke_color,
            )

    # Outer cyan aura blur
    draw_psi(gd, (0, 210, 255, 255), scale_w=1.30)
    aura = glow_layer.filter(ImageFilter.GaussianBlur(radius=size * 0.026))
    img.alpha_composite(aura)
    img.alpha_composite(glow_layer.filter(ImageFilter.GaussianBlur(radius=size * 0.008)))

    # Crisp foreground emblem
    fg = Image.new("RGBA", (size, size), (0, 0, 0, 0))
    fd = ImageDraw.Draw(fg)
    fd.ellipse(
        [cx - glow_ring_r, cy - glow_ring_r, cx + glow_ring_r, cy + glow_ring_r],
        outline=(110, 240, 255, 245),
        width=max(2, int(size * 0.010)),
    )
    draw_psi(fd, (215, 250, 255, 255), scale_w=1.00)
    draw_psi(fd, (255, 255, 255, 230), scale_w=0.48)
    img.alpha_composite(fg)
    return img


def main() -> int:
    repo_root = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser(description="Generate procedural Psi-Ops Windows .ico and Linux .png icons.")
    parser.add_argument("--out-dir", type=Path, default=repo_root / "resources")
    args = parser.parse_args()

    out_dir: Path = args.out_dir
    out_dir.mkdir(parents=True, exist_ok=True)

    master = render_emblem(1024)
    png_256 = master.resize((256, 256), Image.Resampling.LANCZOS).convert("RGBA")
    png_path = out_dir / "psiops.png"
    png_256.save(png_path, format="PNG", optimize=True)

    ico_path = out_dir / "psiops.ico"
    ico_frames = [master.resize((s, s), Image.Resampling.LANCZOS).convert("RGBA") for s in ICON_SIZES]
    ico_frames[-1].save(
        ico_path,
        format="ICO",
        sizes=[(s, s) for s in ICON_SIZES],
        append_images=ico_frames[:-1],
    )

    print(f"[gen_icon] Wrote {png_path} ({png_path.stat().st_size} B, 256x256 RGBA8)")
    print(f"[gen_icon] Wrote {ico_path} ({ico_path.stat().st_size} B, sizes={ICON_SIZES})")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
