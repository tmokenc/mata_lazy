/**
 * @file state_types.hh
 * @brief Shared private state and hashing helpers for mata_lazy::detail.
 */

#pragma once

#include "mata_lazy.hh"
#include "mata/utils/ord-vector.hh"

#include <algorithm>
#include <cstdint>
#include <new>
#include <stdexcept>
#include <string>

namespace mata_lazy::detail {


/**
 * @brief SmallVector implementation optimized that inline element in the stack for up to `N` elements.
 *
 * This is a simplified version of LLVM's `SmallVector` (https://llvm.org/docs/doxygen/classllvm_1_1SmallVector.html)
 *
 * @tparam T Element type.
 * @tparam N Inline storage capacity (must be > 0).
 *
 * TODO: should move this to mata::utils instead
 * I place it here just to test the performance impact during development,
 * but it can be generally useful and is not specific to the lazy engine.
 */
template<typename T, size_t N>
class SmallVector {
    static_assert(N > 0);

    using Storage = std::aligned_storage_t<sizeof(T), alignof(T)>;

    Storage inline_buf[N];
    T* data = reinterpret_cast<T*>(inline_buf);
    size_t sz = 0;
    size_t cap = N;

    bool using_inline() const noexcept { return data == reinterpret_cast<const T*>(inline_buf); }

    T* inline_data() noexcept { return std::launder(reinterpret_cast<T*>(inline_buf)); }

    void destroy_range(T* ptr, size_t count) noexcept {
        for (size_t i = 0; i < count; ++i)
            ptr[i].~T();
    }

    void grow(size_t min_cap) {
        size_t new_cap = std::max(cap * 2, min_cap);
        T* new_data = static_cast<T*>(::operator new(sizeof(T) * new_cap));

        for (size_t i = 0; i < sz; ++i)
            ::new (new_data + i) T(std::move_if_noexcept(data[i]));

        destroy_range(data, sz);

        if (!using_inline())
            ::operator delete(data);

        data = new_data;
        cap = new_cap;
    }

public:
    using value_type = T;
    using iterator = T*;
    using const_iterator = const T*;

    SmallVector() noexcept = default;

    SmallVector(size_t n) { resize(n); }

    SmallVector(size_t n, const T& val) { resize(n, val); }

    SmallVector(std::initializer_list<T> il) {
        reserve(il.size());
        for (const auto& v : il)
            emplace_back(v);
    }

    SmallVector(const SmallVector& o) {
        reserve(o.sz);
        for (const auto& v : o)
            emplace_back(v);
    }

    SmallVector(SmallVector&& o) noexcept {
        if (o.using_inline()) {
            data = inline_data();
            cap = N;
            for (size_t i = 0; i < o.sz; ++i)
                emplace_back(std::move(o.data[i]));
            o.clear();
        } else {
            data = o.data;
            sz = o.sz;
            cap = o.cap;

            o.data = o.inline_data();
            o.sz = 0;
            o.cap = N;
        }
    }

    ~SmallVector() {
        destroy_range(data, sz);
        if (!using_inline())
            ::operator delete(data);
    }

    // ===== assignment =====
    SmallVector& operator=(const SmallVector& o) {
        if (this == &o)
            return *this;
        clear();
        reserve(o.sz);
        for (const auto& v : o)
            emplace_back(v);
        return *this;
    }

    SmallVector& operator=(SmallVector&& o) noexcept {
        if (this == &o)
            return *this;

        clear();
        if (!using_inline())
            ::operator delete(data);

        if (o.using_inline()) {
            data = inline_data();
            cap = N;
            for (size_t i = 0; i < o.sz; ++i)
                emplace_back(std::move(o.data[i]));
            o.clear();
        } else {
            data = o.data;
            sz = o.sz;
            cap = o.cap;

            o.data = o.inline_data();
            o.sz = 0;
            o.cap = N;
        }
        return *this;
    }

    SmallVector& operator=(const std::vector<T>& v) {
        clear();
        reserve(v.size());
        for (const auto& x : v)
            emplace_back(x);
        return *this;
    }

    void swap(SmallVector& o) noexcept(std::is_nothrow_move_constructible_v<T>) {
        if (this == &o)
            return;

        if (!using_inline() && !o.using_inline()) {
            std::swap(data, o.data);
            std::swap(sz, o.sz);
            std::swap(cap, o.cap);
            return;
        }

        SmallVector tmp(std::move(o));
        o = std::move(*this);
        *this = std::move(tmp);
    }

    size_t size() const noexcept { return sz; }
    size_t capacity() const noexcept { return cap; }
    bool empty() const noexcept { return sz == 0; }

    void reserve(size_t n) {
        if (n > cap)
            grow(n);
    }

    T& operator[](size_t i) noexcept {
        assert(i < sz);
        return data[i];
    }

    const T& operator[](size_t i) const noexcept {
        assert(i < sz);
        return data[i];
    }

    T& back() noexcept { return data[sz - 1]; }
    const T& back() const noexcept { return data[sz - 1]; }

    // ===== modifiers =====
    template<typename... Args>
    T& emplace_back(Args&&... args) {
        if (sz == cap)
            grow(sz + 1);

        ::new (data + sz) T(std::forward<Args>(args)...);
        return data[sz++];
    }

    void push_back(const T& v) { emplace_back(v); }
    void push_back(T&& v) { emplace_back(std::move(v)); }

    void pop_back() {
        assert(sz > 0);
        data[--sz].~T();
    }

    void clear() noexcept {
        destroy_range(data, sz);
        sz = 0;
    }

    // ===== iterators =====
    iterator begin() noexcept { return data; }
    const_iterator begin() const noexcept { return data; }
    const_iterator cbegin() const noexcept { return data; }

    iterator end() noexcept { return data + sz; }
    const_iterator end() const noexcept { return data + sz; }
    const_iterator cend() const noexcept { return data + sz; }

    // ===== vector-like API =====
    void resize(size_t n) {
        static_assert(std::is_default_constructible_v<T>, "resize(n) requires default-constructible T");

        if (n < sz) {
            while (sz > n)
                pop_back();
        } else {
            reserve(n);
            while (sz < n)
                emplace_back();
        }
    }

    void resize(size_t n, const T& val) {
        if (n < sz) {
            resize(n);
        } else {
            reserve(n);
            while (sz < n)
                emplace_back(val);
        }
    }

    void assign(size_t n, const T& val) {
        clear();
        reserve(n);
        for (size_t i = 0; i < n; ++i)
            emplace_back(val);
    }

    template<typename It>
    void assign(It first, It last) {
        clear();
        for (; first != last; ++first)
            emplace_back(*first);
    }

    // ===== comparisons =====
    bool operator==(const SmallVector& o) const { return sz == o.sz && std::equal(begin(), end(), o.begin()); }

    bool operator!=(const SmallVector& o) const { return !(*this == o); }

    bool operator<(const SmallVector& o) const {
        return std::lexicographical_compare(begin(), end(), o.begin(), o.end());
    }
};

/// Convenience alias for the common arity-2 inline variant.
template<typename T>
using SmallVec2 = SmallVector<T, 2>;

/// DFS visitation state used during lazy DAG traversals.
enum class VisitState : uint8_t {
    Unseen = 0,
    Active = 1,
    Done = 2,
};

/// Throw a uniform diagnostic for a NodeKind that should never reach a given switch.
[[noreturn]] inline void unreachable_kind(const NodeKind kind, const char* context) {
    throw std::logic_error(
            std::string("Unreachable ") + context + " for NodeKind " + std::to_string(static_cast<int>(kind)));
}

/**
 * @brief Reconstructed-DAG operator kind.
 *
 * Mirrors the public @c NodeKind plus @c DiagonalSlice, which is materialised
 * only by the reconstruction pass and never appears in user-built DAGs. Keeping
 * a separate enum lets the public @c NodeKind stay limited to operations a user
 * can actually construct.
 */
enum class ExecKind : uint8_t {
    LeafNfa,
    LeafNft,
    Union,
    Intersect,
    Complement,
    Identity,
    Project,
    SyncProduct,
    DiagonalSlice,
};

/// Map a public @c NodeKind to its identical @c ExecKind value.
constexpr ExecKind to_exec_kind(const NodeKind kind) noexcept {
    return static_cast<ExecKind>(kind);
}

/// Throw a uniform diagnostic for an @c ExecKind that should never reach a given switch.
[[noreturn]] inline void unreachable_kind(const ExecKind kind, const char* context) {
    throw std::logic_error(
            std::string("Unreachable ") + context + " for ExecKind " + std::to_string(static_cast<int>(kind)));
}

/**
 * @brief Compact reconstructed execution node.
 *
 * The plan owned by @c Project / @c SyncProduct exec nodes is reached through
 * the Context's parallel @c plan_at_node side vector, dispatched by @c kind.
 * No payload field lives on the node itself.
 */
struct ExecNode {
    /// Operator kind in the reconstructed DAG.
    ExecKind kind;
    /// Result arity of the reconstructed node.
    uint8_t result_arity;
    /// Left child or leaf index.
    NodeId lhs;
    /// Right child when present.
    NodeId rhs;
};

/**
 * @brief Internal sync-plan form with precomputed per-level peer lookup.
 *
 * Lives here (rather than in iterators.hh) so alphabet_store and other internal
 * modules can read sync-plan fields through @c plan_at_node without dragging
 * the iterator headers in.
 */
struct CompiledSyncPlan {
    SmallVec2<uint8_t> lhs_sync_levels{};
    SmallVec2<uint8_t> rhs_sync_levels{};
    SmallVec2<LevelRef> result_layout{};
    SmallVec2<int16_t> lhs_sync_peer_by_level{};
    SmallVec2<int16_t> rhs_sync_peer_by_level{};
};

/// Sentinel returned by @c compute_user_node_plan_indices for nodes that own no plan.
inline constexpr uint32_t kNoPlanIndex = static_cast<uint32_t>(-1);

/**
 * @brief Build the positional plan index for every node in @p formula.
 *
 * Walks @c formula.nodes in id order, assigning each @c Project node the next
 * @c project_plans index and each @c SyncProduct node the next @c sync_plans
 * index. Other nodes get @c kNoPlanIndex. The mapping is positional because
 * builder methods append plans atomically with their owning nodes.
 *
 * Used by reconstruction and validation; built lazily and discarded after the
 * relevant walk.
 *
 * @param formula User-built symbolic formula.
 * @return Vector of size @c formula.nodes.size() of plan indices or @c kNoPlanIndex.
 */
inline std::vector<uint32_t> compute_user_node_plan_indices(const SymbolicFormula& formula) {
    std::vector<uint32_t> result(formula.nodes.size(), kNoPlanIndex);
    uint32_t project_count = 0;
    uint32_t sync_count = 0;
    for (size_t i = 0; i < formula.nodes.size(); ++i) {
        switch (formula.nodes[i].kind) {
            case NodeKind::Project:
                result[i] = project_count++;
                break;
            case NodeKind::SyncProduct:
                result[i] = sync_count++;
                break;
            default:
                break;
        }
    }
    return result;
}

/**
 * @brief Mix one integer into a stable 64-bit hash state.
 *
 * This follows the SplitMix64-style mixing step: `0x9e3779b97f4a7c15` is the
 * `GOLDEN_GAMMA` increment from Steele, Lea, and Flood's "Fast Splittable
 * Pseudorandom Number Generators" (OOPSLA 2014), while
 * `0xbf58476d1ce4e5b9` and `0x94d049bb133111eb` are the constants from David
 * Stafford's "variant 13" 64-bit mixer, which uses for bit diffusion.
 *
 * @param value Input integer value.
 * @return Mixed 64-bit hash value.
 */
constexpr uint64_t mix_hash64(uint64_t value) noexcept {
    value += 0x9e3779b97f4a7c15ULL;
    value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
    value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
    return value ^ (value >> 31);
}

/**
 * @brief Fold a 64-bit hash down to the 32-bit ids used by the macrostate stores.
 * @param value 64-bit hash value.
 * @return Folded 32-bit hash value.
 */
constexpr uint32_t fold_hash64(uint64_t value) noexcept { return static_cast<uint32_t>(value ^ (value >> 32)); }

/**
 * @brief Pair macrostate stored for binary product-like nodes.
 */
struct PairState {
    MacroStateId lhs;
    MacroStateId rhs;
};

/**
 * @brief Tagged macrostate stored for union-like nodes.
 */
struct TaggedState {
    /// Side tag of the union branch represented by the macrostate.
    enum class Tag : uint8_t {
        Left = 0,
        Right = 1,
    };

    MacroStateId state;
    Tag tag;
};

/// Complement subset macrostate kept in sorted canonical order.
using SetState = mata::utils::OrdVector<MacroStateId>;

/**
 * @brief Canonicalize one subset macrostate in-place.
 *
 * The canonical representation is a sorted vector without duplicates.
 *
 * @param states Subset macrostate to canonicalize.
 */
inline void canonicalize_set_state(SetState& states) { states = SetState{states.begin(), states.end()}; }

/**
 * @brief Hash a subset macrostate.
 * @param states Subset macrostate to hash.
 * @return Hash value used for interning.
 */
inline uint32_t hash_states(const SetState& states) {
    uint64_t hash = mix_hash64(states.size());
    for (const MacroStateId& id : states) {
        hash ^= mix_hash64(id);
    }
    return fold_hash64(mix_hash64(hash));
}

/**
 * @brief Hash a binary-product macrostate.
 * @param pair Pair macrostate to hash.
 * @return Hash value used for interning.
 */
constexpr uint32_t hash_pair(const PairState& pair) noexcept {
    const uint64_t packed = (static_cast<uint64_t>(pair.lhs) << 32) | static_cast<uint64_t>(pair.rhs);
    return fold_hash64(mix_hash64(packed));
}

/**
 * @brief Hash a tagged union macrostate.
 * @param tagged Tagged macrostate to hash.
 * @return Hash value used for interning.
 */
constexpr uint32_t hash_tagged(const TaggedState& tagged) noexcept {
    const uint64_t packed =
            (static_cast<uint64_t>(tagged.state) << 8) | static_cast<uint64_t>(static_cast<uint8_t>(tagged.tag));
    return fold_hash64(mix_hash64(packed));
}

} // namespace mata_lazy::detail
