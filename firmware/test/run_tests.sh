#!/usr/bin/env bash
# Host-side tests for the sensor fault model. No hardware, no ESPHome, ~1s.
set -euo pipefail
cd "$(dirname "$0")"
out="$(mktemp -d)/test_fault_detect"
c++ -std=gnu++17 -Wall -Wextra -Werror -O2 -o "$out" test_fault_detect.cpp
"$out"
