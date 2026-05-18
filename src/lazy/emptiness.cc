/**
 * @file emptiness.cc
 * @brief Private execution engine for mata_lazy::detail.
 */

#include "emptiness.hh"

#include "alphabet_store.hh"
#include "iterators.hh"
#include "macrostate_store.hh"
#include "reconstruction.hh"
#include "subsumption.hh"

#include <algorithm>
#include <cstdint>
#include <deque>
#include <memory>
#include <unordered_set>
#include <utility>
#include <vector>

namespace mata_lazy::detail {

namespace {
    // Only product-like nodes do non-trivial work that can be repeated: their iterators get
    // re-created for the same (node, state) pair across the search.  Leaves, Union, Identity, and
    // Project just forward or relabel — caching them would cost more than it saves.
    constexpr bool should_cache_transitions(const ExecKind kind) noexcept {
        switch (kind) {
            case ExecKind::Intersect:
            case ExecKind::SyncProduct:
            case ExecKind::Complement:
            case ExecKind::DiagonalSlice:
                return true;

            case ExecKind::LeafNfa:
            case ExecKind::LeafNft:
            case ExecKind::Union:
            case ExecKind::Identity:
            case ExecKind::Project:
                return false;
        }
        return false;
    }

    struct TransitionCache {
        using Key = std::pair<NodeId, MacroStateId>;
        using Buffer = std::vector<GeneratedTransition>;

        struct KeyHash {
            size_t operator()(const Key& key) const noexcept {
                return static_cast<size_t>(
                        mix_hash64((static_cast<uint64_t>(key.first) << 32) | static_cast<uint64_t>(key.second)));
            }
        };

        // Only fully enumerated transition lists live in the cache. Storage is a deque of buffers so
        // pointers into the buffers stay valid across later commits (deque guarantees stable refs on
        // push_back), letting BufferedTransitionIterator hold a const-ref into the buffer.
        std::deque<Buffer> buffers{};
        std::unordered_map<Key, Buffer*, KeyHash> index{};

        const Buffer* get(const NodeId node_id, const MacroStateId state) const {
            const auto it = index.find(Key{node_id, state});
            return it == index.end() ? nullptr : it->second;
        }

        void commit(const NodeId node_id, const MacroStateId state, Buffer buffer) {
            buffers.emplace_back(std::move(buffer));
            index.emplace(Key{node_id, state}, &buffers.back());
        }
    };

    // Buffers transitions in iterator-local memory and commits them to the cache on destruction
    // only when the underlying iterator was exhausted. Abandoned (early-exit) iterations leave no
    // trace in the cache, so the cache never holds partial replays.
    class WriteThroughIterator final : public TransitionIterator {
    public:
        WriteThroughIterator(
                IteratorContext& context, TransitionIteratorPtr live, TransitionCache& cache,
                const NodeId node_id, const MacroStateId state)
            : TransitionIterator{context}, live_iter{std::move(live)}, target_cache{cache},
              target_node_id{node_id}, target_state{state} {}

        WriteThroughIterator(const WriteThroughIterator&) = delete;
        WriteThroughIterator& operator=(const WriteThroughIterator&) = delete;

        ~WriteThroughIterator() override {
            if (completed) {
                target_cache.commit(target_node_id, target_state, std::move(buffer));
            }
        }

        const GeneratedTransition* next() override {
            const GeneratedTransition* item = live_iter->next();
            if (!item) {
                completed = true;
                return nullptr;
            }
            current = *item;
            buffer.push_back(current);
            return &current;
        }

    private:
        TransitionIteratorPtr live_iter;
        TransitionCache& target_cache;
        NodeId target_node_id;
        MacroStateId target_state;
        TransitionCache::Buffer buffer{};
        GeneratedTransition current{};
        bool completed{false};
    };

    std::vector<CompiledSyncPlan> compile_sync_plans(const std::vector<SyncPlan>& sync_plans) {
        std::vector<CompiledSyncPlan> compiled_plans{};
        compiled_plans.reserve(sync_plans.size());

        const auto level_count = [](const auto& levels) -> size_t {
            if (levels.empty())
                return 0;
            return static_cast<size_t>(*std::max_element(levels.begin(), levels.end())) + 1;
        };

        for (const SyncPlan& plan : sync_plans) {
            CompiledSyncPlan compiled{};
            compiled.lhs_sync_levels = plan.lhs_sync_levels;
            compiled.rhs_sync_levels = plan.rhs_sync_levels;
            compiled.result_layout = plan.result_layout;

            const size_t lhs_levels = level_count(compiled.lhs_sync_levels);
            const size_t rhs_levels = level_count(compiled.rhs_sync_levels);

            compiled.lhs_sync_peer_by_level.assign(lhs_levels, -1);
            compiled.rhs_sync_peer_by_level.assign(rhs_levels, -1);
            for (size_t i = 0; i < compiled.lhs_sync_levels.size(); ++i) {
                compiled.lhs_sync_peer_by_level[compiled.lhs_sync_levels[i]] = static_cast<int16_t>(i);
                compiled.rhs_sync_peer_by_level[compiled.rhs_sync_levels[i]] = static_cast<int16_t>(i);
            }

            compiled_plans.push_back(std::move(compiled));
        }

        return compiled_plans;
    }

    struct Context final : IteratorContext {
        using Nfa = mata::nfa::Nfa;
        using Nft = mata::nft::Nft;

        const std::vector<Nfa>& nfas;
        const std::vector<Nft>& nfts;

        std::vector<ExecNode> nodes;
        std::vector<CompiledSyncPlan> sync_plans;
        /// Per-exec-node plan pointer, type-erased and dispatched by the node's @c ExecKind
        /// at the cast site (Project → @c ProjectPlan*, SyncProduct → @c CompiledSyncPlan*,
        /// otherwise nullptr). The pointers alias @c formula.project_plans and @c sync_plans;
        /// both must stay at the same address for the whole @c is_empty call — neither may be
        /// resized after Context construction.
        std::vector<const void*> plan_at_node;
        MacroStateStore macro_store_;
        AlphabetStore alphabets;
        SubsumptionEngine subsumption;
        TransitionTupleHelper transition_tuple_helper;
        TransitionCache transition_cache;
        NodeId root_id;

        Context(const SymbolicFormula& formula, NodeId root,
                const std::vector<mata::OnTheFlyAlphabet>* root_level_alphabets = nullptr)
            : nfas(formula.nfas), nfts(formula.nfts), nodes{},
              sync_plans{compile_sync_plans(formula.sync_plans)}, plan_at_node{}, macro_store_{}, alphabets{},
              subsumption{SubsumptionContext{nfas, nfts, nodes, macro_store_}},
              transition_tuple_helper{nodes, alphabets}, transition_cache{}, root_id{0} {

            root_id = reconstruct_nodes(formula, root, sync_plans, nodes, plan_at_node);
            macro_store_ = MacroStateStore(nodes, nfas, nfts);
            alphabets = AlphabetStore{nodes, root_id, nfas, nfts, plan_at_node, root_level_alphabets};
            subsumption.initialize_leaf_simulations(root_id);
        }

        MacroStateStore& macro_store() override { return macro_store_; }

        TransitionIteratorPtr make_transition_iterator(const NodeId node_id, const MacroStateId state) override {
            // Root-node states are visited at most once (the `visited` set in is_empty guarantees
            // this), so any cache entry for root would be permanently unreachable — skip it.
            if (!should_cache_transitions(nodes[node_id].kind) || node_id == root_id) {
                return make_uncached_transition_iterator(node_id, state);
            }

            if (const auto* cached = transition_cache.get(node_id, state)) {
                return std::make_unique<BufferedTransitionIterator>(*this, *cached);
            }

            return std::make_unique<WriteThroughIterator>(
                    *this, make_uncached_transition_iterator(node_id, state), transition_cache, node_id, state);
        }

    private:
        TransitionIteratorPtr make_uncached_transition_iterator(const NodeId node_id, const MacroStateId state) {
            const ExecNode& node = nodes[node_id];

            switch (node.kind) {
                case ExecKind::LeafNfa:
                    return std::make_unique<LeafNfaTransitionIterator>(
                            *this, nfas[node.lhs], alphabets, node_id, state);

                case ExecKind::LeafNft:
                    return std::make_unique<LeafNftTransitionIterator>(
                            *this, nfts[node.lhs], alphabets, node_id, state, node.result_arity);

                case ExecKind::Union: {
                    const TaggedState tagged = macro_store_.get_tagged(node_id, state);
                    const NodeId child_id = tagged.tag == TaggedState::Tag::Left ? node.lhs : node.rhs;
                    return make_mapped_transition_iterator(
                            *this, make_transition_iterator(child_id, tagged.state),
                            [node_id, tag = tagged.tag](IteratorContext& ctx, const GeneratedTransition& t) {
                                return GeneratedTransition{
                                        t.tuple,
                                        GeneratedMacroState{
                                                ctx.macro_store().intern(node_id, TaggedState{t.state.id, tag}),
                                                t.state.accepting}};
                            });
                }

                case ExecKind::Intersect: {
                    const PairState pair = macro_store_.get_pair(node_id, state);
                    return std::make_unique<IntersectTransitionIterator>(
                            *this, transition_tuple_helper, node_id, node.lhs, pair.lhs, node.rhs, pair.rhs);
                }

                case ExecKind::SyncProduct: {
                    const PairState pair = macro_store_.get_pair(node_id, state);
                    const auto& plan = *static_cast<const CompiledSyncPlan*>(plan_at_node[node_id]);
                    return std::make_unique<SyncProductTransitionIterator>(
                            *this, transition_tuple_helper, node_id, node.lhs, pair.lhs, node.rhs, pair.rhs, plan);
                }

                case ExecKind::Complement:
                    return std::make_unique<ComplementTransitionIterator>(
                            *this, node_id, node.lhs, macro_store_.get_set(node_id, state), subsumption,
                            alphabets.level_symbols(node_id));

                case ExecKind::Identity:
                    return make_mapped_transition_iterator(
                            *this, make_transition_iterator(node.lhs, state),
                            [](IteratorContext&, const GeneratedTransition& t) {
                                assert(t.tuple.size() == 1);
                                return GeneratedTransition{SymbolTuple{t.tuple[0], t.tuple[0]}, t.state};
                            });

                case ExecKind::Project: {
                    const auto& plan = *static_cast<const ProjectPlan*>(plan_at_node[node_id]);
                    return make_mapped_transition_iterator(
                            *this, make_transition_iterator(node.lhs, state),
                            [&plan](IteratorContext&, const GeneratedTransition& t) {
                                SymbolTuple projected{};
                                projected.reserve(plan.kept_levels.size());
                                for (const uint8_t level : plan.kept_levels) {
                                    projected.push_back(t.tuple[level]);
                                }
                                return GeneratedTransition{std::move(projected), t.state};
                            });
                }

                case ExecKind::DiagonalSlice: {
                    const PairState pair = macro_store_.get_pair(node_id, state);
                    return std::make_unique<DiagonalSliceTransitionIterator>(
                            *this, node_id, node.lhs, pair.lhs, node.rhs, pair.rhs);
                }
            }

            unreachable_kind(node.kind, "transition iterator");
        }

    public:
        InitialStateIteratorPtr make_initial_state_iterator(const NodeId node_id) override {
            const ExecNode& node = nodes[node_id];

            switch (node.kind) {
                case ExecKind::LeafNfa:
                    return std::make_unique<LeafInitialStateIterator<Nfa>>(*this, nfas[node.lhs]);

                case ExecKind::LeafNft:
                    return std::make_unique<LeafInitialStateIterator<Nft>>(*this, nfts[node.lhs]);

                case ExecKind::Union:
                    return std::make_unique<UnionInitialStateIterator>(
                            *this, node_id, make_initial_state_iterator(node.lhs),
                            make_initial_state_iterator(node.rhs));

                case ExecKind::Intersect:
                case ExecKind::SyncProduct:
                case ExecKind::DiagonalSlice:
                    return std::make_unique<ProductInitialStateIterator>(
                            *this, node_id, node.rhs, make_initial_state_iterator(node.lhs));

                case ExecKind::Complement:
                    return std::make_unique<ComplementInitialStateIterator>(
                            *this, node_id, node.lhs, make_initial_state_iterator(node.lhs), subsumption);

                case ExecKind::Identity:
                case ExecKind::Project:
                    return make_initial_state_iterator(node.lhs);
            }

            unreachable_kind(node.kind, "initial-state iterator");
        }
    };

} // namespace

bool is_empty(
        const SymbolicFormula& formula, const Term& root_node,
        const std::vector<mata::OnTheFlyAlphabet>* level_alphabets) {
    Context ctx(formula, root_node.get_id(), level_alphabets);

    std::vector<MacroStateId> worklist{};
    std::unordered_set<MacroStateId> visited{};

    const auto enqueue_if_relevant = [&](const GeneratedMacroState& generated_state) {
        const MacroStateId state = generated_state.id;

        if (!visited.insert(state).second) {
            return;
        }

        if (ctx.subsumption.is_subsumed(ctx.root_id, state)) {
            return;
        }

        worklist.push_back(state);
    };

    InitialStateIteratorPtr initial_states = ctx.make_initial_state_iterator(ctx.root_id);

    while (const GeneratedMacroState* initial_state = initial_states->next()) {
        if (initial_state->accepting) {
            return false;
        }

        enqueue_if_relevant(*initial_state);
    }

    while (!worklist.empty()) {
        const MacroStateId current_state = worklist.back();
        worklist.pop_back();

        if (ctx.subsumption.is_pruned(current_state)) {
            continue;
        }

        TransitionIteratorPtr transitions = ctx.make_transition_iterator(ctx.root_id, current_state);
        while (const GeneratedTransition* transition = transitions->next()) {
            if (transition->state.accepting) {
                return false;
            }

            enqueue_if_relevant(transition->state);
        }
    }

    // Exhausted the reachable state space without hitting an accepting state.
    return true;
}

} // namespace mata_lazy::detail
