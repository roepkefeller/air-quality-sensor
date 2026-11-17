#pragma once
// ============================================================================
// reset_reason.h — why did this boot happen?
// ============================================================================
//
// Kept separate from fault_detect.h on purpose: this one pulls in an ESP-IDF
// header, and fault_detect.h has to stay compilable on a host so the scenario
// tests can run without a toolchain.
//
// This matters more than it looks for this board. A latched-up sensor is not
// cleared by a software reset — only by actually losing power. So when a chip
// comes back (or doesn't) after a reboot, the first question is always "was
// that a real power cycle?". Before this, the firmware could not answer it:
// "Last Crash Reason" was a placeholder that only ever said "None".
//
// Read it together with the fault entities:
//   BMP280 still frozen + "Software reset"   → reboot did not help, pull power
//   BMP280 still frozen + "Power-on"         → the chip is genuinely failing
//   BMP280 clean        + "Power-on"         → the latch-up cleared as expected
// ============================================================================

#include <esp_system.h>

namespace aqs {

inline const char *reset_reason_string() {
  switch (esp_reset_reason()) {
    case ESP_RST_POWERON:
      return "Power-on (cold boot)";
    case ESP_RST_EXT:
      return "External reset pin";
    case ESP_RST_SW:
      return "Software reset";  // OTA, restart button, App.safe_reboot()
    case ESP_RST_PANIC:
      return "Panic / exception";
    case ESP_RST_INT_WDT:
      return "Interrupt watchdog";
    case ESP_RST_TASK_WDT:
      return "Task watchdog";
    case ESP_RST_WDT:
      return "Other watchdog";
    case ESP_RST_BROWNOUT:
      return "Brownout (supply sag)";
    case ESP_RST_DEEPSLEEP:
      return "Wake from deep sleep";
    case ESP_RST_SDIO:
      return "SDIO reset";
    default:
      return "Unknown";
  }
}

}  // namespace aqs
