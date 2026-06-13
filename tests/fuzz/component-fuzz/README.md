# Component Model binary-parser fuzzing

A libFuzzer target for the WebAssembly [Component Model](https://github.com/WebAssembly/component-model)
binary parser (`wasm_component_parse_sections`).

The existing `tests/fuzz/wasm-mutator-fuzz` target only loads **core**
wasm modules; it never reaches the component parser. The component
parser, however, decodes a fully untrusted binary across all 13
section types and drives a large number of LEB-count-controlled
allocations and pointer walks — exactly the surface a fuzzer should
cover. This target closes that gap.

It is intentionally **LLVM-free**: it builds an interpreter-only
runtime with the component model enabled and the WASI Preview 2 host
layer disabled (`WAMR_BUILD_LIBC_WASI=0`), so only the parser,
validator and WAVE helpers are exercised. Unreferenced instantiation /
canonical-ABI / host code is removed by dead-strip; two inert host
stubs (`component-parser/host_resolve_stubs.c`) satisfy the symbols
that `wasm_resolve_imports_WASI` references but the parser never calls.

## Build

Requires Clang (for libFuzzer) and a modern `bison` / `flex` (the WAVE
parser is generated). On macOS:

```sh
brew install llvm bison flex
export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
```

```sh
cd tests/fuzz/component-fuzz
mkdir build && cd build
CC=clang CXX=clang++ cmake ..
cmake --build . -j
```

ASan + UBSan + libFuzzer are enabled by default (skipped automatically
under oss-fuzz, which provides its own instrumentation).

## Run

Seed the corpus from the component fixtures already in the tree, then
fuzz:

```sh
mkdir -p corpus
find ../../../unit -name '*.wasm' \( -path '*component*' -o -path '*canonical*' \) \
  -exec cp {} corpus/ \;

ASAN_OPTIONS=detect_leaks=0 ./component_parser_fuzz corpus/ -close_fd_mask=1
```

`-close_fd_mask=1` suppresses the parser's debug output.

Embedded **core** modules (section 0x01) are handed to the core wasm
loader, which can allocate proportionally to a declared (large)
function count and trip libFuzzer's RSS limit. Those OOMs are core-loader
behaviour, not component-parser defects (the `wasm-mutator-fuzz` target
already covers the core loader); pass `-ignore_ooms=1 -rss_limit_mb=3000`
to keep hunting component-parser bugs past them.

## ILP32 / 32-bit `size_t`

Several parser allocations are of the form
`wasm_runtime_malloc(sizeof(T) * count)` with a 32-bit `count` decoded
from the input. On a 64-bit host the multiply cannot overflow (`size_t`
is 64-bit) and a huge `count` simply fails the allocation. On a 32-bit
target (`arm64_32-apple-watchos`, `i686`, ESP32, …) `size_t` is 32-bit
and the multiply can wrap, under-allocating before the populate loop
runs. The count-bounds checks this work adds (a count cannot exceed the
remaining input) defend both cases. To also *fuzz* the ILP32 path,
point `CC`/`CXX` at a 32-bit toolchain (e.g. `clang -m32` on Linux) and
rebuild — the harness is target-independent.

## Findings

The first runs of this target found four distinct memory-safety bugs in
the parser's partial-parse / error-path cleanup, all fixed in the same
change set:

1. **Double-free** in `parse_component_decl_export`: `export_name` was
   freed locally on the `extern_desc` error paths after ownership had
   already transferred to `*out`, so the centralized cleanup freed it
   again.
2. **Use-after-free** in the instances-section inline-export parse: a
   `WASMComponentCoreName` was pre-allocated and passed to
   `parse_core_name` (which allocates its own and only writes `*out` on
   success); on failure the uninitialized pre-allocation's `->name`
   field was dereferenced by `free_core_name` (and the struct leaked on
   success).
3. **Uninitialized count-arrays** in the type-section parsers
   (`parse_func_type` params, `parse_component_instance_type` decls,
   `parse_component_type` decls): the array's element count was set to
   the full declared count before population, but the array was not
   zeroed, so a parse failure partway left an uninitialized tail that
   the cleanup walk dereferenced (`free_*` reading a wild `tag` /
   `label` pointer at arbitrary nesting depth).
4. Each of those allocation sites also lacked a bound on the
   LEB-decoded count (the ILP32 overflow class above).
