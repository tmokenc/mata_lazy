/**
 * @file iterators.hh
 * @brief Private iterator helpers for mata_lazy::detail.
 */

#pragma once

#include "alphabet_store.hh"
#include "subsumption.hh"

#include <algorithm>
#include <cassert>
#include <memory>
#include <optional>
#include <utility>
#include <vector>

namespace mata_lazy::detail {

/// Visible label tuple used by the lazy transition iterators.
using SymbolTuple = SmallVec2<mata::Symbol>;

/// One generated successor macrostate paired with its acceptance flag.
struct GeneratedMacroState {
    MacroStateId id;
    bool accepting;
};

/// Abstract iterator over generated initial macrostates.
struct InitialStateIterator;
/// Abstract iterator over visible outgoing transitions.
struct TransitionIterator;

/// Owning pointer to an initial-state iterator.
using InitialStateIteratorPtr = std::unique_ptr<InitialStateIterator>;
/// Owning pointer to a transition iterator.
using TransitionIteratorPtr = std::unique_ptr<TransitionIterator>;

/**
 * @brief Resolved special symbols cached per exec node and level.
 */
struct ResolvedSpecialSymbols {
    enum Flag : uint8_t {
        HasEpsilon = 1U << 0,
        HasDontCare = 1U << 1,
    };

    mata::Symbol epsilon{};
    mata::Symbol dont_care{};
    uint8_t flags{0};
};

/// One symbol observed at a specific level of a specific exec node.
struct SymbolRef {
    NodeId node;
    uint8_t level;
    mata::Symbol symbol;
};

/**
 * @brief Shared tuple-compatibility helper used by transition iterators.
 */
class TransitionTupleHelper {
public:
    TransitionTupleHelper(const std::vector<ExecNode>& nodes, const AlphabetStore& alphabets);

    bool merge_visible_tuples(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            SymbolTuple& merged_tuple);

    /**
     * @brief Return true if each symbol of @p lhs_tuple can possibly match a transition of @p rhs_id.
     *
     * A symbol cannot match when it is not DONT_CARE, it is absent from rhs's effective alphabet,
     * and rhs has no DONT_CARE transitions at that level.  Used to skip lhs transitions early in
     * IntersectTransitionIterator without replaying the full rhs buffer.
     */
    bool lhs_compatible_with_rhs(NodeId lhs_id, const SymbolTuple& lhs_tuple, NodeId rhs_id);

    bool build_visible_sync_result(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            const CompiledSyncPlan& plan, SymbolTuple& result_tuple);

private:
    const std::vector<ExecNode>& nodes;
    const AlphabetStore& alphabets;
    std::vector<SmallVec2<ResolvedSpecialSymbols>> special_symbols_by_level;

    void initialize_special_symbol_cache();

    std::optional<mata::Symbol>
    resolve_special_symbol_id(NodeId node_id, uint8_t level, mata::Symbol special_symbol) const;

    bool is_resolved_epsilon(SymbolRef ref) const;
    bool is_resolved_dont_care(SymbolRef ref) const;

    bool is_resolved_special_symbol(SymbolRef ref, mata::Symbol special_symbol) const;

    bool try_merge_symbols(SymbolRef lhs, SymbolRef rhs, mata::Symbol& merged_symbol) const;

    bool try_merge_tuples(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            SymbolTuple& merged_tuple) const;

    bool sync_levels_match(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const SmallVec2<uint8_t>& lhs_levels, NodeId rhs_node_id,
            const SymbolTuple& rhs_tuple, const SmallVec2<uint8_t>& rhs_levels) const;

    bool build_sync_result_tuple(
            NodeId lhs_node_id, const SymbolTuple& lhs_tuple, NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
            const CompiledSyncPlan& plan, SymbolTuple& result_tuple) const;
};

/**
 * @brief Iterator-facing services provided by the lazy emptiness context.
 */
struct IteratorContext {
    virtual ~IteratorContext() = default;

    virtual MacroStateStore& macro_store() = 0;
    virtual TransitionIteratorPtr make_transition_iterator(NodeId node_id, MacroStateId state) = 0;
    virtual InitialStateIteratorPtr make_initial_state_iterator(NodeId node_id) = 0;
};

/**
 * @brief Polymorphic iterator over initial macrostates.
 */
struct InitialStateIterator {
    IteratorContext& ctx;

    explicit InitialStateIterator(IteratorContext& context) : ctx{context} {}
    virtual ~InitialStateIterator() = default;
    virtual const GeneratedMacroState* next() = 0;
};

/**
 * @brief One generated visible transition paired with its successor macrostate.
 */
struct GeneratedTransition {
    SymbolTuple tuple;
    GeneratedMacroState state;
};

/**
 * @brief Polymorphic iterator over outgoing visible transitions.
 */
struct TransitionIterator {
    IteratorContext& ctx;

    explicit TransitionIterator(IteratorContext& context) : ctx{context} {}
    virtual ~TransitionIterator() = default;
    virtual const GeneratedTransition* next() = 0;
};

// ---------------------------------------------------------------------------
// Replay helpers
// ---------------------------------------------------------------------------

template<typename Item, typename IteratorPtr>
class ReplayBuffer {
public:
    ReplayBuffer() = default;
    explicit ReplayBuffer(IteratorPtr iterator) : live_iterator{std::move(iterator)}, replayed_items{} {}

    const Item* next() {
        if (replay_index < replayed_items.size()) {
            return &replayed_items[replay_index++];
        }

        if (replay_complete || live_iterator == nullptr) {
            return nullptr;
        }

        const Item* item = live_iterator->next();
        if (!item) {
            live_iterator.reset();
            replay_complete = true;
            return nullptr;
        }

        replayed_items.push_back(*item);
        replay_index = replayed_items.size();
        return &replayed_items.back();
    }

    void rewind() { replay_index = 0; }

private:
    IteratorPtr live_iterator{};
    std::vector<Item> replayed_items{};
    size_t replay_index{0};
    bool replay_complete{false};
};

template<typename Item, typename IteratorPtr>
class ReplayJoinCursor {
public:
    ReplayJoinCursor(IteratorPtr lhs_iterator, IteratorPtr rhs_iterator)
        : lhs_iter{std::move(lhs_iterator)}, rhs_items{std::move(rhs_iterator)}, current_lhs{nullptr} {
        advance_lhs();
    }

    ReplayJoinCursor(const ReplayJoinCursor&) = delete;
    ReplayJoinCursor& operator=(const ReplayJoinCursor&) = delete;

    const Item* lhs() const { return current_lhs; }
    const Item* next_rhs() { return rhs_items.next(); }

    bool advance_lhs() {
        current_lhs = lhs_iter->next();
        if (!current_lhs) {
            return false;
        }
        rhs_items.rewind();
        return true;
    }

private:
    IteratorPtr lhs_iter;
    ReplayBuffer<Item, IteratorPtr> rhs_items;
    const Item* current_lhs;
};

// ---------------------------------------------------------------------------
// Mapped transition iterator (used for Union, Identity, Project)
// ---------------------------------------------------------------------------

template<typename Mapper>
class MappedTransitionIterator final : public TransitionIterator {
public:
    MappedTransitionIterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter, Mapper mapper)
        : TransitionIterator{context}, child_iter{std::move(child_transition_iter)}, map{std::move(mapper)} {}

    const GeneratedTransition* next() override {
        const GeneratedTransition* child_transition = child_iter->next();
        if (!child_transition) {
            return nullptr;
        }
        current = map(this->ctx, *child_transition);
        return &current;
    }

private:
    TransitionIteratorPtr child_iter;
    Mapper map;
    GeneratedTransition current{};
};

template<typename Mapper>
TransitionIteratorPtr
make_mapped_transition_iterator(IteratorContext& context, TransitionIteratorPtr child_transition_iter, Mapper mapper) {
    return std::make_unique<MappedTransitionIterator<Mapper>>(
            context, std::move(child_transition_iter), std::move(mapper));
}

// ---------------------------------------------------------------------------
// Initial-state iterator concrete types
// ---------------------------------------------------------------------------

template<typename Automaton>
struct LeafInitialStateIterator final : InitialStateIterator {
    using InitialIterator = decltype(std::declval<const Automaton&>().initial.begin());

    const Automaton& automaton;
    InitialIterator pos;
    InitialIterator end;
    GeneratedMacroState current{};

    LeafInitialStateIterator(IteratorContext& context, const Automaton& source)
        : InitialStateIterator{context}, automaton{source}, pos{automaton.initial.begin()},
          end{automaton.initial.end()} {}

    const GeneratedMacroState* next() override {
        if (pos == end) {
            return nullptr;
        }
        const auto initial_state = *pos;
        ++pos;
        current =
                GeneratedMacroState{static_cast<MacroStateId>(initial_state), automaton.final.contains(initial_state)};
        return &current;
    }
};

struct UnionInitialStateIterator final : InitialStateIterator {
    const NodeId parent_id;
    InitialStateIteratorPtr lhs_iter;
    InitialStateIteratorPtr rhs_iter;
    GeneratedMacroState current{};

    UnionInitialStateIterator(
            IteratorContext& context, const NodeId node_id, InitialStateIteratorPtr lhs_initial_iter,
            InitialStateIteratorPtr rhs_initial_iter)
        : InitialStateIterator{context}, parent_id{node_id}, lhs_iter{std::move(lhs_initial_iter)},
          rhs_iter{std::move(rhs_initial_iter)} {}

    const GeneratedMacroState* next() override;
};

struct ProductInitialStateIterator final : InitialStateIterator {
    const NodeId parent_id;
    ReplayJoinCursor<GeneratedMacroState, InitialStateIteratorPtr> state_pairs;
    GeneratedMacroState current{};

    ProductInitialStateIterator(
            IteratorContext& context, const NodeId node_id, const NodeId next_rhs_id,
            InitialStateIteratorPtr lhs_initial_iter)
        : InitialStateIterator{context}, parent_id{node_id},
          state_pairs{std::move(lhs_initial_iter), this->ctx.make_initial_state_iterator(next_rhs_id)} {}

    const GeneratedMacroState* next() override;
};

struct ComplementInitialStateIterator final : InitialStateIterator {
    const NodeId parent_id;
    const NodeId child_id;
    InitialStateIteratorPtr child_iter;
    SubsumptionEngine& subsumption;
    bool emitted;

    ComplementInitialStateIterator(
            IteratorContext& context, const NodeId node_id, const NodeId next_child_id,
            InitialStateIteratorPtr child_initial_iter, SubsumptionEngine& subsumption)
        : InitialStateIterator{context}, parent_id{node_id}, child_id{next_child_id},
          child_iter{std::move(child_initial_iter)}, subsumption{subsumption}, emitted{false} {}

    GeneratedMacroState current{};

    const GeneratedMacroState* next() override;
};


// ---------------------------------------------------------------------------
// Transition iterator concrete types
// ---------------------------------------------------------------------------

struct LeafNfaTransitionIterator final : TransitionIterator {
    using MoveIterator = mata::nfa::StatePost::Moves::const_iterator;

    const mata::nfa::Nfa& nfa;
    const AlphabetStore& alphabets;
    const NodeId node_id;
    mata::nfa::StatePost::Moves moves;
    MoveIterator pos;
    MoveIterator end;

    LeafNfaTransitionIterator(
            IteratorContext& context, const mata::nfa::Nfa& automaton, const AlphabetStore& alphabet_store,
            const NodeId exec_node_id, const MacroStateId state)
        : TransitionIterator{context}, nfa{automaton}, alphabets{alphabet_store}, node_id{exec_node_id},
          moves{nfa.delta.state_post(static_cast<mata::nfa::State>(state)).moves()}, pos{moves.begin()},
          end{mata::nfa::StatePost::Moves::end()} {}

    GeneratedTransition current{};

    const GeneratedTransition* next() override;
};

struct LeafNftTransitionIterator final : TransitionIterator {
    using MoveIterator = mata::nfa::StatePost::Moves::const_iterator;

    struct Frame {
        mata::nfa::StatePost::Moves moves;
        MoveIterator current;
        MoveIterator end;

        explicit Frame(const mata::nfa::StatePost& state_post)
            : moves{state_post.moves()}, current{moves.begin()}, end{mata::nfa::StatePost::Moves::end()} {}
    };

    const mata::nft::Nft& nft;
    const AlphabetStore& alphabets;
    const NodeId node_id;
    const size_t arity;
    const mata::nfa::State source_state;
    SmallVec2<Frame> frames;
    SymbolTuple current_tuple;
    bool initialized;
    bool emitted_empty;

    LeafNftTransitionIterator(
            IteratorContext& context, const mata::nft::Nft& automaton, const AlphabetStore& alphabet_store,
            const NodeId exec_node_id, const MacroStateId state, const size_t result_arity)
        : TransitionIterator{context}, nft{automaton}, alphabets{alphabet_store}, node_id{exec_node_id},
          arity{result_arity}, source_state{static_cast<mata::nfa::State>(state)}, frames{},
          current_tuple(result_arity, 0), initialized{false}, emitted_empty{false} {}

    GeneratedTransition current{};

    const GeneratedTransition* next() override;

private:
    const GeneratedTransition* next_empty_transition();
    void initialize_once();
    bool skip_exhausted_frames();
    const GeneratedTransition* try_emit_current_transition();
    bool try_translate_symbol(size_t level, mata::Symbol local_symbol, mata::Symbol& resolved_symbol) const;
    void push_frame(mata::nfa::State state);
};

struct BufferedTransitionIterator final : TransitionIterator {
    const std::vector<GeneratedTransition>& transitions;
    size_t index;

    BufferedTransitionIterator(IteratorContext& context, const std::vector<GeneratedTransition>& buffered_transitions)
        : TransitionIterator{context}, transitions{buffered_transitions}, index{0} {}

    const GeneratedTransition* next() override;
};

struct IntersectTransitionIterator final : TransitionIterator {
    TransitionTupleHelper& transition_tuple_helper;
    const NodeId parent_id;
    const NodeId lhs_id;
    const NodeId rhs_id;
    ReplayJoinCursor<GeneratedTransition, TransitionIteratorPtr> transition_pairs;

    IntersectTransitionIterator(
            IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id,
            const NodeId next_lhs_id, const MacroStateId lhs_state, const NodeId next_rhs_id,
            const MacroStateId next_rhs_state)
        : TransitionIterator{context}, transition_tuple_helper{tuple_helper}, parent_id{node_id}, lhs_id{next_lhs_id},
          rhs_id{next_rhs_id}, transition_pairs{
                                       this->ctx.make_transition_iterator(lhs_id, lhs_state),
                                       this->ctx.make_transition_iterator(next_rhs_id, next_rhs_state)} {}

    GeneratedTransition current{};

    const GeneratedTransition* next() override;
};

struct SyncProductTransitionIterator final : TransitionIterator {
    TransitionTupleHelper& transition_tuple_helper;
    const NodeId parent_id;
    const NodeId lhs_id;
    const NodeId rhs_id;
    const CompiledSyncPlan& plan;
    ReplayJoinCursor<GeneratedTransition, TransitionIteratorPtr> transition_pairs;

    SyncProductTransitionIterator(
            IteratorContext& context, TransitionTupleHelper& tuple_helper, const NodeId node_id,
            const NodeId next_lhs_id, const MacroStateId lhs_state, const NodeId next_rhs_id,
            const MacroStateId next_rhs_state, const CompiledSyncPlan& compiled_plan)
        : TransitionIterator{context}, transition_tuple_helper{tuple_helper}, parent_id{node_id}, lhs_id{next_lhs_id},
          rhs_id{next_rhs_id}, plan{compiled_plan},
          transition_pairs{
                  this->ctx.make_transition_iterator(lhs_id, lhs_state),
                  this->ctx.make_transition_iterator(next_rhs_id, next_rhs_state)} {}

    GeneratedTransition current{};

    const GeneratedTransition* next() override;
};

// ---------------------------------------------------------------------------
// DiagonalSlice transition iterator
//
// Captures the pattern Diag(U, X) = { w : w ∈ U ∧ (w, w) ∈ X } where U is an
// arity-1 language node and X is an arity-2 relation node. The macrostate is
// PairState{u_state, x_state}, the same shape the equivalent
// project(intersect(identity(U), X), [0|1]) tree would have produced, but the
// iterator avoids the generic Identity, Intersect, and Project plumbing.
// ---------------------------------------------------------------------------

struct DiagonalSliceTransitionIterator final : TransitionIterator {
    const NodeId parent_id;
    TransitionIteratorPtr u_iter;
    TransitionIteratorPtr x_iter;
    /// Buffered transitions of the U side, indexed by symbol for diagonal lookup.
    std::unordered_map<mata::Symbol, std::vector<GeneratedMacroState>> u_by_symbol{};
    /// Snapshot of the current X transition's payload, copied out of the live iterator so the
    /// pointer it returned does not have to outlive the next call to x_iter.
    GeneratedMacroState current_x_state{};
    mata::Symbol current_diagonal_symbol{0};
    size_t u_match_index{0};
    bool u_buffered{false};
    bool x_loaded{false};
    bool finished{false};

    DiagonalSliceTransitionIterator(
            IteratorContext& context, const NodeId node_id, const NodeId u_node_id, const MacroStateId u_state,
            const NodeId x_node_id, const MacroStateId x_state)
        : TransitionIterator{context}, parent_id{node_id},
          u_iter{this->ctx.make_transition_iterator(u_node_id, u_state)},
          x_iter{this->ctx.make_transition_iterator(x_node_id, x_state)} {}

    GeneratedTransition current{};

    const GeneratedTransition* next() override;

private:
    void buffer_u_transitions();
    bool advance_to_next_diagonal_x();
};

// ---------------------------------------------------------------------------
// Complement transition iterator
// ---------------------------------------------------------------------------

struct ComplementTransitionIterator final : TransitionIterator {
    struct IndexedChildTransitions {
        std::vector<GeneratedTransition> transitions;
        size_t next_index{0};
    };

    const NodeId parent_id;
    const NodeId child_id;
    const SetState& sub_states;
    SubsumptionEngine& subsumption;
    const SmallVec2<std::vector<mata::Symbol>>& level_symbols;
    std::vector<IndexedChildTransitions> indexed_child_transitions;
    SmallVec2<size_t> indices;
    SymbolTuple current_tuple;
    SetState next_sub_states{};
    bool use_monotonic_child_index;
    bool finished;

    ComplementTransitionIterator(
            IteratorContext& context, NodeId node_id, NodeId next_child_id, const SetState& child_states,
            SubsumptionEngine& subsumption, const SmallVec2<std::vector<mata::Symbol>>& symbols_per_level);

    GeneratedTransition current{};

    const GeneratedTransition* next() override;

private:
    void collect_matching_successors(const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting);
    void build_child_transition_index();
    void collect_matching_successors_direct(const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting);
    void collect_matching_successors_indexed(const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting);
    void advance_tuple();
};

} // namespace mata_lazy::detail
