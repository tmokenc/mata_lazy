/**
 * @file subsumption.cc
 * @brief Private subsumption and antichain-pruning implementation for mata::nft::lazy::detail.
 */

#include "subsumption.hh"

#include "mata/nfa/algorithms.hh"
#include "mata/nft/algorithms.hh"

#include <algorithm>
#include <iostream>

namespace mata::nft::lazy::detail {

SubsumptionEngine::SubsumptionEngine(const SubsumptionContext& context)
    : context(context), precomputed_simulation_nfas(context.nfas.size()),
      precomputed_simulation_nfts(context.nfts.size()), antichain{}, buckets{}, caches{}, filter_lhs_nfa_index{},
      lhs_fwd_sim_neighbors{}, lhs_bwd_sim_neighbors{} {}

namespace {

    /// Walk through macrostate-transparent wrappers (Identity, Project) until a node that
    /// owns its own macrostate store is reached. The macrostate id is invariant across
    /// the walk (transparent kinds share their child's store).
    NodeId resolve_macrostate_owner(const std::vector<ExecNode>& nodes, NodeId nid) {
        while (nid < nodes.size()) {
            const ExecKind k = nodes[nid].kind;
            if (k == ExecKind::Identity || k == ExecKind::Project) {
                nid = nodes[nid].lhs;
            } else {
                break;
            }
        }
        return nid;
    }

} // namespace

void SubsumptionEngine::initialize_leaf_simulations(const NodeId root_id) {
    antichain.clear();
    buckets.clear();
    caches.clear();
    caches.resize(context.nodes.size());
    lhs_fwd_sim_neighbors.clear();
    lhs_bwd_sim_neighbors.clear();
    filter_lhs_nfa_index = std::nullopt;

    configure_antichain_filter(root_id);

    std::vector<bool> visited(context.nodes.size(), false);
    initialize_leaf_simulations_impl(root_id, visited);

    build_lhs_sim_neighbors();
}

void SubsumptionEngine::configure_antichain_filter(const NodeId root_id) {
    filter_root_node = root_id;
    fingerprint_program = FingerprintProgram{};
    outer_key_program = OuterKeyProgram{};

    if (root_id >= context.nodes.size()) {
        return;
    }

    // Resolve the root through any leading transparent wrappers; macrostate ids are
    // shared across the chain so the resolved node owns the relevant store.
    const NodeId root_owner = resolve_macrostate_owner(context.nodes, root_id);
    const ExecKind root_kind = context.nodes[root_owner].kind;

    switch (root_kind) {
        case ExecKind::Union:
            // Subsumption requires matching tags — use the tag as an equality discriminator
            // so only same-side entries are compared.
            fingerprint_program.steps.push_back({FpOp::EmitTag, root_owner});
            fingerprint_program.range_kind = FpRangeKind::Equality;
            break;

        case ExecKind::Complement:
            // Complement subsumption reverses the subset order: s1 ⊑ s2 iff sub(s2) ⊆ sub(s1).
            // Bucket by set size and check only entries with size ≤ the query.
            fingerprint_program.steps.push_back({FpOp::EmitSetSize, root_owner});
            fingerprint_program.range_kind = FpRangeKind::LessOrEqual;
            break;

        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::DiagonalSlice: {
            // Look for a Complement on the rhs (possibly through transparent wrappers).
            // Walking through Identity / Project is sound because they share their child's
            // macrostate store, so the unpacked pair.rhs id is valid in the resolved
            // Complement's set store.
            const NodeId rhs_raw = context.nodes[root_owner].rhs;
            const NodeId rhs_owner = resolve_macrostate_owner(context.nodes, rhs_raw);

            if (rhs_owner < context.nodes.size() &&
                context.nodes[rhs_owner].kind == ExecKind::Complement) {
                // Outer key: pair.lhs id (equality grouping — pair subsumption requires
                // lhs1 ⊑ lhs2, which without sim means equality).
                outer_key_program.steps.push_back({FpOp::UnpackPairLhs, root_owner});
                outer_key_program.steps.push_back({FpOp::EmitId, 0});

                // Inner fingerprint: pair.rhs → resolved Complement → set size.
                fingerprint_program.steps.push_back({FpOp::UnpackPairRhs, root_owner});
                fingerprint_program.steps.push_back({FpOp::EmitSetSize, rhs_owner});
                fingerprint_program.range_kind = FpRangeKind::LessOrEqual;

                // Simulation widening when the lhs (after wrapper walk) is a plain NFA leaf.
                const NodeId lhs_raw = context.nodes[root_owner].lhs;
                const NodeId lhs_owner = resolve_macrostate_owner(context.nodes, lhs_raw);
                if (lhs_owner < context.nodes.size() &&
                    context.nodes[lhs_owner].kind == ExecKind::LeafNfa) {
                    filter_lhs_nfa_index = context.nodes[lhs_owner].lhs;
                }
            }
            break;
        }

        default:
            break;
    }
}

uint32_t SubsumptionEngine::compute_fingerprint(const MacroStateId state) const {
    MacroStateId cursor = state;
    for (const FpInstr& instr : fingerprint_program.steps) {
        switch (instr.op) {
            case FpOp::UnpackPairLhs:
                cursor = context.macro_store.get_pair(instr.node, cursor).lhs;
                break;
            case FpOp::UnpackPairRhs:
                cursor = context.macro_store.get_pair(instr.node, cursor).rhs;
                break;
            case FpOp::EmitSetSize:
                return static_cast<uint32_t>(context.macro_store.get_set(instr.node, cursor).size());
            case FpOp::EmitTag:
                return static_cast<uint32_t>(context.macro_store.get_tagged(instr.node, cursor).tag);
            case FpOp::EmitId:
                return static_cast<uint32_t>(cursor);
        }
    }
    return 0;
}

MacroStateId SubsumptionEngine::compute_outer_key(const MacroStateId state) const {
    if (outer_key_program.steps.empty()) {
        return MacroStateId{0};
    }
    MacroStateId cursor = state;
    for (const FpInstr& instr : outer_key_program.steps) {
        switch (instr.op) {
            case FpOp::UnpackPairLhs:
                cursor = context.macro_store.get_pair(instr.node, cursor).lhs;
                break;
            case FpOp::UnpackPairRhs:
                cursor = context.macro_store.get_pair(instr.node, cursor).rhs;
                break;
            case FpOp::EmitId:
                return cursor;
            case FpOp::EmitSetSize:
            case FpOp::EmitTag:
                // Not meaningful as outer keys; defensive fallthrough returns cursor.
                return cursor;
        }
    }
    return cursor;
}

SubsumptionEngine::InnerBuckets& SubsumptionEngine::inner_bucket(const MacroStateId state) {
    return buckets[compute_outer_key(state)];
}

std::vector<MacroStateId>& SubsumptionEngine::get_or_insert_bucket(InnerBuckets& inner, const uint32_t key) {
    const auto cmp = [](const InnerBucket& b, uint32_t k) { return b.first < k; };
    auto it = std::lower_bound(inner.begin(), inner.end(), key, cmp);
    if (it == inner.end() || it->first != key) {
        it = inner.insert(it, {key, {}});
    }
    return it->second;
}

// check_range: buckets that might contain an antichain entry subsuming the query state.
// remove_range: buckets that might contain entries the query state now makes redundant.
//
// Driven by fingerprint_program.range_kind:
//   Equality:    only the exact-fp bucket matters for both check and remove.
//   LessOrEqual: complement-style monotonicity. check = sizes ≤ fp; remove = sizes ≥ fp.
//   All:         no useful inner discrimination — scan everything.

SubsumptionEngine::BucketsRange SubsumptionEngine::check_range(InnerBuckets& inner, const uint32_t fp) const {
    const auto lower_cmp = [](const InnerBucket& b, uint32_t k) { return b.first < k; };
    switch (fingerprint_program.range_kind) {
        case FpRangeKind::Equality: {
            const auto it = std::lower_bound(inner.begin(), inner.end(), fp, lower_cmp);
            return (it != inner.end() && it->first == fp) ? BucketsRange{it, std::next(it)}
                                                          : BucketsRange{inner.end(), inner.end()};
        }
        case FpRangeKind::LessOrEqual: {
            const auto upper_cmp = [](uint32_t k, const InnerBucket& b) { return k < b.first; };
            return {inner.begin(), std::upper_bound(inner.begin(), inner.end(), fp, upper_cmp)};
        }
        case FpRangeKind::All:
            return {inner.begin(), inner.end()};
    }
    return {inner.begin(), inner.end()};
}

SubsumptionEngine::BucketsRange SubsumptionEngine::remove_range(InnerBuckets& inner, const uint32_t fp) const {
    const auto lower_cmp = [](const InnerBucket& b, uint32_t k) { return b.first < k; };
    switch (fingerprint_program.range_kind) {
        case FpRangeKind::Equality: {
            const auto it = std::lower_bound(inner.begin(), inner.end(), fp, lower_cmp);
            return (it != inner.end() && it->first == fp) ? BucketsRange{it, std::next(it)}
                                                          : BucketsRange{inner.end(), inner.end()};
        }
        case FpRangeKind::LessOrEqual:
            return {std::lower_bound(inner.begin(), inner.end(), fp, lower_cmp), inner.end()};
        case FpRangeKind::All:
            return {inner.begin(), inner.end()};
    }
    return {inner.begin(), inner.end()};
}

void SubsumptionEngine::initialize_leaf_simulations_impl(const NodeId node_id, std::vector<bool>& visited) {
    if (visited[node_id]) {
        return;
    }

    const ExecNode& node = context.nodes[node_id];

    switch (node.kind) {
        case ExecKind::LeafNfa: {
            Simlib::Util::BinaryRelation relation = mata::nfa::algorithms::compute_relation(context.nfas[node.lhs]);
            precomputed_simulation_nfas[node.lhs] = std::move(relation);
            break;
        }

        case ExecKind::LeafNft: {
            Simlib::Util::BinaryRelation relation = mata::nft::algorithms::compute_relation(context.nfts[node.lhs]);
            precomputed_simulation_nfts[node.lhs] = std::move(relation);
            break;
        }

        case ExecKind::Union:
        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::DiagonalSlice:
            initialize_leaf_simulations_impl(node.lhs, visited);
            initialize_leaf_simulations_impl(node.rhs, visited);
            break;

        case ExecKind::Complement:
        case ExecKind::Identity:
        case ExecKind::Project:
            initialize_leaf_simulations_impl(node.lhs, visited);
            break;
    }

    visited[node_id] = true;
}

bool SubsumptionEngine::subsumed_state(const NodeId node_id, const MacroStateId state1, const MacroStateId state2) {
    if (state1 == state2) {
        return true;
    }

    const ExecNode& node = context.nodes[node_id];
    const State s1 = static_cast<State>(state1);
    const State s2 = static_cast<State>(state2);
    bool result = false;

    switch (node.kind) {
        case ExecKind::LeafNfa:
            result = precomputed_simulation_nfas[node.lhs].get(s1, s2);
            break;

        case ExecKind::LeafNft:
            result = precomputed_simulation_nfts[node.lhs].get(s1, s2);
            break;

        case ExecKind::Union: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const TaggedState tagged1 = context.macro_store.get_tagged(node_id, state1);
            const TaggedState tagged2 = context.macro_store.get_tagged(node_id, state2);
            if (tagged1.tag != tagged2.tag) {
                result = false;
                break;
            }

            result = tagged1.tag == TaggedState::Tag::Left ? subsumed_state(node.lhs, tagged1.state, tagged2.state)
                                                           : subsumed_state(node.rhs, tagged1.state, tagged2.state);
            caches[node_id].set(state1, state2, result);
            break;
        }

        case ExecKind::Intersect:
        case ExecKind::SyncProduct:
        case ExecKind::DiagonalSlice: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            const PairState pair1 = context.macro_store.get_pair(node_id, state1);
            const PairState pair2 = context.macro_store.get_pair(node_id, state2);

            result = subsumed_state(node.lhs, pair1.lhs, pair2.lhs) && subsumed_state(node.rhs, pair1.rhs, pair2.rhs);
            caches[node_id].set(state1, state2, result);
            break;
        }

        case ExecKind::Complement: {
            if (const std::optional<bool> cached_result = caches[node_id].get(state1, state2)) {
                return *cached_result;
            }

            // Complement reverses the subsumption order: complement(s1) ⊑ complement(s2) iff
            // sub(s2) ⊆ sub(s1).  The argument roles are therefore intentionally swapped here.
            const SetState& lhs_sub_states = context.macro_store.get_set(node_id, state2);
            const SetState& rhs_sub_states = context.macro_store.get_set(node_id, state1);

            if (lhs_sub_states.size() > rhs_sub_states.size()) {
                result = false;
                break;
            }

            result = true;
            for (const MacroStateId lhs_sub_state : lhs_sub_states) {
                bool subsumed = false;

                for (const MacroStateId rhs_sub_state : rhs_sub_states) {
                    if (subsumed_state(node.lhs, lhs_sub_state, rhs_sub_state)) {
                        subsumed = true;
                        break;
                    }
                }

                if (!subsumed) {
                    result = false;
                    break;
                }
            }

            caches[node_id].set(state1, state2, result);

            break;
        }

        case ExecKind::Identity:
        case ExecKind::Project:
            result = subsumed_state(node.lhs, state1, state2);
            break;
    }

    return result;
}

void SubsumptionEngine::build_lhs_sim_neighbors() {
    if (!filter_lhs_nfa_index.has_value())
        return;

    const size_t nfa_idx = *filter_lhs_nfa_index;
    const Simlib::Util::BinaryRelation& sim = precomputed_simulation_nfas[nfa_idx];
    const size_t n = context.nfas[nfa_idx].num_of_states();

    lhs_fwd_sim_neighbors.resize(n);
    lhs_bwd_sim_neighbors.resize(n);

    // TODO: Should unroll this loop?
    for (size_t q = 0; q < n; ++q) {
        for (size_t q2 = 0; q2 < n; ++q2) {
            if (sim.get(q, q2))
                lhs_fwd_sim_neighbors[q].push_back(static_cast<MacroStateId>(q2));
            if (sim.get(q2, q))
                lhs_bwd_sim_neighbors[q].push_back(static_cast<MacroStateId>(q2));
        }
    }
}

bool SubsumptionEngine::is_subsumed_with_lhs_sim(const NodeId root_id, const MacroStateId state, const uint32_t fp) {
    const MacroStateId lhs = context.macro_store.get_pair(filter_root_node, state).lhs;

    // Check: find any antichain entry (q', S') in outer bucket q' where lhs ≤_sim q'.
    if (lhs < static_cast<MacroStateId>(lhs_fwd_sim_neighbors.size())) {
        for (const MacroStateId sim_key : lhs_fwd_sim_neighbors[lhs]) {
            const auto outer_it = buckets.find(sim_key);
            if (outer_it == buckets.end())
                continue;
            auto [cb, ce] = check_range(outer_it->second, fp);
            for (auto it = cb; it != ce; ++it) {
                for (const MacroStateId entry : it->second) {
                    if (subsumed_state(root_id, state, entry))
                        return true;
                }
            }
        }
    }

    // Remove: find antichain entries (q', S') in outer bucket q' where q' ≤_sim lhs.
    if (lhs < static_cast<MacroStateId>(lhs_bwd_sim_neighbors.size())) {
        for (const MacroStateId sim_key : lhs_bwd_sim_neighbors[lhs]) {
            const auto outer_it = buckets.find(sim_key);
            if (outer_it == buckets.end())
                continue;
            auto [rb, re] = remove_range(outer_it->second, fp);
            for (auto it = rb; it != re; ++it) {
                std::erase_if(it->second, [&](const MacroStateId entry) {
                    if (subsumed_state(root_id, entry, state)) {
                        antichain.erase(entry);
                        return true;
                    }
                    return false;
                });
            }
        }
    }

    InnerBuckets& inner = inner_bucket(state);
    antichain.insert(state);
    get_or_insert_bucket(inner, fp).push_back(state);
    return false;
}

bool SubsumptionEngine::is_subsumed(const NodeId root_id, const MacroStateId state) {
    const uint32_t fp = compute_fingerprint(state);

    if (!lhs_fwd_sim_neighbors.empty()) {
        return is_subsumed_with_lhs_sim(root_id, state, fp);
    }

    InnerBuckets& inner = inner_bucket(state);

    auto [cb, ce] = check_range(inner, fp);
    for (auto it = cb; it != ce; ++it) {
        for (const MacroStateId entry : it->second) {
            if (subsumed_state(root_id, state, entry))
                return true;
        }
    }

    auto [rb, re] = remove_range(inner, fp);
    for (auto it = rb; it != re; ++it) {
        std::erase_if(it->second, [&](const MacroStateId entry) {
            if (subsumed_state(root_id, entry, state)) {
                antichain.erase(entry);
                return true;
            }
            return false;
        });
    }

    antichain.insert(state);
    get_or_insert_bucket(inner, fp).push_back(state);
    return false;
}

void SubsumptionEngine::minimize(const NodeId root_id, SetState& state) {
    canonicalize_set_state(state);

    bool changed = true;
    while (changed) {
        changed = false;

        for (auto candidate_it = state.begin(); candidate_it != state.end(); ++candidate_it) {
            const MacroStateId candidate = *candidate_it;
            bool remove_candidate = false;

            for (const MacroStateId other : state) {
                if (candidate != other && subsumed_state(root_id, candidate, other)) {
                    remove_candidate = true;
                    break;
                }
            }

            if (remove_candidate) {
                state.erase(candidate_it);
                changed = true;
                break;
            }
        }
    }
}

bool SubsumptionEngine::is_pruned(const MacroStateId state) const { return !antichain.contains(state); }

void SubsumptionEngine::print_statistics() const {
    size_t cache_stored = 0;
    size_t cached_true = 0;

    for (const SubsumptionCache& cache : caches) {
        cache_stored += cache.size();
        cached_true += cache.count_true();
    }

    std::cout << "Subsumption cache size: " << cache_stored << std::endl;
    std::cout << "Subsumption cache true ratio: "
              << (cache_stored == 0 ? 0 : static_cast<double>(cached_true) / static_cast<double>(cache_stored))
              << std::endl;
    std::cout << "Current antichain size: " << antichain.size() << std::endl;
}

} // namespace mata::nft::lazy::detail
