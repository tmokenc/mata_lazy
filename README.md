# mata_lazy

Standalone version of the lazy module (namespace `mata::nft::lazy`) implemented in [this fork of MATA](https://github.com/tmokenc/mata/tree/lazy-nft-generic-arity). Provides lazy on-the-fly emptiness checking for symbolic combinations of NFA and arbitrary-arity NFT relations.

## libmata requirement

mata_lazy needs the per-level-alphabet API (a different alphabet at each NFT
level). That lives on the [`nft-per-level-alphabet`](https://github.com/tmokenc/mata/tree/nft-per-level-alphabet)
branch of `tmokenc/mata` and is **not yet merged into upstream `VeriFIT/mata`**.
If no local mata is provided, the CMake build clones that branch automatically
via `FetchContent`.

## Public API

One header:

```cpp
#include "mata_lazy.hh"
```

Two namespaces:

- `mata::nft::lazy::SymbolicFormula` — arbitrary-arity NFT + NFA leaves
- `mata::nfa::lazy::SymbolicFormula` — arity-1 facade if you only need NFAs

## Build

mata_lazy depends on libmata. Pick one of:

### Against installed libmata

```sh
cd ../mata && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release && \
  cmake --build build -j && sudo cmake --install build
cd ../mata_lazy && cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build
```

### Against a sibling mata source tree

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release \
  -DMATA_LAZY_USE_SUBDIR_MATA=ON -DMATA_LAZY_MATA_SUBDIR=../mata
cmake --build build -j
```

## Bundled artifacts (single-file linking)

`-DMATA_LAZY_BUNDLE=ON` produces two extra files alongside the usual targets:

- `libmata_lazy_bundled.a` — single static archive containing mata_lazy + libmata + CUDD + RE2 + simlib. Link with `-lpthread`.
- `libmata_lazy.so` — shared library with libmata embedded. Link with `-lmata_lazy -lpthread`.

Linux only (uses GNU `ar -M` and `--whole-archive`).

Example:

```sh
g++ -std=c++20 my.cc libmata_lazy_bundled.a -lpthread -o my
# or
g++ -std=c++20 my.cc -L. -lmata_lazy -lpthread -Wl,-rpath,. -o my
```

## Downstream CMake usage

After `cmake --install`:

```cmake
find_package(mata_lazy REQUIRED)
target_link_libraries(my_target PRIVATE mata_lazy)
```
