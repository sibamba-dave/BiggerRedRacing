#include "scenery.h"

#include <cstring>

namespace bigred {

namespace {
constexpr size_t kDatRecordSize = 22;
constexpr size_t kGrpRecordSize = 22; // same stride as MAP.DAT, confirmed session 7
constexpr size_t kShlRecordSize = 13;
} // namespace

std::vector<SceneryInstance> parse_map_dat(const uint8_t* dat_data, size_t dat_size,
                                            const uint8_t* grp_data, size_t grp_size) {
    std::vector<SceneryInstance> out;
    size_t count = dat_size / kDatRecordSize;
    out.reserve(count);

    for (size_t i = 0; i < count; ++i) {
        const uint8_t* rec = dat_data + i * kDatRecordSize;
        SceneryInstance inst;
        int32_t x, y, z;
        std::memcpy(&x, rec + 1, 4);
        std::memcpy(&y, rec + 5, 4);
        std::memcpy(&z, rec + 9, 4);
        inst.x = static_cast<float>(x);
        inst.y = static_cast<float>(y);
        inst.z = static_cast<float>(z);
        inst.shape_index = static_cast<int>(static_cast<int8_t>(rec[0])); // signed, see scenery.h
        int16_t ax, ay, az;
        std::memcpy(&ax, rec + 13, 2);
        std::memcpy(&ay, rec + 15, 2);
        std::memcpy(&az, rec + 17, 2);
        inst.angle_x = ax;
        inst.angle_y = ay;
        inst.angle_z = az;
        inst.variant_index = rec[19];
        inst.collision_category_byte = rec[20];
        inst.aux_byte = rec[21];
        out.push_back(inst);
    }

    if (grp_data != nullptr) {
        size_t grp_count = grp_size / kGrpRecordSize;
        for (size_t g = 0; g < grp_count; ++g) {
            const uint8_t* grec = grp_data + g * kGrpRecordSize;
            int16_t start, end;
            std::memcpy(&start, grec + 16, 2);
            std::memcpy(&end, grec + 18, 2);
            for (int32_t i = start; i < end && i >= 0 && static_cast<size_t>(i) < out.size(); ++i) {
                out[static_cast<size_t>(i)].group_index = static_cast<int>(g);
            }
        }
    }

    return out;
}

std::vector<std::string> parse_map_shl(const uint8_t* shl_data, size_t shl_size) {
    std::vector<std::string> out;
    size_t count = shl_size / kShlRecordSize;
    out.reserve(count);
    for (size_t i = 0; i < count; ++i) {
        const char* rec = reinterpret_cast<const char*>(shl_data + i * kShlRecordSize);
        size_t len = 0;
        while (len < kShlRecordSize && rec[len] != '\0') ++len;
        out.emplace_back(rec, len);
    }
    return out;
}

// The engine's 37-entry category table. An instance's raw MAP.DAT category byte
// is looked up here and its POSITION is the internal index everything else switches on -
// CONFIRMED: the five index/category pairs already confirmed for the animation dispatch (6/10,
// 7/20, 8/30, 9/40, 29/252) all land exactly right.
static const uint8_t kCategoryTable[37] = {
    0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x0a, 0x14, 0x1e, 0x28, 0x32, 0x33, 0x34, 0x35, 0x36,
    0x37, 0x64, 0x80, 0x81, 0x83, 0x84, 0xf4, 0xf5, 0xf6, 0xf7, 0xf8, 0xf9, 0xfa, 0xfb, 0xfc,
    0xfd, 0xfe, 0x06, 0x07, 0x08, 0xf2, 0xf3,
};

int collision_category_index(uint8_t raw_category) {
    for (int i = 0; i < 37; ++i)
        if (kCategoryTable[i] == raw_category) return i;
    return -1;
}

bool category_is_tunnel(int category_index) {
    return (category_index >= 0x13 && category_index <= 0x14) ||
           (category_index >= 0x1b && category_index <= 0x1c);
}

bool category_is_destructible(int category_index) {
    return category_index == 0x04 || category_index == 0x05 || category_index == 0x09;
}

uint8_t wrecked_variant(uint8_t variant, int category_index, int step) {
    if (category_index == 0x04) {
        // The original's case 4: `variant |= 0x40`, and that is the whole of it. The low bits keep
        // whatever texture design the instance was placed with.
        return static_cast<uint8_t>(variant | 0x40);
    }
    // Cases 5 and 9: the original OVERWRITES the byte, so nothing of the intact selection
    // survives, and its wreck animation counts it up from 0x40 before settling on 0x60.
    if (step < 0) step = 0;
    if (step >= kWreckAnimSteps) return kWreckSettled;
    return static_cast<uint8_t>(kWreckAnimFirst + step);
}

} // namespace bigred
