#!/usr/bin/env python3
"""raster.py -- glyph outlines (fontTools) rasterised exactly: flattened to edges, filled by the nonzero rule on a
supersampled grid, box-filtered to coverage. No hinting, positions to a fraction of a pixel: a glyph and the strokes that
continue it past its cell (the joining strokes of Arabic) land where the font's own metrics put them.
"""
import numpy as np
from fontTools.pens.basePen import BasePen

SUPERSAMPLE = 8
CURVE_STEPS = 12


class EdgePen(BasePen):
    """Collects line edges in pixel space: x = ox + u * scale, y = oy - v * scale (y down)."""

    def __init__(self, glyphset, ox, oy, scale):
        super().__init__(glyphset)
        self.ox, self.oy, self.s = ox, oy, scale
        self.edges = []
        self.start = self.cur = None

    def _pt(self, p):
        return (self.ox + p[0] * self.s, self.oy - p[1] * self.s)

    def _moveTo(self, p):
        self.start = self.cur = self._pt(p)

    def _lineTo(self, p):
        q = self._pt(p)
        self.edges.append((self.cur, q))
        self.cur = q

    def _qCurveToOne(self, p1, p2):
        a, b, c = self.cur, self._pt(p1), self._pt(p2)
        prev = a
        for i in range(1, CURVE_STEPS + 1):
            t = i / CURVE_STEPS
            q = ((1 - t) ** 2 * a[0] + 2 * (1 - t) * t * b[0] + t * t * c[0], (1 - t) ** 2 * a[1] + 2 * (1 - t) * t * b[1] + t * t * c[1])
            self.edges.append((prev, q))
            prev = q
        self.cur = c

    def _curveToOne(self, p1, p2, p3):
        a, b, c, d = self.cur, self._pt(p1), self._pt(p2), self._pt(p3)
        prev = a
        for i in range(1, CURVE_STEPS + 1):
            t = i / CURVE_STEPS
            mt = 1 - t
            q = (mt ** 3 * a[0] + 3 * mt * mt * t * b[0] + 3 * mt * t * t * c[0] + t ** 3 * d[0],
                 mt ** 3 * a[1] + 3 * mt * mt * t * b[1] + 3 * mt * t * t * c[1] + t ** 3 * d[1])
            self.edges.append((prev, q))
            prev = q
        self.cur = d

    def _closePath(self):
        if self.cur is not None and self.start is not None and self.cur != self.start:
            self.edges.append((self.cur, self.start))
        self.cur = self.start

    _endPath = _closePath


def fill(edges, width, height):
    """Coverage (height x width floats 0..1) of the edges' nonzero-winding interior."""
    ss = SUPERSAMPLE
    H, W = height * ss, width * ss
    acc = np.zeros((H, W), dtype=np.float32)
    if not edges:
        return np.zeros((height, width), dtype=np.float32)
    e = np.array([(x0 * ss, y0 * ss, x1 * ss, y1 * ss) for (x0, y0), (x1, y1) in edges], dtype=np.float64)
    e = e[e[:, 1] != e[:, 3]]
    up = e[:, 3] > e[:, 1]
    ymin = np.where(up, e[:, 1], e[:, 3])
    ymax = np.where(up, e[:, 3], e[:, 1])
    direction = np.where(up, 1, -1)
    cols = np.arange(W) + 0.5
    for j in range(H):
        yc = j + 0.5
        m = (ymin <= yc) & (ymax > yc)
        if not m.any():
            continue
        ee = e[m]
        t = (yc - ee[:, 1]) / (ee[:, 3] - ee[:, 1])
        xs = ee[:, 0] + t * (ee[:, 2] - ee[:, 0])
        order = np.argsort(xs)
        xs, ds = xs[order], direction[m][order]
        winding = np.cumsum(ds)
        row = acc[j]
        for k in range(len(xs) - 1):
            if winding[k] != 0:
                lo, hi = xs[k], xs[k + 1]
                row[(cols >= lo) & (cols < hi)] = 1.0
    return acc.reshape(height, ss, width, ss).mean(axis=(1, 3))


def coverage(glyphset, placements, width, height):
    """Coverage of several glyphs: placements = [(glyph name, origin x px, baseline y px, scale px per unit)]."""
    edges = []
    for name, ox, oy, scale in placements:
        pen = EdgePen(glyphset, ox, oy, scale)
        glyphset[name].draw(pen)
        edges += pen.edges
    return fill(edges, width, height)
