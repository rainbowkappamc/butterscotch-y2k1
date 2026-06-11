#!/usr/bin/env python3
"""
h_viewer.py  –  Pygame GUI viewer for RGB565 .h files produced by img_to_h.py

Usage:
    python h_viewer.py [file.h]          # open a specific file
    python h_viewer.py                   # opens a file-picker dialog

Controls:
    Drag & drop   – load a .h file
    O / Ctrl+O    – open file dialog
    Scroll wheel  – zoom in / out
    Click & drag  – pan when zoomed
    R             – reset zoom & pan
    S             – save current view as PNG
    Esc / Q       – quit

Requires:
    pip install pygame tkinter   (tkinter ships with most Python installs)
"""

import sys
import re
import os
from pathlib import Path

try:
    import pygame
except ImportError:
    sys.exit("pygame is required.  Install it with:  pip install pygame")

import tkinter as tk
from tkinter import filedialog, messagebox

# ---------------------------------------------------------------------------
# Constants
# ---------------------------------------------------------------------------
IMG_W, IMG_H    = 256, 256
WIN_W, WIN_H    = 800, 640
ZOOM_STEP       = 0.15
ZOOM_MIN        = 0.5
ZOOM_MAX        = 8.0
BG_COLOUR       = (30, 30, 30)
PANEL_COLOUR    = (20, 20, 20)
TEXT_COLOUR     = (200, 200, 200)
ACCENT_COLOUR   = (80, 160, 255)
BORDER_COLOUR   = (60, 60, 60)
PANEL_H         = 56

# ---------------------------------------------------------------------------
# RGB565 decode
# ---------------------------------------------------------------------------
def rgb565_to_rgb(word: int):
    r = ((word >> 11) & 0x1F) << 3
    g = ((word >>  6) & 0x1F) << 3
    b = ((word >>  1) & 0x1F) << 3
    return r, g, b

# ---------------------------------------------------------------------------
# Parse .h file  →  pygame.Surface (256×256, RGB)
# ---------------------------------------------------------------------------
def load_h_file(path: Path) -> pygame.Surface:
    text = path.read_text(encoding="utf-8", errors="replace")

    # Extract all 0xXXXX / 0xXXX tokens
    tokens = re.findall(r"0[xX][0-9A-Fa-f]+", text)
    if not tokens:
        raise ValueError("No hex values found in the file.")

    expected = IMG_W * IMG_H
    if len(tokens) < expected:
        raise ValueError(
            f"Expected {expected} pixel values, found only {len(tokens)}."
        )
    if len(tokens) > expected:
        tokens = tokens[:expected]          # silently trim extras

    values = [int(t, 16) for t in tokens]

    surf = pygame.Surface((IMG_W, IMG_H))
    px   = pygame.PixelArray(surf)
    for idx, word in enumerate(values):
        x, y = idx % IMG_W, idx // IMG_W
        r, g, b = rgb565_to_rgb(word)
        px[x][y] = surf.map_rgb(r, g, b)
    del px
    return surf

# ---------------------------------------------------------------------------
# Viewer state
# ---------------------------------------------------------------------------
class Viewer:
    def __init__(self):
        pygame.init()
        self.screen  = pygame.display.set_mode((WIN_W, WIN_H), pygame.RESIZABLE)
        pygame.display.set_caption("RGB565 .h Viewer")

        self.font_sm = pygame.font.SysFont("monospace", 13)
        self.font_md = pygame.font.SysFont("monospace", 15, bold=True)

        self.image   = None     # raw 256×256 surface
        self.scaled  = None     # cached scaled surface
        self.h_path  = None

        self.zoom    = 1.0
        self.offset  = [0, 0]  # pan offset in screen pixels
        self.drag_origin  = None
        self.drag_offset  = None

        # Hint surface shown before any file is loaded
        self._make_hint()

    # ------------------------------------------------------------------
    def _make_hint(self):
        lines = [
            "Drop a .h file here",
            "or press  O  to open one",
        ]
        self.hint = [self.font_md.render(l, True, (100, 100, 100)) for l in lines]

    # ------------------------------------------------------------------
    def load(self, path_str: str):
        path = Path(path_str)
        if not path.exists():
            self._show_error(f"File not found:\n{path}")
            return
        if path.suffix.lower() != ".h":
            self._show_error("Please open a .h file.")
            return
        try:
            surf = load_h_file(path)
        except Exception as exc:
            self._show_error(str(exc))
            return

        self.image  = surf
        self.h_path = path
        self.zoom   = 1.0
        self.offset = [0, 0]
        self._cache_scaled()
        pygame.display.set_caption(f"RGB565 .h Viewer  –  {path.name}")

    def _cache_scaled(self):
        if self.image is None:
            return
        w = max(1, int(IMG_W * self.zoom))
        h = max(1, int(IMG_H * self.zoom))
        self.scaled = pygame.transform.scale(self.image, (w, h))

    # ------------------------------------------------------------------
    def open_dialog(self):
        root = tk.Tk(); root.withdraw(); root.attributes("-topmost", True)
        chosen = filedialog.askopenfilename(
            title="Open .h file",
            filetypes=[("C Header", "*.h"), ("All files", "*.*")],
            parent=root,
        )
        root.destroy()
        if chosen:
            self.load(chosen)

    def save_png(self):
        if self.image is None:
            return
        root = tk.Tk(); root.withdraw(); root.attributes("-topmost", True)
        dest = filedialog.asksaveasfilename(
            title="Save as PNG",
            defaultextension=".png",
            initialfile=self.h_path.stem + ".png" if self.h_path else "output.png",
            filetypes=[("PNG image", "*.png")],
            parent=root,
        )
        root.destroy()
        if dest:
            pygame.image.save(self.image, dest)

    def _show_error(self, msg: str):
        root = tk.Tk(); root.withdraw(); root.attributes("-topmost", True)
        messagebox.showerror("Error", msg, parent=root)
        root.destroy()

    # ------------------------------------------------------------------
    def _canvas_rect(self) -> pygame.Rect:
        """Area above the info panel."""
        sw, sh = self.screen.get_size()
        return pygame.Rect(0, 0, sw, sh - PANEL_H)

    def _image_rect(self) -> pygame.Rect | None:
        if self.scaled is None:
            return None
        cr   = self._canvas_rect()
        cx   = cr.centerx + self.offset[0]
        cy   = cr.centery + self.offset[1]
        return self.scaled.get_rect(center=(cx, cy))

    # ------------------------------------------------------------------
    def _pixel_at_mouse(self, mx: int, my: int):
        """Return (px, py, r, g, b, hex_word) for the pixel under the mouse."""
        ir = self._image_rect()
        if ir is None or not ir.collidepoint(mx, my):
            return None
        px = int((mx - ir.x) / self.zoom)
        py = int((my - ir.y) / self.zoom)
        px = max(0, min(IMG_W - 1, px))
        py = max(0, min(IMG_H - 1, py))
        r, g, b, *_ = self.image.get_at((px, py))
        # Reconstruct the approximate RGB565 word
        word = ((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3)
        return px, py, r, g, b, word

    # ------------------------------------------------------------------
    def draw(self):
        sw, sh = self.screen.get_size()
        cr     = self._canvas_rect()

        # Canvas background
        self.screen.fill(BG_COLOUR, cr)

        if self.image:
            # Chequered pattern for transparency hint (under image)
            ir = self._image_rect()
            self._draw_checker(cr)
            self.screen.blit(self.scaled, ir)

            # Thin border around image
            pygame.draw.rect(self.screen, BORDER_COLOUR,
                             ir.inflate(2, 2), 1)

            # Zoom indicator (top-left)
            ztext = self.font_sm.render(f"  {self.zoom:.2f}×", True, TEXT_COLOUR)
            self.screen.blit(ztext, (8, 8))
        else:
            # Hint text centred on canvas
            total_h = sum(s.get_height() + 6 for s in self.hint)
            y0 = cr.centery - total_h // 2
            for surf in self.hint:
                self.screen.blit(surf, (cr.centerx - surf.get_width() // 2, y0))
                y0 += surf.get_height() + 6

        # Info panel
        panel_rect = pygame.Rect(0, sh - PANEL_H, sw, PANEL_H)
        pygame.draw.rect(self.screen, PANEL_COLOUR, panel_rect)
        pygame.draw.line(self.screen, BORDER_COLOUR,
                         (0, sh - PANEL_H), (sw, sh - PANEL_H))

        mx, my = pygame.mouse.get_pos()
        info   = self._pixel_at_mouse(mx, my)

        if info:
            px, py, r, g, b, word = info
            # Colour swatch
            swatch_rect = pygame.Rect(12, sh - PANEL_H + 10, 32, 32)
            pygame.draw.rect(self.screen, (r, g, b), swatch_rect)
            pygame.draw.rect(self.screen, BORDER_COLOUR, swatch_rect, 1)

            text = (f"  Pixel ({px:>3}, {py:>3})    "
                    f"RGB  ({r:>3}, {g:>3}, {b:>3})    "
                    f"RGB565  0x{word:04X}")
            t_surf = self.font_sm.render(text, True, TEXT_COLOUR)
            self.screen.blit(t_surf, (52, sh - PANEL_H + 20))
        else:
            # Keyboard hints
            hint_str = "O open   S save PNG   Scroll zoom   Drag pan   R reset   Q quit"
            t_surf   = self.font_sm.render(hint_str, True, (80, 80, 80))
            self.screen.blit(t_surf, (12, sh - PANEL_H + 20))

        pygame.display.flip()

    def _draw_checker(self, clip: pygame.Rect):
        """Faint chequered background so transparency (black) is obvious."""
        size = max(8, int(16 * self.zoom))
        ir   = self._image_rect()
        if ir is None:
            return
        c1, c2 = (45, 45, 45), (55, 55, 55)
        for y in range(ir.top, ir.bottom, size):
            for x in range(ir.left, ir.right, size):
                cell = pygame.Rect(x, y, size, size).clip(clip)
                col  = c1 if ((x // size + y // size) % 2 == 0) else c2
                pygame.draw.rect(self.screen, col, cell)

    # ------------------------------------------------------------------
    def run(self):
        clock = pygame.time.Clock()
        while True:
            for event in pygame.event.get():

                # ---- quit ----
                if event.type == pygame.QUIT:
                    pygame.quit(); sys.exit()

                if event.type == pygame.KEYDOWN:
                    mods = pygame.key.get_mods()
                    if event.key in (pygame.K_ESCAPE, pygame.K_q):
                        pygame.quit(); sys.exit()
                    elif event.key == pygame.K_o:
                        self.open_dialog()
                    elif event.key == pygame.K_r:
                        self.zoom = 1.0; self.offset = [0, 0]
                        self._cache_scaled()
                    elif event.key == pygame.K_s:
                        self.save_png()

                # ---- drag & drop ----
                if event.type == pygame.DROPFILE:
                    self.load(event.file)

                # ---- mouse wheel zoom ----
                if event.type == pygame.MOUSEWHEEL and self.image:
                    old_zoom = self.zoom
                    self.zoom += event.y * ZOOM_STEP * self.zoom
                    self.zoom  = max(ZOOM_MIN, min(ZOOM_MAX, self.zoom))
                    # Zoom toward cursor
                    mx, my = pygame.mouse.get_pos()
                    cr = self._canvas_rect()
                    scale = self.zoom / old_zoom
                    self.offset[0] = int(mx - cr.centerx - (mx - cr.centerx - self.offset[0]) * scale)
                    self.offset[1] = int(my - cr.centery - (my - cr.centery - self.offset[1]) * scale)
                    self._cache_scaled()

                # ---- pan (click & drag) ----
                if event.type == pygame.MOUSEBUTTONDOWN and event.button == 1:
                    if self.image:
                        self.drag_origin = event.pos
                        self.drag_offset = list(self.offset)

                if event.type == pygame.MOUSEBUTTONUP and event.button == 1:
                    self.drag_origin = None

                if event.type == pygame.MOUSEMOTION and self.drag_origin:
                    dx = event.pos[0] - self.drag_origin[0]
                    dy = event.pos[1] - self.drag_origin[1]
                    self.offset[0] = self.drag_offset[0] + dx
                    self.offset[1] = self.drag_offset[1] + dy

            self.draw()
            clock.tick(60)

# ---------------------------------------------------------------------------
# Entry point
# ---------------------------------------------------------------------------
if __name__ == "__main__":
    viewer = Viewer()
    if len(sys.argv) == 2:
        viewer.load(sys.argv[1])
    viewer.run()
