/**
 * @file iterators.cc
 * @brief Private iterator helpers for mata::nft::lazy::detail.
 */

#include "iterators.hh"

#include <algorithm>
#include <limits>
#include <stdexcept>

namespace mata::nft::lazy::detail {

TransitionTupleHelper::TransitionTupleHelper(
        const std::vector<ExecNode>& exec_nodes, const AlphabetStore& alphabet_store)
    : nodes{exec_nodes}, alphabets{alphabet_store}, special_symbols_by_level{} {}

bool TransitionTupleHelper::lhs_compatible_with_rhs(
        const NodeId lhs_id, const SymbolTuple& lhs_tuple, const NodeId rhs_id) {
    initialize_special_symbol_cache();

    const SmallVec2<std::vector<mata::Symbol>>& rhs_effective = alphabets.effective_level_symbols(rhs_id);

    for (size_t i = 0; i < lhs_tuple.size(); ++i) {
        const uint8_t level = static_cast<uint8_t>(i);
        const mata::Symbol sym = lhs_tuple[i];

        if (is_resolved_dont_care(SymbolRef{lhs_id, level, sym})) {
            continue; // DONT_CARE on lhs matches any rhs symbol
        }

        const std::vector<mata::Symbol>& level_syms = rhs_effective[i];

        if (std::binary_search(level_syms.begin(), level_syms.end(), sym)) {
            continue; // exact match is possible
        }

        // rhs DONT_CARE in effective means it can match any lhs symbol
        const ResolvedSpecialSymbols& rhs_special = special_symbols_by_level[rhs_id][level];
        if ((rhs_special.flags & ResolvedSpecialSymbols::HasDontCare) != 0 &&
            std::binary_search(level_syms.begin(), level_syms.end(), rhs_special.dont_care)) {
            continue;
        }

        return false;
    }
    return true;
}

bool TransitionTupleHelper::merge_visible_tuples(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        SymbolTuple& merged_tuple) {
    initialize_special_symbol_cache();
    return try_merge_tuples(lhs_node_id, lhs_tuple, rhs_node_id, rhs_tuple, merged_tuple);
}

bool TransitionTupleHelper::build_visible_sync_result(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        const CompiledSyncPlan& plan, SymbolTuple& result_tuple) {
    initialize_special_symbol_cache();
    if (!sync_levels_match(
                lhs_node_id, lhs_tuple, plan.lhs_sync_levels, rhs_node_id, rhs_tuple, plan.rhs_sync_levels)) {
        return false;
    }
    return build_sync_result_tuple(lhs_node_id, lhs_tuple, rhs_node_id, rhs_tuple, plan, result_tuple);
}

void TransitionTupleHelper::initialize_special_symbol_cache() {
    if (special_symbols_by_level.size() == nodes.size()) {
        return;
    }

    special_symbols_by_level.clear();
    special_symbols_by_level.resize(nodes.size());
    for (NodeId node_id = 0; node_id < nodes.size(); ++node_id) {
        special_symbols_by_level[node_id].resize(nodes[node_id].result_arity);
        for (uint8_t level = 0; level < nodes[node_id].result_arity; ++level) {
            ResolvedSpecialSymbols& resolved = special_symbols_by_level[node_id][level];
            if (const std::optional<mata::Symbol> epsilon =
                        resolve_special_symbol_id(node_id, level, mata::nft::EPSILON)) {
                resolved.epsilon = *epsilon;
                resolved.flags |= ResolvedSpecialSymbols::HasEpsilon;
            }
            if (const std::optional<mata::Symbol> dont_care =
                        resolve_special_symbol_id(node_id, level, mata::nft::DONT_CARE)) {
                resolved.dont_care = *dont_care;
                resolved.flags |= ResolvedSpecialSymbols::HasDontCare;
            }
        }
    }
}

std::optional<mata::Symbol> TransitionTupleHelper::resolve_special_symbol_id(
        const NodeId node_id, const uint8_t level, const mata::Symbol special_symbol) const {
    const auto& map = alphabets.level_alphabet(node_id, level).get_symbol_map();
    const auto it = map.find(std::to_string(special_symbol));
    return it != map.end() ? std::optional<mata::Symbol>{it->second} : std::nullopt;
}

bool TransitionTupleHelper::is_resolved_epsilon(const SymbolRef ref) const {
    const ResolvedSpecialSymbols& resolved = special_symbols_by_level[ref.node][ref.level];
    return (resolved.flags & ResolvedSpecialSymbols::HasEpsilon) != 0 && ref.symbol == resolved.epsilon;
}

bool TransitionTupleHelper::is_resolved_dont_care(const SymbolRef ref) const {
    const ResolvedSpecialSymbols& resolved = special_symbols_by_level[ref.node][ref.level];
    return (resolved.flags & ResolvedSpecialSymbols::HasDontCare) != 0 && ref.symbol == resolved.dont_care;
}

bool TransitionTupleHelper::is_resolved_special_symbol(const SymbolRef ref, const mata::Symbol special_symbol) const {
    return special_symbol == mata::nft::EPSILON ? is_resolved_epsilon(ref) : is_resolved_dont_care(ref);
}

bool TransitionTupleHelper::try_merge_symbols(
        const SymbolRef lhs, const SymbolRef rhs, mata::Symbol& merged_symbol) const {
    // Epsilon only merges with epsilon; mixed epsilon/non-epsilon transitions are incompatible.
    const bool lhs_is_epsilon = is_resolved_special_symbol(lhs, mata::nft::EPSILON);
    const bool rhs_is_epsilon = is_resolved_special_symbol(rhs, mata::nft::EPSILON);
    if (lhs_is_epsilon || rhs_is_epsilon) {
        if (!(lhs_is_epsilon && rhs_is_epsilon)) {
            return false;
        }
        merged_symbol = lhs.symbol;
        return true;
    }

    // DONT_CARE is a wildcard: it matches any concrete symbol and yields the other side's symbol.
    // Two concrete symbols merge only when they are identical.
    const bool lhs_is_dont_care = is_resolved_special_symbol(lhs, mata::nft::DONT_CARE);
    const bool rhs_is_dont_care = is_resolved_special_symbol(rhs, mata::nft::DONT_CARE);
    if (!(lhs.symbol == rhs.symbol || lhs_is_dont_care || rhs_is_dont_care)) {
        return false;
    }

    merged_symbol = lhs_is_dont_care ? rhs.symbol : lhs.symbol;
    return true;
}

bool TransitionTupleHelper::try_merge_tuples(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        SymbolTuple& merged_tuple) const {
    if (lhs_tuple.size() != rhs_tuple.size()) {
        return false;
    }

    merged_tuple.clear();
    merged_tuple.reserve(lhs_tuple.size());
    for (size_t i = 0; i < lhs_tuple.size(); ++i) {
        const uint8_t level = static_cast<uint8_t>(i);
        mata::Symbol merged_symbol = 0;
        if (!try_merge_symbols(
                    SymbolRef{lhs_node_id, level, lhs_tuple[i]}, SymbolRef{rhs_node_id, level, rhs_tuple[i]},
                    merged_symbol)) {
            return false;
        }
        merged_tuple.push_back(merged_symbol);
    }

    return true;
}

bool TransitionTupleHelper::sync_levels_match(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const SmallVec2<uint8_t>& lhs_levels,
        const NodeId rhs_node_id, const SymbolTuple& rhs_tuple, const SmallVec2<uint8_t>& rhs_levels) const {
    for (size_t i = 0; i < lhs_levels.size(); ++i) {
        const uint8_t lhs_level = lhs_levels[i];
        const uint8_t rhs_level = rhs_levels[i];
        mata::Symbol merged_symbol = 0;
        if (!try_merge_symbols(
                    SymbolRef{lhs_node_id, lhs_level, lhs_tuple[lhs_level]},
                    SymbolRef{rhs_node_id, rhs_level, rhs_tuple[rhs_level]}, merged_symbol)) {
            return false;
        }
    }
    return true;
}

bool TransitionTupleHelper::build_sync_result_tuple(
        const NodeId lhs_node_id, const SymbolTuple& lhs_tuple, const NodeId rhs_node_id, const SymbolTuple& rhs_tuple,
        const CompiledSyncPlan& plan, SymbolTuple& result_tuple) const {
    result_tuple.clear();
    result_tuple.reserve(plan.result_layout.size());

    for (const LevelRef ref : plan.result_layout) {
        const bool from_lhs = ref.side == LevelRef::Side::Lhs;
        const NodeId own_id = from_lhs ? lhs_node_id : rhs_node_id;
        const NodeId peer_id = from_lhs ? rhs_node_id : lhs_node_id;
        const SymbolTuple& own_tuple = from_lhs ? lhs_tuple : rhs_tuple;
        const SymbolTuple& peer_tuple = from_lhs ? rhs_tuple : lhs_tuple;
        const SmallVec2<int16_t>& peer_by_level = from_lhs ? plan.lhs_sync_peer_by_level : plan.rhs_sync_peer_by_level;
        const SmallVec2<uint8_t>& peer_levels = from_lhs ? plan.rhs_sync_levels : plan.lhs_sync_levels;

        mata::Symbol symbol = own_tuple[ref.level];
        // `peer_by_level[level]` is ≥ 0 only when this level is one of the synchronised levels.
        // In that case the paired level on the other side must carry a compatible symbol.
        if (ref.level < peer_by_level.size() && peer_by_level[ref.level] >= 0) {
            const size_t sync_peer = static_cast<size_t>(peer_by_level[ref.level]);
            const uint8_t peer_level = peer_levels[sync_peer];
            if (!try_merge_symbols(
                        SymbolRef{own_id, ref.level, symbol}, SymbolRef{peer_id, peer_level, peer_tuple[peer_level]},
                        symbol)) {
                return false;
            }
        }
        result_tuple.push_back(symbol);
    }

    return true;
}

// ---------------------------------------------------------------------------
// Initial-state iterator implementations
// ---------------------------------------------------------------------------

const GeneratedMacroState* UnionInitialStateIterator::next() {
    if (lhs_iter != nullptr) {
        if (const GeneratedMacroState* lhs_state = lhs_iter->next()) {
            current = GeneratedMacroState{
                    this->ctx.macro_store().intern(parent_id, TaggedState{lhs_state->id, TaggedState::Tag::Left}),
                    lhs_state->accepting};
            return &current;
        }
        lhs_iter.reset();
    }

    if (rhs_iter == nullptr) {
        return nullptr;
    }

    if (const GeneratedMacroState* rhs_state = rhs_iter->next()) {
        current = GeneratedMacroState{
                this->ctx.macro_store().intern(parent_id, TaggedState{rhs_state->id, TaggedState::Tag::Right}),
                rhs_state->accepting};
        return &current;
    }

    rhs_iter.reset();
    return nullptr;
}

const GeneratedMacroState* ProductInitialStateIterator::next() {
    while (const GeneratedMacroState* lhs_state = state_pairs.lhs()) {
        while (const GeneratedMacroState* rhs_state = state_pairs.next_rhs()) {
            current = GeneratedMacroState{
                    this->ctx.macro_store().intern(parent_id, PairState{lhs_state->id, rhs_state->id}),
                    lhs_state->accepting && rhs_state->accepting};
            return &current;
        }
        if (!state_pairs.advance_lhs()) {
            break;
        }
    }
    return nullptr;
}

const GeneratedMacroState* ComplementInitialStateIterator::next() {
    if (emitted) {
        return nullptr;
    }
    emitted = true;

    SetState sub_initial_states{};
    bool accepting = true;
    while (const GeneratedMacroState* sub_initial_state = child_iter->next()) {
        sub_initial_states.push_back(sub_initial_state->id);
        accepting = accepting && !sub_initial_state->accepting;
    }

    subsumption.minimize(child_id, sub_initial_states);
    const MacroStateId next_id = this->ctx.macro_store().intern(parent_id, std::move(sub_initial_states));
    current = GeneratedMacroState{next_id, accepting};
    return &current;
}

// ---------------------------------------------------------------------------
// Leaf transition iterator implementations
// ---------------------------------------------------------------------------

const GeneratedTransition* LeafNfaTransitionIterator::next() {
    while (pos != end) {
        const mata::nfa::Move move = *pos;
        ++pos;

        mata::Symbol resolved_symbol = 0;
        if (!alphabets.try_resolve_symbol(nfa, node_id, 0, move.symbol, resolved_symbol)) {
            continue;
        }

        current = GeneratedTransition{
                SymbolTuple{resolved_symbol},
                GeneratedMacroState{static_cast<MacroStateId>(move.target), nfa.final.contains(move.target)}};
        return &current;
    }
    return nullptr;
}

const GeneratedTransition* LeafNftTransitionIterator::next() {
    if (arity == 0) {
        return next_empty_transition();
    }
    initialize_once();
    while (skip_exhausted_frames()) {
        if (const GeneratedTransition* transition = try_emit_current_transition()) {
            return transition;
        }
    }
    return nullptr;
}

const GeneratedTransition* LeafNftTransitionIterator::next_empty_transition() {
    if (emitted_empty) {
        return nullptr;
    }
    emitted_empty = true;
    current = GeneratedTransition{
            SymbolTuple{},
            GeneratedMacroState{static_cast<MacroStateId>(source_state), nft.final.contains(source_state)}};
    return &current;
}

void LeafNftTransitionIterator::initialize_once() {
    if (!initialized) {
        initialized = true;
        push_frame(source_state);
    }
}

bool LeafNftTransitionIterator::skip_exhausted_frames() {
    while (!frames.empty() && frames.back().current == frames.back().end) {
        frames.pop_back();
        if (!frames.empty()) {
            // The popped frame finished exploring everything reachable through the parent's
            // current move, so step the parent past it to the next move.
            ++frames.back().current;
        }
    }
    return !frames.empty();
}

const GeneratedTransition* LeafNftTransitionIterator::try_emit_current_transition() {
    const size_t level = frames.size() - 1;
    Frame& frame = frames.back();
    const mata::nfa::Move move = *frame.current;

    mata::Symbol resolved_symbol = 0;
    if (!try_translate_symbol(level, move.symbol, resolved_symbol)) {
        ++frame.current;
        return nullptr;
    }

    current_tuple[level] = resolved_symbol;
    if (frames.size() == arity) {
        ++frame.current;
        current = GeneratedTransition{
                current_tuple,
                GeneratedMacroState{static_cast<MacroStateId>(move.target), nft.final.contains(move.target)}};
        return &current;
    }

    push_frame(move.target);
    return nullptr;
}

bool LeafNftTransitionIterator::try_translate_symbol(
        const size_t level, const mata::Symbol local_symbol, mata::Symbol& resolved_symbol) const {
    return alphabets.try_resolve_symbol(
            nft, static_cast<uint8_t>(level), node_id, static_cast<uint8_t>(level), local_symbol, resolved_symbol);
}

void LeafNftTransitionIterator::push_frame(const mata::nfa::State state) {
    frames.emplace_back(nft.delta.state_post(state));
}

// ---------------------------------------------------------------------------
// Product transition iterator implementations
// ---------------------------------------------------------------------------

const GeneratedTransition* BufferedTransitionIterator::next() {
    if (index >= transitions.size()) {
        return nullptr;
    }
    return &transitions[index++];
}

const GeneratedTransition* IntersectTransitionIterator::next() {
    while (const GeneratedTransition* lhs_transition = transition_pairs.lhs()) {
        if (!transition_tuple_helper.lhs_compatible_with_rhs(lhs_id, lhs_transition->tuple, rhs_id)) {
            if (!transition_pairs.advance_lhs())
                break;
            continue;
        }
        while (const GeneratedTransition* rhs_transition = transition_pairs.next_rhs()) {
            SymbolTuple merged_tuple{};
            if (!transition_tuple_helper.merge_visible_tuples(
                        lhs_id, lhs_transition->tuple, rhs_id, rhs_transition->tuple, merged_tuple)) {
                continue;
            }
            current = GeneratedTransition{
                    std::move(merged_tuple),
                    GeneratedMacroState{
                            this->ctx.macro_store().intern(
                                    parent_id, PairState{lhs_transition->state.id, rhs_transition->state.id}),
                            lhs_transition->state.accepting && rhs_transition->state.accepting}};
            return &current;
        }
        if (!transition_pairs.advance_lhs()) {
            break;
        }
    }
    return nullptr;
}

const GeneratedTransition* SyncProductTransitionIterator::next() {
    while (const GeneratedTransition* lhs_transition = transition_pairs.lhs()) {
        while (const GeneratedTransition* rhs_transition = transition_pairs.next_rhs()) {
            SymbolTuple result_tuple{};
            if (!transition_tuple_helper.build_visible_sync_result(
                        lhs_id, lhs_transition->tuple, rhs_id, rhs_transition->tuple, plan, result_tuple)) {
                continue;
            }
            current = GeneratedTransition{
                    std::move(result_tuple),
                    GeneratedMacroState{
                            this->ctx.macro_store().intern(
                                    parent_id, PairState{lhs_transition->state.id, rhs_transition->state.id}),
                            lhs_transition->state.accepting && rhs_transition->state.accepting}};
            return &current;
        }
        if (!transition_pairs.advance_lhs()) {
            break;
        }
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// DiagonalSlice transition iterator implementation
// ---------------------------------------------------------------------------

void DiagonalSliceTransitionIterator::buffer_u_transitions() {
    if (u_buffered) {
        return;
    }
    u_buffered = true;
    while (const GeneratedTransition* u_t = u_iter->next()) {
        // U is arity 1 by recognition invariant.
        u_by_symbol[u_t->tuple[0]].push_back(u_t->state);
    }
}

bool DiagonalSliceTransitionIterator::advance_to_next_diagonal_x() {
    // The recognizer only fires DiagonalSlice when both leaves are special-symbol free, so a
    // direct symbol equality check is safe. DONT_CARE and EPSILON cases would need the merge
    // logic in TransitionTupleHelper, which the iterator deliberately avoids.
    while (const GeneratedTransition* x_t = x_iter->next()) {
        if (x_t->tuple.size() != 2 || x_t->tuple[0] != x_t->tuple[1]) {
            continue;
        }
        const auto u_it = u_by_symbol.find(x_t->tuple[0]);
        if (u_it == u_by_symbol.end() || u_it->second.empty()) {
            continue;
        }
        // Snapshot the X side, the live pointer may be invalidated by later iterator activity.
        current_x_state = x_t->state;
        current_diagonal_symbol = x_t->tuple[0];
        u_match_index = 0;
        x_loaded = true;
        return true;
    }
    return false;
}

const GeneratedTransition* DiagonalSliceTransitionIterator::next() {
    if (finished) {
        return nullptr;
    }
    buffer_u_transitions();

    while (true) {
        if (x_loaded) {
            const auto& u_matches = u_by_symbol[current_diagonal_symbol];
            if (u_match_index < u_matches.size()) {
                const GeneratedMacroState& u_match = u_matches[u_match_index++];
                current = GeneratedTransition{
                        SymbolTuple{current_diagonal_symbol},
                        GeneratedMacroState{
                                this->ctx.macro_store().intern(parent_id, PairState{u_match.id, current_x_state.id}),
                                u_match.accepting && current_x_state.accepting}};
                return &current;
            }
            x_loaded = false;
        }
        if (!advance_to_next_diagonal_x()) {
            finished = true;
            return nullptr;
        }
    }
}

// ---------------------------------------------------------------------------
// Complement transition iterator implementation
// ---------------------------------------------------------------------------

namespace {
    // Below this estimated (tuples × child-states) work, the per-state direct sweep is cheaper
    // than precomputing a sorted child-transition index.
    constexpr size_t kComplementIndexedSweepThreshold = 256;

    size_t saturating_multiply(const size_t lhs, const size_t rhs) noexcept {
        if (lhs == 0 || rhs == 0) {
            return 0;
        }
        constexpr size_t kMax = std::numeric_limits<size_t>::max();
        if (lhs > kMax / rhs) {
            return kMax;
        }
        return lhs * rhs;
    }

    size_t estimate_complement_tuple_sweep_cost(
            const SmallVec2<std::vector<mata::Symbol>>& level_symbols, const size_t child_state_count) noexcept {
        size_t universe_size = 1;
        for (const auto& symbols : level_symbols) {
            if (symbols.empty()) {
                return 0;
            }
            universe_size = saturating_multiply(universe_size, symbols.size());
        }
        return saturating_multiply(universe_size, std::max<size_t>(child_state_count, 1));
    }
} // namespace

ComplementTransitionIterator::ComplementTransitionIterator(
        IteratorContext& context, const NodeId node_id, const NodeId next_child_id, const SetState& child_states,
        SubsumptionEngine& subsumption_engine, const SmallVec2<std::vector<mata::Symbol>>& symbols_per_level)
    : TransitionIterator{context}, parent_id{node_id}, child_id{next_child_id}, sub_states{child_states},
      subsumption{subsumption_engine}, level_symbols{symbols_per_level}, indexed_child_transitions{},
      indices(level_symbols.size(), 0), current_tuple(level_symbols.size(), 0),
      use_monotonic_child_index{
              estimate_complement_tuple_sweep_cost(symbols_per_level, child_states.size()) >=
              kComplementIndexedSweepThreshold},
      finished{false} {
    for (size_t level = 0; level < level_symbols.size(); ++level) {
        if (level_symbols[level].empty()) {
            finished = true;
            return;
        }
        current_tuple[level] = level_symbols[level][0];
    }

    if (use_monotonic_child_index) {
        build_child_transition_index();
    }
}

const GeneratedTransition* ComplementTransitionIterator::next() {
    if (finished) {
        return nullptr;
    }

    const SymbolTuple tuple = current_tuple;
    advance_tuple();

    next_sub_states.clear();
    next_sub_states.reserve(sub_states.size());
    bool accepting = true;

    collect_matching_successors(tuple, next_sub_states, accepting);

    subsumption.minimize(child_id, next_sub_states);
    const MacroStateId next_id = this->ctx.macro_store().intern(parent_id, std::move(next_sub_states));
    current = GeneratedTransition{tuple, GeneratedMacroState{next_id, accepting}};
    return &current;
}

void ComplementTransitionIterator::collect_matching_successors(
        const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting) {
    if (use_monotonic_child_index) {
        collect_matching_successors_indexed(tuple, next_sub_states, accepting);
    } else {
        collect_matching_successors_direct(tuple, next_sub_states, accepting);
    }
}

void ComplementTransitionIterator::build_child_transition_index() {
    indexed_child_transitions.reserve(sub_states.size());
    for (const MacroStateId sub_state : sub_states) {
        IndexedChildTransitions indexed_child{};
        TransitionIteratorPtr child_iter = this->ctx.make_transition_iterator(child_id, sub_state);
        while (const GeneratedTransition* child_transition = child_iter->next()) {
            indexed_child.transitions.push_back(*child_transition);
        }
        std::sort(
                indexed_child.transitions.begin(), indexed_child.transitions.end(),
                [](const GeneratedTransition& lhs, const GeneratedTransition& rhs) { return lhs.tuple < rhs.tuple; });
        indexed_child_transitions.push_back(std::move(indexed_child));
    }
}

void ComplementTransitionIterator::collect_matching_successors_direct(
        const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting) {
    for (const MacroStateId sub_state : sub_states) {
        TransitionIteratorPtr child_iter = this->ctx.make_transition_iterator(child_id, sub_state);
        while (const GeneratedTransition* child_transition = child_iter->next()) {
            if (child_transition->tuple != tuple) {
                continue;
            }
            next_sub_states.push_back(child_transition->state.id);
            accepting = accepting && !child_transition->state.accepting;
        }
    }
}

void ComplementTransitionIterator::collect_matching_successors_indexed(
        const SymbolTuple& tuple, SetState& next_sub_states, bool& accepting) {
    // advance_tuple emits tuples in lexicographic order matching the sort used to build the
    // index, so each child's next_index only ever moves forward across calls.
    for (IndexedChildTransitions& indexed_child : indexed_child_transitions) {
        // Advance past transitions that sort before the current tuple.
        while (indexed_child.next_index < indexed_child.transitions.size() &&
               indexed_child.transitions[indexed_child.next_index].tuple < tuple) {
            ++indexed_child.next_index;
        }

        size_t match_index = indexed_child.next_index;
        while (match_index < indexed_child.transitions.size() &&
               indexed_child.transitions[match_index].tuple == tuple) {
            next_sub_states.push_back(indexed_child.transitions[match_index].state.id);
            accepting = accepting && !indexed_child.transitions[match_index].state.accepting;
            ++match_index;
        }

        indexed_child.next_index = match_index;
    }
}

void ComplementTransitionIterator::advance_tuple() {
    // Increment the rightmost index that hasn't wrapped, then reset all levels to the right.
    for (size_t level = indices.size(); level-- > 0;) {
        if (++indices[level] < level_symbols[level].size()) {
            current_tuple[level] = level_symbols[level][indices[level]];
            for (size_t reset_level = level + 1; reset_level < indices.size(); ++reset_level) {
                indices[reset_level] = 0;
                current_tuple[reset_level] = level_symbols[reset_level][0];
            }
            return;
        }
    }
    finished = true;
}

} // namespace mata::nft::lazy::detail
