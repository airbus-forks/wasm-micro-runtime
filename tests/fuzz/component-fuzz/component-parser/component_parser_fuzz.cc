/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * libFuzzer target for the WebAssembly Component Model binary parser.
 *
 * The component parser ingests a fully untrusted binary buffer
 * (wasm_component_parse_sections) and decodes all 13 section types,
 * driving a large number of LEB-count-controlled allocations and
 * pointer walks. That is exactly the surface a fuzzer should cover and
 * the existing wasm-mutator fuzz target does not reach it (it only
 * loads core wasm modules).
 *
 * This target is intentionally LLVM-free: it builds an interpreter-only
 * runtime with the component model enabled and the WASI Preview 2 host
 * layer disabled (LIBC_WASI=0), so the only code exercised is the
 * parser + validator + WAVE helpers. Unreferenced instantiation /
 * canonical-ABI / host functions are removed by dead-strip, so no host
 * symbols are required to link.
 *
 * Build + run: see tests/fuzz/component-fuzz/README.md
 */

#include "wasm_export.h"
#include "wasm_component.h"

#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <vector>

static bool
ensure_runtime(void)
{
    /* Initialize the runtime once for the whole campaign. Use the
     * system allocator (not a fixed pool) so AddressSanitizer
     * instruments every component allocation and can see overflows,
     * use-after-free, and leaks in the parser. */
    static bool inited = false;
    if (inited)
        return true;

    RuntimeInitArgs init_args;
    memset(&init_args, 0, sizeof(init_args));
    init_args.mem_alloc_type = Alloc_With_System_Allocator;

    if (!wasm_runtime_full_init(&init_args))
        return false;
    inited = true;
    return true;
}

extern "C" int
LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    /* The 8-byte header is decoded before section parsing; shorter
     * inputs are uninteresting for this target. */
    if (size < 8 || size > UINT32_MAX)
        return 0;

    if (!ensure_runtime())
        return 0;

    /* The parser delegates embedded core modules (section 0x01) to the
     * core wasm loader, which mutates its input buffer in place. Work
     * on a private copy so we never write through libFuzzer's const
     * input and so repeated runs over the same input are deterministic. */
    std::vector<uint8_t> buf(data, data + size);

    WASMHeader header;
    if (!wasm_decode_header(buf.data(), (uint32_t)buf.size(), &header))
        return 0;

    /* Only feed actual components to the component parser; core modules
     * are covered by the existing wasm-mutator target. */
    if (!is_wasm_component(header))
        return 0;

    WASMComponent *component =
        (WASMComponent *)wasm_runtime_malloc(sizeof(WASMComponent));
    if (!component)
        return 0;
    memset(component, 0, sizeof(WASMComponent));

    /* LoadArgs.name must be a non-NULL, mutable buffer (the embedded
     * core-module loader copies from it). */
    char name_buf[] = "component-fuzz";
    LoadArgs load_args;
    memset(&load_args, 0, sizeof(load_args));
    load_args.name = name_buf;
    load_args.is_component = true;

    /* On both success and failure wasm_component_free must leave the
     * parsed-section state clean; the top-level struct is owned by us. */
    wasm_component_parse_sections(buf.data(), (uint32_t)buf.size(),
                                  component, &load_args, 0);
    wasm_component_free(component);
    wasm_runtime_free(component);

    return 0;
}
