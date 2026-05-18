/**
 * @file alphabet_store.cc
 * @brief Private alphabet storage and resolution helpers for mata::nft::lazy::detail.
 */

#include "alphabet_store.hh"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {

    // Merge every symbol from `alphabet` into `canonical_alphabet` by name, assigning a fresh
    // canonical ID when the name is new.  This normalises symbols from different local alphabets
    // (which can assign the same name to different integer values) into a single shared namespace.
    void add_symbols_to_canonical(const mata::OnTheFlyAlphabet& alphabet, mata::OnTheFlyAlphabet& canonical_alphabet) {
        for (const mata::Symbol symbol : alphabet.get_alphabet_symbols()) {
            canonical_alphabet.translate_symb(alphabet.reverse_translate_symbol(symbol));
        }
    }

    void collect_local_level_alphabets(
            const std::vector<ExecNode>& nodes, const NodeId node_id, const std::vector<mata::nfa::Nfa>& nfas,
            const std::vector<mata::nft::Nft>& nfts, std::vector<SmallVec2<mata::OnTheFlyAlphabet>>& level_alphabets,
            std::vector<bool>& visited) {
        if (visited[node_id]) {
            return;
        }

        const ExecNode& node = nodes[node_id];
        level_alphabets[node_id].resize(node.result_arity);

        switch (node.kind) {
            case ExecKind::LeafNfa: {
                const mata::nfa::Nfa& nfa = nfas[node.lhs];
                mata::OnTheFlyAlphabet& alphabet = level_alphabets[node_id][0];
                for (const auto& state_post : nfa.delta) {
                    for (const auto& symbol_post : state_post) {
                        alphabet.translate_symb(AlphabetStore::symbol_name_for(nfa.alphabet, symbol_post.symbol));
                    }
                }
                break;
            }

            case ExecKind::LeafNft: {
                const mata::nft::Nft& nft = nfts[node.lhs];
                SmallVec2<mata::OnTheFlyAlphabet>& node_level_alphabets = level_alphabets[node_id];
                for (mata::nfa::State state = 0; state < nft.delta.num_of_states(); ++state) {
                    const uint8_t level = static_cast<uint8_t>(nft.levels[state]);
                    assert(level < node_level_alphabets.size());

                    for (const auto& symbol_post : nft.delta.state_post(state)) {
                        node_level_alphabets[level].translate_symb(
                                AlphabetStore::symbol_name_for(&nft.alphabets->for_level(level), symbol_post.symbol));
                    }
                }
                break;
            }

            case ExecKind::Union:
            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
            case ExecKind::DiagonalSlice:
                collect_local_level_alphabets(nodes, node.lhs, nfas, nfts, level_alphabets, visited);
                collect_local_level_alphabets(nodes, node.rhs, nfas, nfts, level_alphabets, visited);
                break;

            case ExecKind::Complement:
            case ExecKind::Identity:
            case ExecKind::Project:
                collect_local_level_alphabets(nodes, node.lhs, nfas, nfts, level_alphabets, visited);
                break;
        }

        visited[node_id] = true;
    }

    // Merge related (node, level) pairs into equivalence classes using union-find, then assign each
    // class a single canonical OnTheFlyAlphabet containing all symbols from every member.
    //
    // Two levels are "related" when they represent the same tape position: e.g. both children of an
    // Intersect node share the same level with their parent, so their alphabets are merged.  After
    // this function every node's level_alphabets[node][level] points to the canonical alphabet of
    // its equivalence class.
    void canonicalize_level_alphabets(
            const std::vector<ExecNode>& nodes, const NodeId root_id, const std::vector<const void*>& plan_at_node,
            std::vector<SmallVec2<mata::OnTheFlyAlphabet>>& level_alphabets,
            const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets) {
        // Assign a flat integer index to each (node_id, level) pair so union-find can use plain arrays.
        std::vector<size_t> level_offsets(nodes.size() + 1, 0);
        for (size_t node_id = 0; node_id < nodes.size(); ++node_id) {
            level_offsets[node_id + 1] = level_offsets[node_id] + nodes[node_id].result_arity;
        }

        const size_t total_levels = level_offsets.back();
        std::vector<size_t> parent(total_levels, 0);
        std::vector<uint8_t> rank(total_levels, 0);
        for (size_t i = 0; i < total_levels; ++i) {
            parent[i] = i;
        }

        const auto level_index = [&](const NodeId node_id, const uint8_t level) {
            return level_offsets[node_id] + level;
        };

        // Path-compressing find: locate the representative, then flatten the path to it.
        auto find_root = [&](size_t idx) {
            size_t root = idx;
            while (parent[root] != root) {
                root = parent[root];
            }
            while (parent[idx] != idx) {
                const size_t next = parent[idx];
                parent[idx] = root;
                idx = next;
            }
            return root;
        };

        auto unite = [&](const size_t lhs, const size_t rhs) {
            size_t lhs_root = find_root(lhs);
            size_t rhs_root = find_root(rhs);
            if (lhs_root == rhs_root) {
                return;
            }

            if (rank[lhs_root] < rank[rhs_root]) {
                std::swap(lhs_root, rhs_root);
            }

            parent[rhs_root] = lhs_root;
            if (rank[lhs_root] == rank[rhs_root]) {
                ++rank[lhs_root];
            }
        };

        for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            const NodeId node_id = static_cast<NodeId>(node_index);
            const ExecNode& node = nodes[node_id];

            switch (node.kind) {
                case ExecKind::LeafNfa:
                case ExecKind::LeafNft:
                    break;

                case ExecKind::Union:
                case ExecKind::Intersect:
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        unite(level_index(node_id, level), level_index(node.lhs, level));
                        unite(level_index(node_id, level), level_index(node.rhs, level));
                    }
                    break;

                case ExecKind::Complement:
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        unite(level_index(node_id, level), level_index(node.lhs, level));
                    }
                    break;

                case ExecKind::Identity:
                    unite(level_index(node_id, 0), level_index(node.lhs, 0));
                    unite(level_index(node_id, 1), level_index(node.lhs, 0));
                    break;

                case ExecKind::Project: {
                    const auto& plan = *static_cast<const ProjectPlan*>(plan_at_node[node_id]);
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        unite(level_index(node_id, level), level_index(node.lhs, plan.kept_levels[level]));
                    }
                    break;
                }

                case ExecKind::SyncProduct: {
                    const auto& plan = *static_cast<const CompiledSyncPlan*>(plan_at_node[node_id]);
                    for (size_t i = 0; i < plan.lhs_sync_levels.size(); ++i) {
                        unite(level_index(node.lhs, plan.lhs_sync_levels[i]),
                              level_index(node.rhs, plan.rhs_sync_levels[i]));
                    }

                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        const LevelRef ref = plan.result_layout[level];
                        unite(level_index(node_id, level), ref.side == LevelRef::Side::Lhs
                                                                   ? level_index(node.lhs, ref.level)
                                                                   : level_index(node.rhs, ref.level));
                    }
                    break;
                }

                case ExecKind::DiagonalSlice:
                    // Result is arity 1, both rhs tapes are forced equal (diagonal), and the lhs
                    // language must agree with that shared symbol. So all four positions live in
                    // one equivalence class, the result level, lhs[0], rhs[0], and rhs[1].
                    unite(level_index(node_id, 0), level_index(node.lhs, 0));
                    unite(level_index(node_id, 0), level_index(node.rhs, 0));
                    unite(level_index(node_id, 0), level_index(node.rhs, 1));
                    break;
            }
        }

        // One canonical alphabet per equivalence class root; accumulate all local symbols into it.
        std::vector<mata::OnTheFlyAlphabet> canonical_alphabets(total_levels);
        for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            const NodeId node_id = static_cast<NodeId>(node_index);
            for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
                add_symbols_to_canonical(
                        level_alphabets[node_id][level], canonical_alphabets[find_root(level_index(node_id, level))]);
            }
        }

        if (root_level_alphabets != nullptr) {
            if (root_level_alphabets->size() != nodes[root_id].result_arity) {
                throw std::invalid_argument("The number of root level alphabets must match the root arity");
            }

            for (uint8_t level = 0; level < nodes[root_id].result_arity; ++level) {
                add_symbols_to_canonical(
                        (*root_level_alphabets)[level], canonical_alphabets[find_root(level_index(root_id, level))]);
            }
        }

        for (size_t node_index = 0; node_index < nodes.size(); ++node_index) {
            const NodeId node_id = static_cast<NodeId>(node_index);
            for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
                level_alphabets[node_id][level] = canonical_alphabets[find_root(level_index(node_id, level))];
            }
        }
    }

    void compute_effective_symbol_lists(
            const std::vector<ExecNode>& nodes, const std::vector<const void*>& plan_at_node,
            const std::vector<SmallVec2<std::vector<mata::Symbol>>>& symbol_lists,
            std::vector<SmallVec2<std::vector<mata::Symbol>>>& effective) {
        effective.resize(nodes.size());

        // nodes are in topological order (children before parents), so a single forward pass suffices.
        for (NodeId node_id = 0; node_id < static_cast<NodeId>(nodes.size()); ++node_id) {
            const ExecNode& node = nodes[node_id];
            effective[node_id].resize(node.result_arity);

            switch (node.kind) {
                case ExecKind::LeafNfa:
                case ExecKind::LeafNft:
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        effective[node_id][level] = symbol_lists[node_id][level];
                    }
                    break;

                case ExecKind::Complement:
                    // Complement explicitly sweeps all canonical symbols → full alphabet.
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        effective[node_id][level] = symbol_lists[node_id][level];
                    }
                    break;

                case ExecKind::Union:
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        const auto& lhs = effective[node.lhs][level];
                        const auto& rhs = effective[node.rhs][level];
                        auto& result = effective[node_id][level];
                        result.reserve(lhs.size() + rhs.size());
                        std::set_union(lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(result));
                    }
                    break;

                case ExecKind::Intersect:
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        const auto& lhs = effective[node.lhs][level];
                        const auto& rhs = effective[node.rhs][level];
                        auto& result = effective[node_id][level];
                        result.reserve(std::min(lhs.size(), rhs.size()));
                        std::set_intersection(
                                lhs.begin(), lhs.end(), rhs.begin(), rhs.end(), std::back_inserter(result));
                    }
                    break;

                case ExecKind::SyncProduct:
                    // Conservative: use the full canonical alphabet.
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        effective[node_id][level] = symbol_lists[node_id][level];
                    }
                    break;

                case ExecKind::Identity:
                    effective[node_id][0] = effective[node.lhs][0];
                    effective[node_id][1] = effective[node.lhs][0];
                    break;

                case ExecKind::Project: {
                    const auto& plan = *static_cast<const ProjectPlan*>(plan_at_node[node_id]);
                    for (uint8_t level = 0; level < node.result_arity; ++level) {
                        effective[node_id][level] = effective[node.lhs][plan.kept_levels[level]];
                    }
                    break;
                }

                case ExecKind::DiagonalSlice: {
                    // Effective alphabet is the intersection of the language's level 0 and the
                    // diagonal symbols of the relation, which means symbols that appear on both
                    // tapes of the relation. Approximated as the intersection of all three.
                    const auto& u_syms = effective[node.lhs][0];
                    const auto& x_syms_0 = effective[node.rhs][0];
                    const auto& x_syms_1 = effective[node.rhs][1];

                    std::vector<mata::Symbol> x_diag;
                    x_diag.reserve(std::min(x_syms_0.size(), x_syms_1.size()));
                    std::set_intersection(
                            x_syms_0.begin(), x_syms_0.end(), x_syms_1.begin(), x_syms_1.end(),
                            std::back_inserter(x_diag));

                    std::vector<mata::Symbol>& result = effective[node_id][0];
                    result.reserve(std::min(u_syms.size(), x_diag.size()));
                    std::set_intersection(
                            u_syms.begin(), u_syms.end(), x_diag.begin(), x_diag.end(), std::back_inserter(result));
                    break;
                }
            }
        }
    }

} // namespace

std::string AlphabetStore::symbol_name_for(const mata::Alphabet* alphabet, const mata::Symbol symbol) {
    if (alphabet == nullptr) {
        return std::to_string(symbol);
    }

    return alphabet->reverse_translate_symbol(symbol);
}

AlphabetStore::AlphabetStore(
        const std::vector<ExecNode>& nodes, const NodeId root_id, const std::vector<mata::nfa::Nfa>& nfas,
        const std::vector<mata::nft::Nft>& nfts, const std::vector<const void*>& plan_at_node,
        const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets)
    : level_alphabets(nodes.size()) {
    std::vector<bool> visited(nodes.size(), false);
    collect_local_level_alphabets(nodes, root_id, nfas, nfts, level_alphabets, visited);
    canonicalize_level_alphabets(nodes, root_id, plan_at_node, level_alphabets, root_level_alphabets);

    symbol_lists.resize(nodes.size());
    for (NodeId node_id = 0; node_id < static_cast<NodeId>(nodes.size()); ++node_id) {
        const uint8_t arity = nodes[node_id].result_arity;
        symbol_lists[node_id].resize(arity);
        for (uint8_t level = 0; level < arity; ++level) {
            symbol_lists[node_id][level] = level_alphabets[node_id][level].get_alphabet_symbols().to_vector();
        }
    }

    compute_effective_symbol_lists(nodes, plan_at_node, symbol_lists, effective_symbol_lists);
}

bool AlphabetStore::try_resolve_symbol(
        const mata::nft::Nft& nft, const uint8_t source_level, const NodeId node_id, const uint8_t result_level,
        const mata::Symbol local_symbol, mata::Symbol& resolved_symbol) const {
    const std::string symbol_name = symbol_name_for(&nft.alphabets->for_level(source_level), local_symbol);
    return try_translate_symbol_name_to_resolved(node_id, result_level, symbol_name, resolved_symbol);
}


bool AlphabetStore::try_translate_symbol_name_to_resolved(
        const NodeId node_id, const uint8_t level, const std::string& symbol_name,
        mata::Symbol& resolved_symbol) const {
    const mata::OnTheFlyAlphabet& resolved_alphabet = level_alphabet(node_id, level);
    const auto it = resolved_alphabet.get_symbol_map().find(symbol_name);
    if (it == resolved_alphabet.get_symbol_map().end()) {
        return false;
    }

    resolved_symbol = it->second;
    return true;
}

} // namespace mata::nft::lazy::detail
