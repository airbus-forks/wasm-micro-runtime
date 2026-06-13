/*
 * Copyright (C) 2026 Rebecker Specialties. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

/*
 * Inert stubs for the WASI Preview 2 host-resolution symbols.
 *
 * This fuzz target builds the component model with the WASI Preview 2
 * host layer disabled (LIBC_WASI=0) so that only the binary parser /
 * validator / WAVE helpers are exercised. The binary parser never
 * resolves imports — that is an instantiation-time concern in
 * wasm_resolve_imports_WASI() — but that function references the two
 * symbols below and is not always removed by dead-strip (it is reached
 * through a native-registration table the linker cannot prove dead).
 *
 * These stubs let the parser-only target link without pulling in the
 * (Linux-only) WASI Preview 2 wrappers. They are never called on the
 * parse path the fuzzer drives; if import resolution is ever exercised
 * the conservative "unavailable" return keeps the failure clean.
 */

#include <stdbool.h>

bool
wasm_check_wasi_p2_version(const char *required_interface)
{
    (void)required_interface;
    return false;
}

bool
wasm_native_register_wasi_p2_module_func(const char *module_name,
                                         const char *func_name)
{
    (void)module_name;
    (void)func_name;
    return false;
}
