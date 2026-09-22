// ============================================================================
// test_fault_detect.cpp — host-side scenario tests for the sensor fault model
// ============================================================================
//
// fault_detect.h is deliberately free of ESPHome dependencies, so the whole
// fault model can be exercised on a laptop in milliseconds instead of on a
// board over 15-minute windows. Run with test/run_tests.sh.
//
// The scenarios below are the ones that are painful to reproduce on real
// hardware: a chip that latches up, a room that goes genuinely still, a
// reboot, a gap in the data. If you change a threshold or the peer rule,
// these tell you immediately what you broke.
// ============================================================================

#include "../fault_detect.h"

#include <cstdio>
#include <cstring>
#include <string>

static int g_failures = 0;
static const char *g_scenario = "";

#define CHECK(cond)                                                     \
  do {                                                                  \
    if (!(cond)) {                                                      \
      printf("  FAIL  %s:%d  %s\n", g_scenario, __LINE__, #cond);       \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

#define CHECK_STR(actual, expected)                                     \
  do {                                                                  \
    std::string a_ = (actual);                                          \
    if (a_ != (expected)) {                                             \
      printf("  FAIL  %s:%d\n        got      \"%s\"\n"                 \
             "        expected \"%s\"\n",                               \
             g_scenario, __LINE__, a_.c_str(), (expected));             \
      g_failures++;                                                     \
    }                                                                   \
  } while (0)

static void scenario(const char *name) {
  g_scenario = name;
  printf("- %s\n", name);
  aqs::state() = aqs::State();  // fresh detector, as after a reboot
}

// Deterministic, mean-centred jitter. Over a full 30-slot window every value
// of the sequence appears, so the resulting span is exactly 2 * amp.
static float wob(int i, float amp) {
  static const int seq[7] = {3, -1, 5, -4, 2, -5, 0};
  return amp * seq[i % 7] / 5.0f;
}

// Everything alive, the usual case.
static const bool ALL_ALIVE[aqs::SIG_COUNT] = {true, true, true, true, true, true};

// ---------------------------------------------------------------------------

// The field case: a bedroom board whose BMP280 latched up and served
// 19.8530464172363 °C / 1016.51080322266 hPa for 14 days while the SHT40 and
// SCD41 kept working normally. Every liveness check stayed green.
static void test_field_freeze_is_caught() {
  scenario("field freeze (BMP280 latch-up, peers healthy)");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_T, 22.80f + wob(i + 2, 0.08f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 19.8530464172363f);
    aqs::push(aqs::SIG_BMP280_P, 1016.51080322266f);
    aqs::tick();

    // Nothing may fire during the 20-minute boot grace.
    if (i < 39)
      CHECK(!aqs::stuck(aqs::SIG_BMP280_T));
  }

  CHECK(aqs::stuck(aqs::SIG_BMP280_T));
  CHECK(aqs::stuck(aqs::SIG_BMP280_P));
  CHECK(!aqs::stuck(aqs::SIG_SHT40_T));
  CHECK(!aqs::stuck(aqs::SIG_SCD41_T));
  CHECK(!aqs::stuck(aqs::SIG_SCD41_CO2));

  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, false);
  CHECK(r.any());
  CHECK(r.chip_faulted(aqs::CHIP_BMP280));
  CHECK(!r.chip_faulted(aqs::CHIP_SHT40));
  CHECK(!r.chip_faulted(aqs::CHIP_SCD41));
  CHECK(!r.chip_faulted(aqs::CHIP_SGP41));
  CHECK(aqs::stuck_exact(aqs::SIG_BMP280_T));  // bit-identical: a certainty
  CHECK(aqs::stuck_exact(aqs::SIG_BMP280_P));
  CHECK_STR(r.describe(), "BMP280 frozen (temperature+pressure)");
  CHECK(aqs::chip_health(aqs::CHIP_BMP280) == 50.0f);
  CHECK(aqs::chip_health(aqs::CHIP_SHT40) == 100.0f);
}

// Detection latency once the board is past its boot grace: a chip that
// freezes mid-life must be caught within one window (15 min at 30 s ticks).
static void test_detection_latency_after_grace() {
  scenario("detection latency once armed");

  for (int i = 0; i < 60; i++) {  // everything healthy, grace expires
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_T, 22.80f + wob(i + 2, 0.08f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 22.65f + wob(i + 1, 0.06f));
    aqs::push(aqs::SIG_BMP280_P, 1013.2f + wob(i + 3, 0.20f));
    aqs::tick();
  }
  CHECK(!aqs::stuck(aqs::SIG_BMP280_T));

  // BMP280 freezes now. It takes a full window of frozen samples before the
  // span collapses below threshold: 30 ticks == 15 min at the 30 s cadence.
  int caught_at = -1;
  for (int i = 60; i < 100; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_T, 22.80f + wob(i + 2, 0.08f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 22.6500000f);
    aqs::push(aqs::SIG_BMP280_P, 1013.2000000f);
    aqs::tick();
    if (aqs::stuck(aqs::SIG_BMP280_T) && caught_at < 0)
      caught_at = i - 60 + 1;
  }
  printf("        caught after %d ticks (%d min at 30 s)\n", caught_at, caught_at / 2);
  CHECK(caught_at == aqs::WINDOW_SLOTS);
}

// A genuinely still room flattens every sensor at once. Each one dithers a
// little (real ADC noise) but stays under its threshold, and no peer is
// moving, so the flat rule must stay silent. This is what the peer rule buys.
static void test_quiet_room_no_false_positive() {
  scenario("dead-still room (all flat, dithering, no peer moving)");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 21.00f + wob(i, 0.005f));
    aqs::push(aqs::SIG_SCD41_T, 21.10f + wob(i + 1, 0.005f));
    aqs::push(aqs::SIG_SCD41_CO2, 500.0f + wob(i + 2, 0.4f));
    aqs::push(aqs::SIG_BMP280_T, 21.05f + wob(i + 3, 0.005f));
    aqs::push(aqs::SIG_BMP280_P, 1013.0f + wob(i + 4, 0.010f));
    aqs::tick();
  }

  for (uint8_t s = 0; s < aqs::SIG_COUNT; s++) {
    CHECK(!aqs::stuck(s));
    CHECK(!aqs::frozen_exact(s));
  }
  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, false);
  CHECK(!r.any());
  CHECK_STR(r.describe(), "OK");
}

// The whole I2C bus wedges and every chip freezes at the same instant. No peer
// is moving, so the flat rule cannot help — but bit-identical readings are
// unambiguous on their own, so the frozen rule must still catch all of them.
static void test_bus_wide_latchup() {
  scenario("bus-wide latch-up (every signal bit-identical, no peer moving)");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 21.0f);
    aqs::push(aqs::SIG_SCD41_T, 21.1f);
    aqs::push(aqs::SIG_SCD41_CO2, 500.0f);
    aqs::push(aqs::SIG_BMP280_T, 21.05f);
    aqs::push(aqs::SIG_BMP280_P, 1013.0f);
    aqs::tick();
  }

  CHECK(aqs::stuck(aqs::SIG_SHT40_T));
  CHECK(aqs::stuck(aqs::SIG_BMP280_T));
  CHECK(aqs::stuck(aqs::SIG_BMP280_P));
  CHECK(aqs::stuck(aqs::SIG_SCD41_T));
  CHECK(aqs::stuck(aqs::SIG_SCD41_CO2));
  CHECK(!aqs::stuck(aqs::SIG_SGP41_VOC));  // liveness only, always exempt

  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, false);
  CHECK(r.any());
  CHECK_STR(r.describe(),
            "SHT40 frozen (temperature); "
            "BMP280 frozen (temperature+pressure); "
            "SCD41 frozen (temperature+CO2)");
}

// The frozen rule must not need a peer, but it must still respect boot grace.
static void test_frozen_rule_respects_boot_grace() {
  scenario("frozen rule still honours boot grace");

  for (int i = 0; i < aqs::ARM_AFTER_SAMPLES - 1; i++) {
    aqs::push(aqs::SIG_BMP280_P, 1016.51f);
    aqs::tick();
    CHECK(!aqs::stuck(aqs::SIG_BMP280_P));
  }
  aqs::push(aqs::SIG_BMP280_P, 1016.51f);
  aqs::tick();
  CHECK(aqs::stuck(aqs::SIG_BMP280_P));
}

// Straight after a reboot the windows are empty. Nothing may fire until the
// boot grace has elapsed, or every power cycle would raise a false alarm.
static void test_boot_grace() {
  scenario("boot grace (empty windows must not fault)");

  CHECK(!aqs::armed());
  for (int i = 0; i < aqs::ARM_AFTER_SAMPLES - 1; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_T, 22.80f + wob(i + 2, 0.08f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 19.85f);
    aqs::push(aqs::SIG_BMP280_P, 1016.51f);
    aqs::tick();
    CHECK(!aqs::armed());
    CHECK(!aqs::stuck(aqs::SIG_BMP280_T));
  }
  aqs::push(aqs::SIG_SHT40_T, 22.70f);
  aqs::push(aqs::SIG_BMP280_T, 19.85f);
  aqs::tick();
  CHECK(aqs::armed());
}

// A gap in the data (sensor dropped off the bus and came back) means the
// samples either side are not 15 contiguous minutes. The window must restart.
static void test_data_gap_restarts_window() {
  scenario("data gap restarts the window");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 19.85f);
    aqs::tick();
  }
  CHECK(aqs::stuck(aqs::SIG_BMP280_T));

  // One NaN wipes the window; it needs another full 30 ticks to re-accuse.
  aqs::push(aqs::SIG_BMP280_T, NAN);
  aqs::tick();
  CHECK(!aqs::stuck(aqs::SIG_BMP280_T));
  CHECK(std::isnan(aqs::span_or_nan(aqs::SIG_BMP280_T)));

  for (int i = 0; i < aqs::WINDOW_SLOTS - 1; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 19.85f);
    aqs::tick();
    CHECK(!aqs::stuck(aqs::SIG_BMP280_T));
  }
  aqs::push(aqs::SIG_SHT40_T, 22.75f);
  aqs::push(aqs::SIG_SCD41_CO2, 660.0f);
  aqs::push(aqs::SIG_BMP280_T, 19.85f);
  aqs::tick();
  CHECK(aqs::stuck(aqs::SIG_BMP280_T));
}

// The SGP41 VOC index parks on exactly 100 in clean stable air for hours.
// Flat is its healthy state; it must never be stuck-checked, and it must
// never count as a moving peer for anything else either.
static void test_voc_index_never_stuck() {
  scenario("SGP41 VOC index is liveness-only");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SGP41_VOC, 100.0f);
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_BMP280_T, 19.85f);
    aqs::tick();
  }
  CHECK(!aqs::stuck(aqs::SIG_SGP41_VOC));
  CHECK(!aqs::signal_moving(aqs::SIG_SGP41_VOC));
  CHECK(!aqs::signal_flat(aqs::SIG_SGP41_VOC));

  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, false);
  CHECK(!r.chip_faulted(aqs::CHIP_SGP41));
}

// A signal that stops publishing entirely is reported as offline, not stuck —
// one fault per problem, so the Fault Detail string stays readable.
static void test_dead_beats_stuck() {
  scenario("dead signal reports offline, not stuck");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 19.85f);
    aqs::push(aqs::SIG_BMP280_P, 1016.51f);
    aqs::tick();
  }
  CHECK(aqs::stuck(aqs::SIG_BMP280_P));

  bool alive[aqs::SIG_COUNT];
  memcpy(alive, ALL_ALIVE, sizeof(alive));
  alive[aqs::SIG_BMP280_P] = false;  // pressure channel drops off the bus

  const aqs::Report &r = aqs::evaluate(alive, false);
  CHECK(r.dead[aqs::SIG_BMP280_P]);
  CHECK(!r.stuck[aqs::SIG_BMP280_P]);
  CHECK(r.stuck[aqs::SIG_BMP280_T]);
  CHECK_STR(r.describe(), "BMP280 offline (pressure), BMP280 frozen (temperature)");
  CHECK(aqs::chip_health(aqs::CHIP_BMP280) == 0.0f);
}

// After a power cycle brings the chip back, the fault must clear by itself.
static void test_recovery_clears_fault() {
  scenario("recovery clears the fault");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 19.85f);
    aqs::push(aqs::SIG_BMP280_P, 1016.51f);
    aqs::tick();
  }
  CHECK(aqs::evaluate(ALL_ALIVE, false).any());

  for (int i = 0; i < aqs::WINDOW_SLOTS; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.05f));
    aqs::push(aqs::SIG_SCD41_CO2, 650.0f + wob(i + 4, 25.0f));
    aqs::push(aqs::SIG_BMP280_T, 22.65f + wob(i + 1, 0.06f));
    aqs::push(aqs::SIG_BMP280_P, 1013.2f + wob(i + 3, 0.20f));
    aqs::tick();
  }
  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, false);
  CHECK(!r.any());
  CHECK_STR(r.describe(), "OK");
}

// Drift is written by the temperature fusion. Faulted chips must show up
// per chip with the signed offset, and clear when they come back in line.
static void test_drift_reporting() {
  scenario("drift faults are reported per chip and clear again");

  aqs::set_drift(aqs::CHIP_BMP280, true, -2.9f);
  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, false);
  CHECK(r.any());
  CHECK(r.chip_faulted(aqs::CHIP_BMP280));
  CHECK(aqs::drifting(aqs::CHIP_BMP280));
  CHECK_STR(r.describe(), "BMP280 drift -2.9 °C");
  CHECK(aqs::chip_health(aqs::CHIP_BMP280) == 50.0f);

  aqs::set_drift(aqs::CHIP_BMP280, false, 0.0f);
  const aqs::Report &r2 = aqs::evaluate(ALL_ALIVE, false);
  CHECK(!r2.any());
  CHECK_STR(r2.describe(), "OK");
}

// The PM package's extra_fault must still reach the aggregate.
static void test_extra_fault_propagates() {
  scenario("PM package extra_fault reaches the aggregate");

  const aqs::Report &r = aqs::evaluate(ALL_ALIVE, true);
  CHECK(r.any());
  CHECK_STR(r.describe(), "PM sensor fault");
  // ...but it is not attributed to any of the onboard chips.
  for (uint8_t c = 0; c < aqs::CHIP_COUNT; c++)
    CHECK(!r.chip_faulted(c));
}

// ---------------------------------------------------------------------------
// SENSITIVITY: this one documents a known weak spot rather than a guarantee.
//
// The SHT40 is the quietest part on the board and the SCD41's temperature
// channel is the noisiest. On a very still night the SHT40 can legitimately
// span less than its 0.02 °C threshold while the SCD41 still moves — which
// satisfies both halves of the stuck rule and accuses a perfectly healthy
// SHT40. This asserts the CURRENT behaviour so the trade-off is visible;
// watch the "Span SHT40 Temperature" diagnostic on a real board before
// trusting the threshold.
// ---------------------------------------------------------------------------
static void test_sensitivity_quiet_reference_noisy_peer() {
  scenario("SENSITIVITY: quiet SHT40 next to a noisy SCD41");

  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f + wob(i, 0.004f));   // span 0.008 °C
    aqs::push(aqs::SIG_SCD41_T, 22.80f + wob(i + 2, 0.15f));  // span 0.30 °C
    aqs::push(aqs::SIG_BMP280_T, 22.65f + wob(i + 1, 0.06f));
    aqs::tick();
  }
  printf("        SHT40 span %.4f °C (threshold %.4f) -> stuck=%d exact=%d\n",
         aqs::span_or_nan(aqs::SIG_SHT40_T), aqs::spec(aqs::SIG_SHT40_T).flat_span,
         (int) aqs::stuck(aqs::SIG_SHT40_T), (int) aqs::stuck_exact(aqs::SIG_SHT40_T));
  CHECK(aqs::stuck(aqs::SIG_SHT40_T));        // <-- a false positive of the FLAT rule
  CHECK(!aqs::stuck_exact(aqs::SIG_SHT40_T));  // ...but never of the frozen rule
  // Which is why the two are reported with different words:
  CHECK_STR(aqs::evaluate(ALL_ALIVE, false).describe(), "SHT40 flat (temperature)");

  // The bit-identical case is the one that is never ambiguous:
  aqs::state() = aqs::State();
  for (int i = 0; i < 60; i++) {
    aqs::push(aqs::SIG_SHT40_T, 22.70f);
    aqs::push(aqs::SIG_SCD41_T, 22.80f + wob(i + 2, 0.15f));
    aqs::tick();
  }
  CHECK(aqs::span_or_nan(aqs::SIG_SHT40_T) == 0.0f);
}

int main() {
  printf("\nfault_detect.h scenario tests\n\n");
  test_field_freeze_is_caught();
  test_detection_latency_after_grace();
  test_quiet_room_no_false_positive();
  test_bus_wide_latchup();
  test_boot_grace();
  test_frozen_rule_respects_boot_grace();
  test_data_gap_restarts_window();
  test_voc_index_never_stuck();
  test_dead_beats_stuck();
  test_recovery_clears_fault();
  test_drift_reporting();
  test_extra_fault_propagates();
  test_sensitivity_quiet_reference_noisy_peer();

  printf("\n%s (%d failure%s)\n\n", g_failures ? "FAILED" : "all passed",
         g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
