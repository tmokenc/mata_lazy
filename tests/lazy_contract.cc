/** @file
 * @brief Contract / structural tests for mata::nft::lazy.
 *
 * These tests focus on the public API, tree construction, and validation behavior.
 */

#include <stdexcept>

#include <catch2/catch_test_macros.hpp>

#include "mata/nfa/nfa.hh"
#include "mata_lazy.hh"
#include "mata/nft/nft.hh"

using namespace mata;
using namespace mata::nft;
using namespace mata::nft::lazy;

namespace {

nfa::Nfa make_trivial_nfa(bool accepts_epsilon = false) {
    nfa::Nfa aut{1};
    aut.initial = {0};
    if (accepts_epsilon) {
        aut.final = {0};
    }
    return aut;
}

nfa::Nfa make_epsilon_nfa_with_state_count(size_t state_count) {
    nfa::Nfa aut{state_count};
    if (state_count > 0) {
        aut.initial = {0};
        aut.final = {0};
    }
    return aut;
}

Nft make_two_level_identity(Symbol sym = 'a') {
    Nft nft = Nft::with_levels(2, 2, {0}, {1});
    nft.levels.set({0, 0});
    nft.insert_word_by_levels(0, {Word{sym}, Word{sym}}, 1);
    return nft;
}

Nft make_three_level_chain(Symbol a = 'a', Symbol b = 'b', Symbol c = 'c') {
    Nft nft = Nft::with_levels(3, 4, {0}, {3});
    nft.levels.set({0, 1, 2, 0});
    nft.delta.add(0, a, 1);
    nft.delta.add(1, b, 2);
    nft.delta.add(2, c, 3);
    return nft;
}

} // namespace

TEST_CASE("mata::nft::lazy factory methods build the expected node kinds") {
    SymbolicFormula tree;

    const Term nfa_a = tree.make_term(make_trivial_nfa());
    const Term nfa_b = tree.make_term(make_trivial_nfa(true));
    const Term nft_id = tree.make_term(make_two_level_identity('x'));

    const Term t_union = tree.unite(nfa_a, nfa_b);
    const Term t_inter = tree.intersect(nfa_a, nfa_b);
    const Term t_comp = tree.complement(nfa_a);
    const Term t_comp_nft = tree.complement(nft_id);
    const Term t_pre = tree.pre_image(nfa_a, nft_id);
    const Term t_post = tree.post_image(nfa_b, nft_id);
    const Term t_compose = tree.compose(nft_id, t_comp_nft);

    REQUIRE(tree.nfas.size() == 2);
    REQUIRE(tree.nfts.size() == 1);
    REQUIRE(tree.nodes.size() == 10);

    CHECK(tree.nodes[nfa_a.get_id()].kind == NodeKind::LeafNfa);
    CHECK(tree.nodes[nfa_a.get_id()].lhs == 0);

    CHECK(tree.nodes[nfa_b.get_id()].kind == NodeKind::LeafNfa);
    CHECK(tree.nodes[nfa_b.get_id()].lhs == 1);

    CHECK(tree.nodes[nft_id.get_id()].kind == NodeKind::LeafNft);
    CHECK(tree.nodes[nft_id.get_id()].lhs == 0);

    CHECK(tree.nodes[t_union.get_id()].kind == NodeKind::Union);
    CHECK(tree.nodes[t_inter.get_id()].kind == NodeKind::Intersect);
    CHECK(tree.nodes[t_comp.get_id()].kind == NodeKind::Complement);
    CHECK(tree.nodes[t_comp_nft.get_id()].kind == NodeKind::Complement);
    CHECK(tree.nodes[t_pre.get_id()].kind == NodeKind::SyncProduct);
    CHECK(tree.nodes[t_post.get_id()].kind == NodeKind::SyncProduct);
    CHECK(tree.nodes[t_compose.get_id()].kind == NodeKind::SyncProduct);

    CHECK(tree.nodes[t_union.get_id()].lhs == nfa_a.get_id());
    CHECK(tree.nodes[t_union.get_id()].rhs == nfa_b.get_id());
    CHECK(tree.nodes[t_inter.get_id()].lhs == nfa_a.get_id());
    CHECK(tree.nodes[t_inter.get_id()].rhs == nfa_b.get_id());
    CHECK(tree.nodes[t_comp.get_id()].lhs == nfa_a.get_id());
    CHECK(tree.nodes[t_comp_nft.get_id()].lhs == nft_id.get_id());
    CHECK(tree.nodes[t_pre.get_id()].lhs == nfa_a.get_id());
    CHECK(tree.nodes[t_pre.get_id()].rhs == nft_id.get_id());
    CHECK(tree.nodes[t_post.get_id()].lhs == nfa_b.get_id());
    CHECK(tree.nodes[t_post.get_id()].rhs == nft_id.get_id());
    CHECK(tree.nodes[t_compose.get_id()].lhs == nft_id.get_id());
    CHECK(tree.nodes[t_compose.get_id()].rhs == t_comp_nft.get_id());
}

TEST_CASE("mata::nft::lazy post_image and pre_image are fixed to arity-1 over 2-tape transducers") {
    SymbolicFormula tree;

    const Term lang = tree.make_term(make_trivial_nfa(true));
    const Term two_tape = tree.make_term(make_two_level_identity('x'));
    const Term three_tape = tree.make_term(make_three_level_chain('a', 'b', 'c'));

    CHECK_NOTHROW(tree.post_image(lang, two_tape));
    CHECK_NOTHROW(tree.pre_image(lang, two_tape));
    CHECK_THROWS_AS(tree.post_image(lang, three_tape), std::invalid_argument);
    CHECK_THROWS_AS(tree.pre_image(lang, three_tape), std::invalid_argument);
}

TEST_CASE("mata::nft::lazy accepts NFTs with arbitrary arity") {
    SymbolicFormula tree;

    const Nft one_level = Nft::with_levels(1, 1, {0}, {0});
    const Nft three_level = Nft::with_levels(3, 1, {0}, {0});

    const Term unary = tree.make_term(one_level);
    const Term ternary = tree.make_term(three_level);

    CHECK(tree.arity_of(unary) == 1);
    CHECK(tree.arity_of(ternary) == 3);
}

TEST_CASE("mata::nft::lazy identity turns an arity-1 language into an arity-2 relation") {
    SymbolicFormula tree;
    const Term lang = tree.make_term(make_trivial_nfa());
    const Term id = tree.identity(lang);

    CHECK(tree.arity_of(id) == 2);
    CHECK(tree.nodes[id.get_id()].kind == NodeKind::Identity);
}

TEST_CASE("mata::nft::lazy generic compose keeps all non-synchronized levels") {
    SymbolicFormula tree;
    const Term lhs = tree.make_term(make_three_level_chain('a', 'b', 'c'));
    const Term rhs = tree.make_term(make_three_level_chain('c', 'd', 'e'));
    const Term composed = tree.compose(lhs, rhs, {2}, {0});

    CHECK(tree.nodes[composed.get_id()].kind == NodeKind::SyncProduct);
    CHECK(tree.arity_of(composed) == 4);
}

TEST_CASE("mata::nft::lazy is_valid should reject cycles") {
    // Each section uses a fresh tree with only a leaf NFA so the
    // nfas vector has exactly one entry (index 0). The raw node
    // manipulation is therefore consistent with the stored automata.

    SECTION("direct self-loop: node 0 points to itself as a child") {
        SymbolicFormula tree;
        tree.nfas.push_back(make_trivial_nfa());
        // Node 0: LeafNfa referencing nfas[0]  (valid leaf, needed as base)
        // Node 1: Complement whose child is node 1 (self-loop)
        tree.nodes = {
                {NodeKind::LeafNfa, 1, 0, 0},
                {NodeKind::Complement, 1, 1, 0},
        };
        CHECK_FALSE(tree.is_valid(Term{1}));
    }

    SECTION("two-node cycle: node 0 -> node 1 -> node 0") {
        SymbolicFormula tree;
        tree.nfas.push_back(make_trivial_nfa());
        tree.nfas.push_back(make_trivial_nfa(true));
        // Node 0: Union(node 1, node 1)
        // Node 1: Complement(node 0)  -- back-edge to node 0
        tree.nodes = {
                {NodeKind::Union, 1, 1, 1},
                {NodeKind::Complement, 1, 0, 0},
        };
        CHECK_FALSE(tree.is_valid(Term{0}));
    }

    SECTION("deeper cycle: node 2 -> node 1 -> node 0 -> node 2") {
        SymbolicFormula tree;
        tree.nfas.push_back(make_trivial_nfa());
        // Node 0: Complement(node 2)  -- back-edge
        // Node 1: Complement(node 0)
        // Node 2: Complement(node 1)
        tree.nodes = {
                {NodeKind::Complement, 1, 2, 0},
                {NodeKind::Complement, 1, 0, 0},
                {NodeKind::Complement, 1, 1, 0},
        };
        CHECK_FALSE(tree.is_valid(Term{2}));
    }

    SECTION("cycle only on unreachable branch does not affect a valid root") {
        SymbolicFormula tree;
        tree.nfas.push_back(make_trivial_nfa());
        // Node 0: LeafNfa (valid, reachable)
        // Node 1: Complement(node 1) -- self-loop, but NOT reachable from node 0
        tree.nodes = {
                {NodeKind::LeafNfa, 1, 0, 0},
                {NodeKind::Complement, 1, 1, 0},
        };
        // Validating from node 0 should succeed; the cycle at node 1 is unreachable.
        CHECK(tree.is_valid(Term{0}));
        // Validating from node 1 should fail.
        CHECK_FALSE(tree.is_valid(Term{1}));
    }
}


TEST_CASE("mata::nft::lazy is_valid should accept DAG sharing") {
    SymbolicFormula tree;

    const Term lhs = tree.make_term(make_trivial_nfa());
    const Term rhs = tree.make_term(make_trivial_nfa(true));
    const Term shared = tree.unite(lhs, rhs);
    [[maybe_unused]] const Term root = tree.intersect(shared, shared);

    CHECK(tree.is_valid(root));
}

TEST_CASE("mata::nft::lazy public API preserves semantics for asymmetric union and intersect trees") {
    SECTION("union remains non-empty regardless of subtree depth ordering") {
        SymbolicFormula tree;

        const Term deep_empty = tree.unite(tree.make_term(make_trivial_nfa()), tree.make_term(make_trivial_nfa()));
        const Term shallow_eps = tree.make_term(make_trivial_nfa(true));

        CHECK_FALSE(tree.is_empty(tree.unite(deep_empty, shallow_eps)));
        CHECK_FALSE(tree.is_empty(tree.unite(shallow_eps, deep_empty)));
    }

    SECTION("intersect remains non-empty regardless of equal-depth state-count ordering") {
        SymbolicFormula tree;

        const Term big_eps = tree.make_term(make_epsilon_nfa_with_state_count(8));
        const Term small_eps = tree.make_term(make_epsilon_nfa_with_state_count(2));

        CHECK_FALSE(tree.is_empty(tree.intersect(big_eps, small_eps)));
        CHECK_FALSE(tree.is_empty(tree.intersect(small_eps, big_eps)));
    }

    SECTION("complement over asymmetric boolean trees preserves the same public result") {
        SymbolicFormula tree;

        const Term deep_empty = tree.unite(tree.make_term(make_trivial_nfa()), tree.make_term(make_trivial_nfa()));
        const Term shallow_eps = tree.make_term(make_trivial_nfa(true));
        const Term union_left = tree.complement(tree.unite(deep_empty, shallow_eps));
        const Term union_right = tree.complement(tree.unite(shallow_eps, deep_empty));

        CHECK(tree.is_empty(union_left));
        CHECK(tree.is_empty(union_right));
    }
}
