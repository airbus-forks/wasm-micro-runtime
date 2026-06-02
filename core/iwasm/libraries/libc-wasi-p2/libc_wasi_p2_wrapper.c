/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#include "libc_wasi_p2_wrapper.h"
#include "wasm_native.h"
#include "wasi_p2_random_wrapper.h"
#include "wasm_export.h"
#include <errno.h>

static bool
is_module_equal(const char *module_name, const char *registered_module_name)
{
    const char *at_module = strchr(module_name, '@');
    size_t module_len =
        at_module ? (size_t)(at_module - module_name) : strlen(module_name);

    const char *at_registered = strchr(registered_module_name, '@');
    size_t registered_len =
        at_registered ? (size_t)(at_registered - registered_module_name)
                      : strlen(registered_module_name);

    if (module_len != registered_len) {
        return false;
    }

    return strncmp(module_name, registered_module_name, module_len) == 0;
}

/**
 * @brief A comparison function for qsort to sort native symbols by name.
 * @param native_symbol1 The first native symbol.
 * @param native_symbol2 The second native symbol.
 * @return An integer less than, equal to, or greater than zero if the first
 *         symbol is found, respectively, to be less than, to match, or be
 *         greater than the second.
 */
static int
native_symbol_cmp(const void *native_symbol1, const void *native_symbol2)
{
    return strcmp(((const NativeSymbol *)native_symbol1)->symbol,
                  ((const NativeSymbol *)native_symbol2)->symbol);
}

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief A macro to define a native symbol for a WASI P2 function.
 * @details This macro creates a NativeSymbol struct with the given name, a
 *          pointer to the wrapper function, and the function signature.
 * @param name The name of the WASI function.
 * @param func The name of the wrapper function.
 * @param sig The function signature.
 */
#define REG_WASI_P2_FUNCTION(name, func, sig)   \
    {                                           \
        name, (void *)func##_wrapper, sig, NULL \
    }

/**
 * @brief A macro to define a WASI P2 module.
 * @details This macro creates a wasi_p2_module_t struct with the module name,
 *          a pointer to the native symbols array, and the number of symbols.
 * @param name The name of the module.
 * @param name_str The string representation of the module name.
 */
#define WASI_P2_MODULE(name, name_str, ver)                    \
    {                                                          \
        "wasi:" name_str, ver, (NativeSymbol *)name##_symbols, \
            sizeof(name##_symbols) / sizeof(NativeSymbol)      \
    }

static NativeSymbol random_random_symbols[] = {
    REG_WASI_P2_FUNCTION("get-random-bytes", wasi_random_get_random_bytes,
                         "(Ii)"),
    REG_WASI_P2_FUNCTION("get-random-u64", wasi_random_get_random_u64, "()I"),
};

static NativeSymbol random_insecure_symbols[] = {
    REG_WASI_P2_FUNCTION("get-insecure-random-bytes",
                         wasi_random_get_insecure_random_bytes, "(Ii)"),
    REG_WASI_P2_FUNCTION("get-insecure-random-u64",
                         wasi_random_get_insecure_random_u64, "()I"),
};

static NativeSymbol random_insecure_seed_symbols[] = {
    REG_WASI_P2_FUNCTION("insecure-seed", wasi_random_insecure_seed, "(i)"),
};

static wasi_p2_module_t wasi_p2_modules[] = {
    WASI_P2_MODULE(random_random, "random/random", "0.2.0"),
    WASI_P2_MODULE(random_insecure, "random/insecure", "0.2.0"),
    WASI_P2_MODULE(random_insecure_seed, "random/insecure-seed", "0.2.0"),
};

static bool
convert_version(int *value, const char version_string[])
{
    errno = 0;
    char *endptr = NULL;
    *value = (int)strtol(version_string, &endptr, 10);
    if (version_string == endptr || errno == ERANGE || *endptr != '\0')
        return false;

    return true;
}

bool
wasm_check_wasi_p2_version(const char *required_interface)
{
    const char *at = strchr(required_interface, '@');
    if (!at)
        return true; // no version requirement, always ok

    const char *required_ver = at + 1;
    wasi_p2_module_t *modules = NULL;
    uint32_t count = get_libc_wasi_p2_export_apis(&modules);
    for (uint32_t i = 0; i < count; i++) {
        if (is_module_equal(modules[i].module_name, required_interface)) {
            int req_maj = 0, req_min = 0, req_pat = 0, run_maj = 0, run_min = 0,
                run_pat = 0;
            char req_maj_str[20] = { 0 }, req_min_str[20] = { 0 },
                 req_pat_str[20] = { 0 }, run_maj_str[20] = { 0 },
                 run_min_str[20] = { 0 }, run_pat_str[20] = { 0 };

            if (sscanf(required_ver, "%19[^.].%19[^.].%19[^.]", req_maj_str,
                       req_min_str, req_pat_str)
                    != 3
                || sscanf(modules[i].version, "%19[^.].%19[^.].%19[^.]",
                          run_maj_str, run_min_str, run_pat_str)
                       != 3) {
                return false;
            }

            if (!convert_version(&req_maj, req_maj_str)
                || !convert_version(&req_min, req_min_str)
                || !convert_version(&req_pat, req_pat_str)
                || !convert_version(&run_maj, run_maj_str)
                || !convert_version(&run_min, run_min_str)
                || !convert_version(&run_pat, run_pat_str)) {
                return false;
            }

            // Hard fail: major or minor mismatch = incompatible API
            if (req_maj != run_maj || req_min != run_min || req_pat < run_pat) {
                LOG_ERROR("Incompatible WASI version for %s: "
                          "required %d.%d.%d, runtime implements %d.%d.%d",
                          required_interface, req_maj, req_min, req_pat,
                          run_maj, run_min, run_pat);
                return false;
            }
            return true;
        }
    }
    return true;
}

/**
 * @brief Get the exported APIs for the WASI P2 modules.
 * @details This function returns a pointer to the array of WASI P2 modules
 *          and the number of modules in the array. It also sorts the native
 *          symbols in each module by name on the first call.
 * @param p_libc_wasi_p2_apis A pointer to a pointer to a wasi_p2_module_t
 *                            struct. This will be updated to point to the
 *                            array of WASI P2 modules.
 * @return The number of WASI P2 modules.
 */
uint32_t
get_libc_wasi_p2_export_apis(wasi_p2_module_t **p_libc_wasi_p2_apis)
{
    static bool wasi_p2_native_symbols_sorted = false;
    if (!wasi_p2_native_symbols_sorted) {
        for (uint32_t i = 0;
             i < sizeof(wasi_p2_modules) / sizeof(wasi_p2_module_t); i++) {
            qsort((void *)wasi_p2_modules[i].symbols,
                  wasi_p2_modules[i].symbol_count, sizeof(NativeSymbol),
                  native_symbol_cmp);
        }
        wasi_p2_native_symbols_sorted = true;
    }

    *p_libc_wasi_p2_apis = wasi_p2_modules;
    return sizeof(wasi_p2_modules) / sizeof(wasi_p2_module_t);
}

/**
 * @brief Register all WASI P2 modules.
 * @return True if successful, false otherwise.
 */
bool
wasm_native_register_wasi_p2_modules()
{
    wasi_p2_module_t *modules = NULL;
    uint32_t wasi_p2_module_count = get_libc_wasi_p2_export_apis(&modules);

    for (uint32_t i = 0; i < wasi_p2_module_count; i++) {
        if (!wasm_native_register_natives(modules[i].module_name,
                                          (NativeSymbol *)modules[i].symbols,
                                          modules[i].symbol_count)) {
            return false;
        }
    }
    return true;
}

/**
 * @brief Unregister all WASI P2 modules.
 */
void
wasm_native_unregister_wasi_p2_modules()
{
    wasi_p2_module_t *modules = NULL;
    uint32_t wasi_p2_module_count = get_libc_wasi_p2_export_apis(&modules);

    for (uint32_t i = 0; i < wasi_p2_module_count; i++) {
        wasm_native_unregister_natives(modules[i].module_name,
                                       (NativeSymbol *)modules[i].symbols);
    }
}

/**
 * @brief Register a single WASI P2 module.
 * @param module_name The name of the module to register.
 * @return True if successful, false otherwise.
 */
bool
wasm_native_register_wasi_p2_module(const char *module_name)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t wasi_p2_module_count = get_libc_wasi_p2_export_apis(&modules);

    for (uint32_t i = 0; i < wasi_p2_module_count; i++) {
        if (is_module_equal(modules[i].module_name, module_name)) {
            return wasm_native_register_natives(
                modules[i].module_name, (NativeSymbol *)modules[i].symbols,
                modules[i].symbol_count);
        }
    }

    return false;
}

/**
 * @brief Unregister a single WASI P2 module.
 * @param module_name The name of the module to unregister.
 */
void
wasm_native_unregister_wasi_p2_module(const char *module_name)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t wasi_p2_module_count = get_libc_wasi_p2_export_apis(&modules);

    for (uint32_t i = 0; i < wasi_p2_module_count; i++) {
        if (is_module_equal(modules[i].module_name, module_name)) {
            wasm_native_unregister_natives(modules[i].module_name,
                                           (NativeSymbol *)modules[i].symbols);
            return;
        }
    }
}

/**
 * @brief Find a function in a WASI P2 module.
 * @param module_name The name of the module to search.
 * @param func_name The name of the function to find.
 * @return A pointer to the native symbol if found, otherwise NULL.
 */
static const NativeSymbol *
find_wasi_p2_module_func(const char *module_name, const char *func_name)
{
    wasi_p2_module_t *modules = NULL;
    uint32_t wasi_p2_module_count = get_libc_wasi_p2_export_apis(&modules);

    for (uint32_t i = 0; i < wasi_p2_module_count; i++) {
        if (is_module_equal(modules[i].module_name, module_name)) {
            for (uint32_t j = 0; j < modules[i].symbol_count; j++) {
                if (strcmp(modules[i].symbols[j].symbol, func_name) == 0) {
                    return &modules[i].symbols[j];
                }
            }
        }
    }
    return NULL;
}

/**
 * @brief Register a single function in a WASI P2 module.
 * @param module_name The name of the module.
 * @param func_name The name of the function to register.
 * @return True if successful, false otherwise.
 */
bool
wasm_native_register_wasi_p2_module_func(const char *module_name,
                                         const char *func_name)
{
    const NativeSymbol *symbol =
        find_wasi_p2_module_func(module_name, func_name);
    if (symbol) {
        return wasm_native_register_natives(module_name, (NativeSymbol *)symbol,
                                            1);
    }
    return false;
}

/**
 * @brief Unregister a single function in a WASI P2 module.
 * @param module_name The name of the module.
 * @param func_name The name of the function to unregister.
 */
void
wasm_native_unregister_wasi_p2_module_func(const char *module_name,
                                           const char *func_name)
{
    const NativeSymbol *symbol =
        find_wasi_p2_module_func(module_name, func_name);
    if (symbol) {
        wasm_native_unregister_natives(module_name, (NativeSymbol *)symbol);
    }
}

#ifdef __cplusplus
}
#endif
