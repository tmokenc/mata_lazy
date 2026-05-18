/**
 * @file emptiness.hh
 * @brief Private emptiness-check entry points for mata_lazy::detail.
 */

#pragma once

#include "mata_lazy.hh"

#include <vector>

namespace mata_lazy::detail {

/**
 * @brief Lazily decide emptiness of the symbolic formula rooted at @p root_node.
 * @param formula Symbolic formula DAG containing the operators and leaves.
 * @param root_node Root symbolic term to evaluate.
 * @param level_alphabets Optional explicit per-level alphabets for the root relation.
 * @return `true` when the relation is empty, `false` otherwise.
 */
bool is_empty(
        const SymbolicFormula& formula, const Term& root_node,
        const std::vector<mata::OnTheFlyAlphabet>* level_alphabets = nullptr);

} // namespace mata_lazy::detail
