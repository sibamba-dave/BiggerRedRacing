// The cabin view's dashboard: the `dash` sheet of a vehicle, redrawn by its `ndial` gauges.
//
// Pure pixel work on the 8-bit sheet, no GL, so that physics_bench can check it: the caller
// turns the sheet into RGBA (decode_tm_rgba) and uploads it where the interior's faces sample.
//
// ⚠️ THE SHEET IS PERSISTENT, as in the original: each frame draws OVER the last one. A needle
// is erased only because the script's `copy` lines lay a clean dial face over it first - which is
// why the order of the lines matters and is kept.
#pragma once

#include <array>
#include <cstdint>
#include <vector>

#include "formats/vehicle_script.h"

namespace bigred {

// What the gauges read, by SOURCE number.
struct DashValues {
    std::array<int32_t, 5> by_source{};   // 0 speed, 1 revs, 2 nitro, 3 gear, 4 height above ground
};

class Dashboard {
public:
    // The vehicle's `dash` sheet, out of its `.VHC`, and the `ndial`s of its INTERIOR section.
    // False (and !valid()) when the vehicle has no dashboard - then there is nothing to draw.
    bool load(const Archive& vhc, const VehicleTuning& tune);
    bool valid() const { return !pixels_.empty(); }
    const std::string& sheet_name() const { return sheet_; }

    // Runs every gauge once, in script order, onto the sheet.
    void draw(const DashValues& values);

    // The sheet as it stands, `kTmWidth` x `kTmHeight` palette indices.
    const std::vector<uint8_t>& pixels() const { return pixels_; }

private:
    void blit(int sx, int sy, int dx, int dy, int w, int h);
    void line(int x0, int y0, int x1, int y1, uint8_t colour);
    void fill_triangle(const int (&x)[3], const int (&y)[3], uint8_t colour);
    void plot(int x, int y, uint8_t colour);

    std::string sheet_;
    std::vector<uint8_t> pixels_;
    std::vector<DashDial> dials_;
};

} // namespace bigred
