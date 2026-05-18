// example07 - lazy language inclusion with nfa::lazy

#include "mata_lazy.hh"
#include "mata/nfa/nfa.hh"

#include <cassert>
#include <iostream>

using namespace mata;

namespace nfa_ops = mata::nfa;
namespace nfa_lazy = mata::nfa::lazy;

namespace {

nfa_ops::Nfa words_starting_with_a() {
    // Accepts a(a|b)*.
    nfa_ops::Nfa aut{2};
    aut.initial = {0};
    aut.final = {1};

    aut.delta.add(0, 'a', 1);
    aut.delta.add(1, 'a', 1);
    aut.delta.add(1, 'b', 1);

    return aut;
}

nfa_ops::Nfa words_ending_with_b() {
    // Accepts (a|b)*b.
    nfa_ops::Nfa aut{2};
    aut.initial = {0};
    aut.final = {1};

    aut.delta.add(0, 'a', 0);
    aut.delta.add(0, 'b', 1);
    aut.delta.add(1, 'a', 0);
    aut.delta.add(1, 'b', 1);

    return aut;
}

nfa_ops::Nfa words_starting_with_a_and_ending_with_b() {
    // Accepts a(a|b)*b.
    nfa_ops::Nfa aut{4};
    aut.initial = {0};
    aut.final = {2};

    aut.delta.add(0, 'a', 1);

    aut.delta.add(1, 'a', 1);
    aut.delta.add(1, 'b', 2);

    aut.delta.add(2, 'a', 1);
    aut.delta.add(2, 'b', 2);

    return aut;
}

} // namespace

int main() {
    nfa_lazy::SymbolicFormula tree;

    const nfa_lazy::Term l_a = tree.make_term(words_starting_with_a());
    const nfa_lazy::Term l_b = tree.make_term(words_ending_with_b());
    const nfa_lazy::Term l_ab = tree.make_term(words_starting_with_a_and_ending_with_b());

    // Lazy inclusion is reduced to lazy emptiness:
    //   L1 subseteq L2
    // iff
    //   L1 \ L2 = L1 ∩ complement(L2)
    // is empty.

    // Check l_ab ⊆ l_a via emptiness of l_ab ∩ complement(l_a).
    const nfa_lazy::Term witness_ab_not_a = tree.intersect(l_ab, tree.complement(l_a));
    const bool ab_subset_a = tree.is_empty(witness_ab_not_a);

    // Check l_a ⊆ l_ab via emptiness of l_a ∩ complement(l_ab).
    const nfa_lazy::Term witness_a_not_ab = tree.intersect(l_a, tree.complement(l_ab));
    const bool a_subset_ab = tree.is_empty(witness_a_not_ab);

    // Check l_ab ⊆ l_b via emptiness of l_ab ∩ complement(l_b).
    const nfa_lazy::Term witness_ab_not_b = tree.intersect(l_ab, tree.complement(l_b));
    const bool ab_subset_b = tree.is_empty(witness_ab_not_b);

    assert(tree.is_valid(witness_ab_not_a));

    std::cout << std::boolalpha;
    std::cout << "a(a|b)*b is included in a(a|b)*: " << ab_subset_a << std::endl;
    std::cout << "a(a|b)* is included in a(a|b)*b: " << a_subset_ab << std::endl;
    std::cout << "a(a|b)*b is included in (a|b)*b: " << ab_subset_b << std::endl;

    return 0;
}
