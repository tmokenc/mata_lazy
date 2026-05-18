// example08 - lazy inductive invariant regular model.
//
// The input has four automata:
//   1. NFA of initial states over Sigma
//   2. NFA of unsafe over Sigma
//   3. NFT of abstract transition relation delta over Gamma
//   4. NFT of interpretation v between Gamma and Sigma
//
// The system is safe if no unsafe state is reachable from an initial state
// through a sequence of transitions in delta, where the abstract states
// along the way are always consistent with their interpretation in Sigma.
//
// The problem itself is undecidable, but exists a sound (but incomplete) proof rule that can be used to show safety
// by over-approximating the set of reachable states using inductive invariant. The proof rule is as follows:
//  ind = complement(pi_1(intersection(id_Gamma*, v o delta o reverse(complement(v)))))
//  preach = complement(reverse(v) o id_ind o complement(v)))
//  if intersection(apply(initial, preach), unsafe) is empty, then the
//  system is safe. If not, then the system may or may not be safe, but we can't conclude either way.


#include "mata/alphabet.hh"
#include "mata/nfa/nfa.hh"
#include "mata_lazy.hh"
#include "mata/nft/nft.hh"

#include <cassert>
#include <iostream>
#include <string>
#include <utility>
#include <vector>

using namespace mata;
using namespace mata::nfa;
using namespace mata::nft;
using namespace mata_lazy;

namespace {

OnTheFlyAlphabet build_sigma_alphabet() {
    OnTheFlyAlphabet sigma{};
    sigma.add_new_symbol("0", '0');
    sigma.add_new_symbol("1", '1');
    return sigma;
}

OnTheFlyAlphabet build_gamma_alphabet() {
    OnTheFlyAlphabet gamma{};
    gamma.add_new_symbol("A", 'A');
    gamma.add_new_symbol("B", 'B');
    return gamma;
}

Nfa single_symbol_language(const Symbol symbol, Alphabet* alphabet) {
    Nfa aut{2, {0}, {1}, alphabet};
    aut.delta.add(0, symbol, 1);
    return aut;
}

Nfa universal_language(const std::vector<Symbol>& symbols, Alphabet* alphabet) {
    Nfa aut{1, {0}, {0}, alphabet};
    for (const Symbol symbol : symbols) {
        aut.delta.add(0, symbol, 0);
    }
    return aut;
}

Nft single_symbol_relation(
        const std::vector<std::pair<Symbol, Symbol>>& pairs, AlphabetLevels* alphabets) {
    Nft nft = Nft::with_levels(2, 1 + pairs.size(), {0}, {0}, alphabets);

    nft.levels[0] = 0;
    for (size_t i = 0; i < pairs.size(); ++i) {
        const State mid = static_cast<State>(i + 1);
        nft.levels[mid] = 1;
        nft.delta.add(0, pairs[i].first, mid);
        nft.delta.add(mid, pairs[i].second, 0);
    }

    return nft;
}

} // namespace

int main() {
    OnTheFlyAlphabet sigma = build_sigma_alphabet();
    OnTheFlyAlphabet gamma = build_gamma_alphabet();

    const Symbol sigma_0 = sigma.translate_symb("0");
    const Symbol sigma_1 = sigma.translate_symb("1");
    const Symbol gamma_a = gamma.translate_symb("A");
    const Symbol gamma_b = gamma.translate_symb("B");

    // ---------------------------------------------------------------------
    // Part 1: build the four inputs
    // ---------------------------------------------------------------------

    Nfa initial = single_symbol_language(sigma_1, &sigma);
    Nfa unsafe = single_symbol_language(sigma_0, &sigma);
    Nfa gamma_univ = universal_language({gamma_a, gamma_b}, &gamma);

    // delta abstracts one step on Gamma: A -> B and B -> B.
    AlphabetLevels delta_alphabets{ std::vector<Alphabet*>{&gamma, &gamma} };
    Nft delta = single_symbol_relation({{gamma_a, gamma_b}, {gamma_b, gamma_b}}, &delta_alphabets);

    // Interpretation:
    //   A means concrete symbol 0
    //   B means concrete symbol 1
    //
    // `v` goes from Gamma to Sigma.
    AlphabetLevels v_alphabets{ std::vector<Alphabet*>{&gamma, &sigma} };
    Nft v = single_symbol_relation({{gamma_a, sigma_0}, {gamma_b, sigma_1}}, &v_alphabets);

    SymbolicFormula tree;

    const Term t_initial = tree.make_term(initial);
    const Term t_unsafe = tree.make_term(unsafe);
    const Term t_gamma_univ = tree.make_term(gamma_univ);
    const Term t_delta = tree.make_term(delta);
    const Term t_v = tree.make_term(v);

    const Term not_v = tree.complement(t_v);
    const Term id_gamma_univ = tree.identity(t_gamma_univ);

    // ---------------------------------------------------------------------
    // Part 2: build `ind`, a.k.a. inductive invariants
    // complement(pi_1(intersection(id_Gamma*, v o delta o reverse(complement(v)))))
    //
    // There is no explicit reverse node in the lazy API. Instead:
    // - compose(delta, complement(v)) encodes delta o reverse(complement(v))
    //   by synchronizing delta's output tape with complement(v)'s input tape
    // - compose(..., v, {1}, {1}) then synchronizes on the concrete Sigma tape
    // ---------------------------------------------------------------------

    const Term delta_then_not_v = tree.compose(t_delta, not_v);
    const Term v_delta_reverse_not_v = tree.compose(delta_then_not_v, t_v, {1}, {1});
    const Term bad_source_pairs = tree.intersect(id_gamma_univ, v_delta_reverse_not_v);
    const Term bad_abstract_sources = tree.project(bad_source_pairs, {0});
    const Term ind = tree.complement(bad_abstract_sources);

    // ---------------------------------------------------------------------
    // Part 3: build `preach`, a.k.a. potentially reachable abstract states.
    // complement(reverse(v) o id_ind o complement(v))
    // ---------------------------------------------------------------------

    const Term id_ind = tree.identity(ind);
    const Term preach_bad = tree.compose(tree.compose(t_v, id_ind, {0}, {0}), not_v);
    const Term preach = tree.complement(preach_bad);

    // ---------------------------------------------------------------------
    // Part 4: final safety query
    // check whether intersection(apply(initial, preach), unsafe) is empty.
    // ---------------------------------------------------------------------

    const Term reachable_under_preach = tree.post_image(t_initial, preach);
    const Term unsafe_reachable = tree.intersect(reachable_under_preach, t_unsafe);

    // Check if the built tree is valid. This is a sanity check that we didn't mess up the internal invariants of the
    // tree. Should be valid if only used the public API to build the tree. Not some hacky internal manipulation that
    // could have broken invariants.
    assert(tree.is_valid(unsafe_reachable));

    // Up until now, we were just building a symbolic representation of the problem.
    // Nothing was done in terms of actual automata operations, only building the structure that encodes the problem.
    // The actual evaluation only happens when calling `is_empty`.
    const bool is_safe = tree.is_empty(unsafe_reachable, sigma);

    // Or can be called without having to provide the alphabet.
    // It will infer the alphabet itself based on operations used in the structure.
    const bool is_safe_without_provided_alphabet = tree.is_empty(unsafe_reachable);

    assert(is_safe == is_safe_without_provided_alphabet);

    std::cout << std::boolalpha;
    std::cout << "Is the system safe? " << is_safe << std::endl;

    return 0;
}
