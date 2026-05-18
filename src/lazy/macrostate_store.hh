/**
 * @file macrostate_store.hh
 * @brief Private macrostate storage declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "state_types.hh"

#include "mata/utils/two-dimensional-map.hh"

#include <cassert>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <unordered_map>
#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief Interning storage for reconstructed lazy macrostates.
 */
struct MacroStateStore {
    /// Sparse storage used for complement subset states.
    using SetStore = std::unordered_map<MacroStateId, SetState>;
    /// Sparse storage used for union tag states.
    using TaggedStore = std::unordered_map<MacroStateId, TaggedState>;
    /// Maximum total elements (lhs_states × rhs_states) for the dense pair-store matrix to be feasible.
    /// This is a tunable heuristic: smaller values save memory at the cost of more hashing, while larger
    /// values allow faster lookups for larger state spaces at the cost of more memory usage.
    /// At MacroStateId = uint32_t (4 bytes per cell) the cap is the per-pair-store upper bound on dense
    /// matrix memory, so 1M cells = 4 MB ceiling. The matrix is allocated up front for every product-like
    /// node whose bounds fit, so a generous cap pays for itself only when the search actually visits a
    /// large fraction of (lhs, rhs) combinations.
    /// TODO: Maybe make this configurable at runtime and/or adapt it dynamically based on observed state counts.
    static constexpr size_t DENSE_PAIR_MAX_MATRIX_SIZE = 1'000'000;

    /// Return whether a dense matrix pair store is feasible for the given bounds.
    static bool can_use_dense_pair_store(std::optional<size_t> lhs_bound, std::optional<size_t> rhs_bound);

    /**
     * @brief Interning store for pair macrostates.
     */
    struct PairStore {
        /// Storage mode chosen for the pair store.
        enum class Mode : uint8_t {
            SparseHash,
            DenseMatrix,
        };

        Mode mode;
        std::unordered_map<MacroStateId, PairState> sparse_pairs;
        std::vector<PairState> dense_pairs;
        std::unique_ptr<mata::utils::TwoDimensionalMap<MacroStateId, false, DENSE_PAIR_MAX_MATRIX_SIZE>> dense_index;

        /// Build an empty sparse pair store.
        PairStore();
        /// Build a pair store using bounds that allow dense indexing.
        explicit PairStore(size_t lhs_bound, size_t rhs_bound);

        /// Lookup an interned pair by id.
        PairState get(MacroStateId id) const;
        /// Intern one pair state and return its canonical id.
        MacroStateId intern(PairState pair);
    };

    std::vector<size_t> node_to_store_index;
    std::vector<PairStore> pair_stores;
    std::vector<SetStore> set_stores;
    std::vector<TaggedStore> tagged_stores;

    /// Build an empty macrostate store.
    MacroStateStore();
    /// Build stores sized for the reconstructed exec DAG.
    explicit MacroStateStore(
            const std::vector<ExecNode>& nodes, const std::vector<mata::nfa::Nfa>& nfas,
            const std::vector<mata::nft::Nft>& nfts);

    PairState get_pair(NodeId idx, MacroStateId id) const { return pair_stores[node_to_store_index[idx]].get(id); }

    const SetState& get_set(NodeId idx, MacroStateId id) const {
        const auto it = set_stores[node_to_store_index[idx]].find(id);
        assert(it != set_stores[node_to_store_index[idx]].end());
        return it->second;
    }

    TaggedState get_tagged(NodeId idx, MacroStateId id) const {
        const auto it = tagged_stores[node_to_store_index[idx]].find(id);
        assert(it != tagged_stores[node_to_store_index[idx]].end());
        return it->second;
    }

    /// Intern a subset state for one exec node.
    MacroStateId intern(const NodeId idx, SetState states);
    /// Intern a pair state for one exec node.
    MacroStateId intern(const NodeId idx, const PairState pair);
    /// Intern a tagged state for one exec node.
    MacroStateId intern(const NodeId idx, const TaggedState& tagged);
};

} // namespace mata::nft::lazy::detail
