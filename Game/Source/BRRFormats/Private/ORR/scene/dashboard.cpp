#include "scene/dashboard.h"

#include <algorithm>

#include "formats/texture.h" // kTmWidth/kTmHeight/kTmSize
#include "sim/fixed.h"       // the original's sine table

namespace bigred {

bool Dashboard::load(const Archive& vhc, const VehicleTuning& tune) {
    sheet_.clear();
    pixels_.clear();
    dials_.clear();
    if (tune.dash.empty() || tune.sections.size() < 2) return false;
    const auto it = vhc.find(tune.dash + ".TM");
    if (it == vhc.end() || it->second.size() != kTmSize) return false;
    sheet_ = tune.dash;
    pixels_ = it->second;
    dials_ = tune.sections[1].dials;
    return true;
}

void Dashboard::plot(int x, int y, uint8_t colour) {
    if (x < 0 || y < 0 || x >= kTmWidth || y >= kTmHeight) return;
    pixels_[static_cast<size_t>(y) * kTmWidth + static_cast<size_t>(x)] = colour;
}

// The original's blit: w x h texels from (sx,sy) to (dx,dy), row by row. The original has no bounds;
// a rectangle leaving the sheet is clipped here rather than written past it.
void Dashboard::blit(int sx, int sy, int dx, int dy, int w, int h) {
    if (w <= 0 || h <= 0) return;
    for (int row = 0; row < h; ++row) {
        const int ys = sy + row, yd = dy + row;
        if (ys < 0 || yd < 0 || ys >= kTmHeight || yd >= kTmHeight) continue;
        for (int col = 0; col < w; ++col) {
            const int xs = sx + col, xd = dx + col;
            if (xs < 0 || xd < 0 || xs >= kTmWidth || xd >= kTmWidth) continue;
            pixels_[static_cast<size_t>(yd) * kTmWidth + static_cast<size_t>(xd)] =
                pixels_[static_cast<size_t>(ys) * kTmWidth + static_cast<size_t>(xs)];
        }
    }
}

// The original's line: a Bresenham line, both ends included, error started at half the major axis.
void Dashboard::line(int x0, int y0, int x1, int y1, uint8_t colour) {
    int dx = std::abs(x1 - x0), dy = std::abs(y1 - y0);
    const int sx = x0 < x1 ? 1 : -1, sy = y0 < y1 ? 1 : -1;
    int x = x0, y = y0;
    if (dx >= dy) {
        int err = dx / 2;
        for (int i = 0; i <= dx; ++i) {
            plot(x, y, colour);
            err -= dy;
            if (err < 0) { y += sy; err += dx; }
            x += sx;
        }
    } else {
        int err = dy / 2;
        for (int i = 0; i <= dy; ++i) {
            plot(x, y, colour);
            err -= dx;
            if (err < 0) { x += sx; err += dy; }
            y += sy;
        }
    }
}

// The original's polygon fill: a convex polygon filled by scanlines, each span from its left edge to its right
// edge inclusive. For a triangle: per row, the extent of the three edges crossing it.
void Dashboard::fill_triangle(const int (&x)[3], const int (&y)[3], uint8_t colour) {
    const int top = std::min({y[0], y[1], y[2]}), bottom = std::max({y[0], y[1], y[2]});
    for (int row = top; row <= bottom; ++row) {
        int lo = 1 << 30, hi = -(1 << 30);
        for (int e = 0; e < 3; ++e) {
            const int ax = x[e], ay = y[e], bx = x[(e + 1) % 3], by = y[(e + 1) % 3];
            if ((row < ay && row < by) || (row > ay && row > by)) continue;
            int cx;
            if (ay == by) {
                lo = std::min({lo, ax, bx});
                hi = std::max({hi, ax, bx});
                continue;
            }
            cx = ax + (bx - ax) * (row - ay) / (by - ay);
            lo = std::min(lo, cx);
            hi = std::max(hi, cx);
        }
        for (int c = lo; c <= hi; ++c) plot(c, row, colour);
    }
}

void Dashboard::draw(const DashValues& values) {
    if (!valid()) return;
    const auto value = [&](const DashDial& d) -> int32_t {
        const int32_t s = d.r[1];
        return (s >= 0 && s < static_cast<int32_t>(values.by_source.size()))
                   ? values.by_source[static_cast<size_t>(s)] : 0;
    };
    const auto clamp = [](int32_t v, int32_t hi) { return std::max<int32_t>(0, std::min(v, hi)); };
    for (const DashDial& d : dials_) {
        const int32_t* r = d.r;
        switch (d.type) {
        case DashDial::Copy:
            blit(r[1], r[2], r[3], r[4], r[5], r[6]);
            break;
        case DashDial::Point: {
            // Angle 0 points UP and grows clockwise: x = c + d.sin, y = c - d.cos.
            const int32_t v = clamp(value(d), r[6]);
            const int32_t a = r[4] + (r[6] != 0 ? static_cast<int32_t>(
                                                      static_cast<int64_t>(v) * r[5] / r[6])
                                                : 0);
            const auto at = [&](int32_t ang, int32_t len, int& px, int& py) {
                px = r[2] + static_cast<int>(static_cast<int64_t>(len) * fx::sin16(ang) / 0x8000);
                py = r[3] - static_cast<int>(static_cast<int64_t>(len) * fx::cos16(ang) / 0x8000);
            };
            int xs[3], ys[3];
            at(a, r[8], xs[0], ys[0]);            // the tip
            at(a + 0x5000, r[7], xs[1], ys[1]);   // the two corners of the base
            at(a - 0x5000, r[7], xs[2], ys[2]);
            fill_triangle(xs, ys, static_cast<uint8_t>(r[10]));
            const uint8_t edge = static_cast<uint8_t>(r[9]);
            line(xs[0], ys[0], xs[1], ys[1], edge);
            line(xs[0], ys[0], xs[2], ys[2], edge);
            line(xs[1], ys[1], xs[2], ys[2], edge);
            break;
        }
        case DashDial::Pill: {
            const int32_t v = value(d);
            for (int32_t i = 0; i < r[8]; ++i) {
                const int sx = (i < v) ? r[2] : r[2] + r[9];   // lit, or the unlit one beside it
                blit(sx, r[3], r[4] + r[6] * i, r[5] + r[7] * i, r[9], r[10]);
            }
            break;
        }
        case DashDial::Block:
            break;
        case DashDial::HArea: {
            const int32_t v = clamp(value(d), r[8]);
            const int f = r[8] != 0 ? static_cast<int>(static_cast<int64_t>(v) * r[9] / r[8]) : 0;
            blit(r[4], r[5], r[6], r[7], f, r[10]);
            blit(r[2] + f, r[3], r[6] + f, r[7], r[9] - f, r[10]);
            break;
        }
        case DashDial::VArea: {
            const int32_t v = clamp(value(d), r[8]);
            const int filled =
                r[8] != 0 ? static_cast<int>(static_cast<int64_t>(v) * r[10] / r[8]) : 0;
            const int empty = r[10] - filled;
            blit(r[4], r[5], r[6], r[7], r[9], empty);
            blit(r[2], r[3] + empty, r[6], r[7] + empty, r[9], filled);
            break;
        }
        default:
            break;
        }
    }
}

} // namespace bigred
