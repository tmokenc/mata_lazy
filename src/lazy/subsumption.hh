/**
 * @file subsumption.hh
 * @brief Private subsumption and antichain-pruning declarations for mata_lazy::detail.
 */

#pragma once

#include "macrostate_store.hh"
#include "state_types.hh"

#include <mata/simlib/explicit_lts.hh>

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mata_lazy::detail {

/**
 * @brief External references needed by the lazy subsumption engine.
 */
struct SubsumptionContext {
    /// NFAs referenced by the execution DAG.
    const std::vector<mata::nfa::Nfa>& nfas;
    /// NFTs referenced by the execution DAG.
    const std::vector<mata::nft::Nft>& nfts;
    /// Execution DAG nodes used during lazy evaluation.
    const std::vector<ExecNode>& nodes;
    /// Shared store of macro states.
    const MacroStateStore& macro_store;
};

/**
 * @brief Cache for previously computed subsumption results between pairs of macro states.
 *
 * Uses a flat open-addressing hash table with linear probing for cache locality.
 * The sentinel value ~uint64_t{0} is assumed never to be a valid packed key; in practice
 * macro-state IDs never reach 2^32-1 during any reachable emptiness search.
 *
 * When update the MacroStateId values to uint64_t, we can remove the packing and directly use the pair of IDs as the
 * key.
 */
struct SubsumptionCache {
    SubsumptionCache() : table(kInitialCapacity), count(0), true_count(0) {}

    std::optional<bool> get(const MacroStateId s1, const MacroStateId s2) const {
        const uint64_t k = pack(s1, s2);
        size_t idx = slot(k);

        while (true) {
            const Entry& e = table[idx];

            if (e.key == kEmpty) {
                return std::nullopt;
            }
            if (e.key == k) {
                return e.value;
            }

            idx = (idx + 1) & mask();
        }
    }

    void set(const MacroStateId s1, const MacroStateId s2, const bool result) {
        if ((count + 1) * 2 >= table.size()) {
            rehash();
        }

        insert_into(table, pack(s1, s2), result);
        ++count;

        if (result) {
            ++true_count;
        }
    }

    size_t size() const { return count; }
    size_t count_true() const { return true_count; }

private:
    struct Entry {
        uint64_t key{kEmpty};
        bool value{false};
    };

    static constexpr uint64_t kEmpty = ~uint64_t{0};
    static constexpr size_t kInitialCapacity = 16;

    std::vector<Entry> table;
    size_t count;
    size_t true_count;

    // Capacity is always a power of two (kInitialCapacity, doubled on rehash), so mask = size - 1.
    size_t mask() const { return table.size() - 1; }

    static uint64_t pack(const MacroStateId s1, const MacroStateId s2) {
        return (static_cast<uint64_t>(s1) << 32) | static_cast<uint64_t>(s2);
    }

    size_t slot(const uint64_t k) const { return static_cast<size_t>(fold_hash64(mix_hash64(k))) & mask(); }

    static void insert_into(std::vector<Entry>& tbl, const uint64_t k, const bool val) {
        const size_t msk = tbl.size() - 1;
        size_t idx = static_cast<size_t>(fold_hash64(mix_hash64(k))) & msk;
        while (tbl[idx].key != kEmpty) {
            idx = (idx + 1) & msk;
        }
        tbl[idx] = {k, val};
    }

    void rehash() {
        std::vector<Entry> new_table(table.size() * 2);
        for (const Entry& e : table) {
            if (e.key != kEmpty) {
                insert_into(new_table, e.key, e.value);
            }
        }
        table = std::move(new_table);
    }
};

/**
 * @brief Subsumption and antichain-pruning engine for lazy emptiness.
 */
class SubsumptionEngine {
public:
    /**
     * @brief Construct the engine over one reconstructed execution DAG.
     *
     * @param context External references used by the engine.
     */
    explicit SubsumptionEngine(const SubsumptionContext& context);

    /**
     * @brief Initialize simulation relations for leaves reachable from @p root_id.
     *
     * @param root_id Root node of the execution DAG.
     */
    void initialize_leaf_simulations(NodeId root_id);

    /**
     * @brief Check whether @p state is subsumed.
     *
     * Updates antichains by removing weaker states.
     *
     * @param root_id Root node of the macro state.
     * @param state Macro state to test.
     * @return True if the state is subsumed, false otherwise.
     */
    bool is_subsumed(NodeId root_id, MacroStateId state);

    /**
     * @brief Minimize @p state by removing all internally subsumed elements.
     *
     * Applies a fixed-point iteration to remove states dominated by other states
     * within the same set (Optimization 2 from "Simulation meets antichains").
     *
     * @param root_id Root node of the macro state.
     * @param state Set state to minimize in-place.
     */
    void minimize(NodeId root_id, SetState& state);

    /**
     * @brief Check whether @p state is pruned by the current antichain.
     *
     * The state must be inserted by a previous call to @c is_subsumed before this check is made,
     * otherwise it will be considered pruned by default.
     *
     * @param state Macro state to check.
     * @return True if the state is pruned, false otherwise.
     */
    bool is_pruned(MacroStateId state) const;

    /**
     * @brief Print subsumption statistics to stdout: antichain size and per-node cache hit ratios.
     */
    void print_statistics() const;

private:
    using State = mata::nfa::State;

    /// External references.
    const SubsumptionContext context;

    /// Precomputed simulation relations for NFA leaves.
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulation_nfas;
    /// Precomputed simulation relations for NFT leaves.
    std::vector<Simlib::Util::BinaryRelation> precomputed_simulation_nfts;
    /// One bucket: (fingerprint value, list of antichain entries with that fingerprint).
    using InnerBucket = std::pair<uint32_t, std::vector<MacroStateId>>;
    /// Sorted-by-fingerprint vector of buckets, enabling range queries by fingerprint value.
    using InnerBuckets = std::vector<InnerBucket>;
    using BucketsRange = std::pair<InnerBuckets::iterator, InnerBuckets::iterator>;

    /// Current antichain of non-subsumed states (flat view used by is_pruned).
    std::unordered_set<MacroStateId> antichain;
    /// Antichain: outer key = result of @c outer_key_program (often pair.lhs id, else 0);
    /// inner key = result of @c fingerprint_program. Both produced from the root macrostate.
    std::unordered_map<MacroStateId, InnerBuckets> buckets;
    /// Per-node subsumption caches.
    std::vector<SubsumptionCache> caches;

    /**
     * @brief One step in a fingerprint or outer-key program.
     *
     * Programs are short scripts (≤ 4 steps in practice) compiled once at root
     * setup time from the exec DAG's structural shape, then interpreted per
     * @c is_subsumed call. Each step transforms the (cursor) macrostate or emits
     * a final fingerprint value.
     */
    enum class FpOp : uint8_t {
        /// Replace cursor with @c get_pair(node, cursor).lhs.
        UnpackPairLhs,
        /// Replace cursor with @c get_pair(node, cursor).rhs.
        UnpackPairRhs,
        /// Emit @c |get_set(node, cursor)| (range-based fingerprint).
        EmitSetSize,
        /// Emit @c get_tagged(node, cursor).tag (equality fingerprint).
        EmitTag,
        /// Emit @c cursor as the result (raw id; outer key or equality leaf id).
        EmitId,
    };

    struct FpInstr {
        /// Operation to perform.
        FpOp op;
        /// Exec node whose store the operation reads (ignored by EmitId).
        NodeId node;
    };

    /**
     * @brief Subsumption-direction semantics of the inner fingerprint.
     *
     * Determines how @c check_range and @c remove_range scan the sorted inner buckets.
     */
    enum class FpRangeKind : uint8_t {
        /// No useful inner discrimination — single bucket, scan all entries.
        All = 0,
        /// Exact-match inner buckets (Tag, leaf id).
        Equality,
        /// Set-size monotonicity: smaller fp can subsume larger; larger fp may now be redundant.
        LessOrEqual,
    };

    /// Compiled walk script that transforms a root macrostate into a uint32_t fingerprint.
    struct FingerprintProgram {
        SmallVector<FpInstr, 4> steps{};
        FpRangeKind range_kind = FpRangeKind::All;
    };

    /// Compiled walk script that transforms a root macrostate into the outer bucket key.
    struct OuterKeyProgram {
        SmallVector<FpInstr, 4> steps{};
    };

    /// Compiled inner-fingerprint program for the current root.
    FingerprintProgram fingerprint_program{};
    /// Compiled outer-key program for the current root.
    OuterKeyProgram outer_key_program{};
    /// Root exec node used when computing fingerprints (also drives the lhs-sim widening).
    NodeId filter_root_node = 0;

    /// NFA leaf index for the LHS child when root is Intersect(LeafNfa, Complement(…)).
    std::optional<size_t> filter_lhs_nfa_index{};
    /// Per-NFA-A-state forward simulation neighbors: lhs_fwd_sim_neighbors[q] = {q' : q ≤_sim q'}.
    std::vector<std::vector<MacroStateId>> lhs_fwd_sim_neighbors{};
    /// Per-NFA-A-state backward simulation neighbors: lhs_bwd_sim_neighbors[q] = {q' : q' ≤_sim q}.
    std::vector<std::vector<MacroStateId>> lhs_bwd_sim_neighbors{};

    /**
     * @brief Recursive worker for reachable-leaf simulation initialization.
     *
     * @param node_id Current node.
     * @param visited Marks already processed nodes.
     */
    void initialize_leaf_simulations_impl(NodeId node_id, std::vector<bool>& visited);

    /**
     * @brief Semantic subsumption test for two states at one execution node.
     *
     * @param node_id Execution node where subsumption is checked.
     * @param state1 First macro state.
     * @param state2 Second macro state.
     * @return True if @p state1 is subsumed by @p state2, false otherwise.
     */
    bool subsumed_state(NodeId node_id, MacroStateId state1, MacroStateId state2);

    /// Choose an antichain filter strategy for the current root.
    void configure_antichain_filter(NodeId root_id);

    /// Compute the inner-bucket fingerprint of @p state by interpreting @c fingerprint_program.
    uint32_t compute_fingerprint(MacroStateId state) const;

    /// Compute the outer-bucket key of @p state by interpreting @c outer_key_program.
    MacroStateId compute_outer_key(MacroStateId state) const;

    /// Select or create the inner bucket map for @p state given the current outer-key program.
    InnerBuckets& inner_bucket(MacroStateId state);

    /// Returns the range of inner bucket entries that could subsume @p state.
    BucketsRange check_range(InnerBuckets& inner, uint32_t fp) const;

    /// Returns the range of inner bucket entries that @p state could make redundant.
    BucketsRange remove_range(InnerBuckets& inner, uint32_t fp) const;

    /// Find or insert the entry for @p key in a sorted InnerBuckets vector, returning its state list.
    static std::vector<MacroStateId>& get_or_insert_bucket(InnerBuckets& inner, uint32_t key);

    /**
     * @brief Precompute per-NFA-state simulation neighbor lists for the LHS leaf.
     *
     * Called only when the filter program identifies an LHS leaf NFA (with optional
     * Identity / Project wrappers) at the root pair. After this call:
     *
     * - @c lhs_fwd_sim_neighbors[q] = { q' : q ≤_sim q' }
     *   Forward neighbors used in the *check* phase: antichain entries whose LHS
     *   simulates @c q may subsume the query state.
     *
     * - @c lhs_bwd_sim_neighbors[q] = { q' : q' ≤_sim q }
     *   Backward neighbors used in the *remove* phase: antichain entries whose LHS
     *   is simulated by @c q can be subsumed by the query state.
     */
    void build_lhs_sim_neighbors();

    /**
     * @brief Simulation-aware fast path of @c is_subsumed when the LHS sim widening fires.
     *
     * Called when @c lhs_fwd_sim_neighbors is populated.  Scans all outer buckets
     * reachable via the precomputed simulation neighbors instead of only the bucket
     * matching the exact @c pair.lhs.
     */
    bool is_subsumed_with_lhs_sim(NodeId root_id, MacroStateId state, uint32_t fp);
};

} // namespace mata_lazy::detail
