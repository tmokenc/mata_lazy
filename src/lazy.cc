/** @file lazy.cc
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of arbitrary-arity relations.
 */

#include "mata_lazy.hh"

#include <stdexcept>
#include <utility>
#include <vector>

#include "lazy/emptiness.hh"
#include "lazy/validation.hh"

namespace mata_lazy {

namespace {
    uint8_t require_same_arity(const SymbolicFormula& tree, const char* operation, const Term& lhs, const Term& rhs) {
        const uint8_t lhs_arity = tree.arity_of(lhs);
        const uint8_t rhs_arity = tree.arity_of(rhs);
        if (lhs_arity != rhs_arity) {
            throw std::invalid_argument(std::string(operation) + " requires operands with equal arity");
        }
        return lhs_arity;
    }

    void require_unique_levels_in_range(
            const char* operation, const char* level_kind, const std::vector<uint8_t>& levels, const uint8_t arity) {
        if (!detail::levels_unique(levels)) {
            throw std::invalid_argument(std::string(operation) + " expects unique " + level_kind);
        }

        for (const uint8_t level : levels) {
            if (level >= arity) {
                throw std::invalid_argument(std::string(operation) + " " + level_kind + " out of range");
            }
        }
    }

    std::vector<LevelRef> build_compose_result_layout(
            const SymbolicFormula& tree, const Term& lhs, const Term& rhs, const std::vector<uint8_t>& lhs_sync_levels,
            const std::vector<uint8_t>& rhs_sync_levels) {
        const uint8_t lhs_arity = tree.arity_of(lhs);
        const uint8_t rhs_arity = tree.arity_of(rhs);
        std::vector<bool> lhs_is_sync(lhs_arity, false);
        std::vector<bool> rhs_is_sync(rhs_arity, false);

        for (const uint8_t level : lhs_sync_levels) {
            lhs_is_sync[level] = true;
        }
        for (const uint8_t level : rhs_sync_levels) {
            rhs_is_sync[level] = true;
        }

        std::vector<LevelRef> result_layout{};
        result_layout.reserve(lhs_arity + rhs_arity - lhs_sync_levels.size());

        for (uint8_t level = 0; level < lhs_arity; ++level) {
            if (!lhs_is_sync[level]) {
                result_layout.push_back(LevelRef{LevelRef::Side::Lhs, level});
            }
        }

        for (uint8_t level = 0; level < rhs_arity; ++level) {
            if (!rhs_is_sync[level]) {
                result_layout.push_back(LevelRef{LevelRef::Side::Rhs, level});
            }
        }

        return result_layout;
    }

    void require_language_and_two_tape_transducer(
            const SymbolicFormula& tree, const char* operation, const Term& language, const Term& transducer) {
        if (tree.arity_of(language) != 1) {
            throw std::invalid_argument(std::string(operation) + " expects an arity-1 language term");
        }
        if (tree.arity_of(transducer) != 2) {
            throw std::invalid_argument(std::string(operation) + " expects a 2-tape transducer term");
        }
    }

} // namespace

// -----------------------------------------------------------------------------
// Internal node insertion
//
// Plans are associated with their owning node positionally: the k-th `Project`
// node corresponds to `project_plans[k]`, and the k-th `SyncProduct` node to
// `sync_plans[k]`. To preserve this invariant, callers MUST append a plan
// immediately before inserting its node; never reorder or interleave.
// -----------------------------------------------------------------------------

NodeId SymbolicFormula::insert_leaf(NodeKind kind, NodeId leaf_id, uint8_t result_arity) {
    const NodeId id = static_cast<NodeId>(nodes.size());
    nodes.push_back(Node{kind, result_arity, leaf_id, 0});
    return id;
}

NodeId SymbolicFormula::insert_unary(NodeKind kind, NodeId child, uint8_t result_arity) {
    const NodeId id = static_cast<NodeId>(nodes.size());
    nodes.push_back(Node{kind, result_arity, child, 0});
    return id;
}

NodeId SymbolicFormula::insert_binary(NodeKind kind, NodeId lhs, NodeId rhs, uint8_t result_arity) {
    const NodeId id = static_cast<NodeId>(nodes.size());
    nodes.push_back(Node{kind, result_arity, lhs, rhs});
    return id;
}

// -----------------------------------------------------------------------------
// Generic relation builders
// -----------------------------------------------------------------------------

Term SymbolicFormula::make_term(const nfa::Nfa& nfa) {
    const NodeId nfa_id = static_cast<NodeId>(nfas.size());
    nfas.push_back(nfa);
    return Term{insert_leaf(NodeKind::LeafNfa, nfa_id, 1)};
}

Term SymbolicFormula::make_term(const nft::Nft& nft) {
    const NodeId nft_id = static_cast<NodeId>(nfts.size());
    const uint8_t arity = static_cast<uint8_t>(nft.levels.num_of_levels);
    nfts.push_back(nft);
    return Term{insert_leaf(NodeKind::LeafNft, nft_id, arity)};
}

Term SymbolicFormula::unite(const Term& lhs, const Term& rhs) {
    return insert_binary(NodeKind::Union, lhs.get_id(), rhs.get_id(), require_same_arity(*this, "union", lhs, rhs));
}

Term SymbolicFormula::intersect(const Term& lhs, const Term& rhs) {
    return insert_binary(
            NodeKind::Intersect, lhs.get_id(), rhs.get_id(), require_same_arity(*this, "intersect", lhs, rhs));
}

Term SymbolicFormula::complement(const Term& sub) {
    return insert_unary(NodeKind::Complement, sub.get_id(), arity_of(sub));
}

Term SymbolicFormula::identity(const Term& sub) {
    if (arity_of(sub) != 1) {
        throw std::invalid_argument("identity expects an arity-1 term");
    }
    return insert_unary(NodeKind::Identity, sub.get_id(), 2);
}

Term SymbolicFormula::project(const Term& sub, const std::vector<uint8_t>& kept_levels) {
    require_unique_levels_in_range("project", "kept levels", kept_levels, arity_of(sub));
    project_plans.push_back(ProjectPlan{kept_levels});
    return insert_unary(NodeKind::Project, sub.get_id(), static_cast<uint8_t>(kept_levels.size()));
}

Term SymbolicFormula::sync_product(
        const Term& lhs, const Term& rhs, const std::vector<uint8_t>& lhs_sync_levels,
        const std::vector<uint8_t>& rhs_sync_levels, const std::vector<LevelRef>& result_layout) {
    if (lhs_sync_levels.size() != rhs_sync_levels.size()) {
        throw std::invalid_argument("sync_product expects equally many synchronization levels on both sides");
    }
    if (!detail::level_refs_unique(result_layout)) {
        throw std::invalid_argument("sync_product expects unique synchronization and result levels");
    }

    const uint8_t lhs_arity = arity_of(lhs);
    const uint8_t rhs_arity = arity_of(rhs);
    require_unique_levels_in_range("sync_product", "lhs synchronization levels", lhs_sync_levels, lhs_arity);
    require_unique_levels_in_range("sync_product", "rhs synchronization levels", rhs_sync_levels, rhs_arity);

    for (const LevelRef ref : result_layout) {
        if (ref.side == LevelRef::Side::Lhs) {
            if (ref.level >= lhs_arity) {
                throw std::invalid_argument("sync_product result layout references invalid lhs level");
            }
        } else if (ref.level >= rhs_arity) {
            throw std::invalid_argument("sync_product result layout references invalid rhs level");
        }
    }

    sync_plans.push_back(SyncPlan{lhs_sync_levels, rhs_sync_levels, result_layout});
    return insert_binary(
            NodeKind::SyncProduct, lhs.get_id(), rhs.get_id(), static_cast<uint8_t>(result_layout.size()));
}

Term SymbolicFormula::compose(
        const Term& lhs, const Term& rhs, const std::vector<uint8_t>& lhs_sync_levels,
        const std::vector<uint8_t>& rhs_sync_levels) {
    const std::vector<LevelRef> level_layout =
            build_compose_result_layout(*this, lhs, rhs, lhs_sync_levels, rhs_sync_levels);

    return sync_product(lhs, rhs, lhs_sync_levels, rhs_sync_levels, level_layout);
}

Term SymbolicFormula::post_image(const Term& lang_over_input, const Term& transducer) {
    require_language_and_two_tape_transducer(*this, "post_image", lang_over_input, transducer);
    return sync_product(lang_over_input, transducer, {0}, {0}, {LevelRef{LevelRef::Side::Rhs, 1}});
}

Term SymbolicFormula::pre_image(const Term& lang_over_output, const Term& transducer) {
    require_language_and_two_tape_transducer(*this, "pre_image", lang_over_output, transducer);
    return sync_product(lang_over_output, transducer, {0}, {1}, {LevelRef{LevelRef::Side::Rhs, 0}});
}

Term SymbolicFormula::compose(const Term& lhs, const Term& rhs) { return compose(lhs, rhs, {1}, {0}); }

// -----------------------------------------------------------------------------
// Validation and emptiness front-ends
// -----------------------------------------------------------------------------

uint8_t SymbolicFormula::arity_of(const Term& term) const {
    if (term.get_id() >= nodes.size()) {
        throw std::out_of_range("Term node id is out of range");
    }

    return nodes[term.get_id()].result_arity;
}

bool SymbolicFormula::is_valid(const Term& root_node) const { return detail::is_valid(*this, root_node); }

bool SymbolicFormula::is_empty(const Term& root_node) { return detail::is_empty(*this, root_node, nullptr); }

bool SymbolicFormula::is_empty(const Term& root_node, const mata::OnTheFlyAlphabet& alphabet) {
    std::vector<mata::OnTheFlyAlphabet> level_alphabets(arity_of(root_node), alphabet);
    return detail::is_empty(*this, root_node, &level_alphabets);
}

bool SymbolicFormula::is_empty(const Term& root_node, const std::vector<mata::OnTheFlyAlphabet>& level_alphabets) {
    return detail::is_empty(*this, root_node, &level_alphabets);
}

} // namespace mata_lazy
