/**
 * @file reconstruction.cc
 * @brief Private symbolic-formula DAG reconstruction for mata::nft::lazy::detail.
 */

#include "reconstruction.hh"

#include <cassert>
#include <cmath>
#include <limits>
#include <optional>
#include <stdexcept>
#include <utility>
#include <vector>

namespace mata::nft::lazy::detail {

namespace {

    struct ReconstructionMetrics {
        uint32_t depth;
        float log2_total_possible_states;
    };

    float log2_state_count(const size_t state_count) {
        assert(state_count > 0);
        return std::log2(static_cast<float>(state_count));
    }

    float log2_add(const float lhs, const float rhs) {
        if (std::isinf(lhs) || std::isinf(rhs)) {
            return std::numeric_limits<float>::infinity();
        }

        const float max_term = std::max(lhs, rhs);
        return max_term + std::log2(std::exp2(lhs - max_term) + std::exp2(rhs - max_term));
    }

    ReconstructionMetrics complement_metrics(const ReconstructionMetrics& child_metrics) {
        // Subset construction: child has 2^N states (N stored as log2), so complement has up to
        // 2^(2^N), whose log2 is 2^N = exp2(N) = exp2(child.log2_total_possible_states).
        return ReconstructionMetrics{
                child_metrics.depth + 1,
                std::exp2(child_metrics.log2_total_possible_states),
        };
    }

    ReconstructionMetrics unary_metrics(const ExecKind kind, const ReconstructionMetrics& child_metrics) {
        switch (kind) {
            case ExecKind::Complement:
                return complement_metrics(child_metrics);
            case ExecKind::Identity:
            case ExecKind::Project:
                return ReconstructionMetrics{
                        child_metrics.depth + 1,
                        child_metrics.log2_total_possible_states,
                };
            default:
                unreachable_kind(kind, "unary metrics");
        }
    }

    ReconstructionMetrics binary_metrics(
            const ExecKind kind, const ReconstructionMetrics& lhs_metrics, const ReconstructionMetrics& rhs_metrics) {
        switch (kind) {
            case ExecKind::Union:
                return ReconstructionMetrics{
                        1 + std::max(lhs_metrics.depth, rhs_metrics.depth),
                        log2_add(lhs_metrics.log2_total_possible_states, rhs_metrics.log2_total_possible_states),
                };
            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
                return ReconstructionMetrics{
                        1 + std::max(lhs_metrics.depth, rhs_metrics.depth),
                        lhs_metrics.log2_total_possible_states + rhs_metrics.log2_total_possible_states,
                };
            default:
                unreachable_kind(kind, "binary metrics");
        }
    }

    bool lhs_first(const ReconstructionMetrics& lhs_metrics, const ReconstructionMetrics& rhs_metrics) {
        if (lhs_metrics.depth != rhs_metrics.depth) {
            return lhs_metrics.depth < rhs_metrics.depth;
        }

        return lhs_metrics.log2_total_possible_states <= rhs_metrics.log2_total_possible_states;
    }

    bool is_commutative(const ExecKind kind) {
        // SyncProduct is binary but order-sensitive (result_layout pins each output level to a
        // specific side), so swapping its operands would change the result.
        return kind == ExecKind::Union || kind == ExecKind::Intersect;
    }

    bool is_identity_permutation(const std::vector<uint8_t>& kept_levels, const uint8_t child_arity) {
        if (kept_levels.size() != child_arity) {
            return false;
        }
        for (size_t i = 0; i < kept_levels.size(); ++i) {
            if (kept_levels[i] != static_cast<uint8_t>(i)) {
                return false;
            }
        }
        return true;
    }

    /// True when no transition in @p automaton uses EPSILON or DONT_CARE on any tape.
    template<typename Automaton>
    bool has_only_concrete_symbols(const Automaton& automaton) {
        for (const auto& state_post : automaton.delta) {
            for (const auto& move : state_post) {
                if (move.symbol == mata::nft::EPSILON || move.symbol == mata::nft::DONT_CARE) {
                    return false;
                }
            }
        }
        return true;
    }

    /// Pattern matcher for the symbolic shape that DiagonalSlice captures.
    ///
    /// On success, sets @p u_id and @p x_id to the symbolic ids of the language and the relation,
    /// otherwise returns false. The recognizer is conservative, it only fires when the language is
    /// a special-symbol-free LeafNfa and the relation is a special-symbol-free LeafNft. Special
    /// symbols (EPSILON, DONT_CARE) would need richer merge logic in the iterator that is not
    /// worth adding for the leaves the recognizer commonly sees in regular model checking.
    bool try_recognise_diagonal_slice(
            const SymbolicFormula& formula, const Node& project_node, const uint32_t project_plan_idx, NodeId& u_id,
            NodeId& x_id) {
        if (project_node.kind != NodeKind::Project) {
            return false;
        }
        const ProjectPlan& plan = formula.project_plans[project_plan_idx];
        if (plan.kept_levels.size() != 1) {
            return false;
        }

        const Node& intersect_node = formula.nodes[project_node.lhs];
        if (intersect_node.kind != NodeKind::Intersect) {
            return false;
        }
        if (intersect_node.result_arity != 2) {
            return false;
        }

        const Node& lhs = formula.nodes[intersect_node.lhs];
        const Node& rhs = formula.nodes[intersect_node.rhs];

        // Either side may carry the Identity, the other side must be the relation.
        const Node* identity_node = nullptr;
        NodeId other_id = 0;
        if (lhs.kind == NodeKind::Identity) {
            identity_node = &lhs;
            other_id = intersect_node.rhs;
        } else if (rhs.kind == NodeKind::Identity) {
            identity_node = &rhs;
            other_id = intersect_node.lhs;
        } else {
            return false;
        }

        const Node& u_node = formula.nodes[identity_node->lhs];
        const Node& x_node = formula.nodes[other_id];

        if (u_node.kind != NodeKind::LeafNfa || x_node.kind != NodeKind::LeafNft) {
            return false;
        }
        if (x_node.result_arity != 2) {
            return false;
        }
        if (!has_only_concrete_symbols(formula.nfas[u_node.lhs]) ||
            !has_only_concrete_symbols(formula.nfts[x_node.lhs])) {
            return false;
        }

        u_id = identity_node->lhs;
        x_id = other_id;
        return true;
    }

    void reorder_binary(
            const SymbolicFormula& formula, std::vector<ExecNode>& output,
            const std::vector<NodeId>& reorderable_nodes) {
        std::vector<ReconstructionMetrics> metrics(output.size());

        for (size_t node_index = 0; node_index < output.size(); ++node_index) {
            const NodeId node_id = static_cast<NodeId>(node_index);
            const ExecNode& node = output[node_id];
            switch (node.kind) {
                case ExecKind::LeafNfa:
                    metrics[node_id] =
                            ReconstructionMetrics{0, log2_state_count(formula.nfas[node.lhs].num_of_states())};
                    break;

                case ExecKind::LeafNft:
                    metrics[node_id] =
                            ReconstructionMetrics{0, log2_state_count(formula.nfts[node.lhs].num_of_states())};
                    break;

                case ExecKind::Complement:
                case ExecKind::Identity:
                case ExecKind::Project:
                    metrics[node_id] = unary_metrics(node.kind, metrics[node.lhs]);
                    break;

                case ExecKind::Union:
                case ExecKind::Intersect:
                case ExecKind::SyncProduct:
                    metrics[node_id] = binary_metrics(node.kind, metrics[node.lhs], metrics[node.rhs]);
                    break;

                case ExecKind::DiagonalSlice:
                    // Same shape as Intersect at the macrostate level (PairState over child stores),
                    // so use the intersect metric to keep the cost model consistent.
                    metrics[node_id] = binary_metrics(ExecKind::Intersect, metrics[node.lhs], metrics[node.rhs]);
                    break;
            }
        }

        for (const NodeId node_id : reorderable_nodes) {
            ExecNode& node = output[node_id];
            if (!lhs_first(metrics[node.lhs], metrics[node.rhs])) {
                std::swap(node.lhs, node.rhs);
            }
        }
    }

    /// Per-call state threaded through the recursive reconstruction.
    struct ReconstructState {
        const SymbolicFormula& formula;
        const std::vector<uint32_t>& plan_indices;
        const std::vector<CompiledSyncPlan>& compiled_sync_plans;
        std::vector<ExecNode>& output;
        std::vector<const void*>& plan_at_node;
        std::vector<NodeId>& reorderable_nodes;
        std::vector<std::optional<NodeId>>& rebuilt_ids;
        std::vector<bool>& active_path;
    };

    NodeId emit_exec_node(ReconstructState& s, const ExecNode node, const void* plan = nullptr) {
        s.output.push_back(node);
        s.plan_at_node.push_back(plan);
        return static_cast<NodeId>(s.output.size() - 1);
    }

    NodeId reconstruct_nodes_impl(ReconstructState& s, const NodeId id) {
        if (const std::optional<NodeId> rebuilt_id = s.rebuilt_ids[id]; rebuilt_id.has_value()) {
            return *rebuilt_id;
        }

        if (s.active_path[id]) {
            throw std::runtime_error("Cycle detected in the symbolic formula DAG");
        }

        const Node& node = s.formula.nodes[id];
        s.active_path[id] = true;
        const auto finish = [&](const NodeId new_id) {
            s.active_path[id] = false;
            s.rebuilt_ids[id] = new_id;
            return new_id;
        };

        switch (node.kind) {
            case NodeKind::LeafNfa:
            case NodeKind::LeafNft:
                return finish(emit_exec_node(s, ExecNode{to_exec_kind(node.kind), node.result_arity, node.lhs, node.rhs}));

            case NodeKind::Union:
            case NodeKind::Intersect: {
                const Node& lhs_src = s.formula.nodes[node.lhs];
                const Node& rhs_src = s.formula.nodes[node.rhs];
                const ExecKind exec_kind = to_exec_kind(node.kind);

                // Identity fusion, op(Identity(L1), Identity(L2)) → Identity(op(L1, L2)).
                // Both inner languages are arity 1, so the resulting product is over an arity-1
                // pair instead of an arity-2 pair, strictly cheaper macrostates and iteration.
                if (lhs_src.kind == NodeKind::Identity && rhs_src.kind == NodeKind::Identity) {
                    const NodeId l1_id = reconstruct_nodes_impl(s, lhs_src.lhs);
                    const NodeId l2_id = reconstruct_nodes_impl(s, rhs_src.lhs);

                    NodeId fused_lang_id;
                    if (l1_id == l2_id) {
                        // Idempotence also applies under fusion, op(L, L) collapses to L.
                        fused_lang_id = l1_id;
                    } else {
                        fused_lang_id = emit_exec_node(s, ExecNode{exec_kind, 1, l1_id, l2_id});
                        if (is_commutative(exec_kind)) {
                            s.reorderable_nodes.push_back(fused_lang_id);
                        }
                    }

                    return finish(emit_exec_node(s, ExecNode{ExecKind::Identity, 2, fused_lang_id, 0}));
                }

                const NodeId lhs_id = reconstruct_nodes_impl(s, node.lhs);
                const NodeId rhs_id = reconstruct_nodes_impl(s, node.rhs);

                // Idempotence, op(X, X) → X.
                if (lhs_id == rhs_id) {
                    return finish(lhs_id);
                }

                const NodeId new_id =
                        emit_exec_node(s, ExecNode{exec_kind, node.result_arity, lhs_id, rhs_id});
                if (is_commutative(exec_kind)) {
                    s.reorderable_nodes.push_back(new_id);
                }
                return finish(new_id);
            }

            case NodeKind::SyncProduct: {
                const NodeId lhs_id = reconstruct_nodes_impl(s, node.lhs);
                const NodeId rhs_id = reconstruct_nodes_impl(s, node.rhs);

                const uint32_t plan_idx = s.plan_indices[id];
                const void* plan_ptr = &s.compiled_sync_plans[plan_idx];
                return finish(emit_exec_node(
                        s, ExecNode{ExecKind::SyncProduct, node.result_arity, lhs_id, rhs_id}, plan_ptr));
            }

            case NodeKind::Complement: {
                const Node& child_node = s.formula.nodes[node.lhs];

                // De Morgan: ¬(A ∪ B) → ¬A ∩ ¬B,  ¬(A ∩ B) → ¬A ∪ ¬B.
                const auto push_demorgan = [&](const ExecKind dual_kind) {
                    const NodeId lhs_id = reconstruct_nodes_impl(s, child_node.lhs);
                    const NodeId rhs_id = reconstruct_nodes_impl(s, child_node.rhs);

                    // Idempotence across De Morgan, ¬(A op A) → (¬A) dual (¬A) → ¬A.
                    if (lhs_id == rhs_id) {
                        return finish(emit_exec_node(
                                s, ExecNode{ExecKind::Complement, node.result_arity, lhs_id, 0}));
                    }

                    const NodeId complement_lhs_id =
                            emit_exec_node(s, ExecNode{ExecKind::Complement, node.result_arity, lhs_id, 0});
                    const NodeId complement_rhs_id =
                            emit_exec_node(s, ExecNode{ExecKind::Complement, node.result_arity, rhs_id, 0});
                    const NodeId dual_id = emit_exec_node(
                            s, ExecNode{dual_kind, node.result_arity, complement_lhs_id, complement_rhs_id});
                    s.reorderable_nodes.push_back(dual_id);
                    return finish(dual_id);
                };

                switch (child_node.kind) {
                    case NodeKind::Union:
                        return push_demorgan(ExecKind::Intersect);

                    case NodeKind::Intersect:
                        return push_demorgan(ExecKind::Union);

                    case NodeKind::Complement:
                        // ¬¬X reduces to X — skip both complements and reconstruct the grandchild directly.
                        return finish(reconstruct_nodes_impl(s, child_node.lhs));

                    case NodeKind::LeafNfa:
                    case NodeKind::LeafNft:
                    case NodeKind::Identity:
                    case NodeKind::Project:
                    case NodeKind::SyncProduct: {
                        const NodeId child_id = reconstruct_nodes_impl(s, node.lhs);
                        return finish(emit_exec_node(
                                s, ExecNode{ExecKind::Complement, node.result_arity, child_id, 0}));
                    }
                }

                unreachable_kind(child_node.kind, "complement child");
            }

            case NodeKind::Identity: {
                const NodeId child_id = reconstruct_nodes_impl(s, node.lhs);
                return finish(
                        emit_exec_node(s, ExecNode{ExecKind::Identity, node.result_arity, child_id, 0}));
            }

            case NodeKind::Project: {
                const uint32_t plan_idx = s.plan_indices[id];
                const ProjectPlan& plan = s.formula.project_plans[plan_idx];
                const Node& child_src = s.formula.nodes[node.lhs];

                // DiagonalSlice rewrite, project(intersect(identity(U), X), [0|1]) → DiagonalSlice(U, X).
                // The recognizer is conservative, it only fires when both leaves are special-symbol
                // free. The iterator assumes concrete symbols.
                NodeId u_src_id{};
                NodeId x_src_id{};
                if (try_recognise_diagonal_slice(s.formula, node, plan_idx, u_src_id, x_src_id)) {
                    const NodeId u_id = reconstruct_nodes_impl(s, u_src_id);
                    const NodeId x_id = reconstruct_nodes_impl(s, x_src_id);
                    return finish(emit_exec_node(s, ExecNode{ExecKind::DiagonalSlice, 1, u_id, x_id}));
                }

                // Project of Identity. Identity is symmetric so projections collapse cleanly,
                //   project(Identity(L), [0]) → L,  project(Identity(L), [1]) → L,
                //   project(Identity(L), [0,1]) → Identity(L),  project(Identity(L), [1,0]) → Identity(L).
                // The identity-permutation rewrite below also catches [0,1], here we handle the rest.
                if (child_src.kind == NodeKind::Identity) {
                    if (plan.kept_levels.size() == 1) {
                        return finish(reconstruct_nodes_impl(s, child_src.lhs));
                    }
                    if (plan.kept_levels.size() == 2) {
                        // [0,1] or [1,0], both are Identity(L) by symmetry.
                        return finish(reconstruct_nodes_impl(s, node.lhs));
                    }
                }

                // Identity projection, project(X, [0..arity(X)-1]) → X. An identity permutation
                // that keeps every level in original order is just X. Also catches the
                // project(X, []) case when X already has arity 0.
                if (is_identity_permutation(plan.kept_levels, child_src.result_arity)) {
                    return finish(reconstruct_nodes_impl(s, node.lhs));
                }

                const NodeId child_id = reconstruct_nodes_impl(s, node.lhs);
                const void* plan_ptr = &plan;
                return finish(emit_exec_node(
                        s, ExecNode{ExecKind::Project, node.result_arity, child_id, 0}, plan_ptr));
            }
        }

        unreachable_kind(node.kind, "reconstruction");
    }

} // namespace

NodeId reconstruct_nodes(
        const SymbolicFormula& formula, const NodeId id, const std::vector<CompiledSyncPlan>& compiled_sync_plans,
        std::vector<ExecNode>& output, std::vector<const void*>& plan_at_node) {
    std::vector<NodeId> reorderable_nodes{};
    std::vector<std::optional<NodeId>> rebuilt_ids(formula.nodes.size(), std::nullopt);
    std::vector<bool> active_path(formula.nodes.size(), false);
    const std::vector<uint32_t> plan_indices = compute_user_node_plan_indices(formula);

    ReconstructState state{
            formula, plan_indices, compiled_sync_plans, output, plan_at_node, reorderable_nodes, rebuilt_ids,
            active_path};
    const NodeId root_id = reconstruct_nodes_impl(state, id);
    reorder_binary(formula, output, reorderable_nodes);
    return root_id;
}

} // namespace mata::nft::lazy::detail
