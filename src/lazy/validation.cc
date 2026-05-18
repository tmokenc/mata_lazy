/**
 * @file validation.cc
 * @brief Private structural validation helpers for mata_lazy::detail.
 */

#include "validation.hh"

#include "state_types.hh"

#include <type_traits>
#include <unordered_set>

namespace mata_lazy::detail {

namespace {
    template<typename T, typename KeyFn>
    bool all_unique(const std::vector<T>& items, KeyFn key) {
        using Key = std::invoke_result_t<KeyFn&, const T&>;
        std::unordered_set<Key> seen{};
        for (const T& item : items) {
            if (!seen.insert(key(item)).second) {
                return false;
            }
        }
        return true;
    }
} // namespace

bool levels_unique(const std::vector<uint8_t>& levels) {
    return all_unique(levels, [](uint8_t l) { return l; });
}

bool level_refs_unique(const std::vector<LevelRef>& refs) {
    return all_unique(refs, [](LevelRef ref) -> uint16_t {
        return static_cast<uint16_t>((static_cast<unsigned>(ref.side) << 8) | static_cast<unsigned>(ref.level));
    });
}

namespace {
    bool validate_sync_plan(const SymbolicFormula& tree, const Node& node, const SyncPlan& plan) {
        if (plan.lhs_sync_levels.size() != plan.rhs_sync_levels.size()) {
            return false;
        }

        const uint8_t lhs_arity = tree.nodes[node.lhs].result_arity;
        const uint8_t rhs_arity = tree.nodes[node.rhs].result_arity;

        if (!levels_unique(plan.lhs_sync_levels) || !levels_unique(plan.rhs_sync_levels) ||
            !level_refs_unique(plan.result_layout)) {
            return false;
        }

        for (const uint8_t level : plan.lhs_sync_levels) {
            if (level >= lhs_arity) {
                return false;
            }
        }

        for (const uint8_t level : plan.rhs_sync_levels) {
            if (level >= rhs_arity) {
                return false;
            }
        }

        for (const LevelRef ref : plan.result_layout) {
            if (ref.side == LevelRef::Side::Lhs) {
                if (ref.level >= lhs_arity) {
                    return false;
                }
            } else if (ref.level >= rhs_arity) {
                return false;
            }
        }

        return node.result_arity == plan.result_layout.size();
    }

    bool validate_project_plan(const SymbolicFormula& tree, const Node& node, const ProjectPlan& plan) {
        const uint8_t child_arity = tree.nodes[node.lhs].result_arity;
        if (!levels_unique(plan.kept_levels)) {
            return false;
        }

        for (const uint8_t level : plan.kept_levels) {
            if (level >= child_arity) {
                return false;
            }
        }

        return node.result_arity == plan.kept_levels.size();
    }

    bool validate_node(
            const SymbolicFormula& tree, const NodeId node_id, const std::vector<uint32_t>& plan_indices,
            std::vector<VisitState>& marks) {
        if (node_id >= tree.nodes.size()) {
            return false;
        }

        switch (marks[node_id]) {
            case VisitState::Unseen:
                break;
            case VisitState::Active:
                return false;
            case VisitState::Done:
                return true;
        }

        marks[node_id] = VisitState::Active;
        const Node& node = tree.nodes[node_id];
        bool ok = true;

        switch (node.kind) {
            case NodeKind::LeafNfa:
                ok = node.lhs < tree.nfas.size() && node.result_arity == 1;
                break;

            case NodeKind::LeafNft:
                ok = node.lhs < tree.nfts.size() &&
                     node.result_arity == tree.nfts[node.lhs].levels.num_of_levels;
                break;

            case NodeKind::Union:
            case NodeKind::Intersect:
                ok = validate_node(tree, node.lhs, plan_indices, marks) &&
                     validate_node(tree, node.rhs, plan_indices, marks) &&
                     tree.nodes[node.lhs].result_arity == tree.nodes[node.rhs].result_arity &&
                     node.result_arity == tree.nodes[node.lhs].result_arity;
                break;

            case NodeKind::Complement:
                ok = validate_node(tree, node.lhs, plan_indices, marks) &&
                     node.result_arity == tree.nodes[node.lhs].result_arity;
                break;

            case NodeKind::Identity:
                ok = validate_node(tree, node.lhs, plan_indices, marks) &&
                     tree.nodes[node.lhs].result_arity == 1 && node.result_arity == 2;
                break;

            case NodeKind::Project: {
                const uint32_t plan_idx = plan_indices[node_id];
                ok = validate_node(tree, node.lhs, plan_indices, marks) && plan_idx < tree.project_plans.size() &&
                     validate_project_plan(tree, node, tree.project_plans[plan_idx]);
                break;
            }

            case NodeKind::SyncProduct: {
                const uint32_t plan_idx = plan_indices[node_id];
                ok = validate_node(tree, node.lhs, plan_indices, marks) &&
                     validate_node(tree, node.rhs, plan_indices, marks) && plan_idx < tree.sync_plans.size() &&
                     validate_sync_plan(tree, node, tree.sync_plans[plan_idx]);
                break;
            }
        }

        marks[node_id] = ok ? VisitState::Done : VisitState::Unseen;
        return ok;
    }
} // namespace

bool is_valid(const SymbolicFormula& tree, const Term& root_node) {
    std::vector<VisitState> marks(tree.nodes.size(), VisitState::Unseen);
    const std::vector<uint32_t> plan_indices = compute_user_node_plan_indices(tree);
    return validate_node(tree, root_node.get_id(), plan_indices, marks);
}

} // namespace mata_lazy::detail
