/**
 * @file alphabet_store.hh
 * @brief Private alphabet storage and resolution helpers for mata_lazy::detail.
 */

#pragma once

#include "state_types.hh"

#include <stdexcept>
#include <string>
#include <vector>

namespace mata_lazy::detail {

/**
 * @brief Owns resolved visible alphabets for reconstructed lazy exec nodes.
 */
class AlphabetStore {
    /**
     * @brief Translate one visible symbol name into the resolved alphabet of a node level.
     * @param node_id Reconstructed exec node id.
     * @param level Visible level within the node.
     * @param symbol_name Stable visible symbol name to translate.
     * @param resolved_symbol Output parameter receiving the translated symbol on success.
     * @return `true` if the symbol exists in the resolved alphabet, `false` otherwise.
     */
    bool try_translate_symbol_name_to_resolved(
            NodeId node_id, uint8_t level, const std::string& symbol_name, mata::Symbol& resolved_symbol) const;

    /// Resolved alphabet for each (node, level) pair — translated into the unified symbol namespace.
    std::vector<SmallVec2<mata::OnTheFlyAlphabet>> level_alphabets{};
    /// Sorted symbol list for each (node, level) pair, derived from the resolved alphabets.
    std::vector<SmallVec2<std::vector<mata::Symbol>>> symbol_lists{};
    /// Effective symbol list for each (node, level) pair: the subset of canonical symbols that the
    /// node's subtree can actually produce (intersection propagated bottom-up through Intersect nodes).
    std::vector<SmallVec2<std::vector<mata::Symbol>>> effective_symbol_lists{};

public:
    /**
     * @brief Construct an empty store.
     */
    AlphabetStore() = default;

    /**
     * @brief Build resolved level alphabets for the reachable reconstructed exec nodes.
     * @param nodes Reconstructed exec nodes.
     * @param root_id Root exec node id.
     * @param nfas Source NFA leaves.
     * @param nfts Source NFT leaves.
     * @param plan_at_node Per-node type-erased plan pointers (parallel to @p nodes).
     *                     For @c ExecKind::Project: @c const ProjectPlan*; for
     *                     @c ExecKind::SyncProduct: @c const CompiledSyncPlan*; nullptr otherwise.
     * @param root_level_alphabets Optional explicit root alphabets.
     */
    AlphabetStore(
            const std::vector<ExecNode>& nodes, NodeId root_id, const std::vector<mata::nfa::Nfa>& nfas,
            const std::vector<mata::nft::Nft>& nfts, const std::vector<const void*>& plan_at_node,
            const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets = nullptr);

    /**
     * @brief Translate a leaf-local symbol to the resolved visible symbol of a node level.
     * @tparam Automaton Leaf automaton type providing `alphabet`.
     * @param automaton Source leaf automaton.
     * @param node_id Reconstructed exec node whose visible alphabet is queried.
     * @param level Visible level within the reconstructed node.
     * @param local_symbol Leaf-local symbol to translate.
     * @param resolved_symbol Output parameter receiving the translated symbol on success.
     * @return `true` if the symbol is present in the resolved alphabet, `false` otherwise.
     */
    template<typename Automaton>
    bool try_resolve_symbol(
            const Automaton& automaton, NodeId node_id, uint8_t level, mata::Symbol local_symbol,
            mata::Symbol& resolved_symbol) const {
        const std::string symbol_name = symbol_name_for(automaton.alphabet, local_symbol);
        return try_translate_symbol_name_to_resolved(node_id, level, symbol_name, resolved_symbol);
    }

    /**
     * @brief Translate an NFT local symbol to the resolved visible symbol of a node level.
     * @param nft Source NFT leaf.
     * @param source_level NFT level from which `local_symbol` comes.
     * @param node_id Reconstructed exec node whose visible alphabet is queried.
     * @param result_level Visible level within the reconstructed node.
     * @param local_symbol Leaf-local symbol to translate.
     * @param resolved_symbol Output parameter receiving the translated symbol on success.
     * @return `true` if the symbol is present in the resolved alphabet, `false` otherwise.
     */
    bool try_resolve_symbol(
            const mata::nft::Nft& nft, uint8_t source_level, NodeId node_id, uint8_t result_level,
            mata::Symbol local_symbol, mata::Symbol& resolved_symbol) const;

    const mata::OnTheFlyAlphabet& level_alphabet(NodeId node_id, uint8_t level) const {
        return level_alphabets[node_id][level];
    }

    const SmallVec2<std::vector<mata::Symbol>>& level_symbols(NodeId node_id) const { return symbol_lists[node_id]; }

    const SmallVec2<std::vector<mata::Symbol>>& effective_level_symbols(NodeId node_id) const {
        return effective_symbol_lists[node_id];
    }

    /**
     * @brief Convert a symbol to its stable visible name.
     * @param alphabet Source alphabet used for reverse translation.
     * @param symbol Symbol value to stringify.
     * @return Stable visible name, or the numeric symbol value when reverse translation is unavailable.
     */
    static std::string symbol_name_for(const mata::Alphabet* alphabet, mata::Symbol symbol);
};

} // namespace mata_lazy::detail
