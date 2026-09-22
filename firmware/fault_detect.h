#pragma once
// ============================================================================
// fault_detect.h — sensor fault model for the air quality sensor firmware
// ============================================================================
//
// Why this exists
// ---------------
// The original fault check was `has_state() && !isnan(state)`. That only
// catches a sensor that stops publishing. It does not catch the far nastier
// failure mode seen in the field: a BMP280 that latched up and kept happily
// publishing 19.8530464172363 °C and 1016.51080322266 hPa for 14 days —
// min == mean == max across 335 hourly values. Every liveness check was
// green the whole time; only a full power cycle (not a software reset)
// brought the chip back.
//
// So a sensor is now considered faulted if ANY of these hold:
//
//   1. dead   — no state, or NaN                     (the original check)
//   2. stuck  — its value has not moved at all over  (new)
//               a 15 minute window while another
//               chip on the board clearly did move
//   3. drift  — it disagrees with the consensus of   (new)
//               the other chips by more than 1.5 °C,
//               which in a single enclosure is a
//               fault, not a calibration offset
//
// Stuck detection
// ---------------
// Per signal we keep a ring of the last WINDOW_SLOTS samples. The detector
// is fed from its own fixed 30 s interval rather than from the sensors'
// update_interval, so "boost mode" (which drops the sensors to 5 s) does not
// shrink the window's wall-clock span. 30 slots x 30 s = a 15 minute window.
//
// There are TWO stuck rules, and the difference matters:
//
//   FROZEN (exact)  — every sample in a full window is bit-identical.
//                     This is the signature the field failure actually left:
//                     min == mean == max across 335 values, identical to the
//                     last decimal. No live sensor produces it, because every
//                     one of these parts has more ADC noise than one LSB. The
//                     rule has no threshold, so it has nothing to tune and
//                     cannot be set wrong. It fires on its own — it does not
//                     need a moving peer, so it also catches the case where
//                     the whole I2C bus wedges and every chip freezes at once.
//
//   FLAT (threshold)— max-min over a full window stays at or below the
//                     signal's flat_span. This is the softer net for a chip
//                     that is degraded rather than fully latched (dithering
//                     one or two LSBs and going nowhere). It IS tunable, and
//                     therefore it can be wrong: the SHT40 is the quietest
//                     part on the board and can legitimately out-still its
//                     own threshold on a calm night. So this rule fires only
//                     when at least one signal on a DIFFERENT chip is clearly
//                     moving, which rules out "the room simply went still".
//
// Watch the "Span <signal>" diagnostic sensors for a few days before trusting
// a flat_span; the frozen rule needs no such care.
//
// flat_span encodes which rules apply:
//    < 0  — liveness only, never stuck-checked, never counts as a peer
//   == 0  — frozen rule only (no tunable threshold)
//    > 0  — frozen rule plus the flat rule at that threshold
//
// Two signals are deliberately liveness-only (flat_span < 0):
//   * SGP41 VOC index — a self-calibrating index that parks on exactly 100
//     in clean stable air for hours. Flat is its normal, healthy state.
//   * (the PM package's PM2.5 likewise sits at 0 µg/m³ in clean air and is
//     not registered here at all.)
//
// Boot grace
// ----------
// Nothing is stuck-checked until ARM_AFTER_SAMPLES ticks have elapsed
// (40 x 30 s = 20 min). That covers both the empty-window period after a
// reboot and the settling time of the sensors themselves.
//
// State model
// -----------
// This header owns all of the detection state and exposes one cached
// Report. Both the 30 s fault interval and the "Sensor Fault" binary sensor
// read that same cached Report, so the two can never disagree — which they
// could, and did, when each carried its own copy of the condition.
// ============================================================================

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <string>

namespace aqs {

// --- Chips ------------------------------------------------------------------
enum Chip : uint8_t {
  CHIP_SHT40 = 0,
  CHIP_BMP280,
  CHIP_SCD41,
  CHIP_SGP41,
  CHIP_COUNT,
};

// --- Signals ----------------------------------------------------------------
// One entry per independently-freezable channel. The BMP280's temperature and
// pressure are listed separately on purpose: they come from separate ADC
// paths and the original code only ever checked the pressure one.
enum Signal : uint8_t {
  SIG_SHT40_T = 0,
  SIG_BMP280_T,
  SIG_BMP280_P,
  SIG_SCD41_T,
  SIG_SCD41_CO2,
  SIG_SGP41_VOC,
  SIG_COUNT,
};

// 30 samples at the detector's fixed 30 s cadence = a 15 minute window.
static const uint8_t WINDOW_SLOTS = 30;

// 40 ticks at 30 s = 20 minutes of boot grace before stuck checks arm.
static const uint16_t ARM_AFTER_SAMPLES = 40;

// Drift beyond this between chips in one enclosure is a fault, not an offset.
static const float DRIFT_LIMIT_C = 1.5f;

struct SignalSpec {
  const char *label;  // channel name, e.g. "pressure"
  uint8_t chip;
  float flat_span;    // < 0 liveness only | == 0 frozen rule only
                      // > 0 frozen rule + flat rule at this max-min threshold
};

inline const SignalSpec &spec(uint8_t sig) {
  // Thresholds sit just above each part's quantisation/noise floor:
  //   SHT40   T  ~0.01 °C LSB      SCD41  T   ~0.003 °C LSB
  //   BMP280  T  ~0.005 °C LSB     SCD41  CO2  1 ppm LSB, real air wanders
  //   BMP280  P  ~0.013 hPa RMS at 16x oversampling
  //
  // Pressure is the tightest margin of the five: 30 samples of pure BMP280
  // noise span roughly 0.05-0.09 hPa, so 0.05 sits close to the floor on a
  // very calm day. The "Span BMP280 Pressure" diagnostic sensor reports the
  // live value of exactly this quantity — watch it for a few days and raise
  // the threshold here if it ever dips near 0.05 without the chip being
  // frozen. A genuinely frozen register reads exactly 0.000.
  static const SignalSpec S[SIG_COUNT] = {
      {"temperature", CHIP_SHT40, 0.02f},
      {"temperature", CHIP_BMP280, 0.02f},
      {"pressure", CHIP_BMP280, 0.05f},
      {"temperature", CHIP_SCD41, 0.02f},
      {"CO2", CHIP_SCD41, 2.00f},
      {"VOC index", CHIP_SGP41, -1.00f},  // liveness only, see header comment
  };
  return S[sig];
}

inline const char *chip_label(uint8_t chip) {
  static const char *const N[CHIP_COUNT] = {"SHT40", "BMP280", "SCD41", "SGP41"};
  return N[chip];
}

// --- Report -----------------------------------------------------------------
// A snapshot of every fault dimension, addressable per chip and per signal so
// Home Assistant (and the fault LED) can show what actually broke.
struct Report {
  bool valid;                      // false until the first evaluate()
  bool dead[SIG_COUNT];            // no state / NaN
  bool stuck[SIG_COUNT];           // frozen, or flat while peers moved
  bool stuck_exact[SIG_COUNT];     // of those, the bit-identical ones
  bool drift[CHIP_COUNT];          // disagrees with consensus, excluded from fusion
  float drift_amount[CHIP_COUNT];  // signed °C offset from consensus
  bool extra;                      // extra_fault global (PM package)

  bool chip_faulted(uint8_t chip) const {
    if (drift[chip])
      return true;
    for (uint8_t i = 0; i < SIG_COUNT; i++)
      if (spec(i).chip == chip && (dead[i] || stuck[i]))
        return true;
    return false;
  }

  bool any() const {
    if (extra)
      return true;
    for (uint8_t c = 0; c < CHIP_COUNT; c++)
      if (chip_faulted(c))
        return true;
    return false;
  }

  // Human-readable summary for the "Fault Detail" text sensor, e.g.
  //   "BMP280 frozen (temperature+pressure), BMP280 drift -2.9 °C"
  // "frozen" and "flat" are kept distinct on purpose: frozen is a certainty,
  // flat is a threshold judgement that may want tuning.
  std::string describe() const {
    std::string out;
    for (uint8_t c = 0; c < CHIP_COUNT; c++) {
      std::string dead_list, frozen_list, flat_list, part;
      for (uint8_t i = 0; i < SIG_COUNT; i++) {
        if (spec(i).chip != c)
          continue;
        if (dead[i]) {
          if (!dead_list.empty())
            dead_list += "+";
          dead_list += spec(i).label;
        } else if (stuck[i]) {
          std::string &lst = stuck_exact[i] ? frozen_list : flat_list;
          if (!lst.empty())
            lst += "+";
          lst += spec(i).label;
        }
      }
      if (!dead_list.empty())
        part = std::string(chip_label(c)) + " offline (" + dead_list + ")";
      if (!frozen_list.empty()) {
        if (!part.empty())
          part += ", ";
        part += std::string(chip_label(c)) + " frozen (" + frozen_list + ")";
      }
      if (!flat_list.empty()) {
        if (!part.empty())
          part += ", ";
        part += std::string(chip_label(c)) + " flat (" + flat_list + ")";
      }
      if (drift[c]) {
        char buf[24];
        snprintf(buf, sizeof(buf), " drift %+.1f °C", drift_amount[c]);
        if (!part.empty())
          part += ", ";
        part += std::string(chip_label(c)) + buf;
      }
      if (!part.empty()) {
        if (!out.empty())
          out += "; ";
        out += part;
      }
    }
    if (extra) {
      if (!out.empty())
        out += "; ";
      out += "PM sensor fault";
    }
    if (out.empty())
      out = valid ? "OK" : "starting up";
    return out;
  }
};

// --- Detector state ---------------------------------------------------------
struct Window {
  float v[WINDOW_SLOTS];
  uint8_t head;
  uint8_t filled;
};

struct State {
  Window win[SIG_COUNT];
  uint16_t samples;                // saturating tick count since boot
  bool stuck[SIG_COUNT];
  bool stuck_exact[SIG_COUNT];
  bool drift[CHIP_COUNT];
  float drift_amount[CHIP_COUNT];
  Report last;
};

// Zero-initialised at static-init time: empty windows, no faults, valid=false.
inline State &state() {
  static State s;
  return s;
}

// --- Sampling ---------------------------------------------------------------

// Feed one signal. A NaN restarts the window: a gap in the data means the
// samples either side of it are not 15 contiguous minutes any more.
inline void push(uint8_t sig, float value) {
  Window &w = state().win[sig];
  if (std::isnan(value)) {
    w.head = 0;
    w.filled = 0;
    return;
  }
  w.v[w.head] = value;
  w.head = (uint8_t) ((w.head + 1) % WINDOW_SLOTS);
  if (w.filled < WINDOW_SLOTS)
    w.filled++;
}

// max-min over a full window. False while the window is still filling.
inline bool window_span(uint8_t sig, float *out) {
  const Window &w = state().win[sig];
  if (w.filled < WINDOW_SLOTS)
    return false;
  float lo = w.v[0], hi = w.v[0];
  for (uint8_t i = 1; i < WINDOW_SLOTS; i++) {
    if (w.v[i] < lo)
      lo = w.v[i];
    if (w.v[i] > hi)
      hi = w.v[i];
  }
  *out = hi - lo;
  return true;
}

// For the diagnostic span sensors — NaN while the window is still filling.
inline float span_or_nan(uint8_t sig) {
  float s;
  return window_span(sig, &s) ? s : NAN;
}

// True when a signal takes part in stuck detection at all.
inline bool stuck_checked(uint8_t sig) { return spec(sig).flat_span >= 0.0f; }

// Every sample in a full window bit-identical. The float == is deliberate:
// push() never stores NaN, so this is a plain "the register never changed"
// test, and it is exactly what a latched-up chip looks like.
inline bool frozen_exact(uint8_t sig) {
  if (!stuck_checked(sig))
    return false;
  const Window &w = state().win[sig];
  if (w.filled < WINDOW_SLOTS)
    return false;
  for (uint8_t i = 1; i < WINDOW_SLOTS; i++)
    if (w.v[i] != w.v[0])
      return false;
  return true;
}

inline bool signal_moving(uint8_t sig) {
  if (!stuck_checked(sig))
    return false;
  float s;
  return window_span(sig, &s) && s > spec(sig).flat_span;
}

// The tunable rule. flat_span == 0 means "frozen rule only", so no window can
// satisfy this one (a span of exactly 0 is already caught by frozen_exact).
inline bool signal_flat(uint8_t sig) {
  if (spec(sig).flat_span <= 0.0f)
    return false;
  float s;
  return window_span(sig, &s) && s <= spec(sig).flat_span;
}

// Is any signal on a *different* chip showing normal movement? This is what
// separates "this chip is frozen" from "the whole room is perfectly still".
inline bool peer_moving(uint8_t sig) {
  for (uint8_t i = 0; i < SIG_COUNT; i++)
    if (spec(i).chip != spec(sig).chip && signal_moving(i))
      return true;
  return false;
}

inline bool armed() { return state().samples >= ARM_AFTER_SAMPLES; }

// Call once per sampling tick, after pushing every signal.
inline void tick() {
  State &s = state();
  if (s.samples < 0xFFFFu)
    s.samples++;
  for (uint8_t i = 0; i < SIG_COUNT; i++) {
    // Frozen stands on its own; flat needs a moving peer to be meaningful.
    bool frozen = armed() && frozen_exact(i);
    bool flat = armed() && signal_flat(i) && peer_moving(i);
    s.stuck_exact[i] = frozen;
    s.stuck[i] = frozen || flat;
  }
}

// --- Drift ------------------------------------------------------------------
// Written by the temperature fusion, which is the only place that knows the
// consensus value. `amount` is the chip's signed offset from that consensus.
inline void set_drift(uint8_t chip, bool bad, float amount) {
  state().drift[chip] = bad;
  state().drift_amount[chip] = bad ? amount : 0.0f;
}

inline bool drifting(uint8_t chip) { return state().drift[chip]; }
inline float drift_amount(uint8_t chip) { return state().drift_amount[chip]; }
inline bool stuck(uint8_t sig) { return state().stuck[sig]; }
inline bool stuck_exact(uint8_t sig) { return state().stuck_exact[sig]; }

// --- Evaluation -------------------------------------------------------------
// `alive` is indexed by Signal: has_state() && !isnan(state) for each channel.
// Caches the result so every consumer sees one identical verdict.
inline const Report &evaluate(const bool alive[SIG_COUNT], bool extra) {
  State &s = state();
  Report &r = s.last;
  r.valid = true;
  r.extra = extra;
  for (uint8_t i = 0; i < SIG_COUNT; i++) {
    r.dead[i] = !alive[i];
    // A dead signal is reported dead, not stuck — its window is empty anyway.
    r.stuck[i] = s.stuck[i] && alive[i];
    r.stuck_exact[i] = s.stuck_exact[i] && alive[i];
  }
  for (uint8_t c = 0; c < CHIP_COUNT; c++) {
    r.drift[c] = s.drift[c];
    r.drift_amount[c] = s.drift_amount[c];
  }
  return r;
}

inline const Report &cached() { return state().last; }

// Per-chip health percentage for the diagnostic sensors:
//   100 — publishing valid data and in agreement with the other chips
//    50 — still publishing, but frozen (stuck) or drifting out of consensus
//     0 — at least one of the chip's channels has no data at all
inline float chip_health(uint8_t chip) {
  const Report &r = cached();
  for (uint8_t i = 0; i < SIG_COUNT; i++)
    if (spec(i).chip == chip && r.dead[i])
      return 0.0f;
  return r.chip_faulted(chip) ? 50.0f : 100.0f;
}

}  // namespace aqs
