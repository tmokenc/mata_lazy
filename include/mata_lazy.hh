/** @file mata_lazy.hh
 * @brief Lazy on-the-fly emptiness checking for symbolic combinations of NFA / NFT relations.
 *
 * Public entry points (both in @c mata_lazy):
 *
 *   - @c SymbolicFormula — a directed acyclic graph (DAG) of symbolic
 *     operators (@c unite, @c intersect, @c complement, @c compose,
 *     @c post_image, …) over NFA and NFT leaves of arbitrary arity.
 *     Leaves are inserted with @c make_term, which is overloaded for both
 *     @c mata::nfa::Nfa and @c mata::nft::Nft.
 *   - @c Term — opaque handle to a node in the DAG.
 *
 * Evaluation is entirely lazy: building the DAG performs no automata operations.
 * Work begins only when @c SymbolicFormula::is_empty is called, and only the
 * macrostates actually needed to decide emptiness are ever materialised.
 */

#ifndef MATA_LAZY_HH
#define MATA_LAZY_HH

#include <cstdint>
#include <vector>

#include "mata/nfa/nfa.hh"
#include "mata/nft/nft.hh"

namespace mata_lazy {

// Pull in the mata sub-namespaces under short, unqualified names so the rest
// of this header (and the .cc files) can keep using `nfa::Nfa`, `nft::Nft`,
// and `OnTheFlyAlphabet` without the `mata::` prefix.
namespace nfa = mata::nfa;
namespace nft = mata::nft;
using mata::OnTheFlyAlphabet;

/// Identifier of a node in the symbolic formula DAG.
using NodeId = uint32_t;
/// Identifier of a lazily constructed macrostate.
using MacroStateId = uint32_t;


/// Opaque handle to a node in the symbolic formula DAG.
class Term {
    NodeId id;

public:
    /// Create a default-initialized handle.
    Term() : id{0} {}
    /// Wrap an existing node identifier.
    Term(NodeId id) : id{id} {}
    /// Return the underlying node identifier.
    NodeId get_id() const { return id; }
};

/// Kind of symbolic operator represented by a tree node.
enum class NodeKind : uint8_t {
    LeafNfa,
    LeafNft,
    Union,
    Intersect,
    Complement,
    Identity,
    Project,
    SyncProduct,
};

/// Reference one level of one side of a synchronized product.
struct LevelRef {
    /// Side of a synchronized product.
    enum class Side : uint8_t {
        Lhs = 0,
        Rhs = 1,
    };

    /// Selected side.
    Side side;
    /// Selected level on that side.
    uint8_t level;
};

/// Metadata for a synchronized product node.
struct SyncPlan {
    /// Levels on the left side that participate in synchronization.
    std::vector<uint8_t> lhs_sync_levels;
    /// Levels on the right side that participate in synchronization.
    std::vector<uint8_t> rhs_sync_levels;
    /// Output layout assembled from the participating children.
    std::vector<LevelRef> result_layout;
};

/// Metadata for a projection node.
struct ProjectPlan {
    /// Child levels kept in the result, in the requested order.
    std::vector<uint8_t> kept_levels;
};

/// Compact record for one symbolic relation operator.
///
/// Plan-bearing kinds (`Project`, `SyncProduct`) are associated with their plan
/// positionally: the k-th `Project` node (in build order) owns
/// `project_plans[k]`, and analogously for `SyncProduct`. Builder methods
/// maintain this invariant by appending the plan and the node atomically.
struct Node {
    /// Operator kind.
    NodeKind kind;
    /// Arity of the relation denoted by this node.
    uint8_t result_arity;
    /// Left child or leaf index, depending on the node kind.
    NodeId lhs;
    /// Right child when present.
    NodeId rhs;
};

/// DAG of symbolic relation operators evaluated lazily for emptiness checking.
class SymbolicFormula {
    NodeId insert_leaf(NodeKind kind, NodeId leaf_id, uint8_t result_arity);
    NodeId insert_unary(NodeKind kind, NodeId child, uint8_t result_arity);
    NodeId insert_binary(NodeKind kind, NodeId lhs, NodeId rhs, uint8_t result_arity);

public:
    /// Owned NFA leaves referenced by leaf nodes.
    std::vector<nfa::Nfa> nfas;
    /// Owned NFT leaves referenced by leaf nodes.
    std::vector<nft::Nft> nfts;
    /// DAG nodes, one per symbolic operator or leaf.
    std::vector<Node> nodes;
    /// Synchronization plans used by sync-product nodes.
    std::vector<SyncPlan> sync_plans;
    /// Projection plans used by project nodes.
    std::vector<ProjectPlan> project_plans;

    SymbolicFormula() : nfas{}, nfts{}, nodes{}, sync_plans{}, project_plans{} {}

    /**
     * Insert a concrete NFA leaf.
     * @param nfa Language automaton to store as an arity-1 leaf.
     * @return Handle to the inserted symbolic term.
     */
    Term make_term(const nfa::Nfa& nfa);
    /**
     * Insert a concrete NFT leaf.
     * @param nft Relation automaton to store as a leaf of arity `nft.levels.num_of_levels`.
     * @return Handle to the inserted symbolic term.
     */
    Term make_term(const nft::Nft& nft);

    /**
     * Union of two equal-arity relations.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @return Symbolic term for `lhs ∪ rhs`.
     *
     * @note the union is a keyword in C++, so the method is named `unite` instead of `union`.
     */
    Term unite(const Term& lhs, const Term& rhs);
    /**
     * Intersection of two equal-arity relations.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @return Symbolic term for `lhs ∩ rhs`.
     */
    Term intersect(const Term& lhs, const Term& rhs);
    /**
     * Complement of a n-ary relation.
     * @param sub Operand to complement.
     * @return Symbolic term for the complement of @p sub.
     */
    Term complement(const Term& sub);
    /**
     * Existentially drop levels not listed in @p kept_levels.
     * @param sub Input relation.
     * @param kept_levels Levels preserved in the result, in the requested order.
     * @return Symbolic term representing the projection of @p sub.
     */
    Term project(const Term& sub, const std::vector<uint8_t>& kept_levels);
    /**
     * Build a synchronized product with an explicit output layout.
     * @param lhs Left operand.
     * @param rhs Right operand.
     * @param lhs_sync_levels Levels of @p lhs that must synchronize with @p rhs.
     * @param rhs_sync_levels Levels of @p rhs matched position-wise with @p lhs_sync_levels.
     * @param result_layout Output levels assembled from the two operands after synchronization.
     * @return Symbolic term for the requested synchronized product.
     */
    Term sync_product(
            const Term& lhs, const Term& rhs, const std::vector<uint8_t>& lhs_sync_levels,
            const std::vector<uint8_t>& rhs_sync_levels, const std::vector<LevelRef>& result_layout);

    /**
     * Diagonal embedding of an arity-1 language into an arity-2 relation.
     * @param sub Arity-1 language term.
     * @return Symbolic relation `{ (w, w) | w in sub }`.
     */
    Term identity(const Term& sub);

    /**
     * @name Common Derived Operations
     * Convenience wrappers over the core symbolic layer.
     *
     * - compose(R, S) is `sync_product(R, S, ...)` with the standard output layout
     * - post_image(L, R) is a fixed 2-tape `sync_product(L, R, {0}, {0}, {rhs:1})`
     * - pre_image(L, R) is a fixed 2-tape `sync_product(L, R, {0}, {1}, {rhs:0})`
     */
    ///@{

    /**
     * Relation composition with an explicit synchronization layout.
     * @param lhs Left relation.
     * @param rhs Right relation.
     * @param lhs_sync_levels Levels of @p lhs consumed by the composition join.
     * @param rhs_sync_levels Levels of @p rhs consumed by the composition join.
     * @return Composed relation over the unsynchronized levels of both operands.
     */
    Term
    compose(const Term& lhs, const Term& rhs, const std::vector<uint8_t>& lhs_sync_levels,
            const std::vector<uint8_t>& rhs_sync_levels);

    /**
     * Standard 2-tape composition on output tape 1 and input tape 0.
     * @param lhs Left 2-tape relation.
     * @param rhs Right 2-tape relation.
     * @return Standard composition `lhs ∘ rhs`.
     */
    Term compose(const Term& lhs, const Term& rhs);

    /**
     * Post-image of an arity-1 language through a 2-tape transducer.
     * @param lang_over_input Language synchronized with tape 0 of @p transducer.
     * @param transducer Two-tape relation interpreted as input/output.
     * @return Arity-1 language over tape 1 reachable from @p lang_over_input.
     */
    Term post_image(const Term& lang_over_input, const Term& transducer);
    /**
     * Pre-image of an arity-1 language through a 2-tape transducer.
     * @param lang_over_output Language synchronized with tape 1 of @p transducer.
     * @param transducer Two-tape relation interpreted as input/output.
     * @return Arity-1 language over tape 0 that can reach @p lang_over_output.
     */
    Term pre_image(const Term& lang_over_output, const Term& transducer);
    ///@}

    /**
     * Return the arity of the relation denoted by @p term.
     * @param term Symbolic term to inspect.
     * @return Number of levels/tapes carried by @p term.
     */
    uint8_t arity_of(const Term& term) const;

    /**
     * Check that @p root_node refers to a valid node in this DAG.
     * @param root_node Symbolic term handle to validate.
     * @return `true` if the handle refers to a structurally valid reachable node.
     */
    bool is_valid(const Term& root_node) const;

    /**
     * Check emptiness using alphabets inferred from the leaves.
     * @param root_node Root of the symbolic relation to test.
     * @return `true` if the denoted relation is empty.
     */
    bool is_empty(const Term& root_node);

    /**
     * Check emptiness using one shared alphabet for every level.
     * @param root_node Root of the symbolic relation to test.
     * @param alphabet Alphabet reused on every level of the root relation.
     * @return `true` if the denoted relation is empty.
     */
    bool is_empty(const Term& root_node, const OnTheFlyAlphabet& alphabet);

    /**
     * Check emptiness using an explicit alphabet for each level.
     * @param root_node Root of the symbolic relation to test.
     * @param level_alphabets One alphabet per level of the root relation.
     * @return `true` if the denoted relation is empty.
     */
    bool is_empty(const Term& root_node, const std::vector<OnTheFlyAlphabet>& level_alphabets);
};


} // namespace mata_lazy

#endif // MATA_LAZY_HH
