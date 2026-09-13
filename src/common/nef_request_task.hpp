/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */
#pragma once
#include <functional>
#include <map>
#include <string>

namespace oai::nef::app {

// Response sink: invoked once nef_app has produced a result.
// Captured into the task by value, so it must own everything it touches.
using response_sink = std::function<void(int status_code, std::string body)>;

}  // namespace oai::nef::app
