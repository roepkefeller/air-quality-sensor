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
//
// Optional chips
// --------------
// The cycle is observe() for every signal, then one tick(), then evaluate().
// Because observe() only stages, an optional package can feed its own signals
// from its own interval without caring whether it runs before or after the
// base firmware's; every window still advances exactly once per tick. That is
// how the PMSA003I participates on equal terms in the PM variant while staying
// completely silent in the base firmware, where set_installed() is never
// called. It replaced an untyped `extra_fault` boolean that could only say
// "something, somewhere, is wrong".
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
  CHIP_PMSA003I,  // optional add-on, only present in the PM variant
  CHIP_COUNT,
};

// A chip marked optional starts out NOT installed and reports nothing at all
// until a package calls set_installed(). That is what lets the base firmware
// share this header with the PM variant without inventing a PM fault on a
// board that has no PM sensor.
struct ChipSpec {
  const char *label;
  bool optional;
};

inline const ChipSpec &chip_spec(uint8_t chip) {
  static const ChipSpec C[CHIP_COUNT] = {
      {"SHT40", false}, {"BMP280", false},   {"SCD41", false},
      {"SGP41", false}, {"PMSA003I", true},
  };
  return C[chip];
}

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
  SIG_PM_2_5,
  SIG_PM_COUNT_0_3,
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
      // PM2.5 mass concentration legitimately reads 0.0 µg/m³ in clean air for
      // hours on end, so flat says nothing about its health — liveness only.
      {"PM2.5", CHIP_PMSA003I, -1.00f},
      // The 0.3 µm particle count is the opposite: indoor air always carries
      // thousands of them per 0.1 L and the number moves on every single read.
      // Bit-identical counts mean the sensor is serving a stale frame. Set to
      // 0 so ONLY the frozen rule applies — no threshold to get wrong.
      {"0.3µm count", CHIP_PMSA003I, 0.00f},
  };
  return S[sig];
}

inline const char *chip_label(uint8_t chip) { return chip_spec(chip).label; }

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

  bool chip_faulted(uint8_t chip) const {
    if (drift[chip])
      return true;
    for (uint8_t i = 0; i < SIG_COUNT; i++)
      if (spec(i).chip == chip && (dead[i] || stuck[i]))
        return true;
    return false;
  }

  bool any() const {
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
  uint16_t samples;  // saturating tick count since boot
  // Staged by observe(), committed by tick(). Staging rather than pushing
  // directly is what makes the model extensible: an optional package feeds its
  // own signals from its own interval, and every window still advances exactly
  // once per tick no matter who called observe() when, or in what order.
  bool alive[SIG_COUNT];
  float pending[SIG_COUNT];
  bool installed[CHIP_COUNT];
  bool stuck[SIG_COUNT];
  bool stuck_exact[SIG_COUNT];
  bool drift[CHIP_COUNT];
  float drift_amount[CHIP_COUNT];
  Report last;
  bool ready;
};

// Zero-initialised at static-init time: empty windows, no faults, valid=false.
// Non-optional chips are installed on first touch; optional ones wait for a
// package to claim them.
inline State &state() {
  static State s;
  if (!s.ready) {
    s.ready = true;
    for (uint8_t c = 0; c < CHIP_COUNT; c++)
      s.installed[c] = !chip_spec(c).optional;
  }
  return s;
}

// Claim an optional chip. Idempotent — call it from the owning package's
// interval and it costs nothing.
inline void set_installed(uint8_t chip, bool yes) { state().installed[chip] = yes; }
inline bool installed(uint8_t chip) { return state().installed[chip]; }

// --- Sampling ---------------------------------------------------------------

// Report the current reading of one signal. Callers stage every signal they
// own, then exactly one caller runs tick() to commit them.
inline void observe(uint8_t sig, bool alive, float value) {
  State &s = state();
  s.alive[sig] = alive;
  s.pending[sig] = alive ? value : NAN;
}

inline bool alive(uint8_t sig) { return state().alive[sig]; }

// Commit one staged reading into its window. A NaN restarts the window: a gap
// in the data means the samples either side of it are not 15 contiguous
// minutes any more.
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

// Call once per sampling tick, after every signal has been observed. Commits
// the staged readings and re-decides what is stuck.
inline void tick() {
  State &s = state();
  if (s.samples < 0xFFFFu)
    s.samples++;
  for (uint8_t i = 0; i < SIG_COUNT; i++)
    push(i, s.alive[i] ? s.pending[i] : NAN);
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
// Builds the verdict from the liveness staged by observe() plus the stuck and
// drift state. Caches it so every consumer sees one identical answer.
// Signals belonging to an uninstalled optional chip report nothing.
inline const Report &evaluate() {
  State &s = state();
  Report &r = s.last;
  r.valid = true;
  for (uint8_t i = 0; i < SIG_COUNT; i++) {
    bool on = s.installed[spec(i).chip];
    r.dead[i] = on && !s.alive[i];
    // A dead signal is reported dead, not stuck — its window is empty anyway.
    r.stuck[i] = on && s.stuck[i] && s.alive[i];
    r.stuck_exact[i] = on && s.stuck_exact[i] && s.alive[i];
  }
  for (uint8_t c = 0; c < CHIP_COUNT; c++) {
    r.drift[c] = s.installed[c] && s.drift[c];
    r.drift_amount[c] = r.drift[c] ? s.drift_amount[c] : 0.0f;
  }
  return r;
}

inline const Report &cached() { return state().last; }

// Per-chip health percentage for the diagnostic sensors:
//   100 — publishing valid data and in agreement with the other chips
//    50 — still publishing, but frozen (stuck) or drifting out of consensus
//     0 — at least one of the chip's channels has no data at all
inline float chip_health(uint8_t chip) {
  if (!installed(chip))
    return NAN;  // optional chip that is not part of this build
  const Report &r = cached();
  for (uint8_t i = 0; i < SIG_COUNT; i++)
    if (spec(i).chip == chip && r.dead[i])
      return 0.0f;
  return r.chip_faulted(chip) ? 50.0f : 100.0f;
}

}  // namespace aqs
