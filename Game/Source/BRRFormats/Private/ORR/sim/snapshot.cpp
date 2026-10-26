#include "snapshot.h"

#include <cstring>
#include <type_traits>

#include "race_session.h"

namespace bigred {

static_assert(std::is_trivially_copyable<VehicleSim>::value,
              "VehicleSim travels as raw bytes in a resync - see snapshot.h");

namespace {

constexpr uint32_t kMagic = 0x5352424f;   // "OBRS"
constexpr uint32_t kVersion = 3;   // 2: the rules, the finish deadline; 3: the speed class

struct Out {
    std::vector<uint8_t> b;
    void raw(const void* p, size_t n) {
        const uint8_t* c = static_cast<const uint8_t*>(p);
        b.insert(b.end(), c, c + n);
    }
    void u32(uint32_t v) {
        for (int k = 0; k < 4; ++k) b.push_back(static_cast<uint8_t>(v >> (8 * k)));
    }
    void i32(int32_t v) { u32(static_cast<uint32_t>(v)); }
    void flag(bool v) { b.push_back(v ? 1 : 0); }
    template <typename T>
    void vec(const std::vector<T>& v) {
        static_assert(std::is_trivially_copyable<T>::value, "raw vector");
        u32(static_cast<uint32_t>(v.size()));
        if (!v.empty()) raw(v.data(), v.size() * sizeof(T));
    }
};

struct In {
    const uint8_t* p;
    const uint8_t* end;
    bool ok = true;
    bool raw(void* dst, size_t n) {
        if (!ok || static_cast<size_t>(end - p) < n) return ok = false;
        std::memcpy(dst, p, n);
        p += n;
        return true;
    }
    uint32_t u32() {
        uint8_t c[4] = {};
        raw(c, 4);
        return c[0] | (c[1] << 8) | (c[2] << 16) | (static_cast<uint32_t>(c[3]) << 24);
    }
    int32_t i32() { return static_cast<int32_t>(u32()); }
    bool flag() {
        uint8_t c = 0;
        raw(&c, 1);
        return c != 0;
    }
    template <typename T>
    void vec(std::vector<T>& v) {
        const uint32_t n = u32();
        if (!ok || n > static_cast<size_t>(end - p) / sizeof(T)) {
            ok = false;
            return;
        }
        v.resize(n);
        if (n) raw(v.data(), n * sizeof(T));
    }
};

// A pointer into the level's object lists, as an index: -1 none, the drivable list's index,
// or 0x100000 + the blocker list's.
int32_t object_index(const SimTerrain* t, const SimObject* o) {
    if (!o || !t) return -1;
    if (!t->objects.empty() && o >= t->objects.data() && o < t->objects.data() + t->objects.size())
        return static_cast<int32_t>(o - t->objects.data());
    if (!t->blockers.empty() && o >= t->blockers.data() && o < t->blockers.data() + t->blockers.size())
        return 0x100000 + static_cast<int32_t>(o - t->blockers.data());
    return -1;
}

const SimObject* object_at(const SimTerrain* t, int32_t i) {
    if (!t || i < 0) return nullptr;
    if (i >= 0x100000) {
        const size_t k = static_cast<size_t>(i - 0x100000);
        return k < t->blockers.size() ? &t->blockers[k] : nullptr;
    }
    return static_cast<size_t>(i) < t->objects.size() ? &t->objects[static_cast<size_t>(i)] : nullptr;
}

} // namespace

// Everything below needs the private members of the three classes, which name this struct as
// a friend.
struct SnapshotAccess {
    static void progress(Out& o, const RacerProgress& p) {
        o.i32(p.gate); o.i32(p.lateral); o.i32(p.reached); o.i32(p.reached_lateral);
        o.i32(p.along_permille); o.i32(p.lap); o.flag(p.finished);
        o.u32(p.last_check); o.u32(p.last_touched); o.u32(p.window); o.flag(p.skipped_line);
        o.u32(p.window_start); o.flag(p.take_next_gate); o.flag(p.missed_window_now);
        o.vec(p.checks_seen);
        o.i32(p.checks_missing); o.flag(p.line_crossed_once); o.u32(p.lap_start_tick);
        o.flag(p.missed_check); o.u32(p.missed_check_tick);
        o.i32(p.wrong_way_ticks); o.i32(p.prev_gate); o.i32(p.prev_lateral);
        o.u32(p.finish_tick); o.vec(p.lap_ticks); o.i32(p.place);
        o.flag(p.missed_check_now); o.flag(p.wrong_way_now); o.flag(p.wrong_way);
        o.flag(p.gate_crossed); o.flag(p.checkpoint_crossed); o.flag(p.lap_crossed);
    }
    static void progress(In& i, RacerProgress& p) {
        p.gate = i.i32(); p.lateral = i.i32(); p.reached = i.i32(); p.reached_lateral = i.i32();
        p.along_permille = i.i32(); p.lap = i.i32(); p.finished = i.flag();
        p.last_check = static_cast<uint8_t>(i.u32());
        p.last_touched = static_cast<uint8_t>(i.u32());
        p.window = static_cast<uint8_t>(i.u32());
        p.skipped_line = i.flag();
        p.window_start = static_cast<uint8_t>(i.u32());
        p.take_next_gate = i.flag();
        p.missed_window_now = i.flag();
        i.vec(p.checks_seen);
        p.checks_missing = i.i32(); p.line_crossed_once = i.flag(); p.lap_start_tick = i.u32();
        p.missed_check = i.flag(); p.missed_check_tick = i.u32();
        p.wrong_way_ticks = i.i32(); p.prev_gate = i.i32(); p.prev_lateral = i.i32();
        p.finish_tick = i.u32(); i.vec(p.lap_ticks); p.place = i.i32();
        p.missed_check_now = i.flag(); p.wrong_way_now = i.flag(); p.wrong_way = i.flag();
        p.gate_crossed = i.flag(); p.checkpoint_crossed = i.flag(); p.lap_crossed = i.flag();
    }

    static std::vector<uint8_t> save(const RaceSession& s) {
        Out o;
        o.u32(kMagic);
        o.u32(kVersion);
        o.u32(static_cast<uint32_t>(sizeof(VehicleSim)));
        o.u32(static_cast<uint32_t>(s.cars_.size()));
        o.u32(static_cast<uint32_t>(s.race_.gates_.size()));
        o.u32(static_cast<uint32_t>(s.world_ ? s.world_->wrecked.size() : 0));
        // the session
        o.u32(s.ticks_);
        o.i32(s.ghost_rate_);
        o.u32(s.band_clock_);
        {
            int32_t w[RaceRules::kWords];
            s.rules.to_words(w);
            for (int32_t v : w) o.i32(v);
        }
        o.u32(s.finish_deadline_);
        o.flag(s.two_body_contacts);
        o.vec(s.respawned_);
        o.vec(s.respawn_gate_);
        // the cars, raw, and their pointers into the level as indices
        for (const VehicleSim& c : s.cars_) {
            // The pointers are this machine's and mean nothing on another: sent as null.
            VehicleSim bare;
            std::memcpy(static_cast<void*>(&bare), &c, sizeof(VehicleSim));
            bare.tune = nullptr;
            bare.terrain = nullptr;
            bare.rules = nullptr;
            bare.world = nullptr;
            bare.field = nullptr;
            bare.push_object = nullptr;
            for (int k = 0; k < 4; ++k) bare.bridge_floor[k] = nullptr;
            o.raw(&bare, sizeof(VehicleSim));
            o.i32(object_index(c.terrain, c.push_object));
            for (int k = 0; k < 4; ++k) o.i32(object_index(c.terrain, c.bridge_floor[k]));
        }
        // the ghosts' playback
        for (const GhostDriver& g : s.ghosts_) {
            for (int k = 0; k < 6; ++k) o.i32(g.acc[k]);
            o.i32(g.time);
            o.i32(g.rate);
            o.i32(g.index);
        }
        // the race
        const RaceState& r = s.race_;
        o.i32(r.countdown_);
        o.u32(r.race_ticks_);
        o.flag(r.beeped_);
        o.flag(r.over_);
        o.u32(static_cast<uint32_t>(r.progress_.size()));
        for (const RacerProgress& p : r.progress_) progress(o, p);
        // the scenery
        if (s.world_) {
            o.vec(s.world_->wrecked);
            o.vec(s.world_->wreck_tick);
            o.i32(s.world_->animating);
        }
        return std::move(o.b);
    }

    static bool load(RaceSession& s, const uint8_t* data, size_t size) {
        In i{data, data + size};
        if (i.u32() != kMagic || i.u32() != kVersion) return false;
        if (i.u32() != sizeof(VehicleSim)) return false;
        if (i.u32() != s.cars_.size()) return false;
        if (i.u32() != s.race_.gates_.size()) return false;
        if (i.u32() != (s.world_ ? s.world_->wrecked.size() : 0)) return false;
        if (!i.ok) return false;

        // Decoded into copies first: a truncated or foreign snapshot leaves the race alone.
        const uint32_t ticks = i.u32();
        const int32_t ghost_rate = i.i32();
        const uint32_t band_clock = i.u32();
        int32_t rule_words[RaceRules::kWords];
        for (int32_t& v : rule_words) v = i.i32();
        const uint32_t finish_deadline = i.u32();
        const bool two_body = i.flag();
        std::vector<uint8_t> respawned;
        std::vector<int> respawn_gate;
        i.vec(respawned);
        i.vec(respawn_gate);
        std::vector<VehicleSim> cars(s.cars_.size());
        std::vector<int32_t> pointers(s.cars_.size() * 5);
        for (size_t k = 0; k < cars.size(); ++k) {
            i.raw(&cars[k], sizeof(VehicleSim));
            for (int m = 0; m < 5; ++m) pointers[k * 5 + static_cast<size_t>(m)] = i.i32();
        }
        std::vector<GhostDriver> ghosts = s.ghosts_;
        for (GhostDriver& g : ghosts) {
            for (int k = 0; k < 6; ++k) g.acc[k] = i.i32();
            g.time = i.i32();
            g.rate = i.i32();
            g.index = i.i32();
        }
        const int countdown = i.i32();
        const uint32_t race_ticks = i.u32();
        const bool beeped = i.flag();
        const bool over = i.flag();
        if (i.u32() != s.race_.progress_.size()) return false;
        std::vector<RacerProgress> progress(s.race_.progress_.size());
        for (RacerProgress& p : progress) SnapshotAccess::progress(i, p);
        std::vector<uint8_t> wrecked;
        std::vector<int32_t> wreck_tick;
        int animating = 0;
        if (s.world_) {
            i.vec(wrecked);
            i.vec(wreck_tick);
            animating = i.i32();
        }
        if (!i.ok || i.p != i.end) return false;
        if (respawned.size() != s.cars_.size() || respawn_gate.size() != s.cars_.size()) return false;

        // Commit. Each car keeps what points into THIS machine's memory - its tuning, the
        // terrain, the destruction state, the field - and gets the rest.
        for (size_t k = 0; k < cars.size(); ++k) {
            VehicleSim& c = s.cars_[k];
            const VehicleTuning* tune = c.tune;
            const SimTerrain* terrain = c.terrain;
            const RaceRules* rules = c.rules;
            SceneryState* world = c.world;
            VehicleSim* const* field = c.field;
            const int field_count = c.field_count, field_self = c.field_self;
            std::memcpy(static_cast<void*>(&c), &cars[k], sizeof(VehicleSim));
            c.tune = tune;
            c.terrain = terrain;
            c.rules = rules;
            c.world = world;
            c.field = field;
            c.field_count = field_count;
            c.field_self = field_self;
            c.push_object = object_at(terrain, pointers[k * 5]);
            for (int m = 0; m < 4; ++m)
                c.bridge_floor[m] = object_at(terrain, pointers[k * 5 + 1 + static_cast<size_t>(m)]);
        }
        s.ticks_ = ticks;
        s.ghost_rate_ = ghost_rate;
        s.band_clock_ = band_clock;
        s.rules.from_words(rule_words);
        s.finish_deadline_ = finish_deadline;
        s.two_body_contacts = two_body;
        s.respawned_ = std::move(respawned);
        s.respawn_gate_ = std::move(respawn_gate);
        for (size_t k = 0; k < ghosts.size(); ++k) {
            const ReplayTrack* track = s.ghosts_[k].track;
            s.ghosts_[k] = ghosts[k];
            s.ghosts_[k].track = track;
        }
        s.race_.countdown_ = countdown;
        s.race_.race_ticks_ = race_ticks;
        s.race_.beeped_ = beeped;
        s.race_.over_ = over;
        s.race_.progress_ = std::move(progress);
        if (s.world_) {
            s.world_->wrecked = std::move(wrecked);
            s.world_->wreck_tick = std::move(wreck_tick);
            s.world_->animating = animating;
            ++s.world_->revision;   // the renderer re-reads the whole state
        }
        s.link_field();
        return true;
    }
};

std::vector<uint8_t> save_race(const RaceSession& session) { return SnapshotAccess::save(session); }

bool load_race(RaceSession& session, const uint8_t* data, size_t size) {
    return SnapshotAccess::load(session, data, size);
}

} // namespace bigred
