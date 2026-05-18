/**
 * @file reconstruction.hh
 * @brief Private symbolic-formula DAG reconstruction declarations for mata::nft::lazy::detail.
 */

#pragma once

#include "state_types.hh"

#include <vector>

namespace mata::nft::lazy::detail {

/**
 * @brief Reconstruct and normalize the reachable exec DAG below one symbolic term.
 *
 * Also fills @p plan_at_node parallel to @p output: for @c ExecKind::Project
 * nodes the slot points to a @c ProjectPlan inside @c formula.project_plans
 * (stable for the duration of the enclosing @c is_empty call); for
 * @c ExecKind::SyncProduct nodes the slot points to a @c CompiledSyncPlan
 * inside @p compiled_sync_plans (which the caller MUST keep stable — never
 * reallocate after calling this function); other kinds get nullptr.
 *
 * @param formula Source symbolic formula DAG.
 * @param id Root symbolic node to reconstruct.
 * @param compiled_sync_plans Compiled-plan storage parallel to @c formula.sync_plans.
 * @param output Destination exec-node array appended in child-before-parent order.
 * @param plan_at_node Per-exec-node type-erased plan pointers; sized to match @p output.
 * @return Exec-node id of the reconstructed root inside @p output.
 */
NodeId reconstruct_nodes(
        const SymbolicFormula& formula, NodeId id, const std::vector<CompiledSyncPlan>& compiled_sync_plans,
        std::vector<ExecNode>& output, std::vector<const void*>& plan_at_node);

} // namespace mata::nft::lazy::detail
