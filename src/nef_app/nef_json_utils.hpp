/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#pragma once

#include <rfl/Generic.hpp>

namespace oai::nef::app {

/**
 * RFC 7396 merge-patch for rfl::Generic.
 *
 * Rules:
 *  1. Non-object patch replaces base entirely.
 *  2. A null value in patch deletes the corresponding key from base.
 *  3. Nested objects are merged recursively.
 *  4. Arrays replace, never merge.
 *
 * @param base   The base document (moved-from after the call).
 * @param patch  The patch document.
 * @return       The patched result.
 */
rfl::Generic nef_merge_patch(rfl::Generic base, const rfl::Generic& patch);

}  // namespace oai::nef::app
