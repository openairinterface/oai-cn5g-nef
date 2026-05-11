/*
 * SPDX-License-Identifier: LicenseRef-CSSL-1.0
 */

#include "nef_json_utils.hpp"

#include <unordered_set>

namespace oai::nef::app {

rfl::Generic nef_merge_patch(rfl::Generic base, const rfl::Generic& patch) {
  // Rule 1: if patch is not an object, it replaces base entirely.
  const auto* patch_obj =
      std::get_if<rfl::Generic::Object>(&patch.variant());
  if (!patch_obj) {
    return patch;
  }

  // Collect keys that the patch sets to null (rule 2: delete those keys).
  std::unordered_set<std::string> keys_to_delete;
  for (const auto& [key, val] : *patch_obj) {
    if (val.is_null()) {
      keys_to_delete.insert(key);
    }
  }

  // Obtain a mutable base object.
  // If base is not an object, treat it as an empty object (RFC 7396 §2).
  rfl::Generic::Object base_copy;
  if (const auto* b =
          std::get_if<rfl::Generic::Object>(&base.variant())) {
    base_copy = *b;
  }
  // else: base_copy stays empty — patch is applied to an empty object.

  // Build result: copy base entries, skipping keys deleted by patch.
  // rfl::Object is a vector-of-pairs with no erase(); rebuild is required.
  rfl::Generic::Object result;
  for (const auto& [key, val] : base_copy) {
    if (keys_to_delete.find(key) == keys_to_delete.end()) {
      result.insert(key, val);
    }
  }

  // Apply non-null patch entries (rule 3: recurse on nested objects; rule 4:
  // arrays replace).
  for (const auto& [key, val] : *patch_obj) {
    if (val.is_null()) {
      continue;  // already excluded above (rule 2)
    }

    const auto base_child_result = result.get(key);
    if (base_child_result) {
      // Key exists in base: recurse if both are objects, else replace.
      rfl::Generic base_child = base_child_result.value();
      const bool base_is_obj =
          std::get_if<rfl::Generic::Object>(&base_child.variant()) != nullptr;
      const bool patch_is_obj =
          std::get_if<rfl::Generic::Object>(&val.variant()) != nullptr;

      if (base_is_obj && patch_is_obj) {
        result[key] = nef_merge_patch(std::move(base_child), val);  // rule 3
      } else {
        result[key] = val;  // rule 4: replace
      }
    } else {
      // Key absent in base: insert from patch.
      result.insert(key, val);
    }
  }

  return rfl::Generic(std::move(result));
}

}  // namespace oai::nef::app
