/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 *
 * Compiled with c++17 to avoid the spdlog/fmt-consteval mismatch that occurs
 * when building the test file itself with c++20 + -DFMT_CONSTEVAL="".
 * Provides a C-linkage hook so the c++20 test file can call Logger::init()
 * without pulling in spdlog through its own translation unit.
 */

#include "logger.hpp"

extern "C" void nef_test_init_logger() {
  Logger::init("nef_test", false, false);
}
