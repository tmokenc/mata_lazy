/**
 * @file validation.hh
 * @brief Private structural validation entry points for mata_lazy::detail.
 */

#pragma once

#include "mata_lazy.hh"

#include <cstdint>
#include <vector>

namespace mata_lazy::detail {

/// Return whether a level list contains no duplicates.
bool levels_unique(const std::vector<uint8_t>& levels);
/// Return whether a level-reference list contains no duplicates.
bool level_refs_unique(const std::vector<LevelRef>& refs);
/// Check structural validity of one symbolic lazy term.
bool is_valid(const SymbolicFormula& tree, const Term& root_node);

} // namespace mata_lazy::detail
