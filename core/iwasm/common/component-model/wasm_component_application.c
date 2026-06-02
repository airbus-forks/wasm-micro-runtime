/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#if WASM_ENABLE_COMPONENT_MODEL != 0
#include "bh_platform.h"
#include "wasm_component.h"
#include "wasm_component_runtime.h"
#include "wasm_ieee754.h"
#include "wasm_component_flat.h"

static bool
check_main_func_type(const WASMFuncType *type, bool is_memory64)
{
    if (!(type->param_count == 0 || type->param_count == 2)
        || type->result_count > 1) {
        LOG_ERROR(
            "WASM execute application failed: invalid main function type.\n");
        return false;
    }

    if (type->param_count == 2
        && !(type->types[0] == VALUE_TYPE_I32
             && type->types[1]
                    == (is_memory64 ? VALUE_TYPE_I64 : VALUE_TYPE_I32))) {
        LOG_ERROR(
            "WASM execute application failed: invalid main function type.\n");
        return false;
    }

    if (type->result_count
        && type->types[type->param_count] != VALUE_TYPE_I32) {
        LOG_ERROR(
            "WASM execute application failed: invalid main function type.\n");
        return false;
    }

    return true;
}

static void
wasm_component_set_exception(WASMComponentInstance *comp_inst,
                             const char *exception)
{
    if (exception) {
        int ret =
            snprintf(comp_inst->cur_exception, sizeof(comp_inst->cur_exception),
                     "Exception: %s", exception);
        /* Ensure null termination in case of truncation */
        if (ret >= (int)sizeof(comp_inst->cur_exception)) {
            comp_inst->cur_exception[sizeof(comp_inst->cur_exception) - 1] =
                '\0';
        }
    }
    else {
        comp_inst->cur_exception[0] = '\0';
    }
}

static void
print_wit_value(wit_value_t value)
{
    if (!value) {
        return;
    }

    switch (value->type) {
        case COMPONENT_VAL_TYPE_PRIMVAL:
            switch (value->prim_type) {
                case WASM_COMP_PRIMVAL_BOOL:
                {
                    os_printf("%s", value->value.bool_value ? "true" : "false");
                    break;
                }

                case WASM_COMP_PRIMVAL_S8:
                {
                    os_printf("%" PRId32, (int32_t)value->value.s8_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_U8:
                {
                    os_printf("%" PRIu32, (uint32_t)value->value.u8_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_S16:
                {
                    os_printf("%" PRId32, (int32_t)value->value.s16_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_U16:
                {
                    os_printf("%" PRIu32, (uint32_t)value->value.u16_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_S32:
                {
                    os_printf("%" PRId32, value->value.s32_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_U32:
                {
                    os_printf("%" PRIu32, value->value.u32_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_S64:
                {
                    os_printf("%" PRId64, value->value.s64_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_U64:
                {
                    os_printf("%" PRIu64, value->value.u64_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_F32:
                {
                    os_printf("%.7g", (double)value->value.f32_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_F64:
                {
                    os_printf("%.17g", value->value.f64_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_CHAR:
                {
                    os_printf("'%c'", (char)value->value.char_value);
                    break;
                }

                case WASM_COMP_PRIMVAL_STRING:
                {
                    os_printf("\"%.*s\"", value->value.string_value.size_bytes,
                              value->value.string_value.chars);
                    break;
                }

                default:
                {
                    os_printf("<unknown-prim>");
                    break;
                }
            }
            break;

        case COMPONENT_VAL_TYPE_LIST:
        {
            os_printf("[");

            for (uint32 i = 0; i < value->value.list_value.size; i++) {
                if (i > 0)
                    os_printf(", ");
                print_wit_value(value->value.list_value.elems[i]);
            }

            os_printf("]");
            break;
        }

        case COMPONENT_VAL_TYPE_RECORD:
        {
            os_printf("{");

            for (uint32 i = 0; i < value->value.record_value.size; i++) {
                if (i > 0)
                    os_printf(", ");
                os_printf(
                    "%.*s: ", value->value.record_value.fields[i].key_size,
                    value->value.record_value.fields[i].key);
                print_wit_value(value->value.record_value.fields[i].value);
            }

            os_printf("}");
            break;
        }

        case COMPONENT_VAL_TYPE_TUPLE:
        {
            os_printf("(");

            for (uint32 i = 0; i < value->value.tuple_value.size; i++) {
                if (i > 0)
                    os_printf(", ");
                print_wit_value(value->value.tuple_value.elems[i]);
            }

            os_printf(")");
            break;
        }

        case COMPONENT_VAL_TYPE_OPTION:
        {
            if (value->value.option_value.optional_elem) {
                os_printf("some(");
                print_wit_value(value->value.option_value.optional_elem);
                os_printf(")");
            }
            else {
                os_printf("none");
            }

            break;
        }

        case COMPONENT_VAL_TYPE_RESULT:
        {
            if (value->value.result_value.is_err) {
                os_printf("err(");
                print_wit_value(value->value.result_value.result.err);
                os_printf(")");
            }
            else {
                os_printf("ok(");
                print_wit_value(value->value.result_value.result.ok);
                os_printf(")");
            }

            break;
        }

        case COMPONENT_VAL_TYPE_VARIANT:
        {
            os_printf("%.*s", value->value.variant_value.discriminator_size,
                      value->value.variant_value.discriminator);

            if (value->value.variant_value.value) {
                os_printf("(");
                print_wit_value(value->value.variant_value.value);
                os_printf(")");
            }

            break;
        }

        case COMPONENT_VAL_TYPE_ENUM:
        {
            os_printf("%" PRIu32, value->value.enum_value.value);
            break;
        }

        case COMPONENT_VAL_TYPE_FLAGS:
        {
            os_printf("{");

            for (uint32 i = 0; i < value->value.flag_value.size; i++) {
                if (i > 0) {
                    os_printf(", ");
                }

                const char *flag_str = "false";
                if (value->value.flag_value.fields[i].value) {
                    flag_str = value->value.flag_value.fields[i]
                                       .value->value.bool_value
                                   ? "true"
                                   : "false";
                }
                os_printf("%.*s: %s",
                          value->value.flag_value.fields[i].key_size,
                          value->value.flag_value.fields[i].key, flag_str);
            }

            os_printf("}");
            break;
        }

        default:
        {
            os_printf("<unknown-type>");
            break;
        }
    }
}

static void
print_return_values(wit_value_t lifted_results)
{
    if (!lifted_results)
        return;

    if (lifted_results->type == COMPONENT_VAL_TYPE_LIST) {
        for (uint32 i = 0; i < lifted_results->value.list_value.size; i++) {
            if (i > 0)
                os_printf(", ");
            print_wit_value(lifted_results->value.list_value.elems[i]);
        }
    }
    else {
        print_wit_value(lifted_results);
    }

    os_printf("\n");
}

static bool
execute_component_func(WASMComponentInstance *component_inst, const char *name,
                       int32 argc, char *argv[], uint32 *argc1, uint32 **argv1)
{
    if (!component_inst) {
        return false;
    }

    if (!name) {
        return false;
    }

    WASMComponentFunctionInstance *target_func;
    WASMFuncType *type = NULL;
    WASMExecEnv *exec_env = NULL;
    uint32 cell_num = 0;
    uint32 argc1_val = 0;
    uint32 *argv1_val = NULL;
    bool argv1_allocated = false;
    int32 i, p, module_type;
    uint64 total_size;
    char buf[128];

    bh_assert(argc >= 0);
    LOG_DEBUG("call a function \"%s\" with %d arguments", name, argc);

    target_func = wasm_component_lookup_function(component_inst, name);
    if (!target_func) {
        snprintf(buf, sizeof(buf), "lookup function %s failed", name);
        wasm_component_set_exception(component_inst, buf);
        goto fail;
    }

    module_type = target_func->core_func->module_instance->module_type;
    type = wasm_runtime_get_function_type(target_func->core_func, module_type);

    if (!type) {
        LOG_ERROR("invalid module instance type");
        return false;
    }

    if (type->param_count != (uint32)argc) {
        wasm_component_set_exception(component_inst,
                                     "invalid input argument count");
        goto fail;
    }

    argc1_val = type->param_cell_num;
    cell_num =
        (argc1_val > type->ret_cell_num) ? argc1_val : type->ret_cell_num;

    total_size = sizeof(uint32) * (uint64)(cell_num > 2 ? cell_num : 2);

    /* Check if caller provided a pre-allocated buffer */
    if (argv1 && *argv1) {
        /* Use the pre-allocated buffer provided by caller */
        argv1_val = *argv1;
        argv1_allocated = false;
    }
    else {
        /* Allocate memory internally (normal flow) */
        argv1_val = wasm_runtime_malloc((uint32)total_size);
        if (!argv1_val) {
            wasm_component_set_exception(component_inst,
                                         "allocate memory failed");
            goto fail;
        }
        argv1_allocated = true;
    }

    exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)target_func->core_func->module_instance);
    if (!exec_env) {
        wasm_component_set_exception(component_inst,
                                     "create singleton exec_env failed");
        goto fail;
    }

    /* Parse arguments */
    for (i = 0, p = 0; i < argc; i++) {
        char *endptr = NULL;
        bh_assert(argv[i] != NULL);
        if (argv[i][0] == '\0') {
            snprintf(buf, sizeof(buf), "invalid input argument %" PRId32, i);
            wasm_component_set_exception(component_inst, buf);
            goto fail;
        }

        switch (type->types[i]) {
            case VALUE_TYPE_I32:
            {
                argv1_val[p++] = (uint32)strtoul(argv[i], &endptr, 0);
                break;
            }

            case VALUE_TYPE_I64:
            {
                union {
                    uint64 val;
                    uint32 parts[2];
                } u;
                u.val = strtoull(argv[i], &endptr, 0);
                argv1_val[p++] = u.parts[0];
                argv1_val[p++] = u.parts[1];
                break;
            }

            case VALUE_TYPE_F32:
            {
                float32 f32 = strtof(argv[i], &endptr);
                if (isnan(f32)) {
                    if (argv[i][0] == '-') {
                        union ieee754_float u;
                        u.f = f32;
                        if (is_little_endian())
                            u.ieee.ieee_little_endian.negative = 1;
                        else
                            u.ieee.ieee_big_endian.negative = 1;
                        bh_memcpy_s(&f32, sizeof(float), &u.f, sizeof(float));
                    }
                    if (endptr[0] == ':') {
                        uint32 sig;
                        union ieee754_float u;
                        sig = (uint32)strtoul(endptr + 1, &endptr, 0);
                        u.f = f32;
                        if (is_little_endian())
                            u.ieee.ieee_little_endian.mantissa = sig;
                        else
                            u.ieee.ieee_big_endian.mantissa = sig;
                        bh_memcpy_s(&f32, sizeof(float), &u.f, sizeof(float));
                    }
                }
                bh_memcpy_s(&argv1_val[p], (uint32)total_size - p, &f32,
                            (uint32)sizeof(float));
                p++;
                break;
            }

            case VALUE_TYPE_F64:
            {
                union {
                    float64 val;
                    uint32 parts[2];
                } u;
                u.val = strtod(argv[i], &endptr);
                if (isnan(u.val)) {
                    if (argv[i][0] == '-') {
                        union ieee754_double ud;
                        ud.d = u.val;
                        if (is_little_endian())
                            ud.ieee.ieee_little_endian.negative = 1;
                        else
                            ud.ieee.ieee_big_endian.negative = 1;
                        bh_memcpy_s(&u.val, sizeof(double), &ud.d,
                                    sizeof(double));
                    }
                    if (endptr && endptr[0] == ':') {
                        uint64 sig;
                        union ieee754_double ud;
                        sig = strtoull(endptr + 1, &endptr, 0);
                        ud.d = u.val;
                        if (is_little_endian()) {
                            ud.ieee.ieee_little_endian.mantissa0 = sig >> 32;
                            ud.ieee.ieee_little_endian.mantissa1 = (uint32)sig;
                        }
                        else {
                            ud.ieee.ieee_big_endian.mantissa0 = sig >> 32;
                            ud.ieee.ieee_big_endian.mantissa1 = (uint32)sig;
                        }
                        bh_memcpy_s(&u.val, sizeof(double), &ud.d,
                                    sizeof(double));
                    }
                }
                argv1_val[p++] = u.parts[0];
                argv1_val[p++] = u.parts[1];
                break;
            }

            case VALUE_TYPE_FUNCREF:
            case VALUE_TYPE_EXTERNREF:
            {
                if (strncasecmp(argv[i], "null", 4) == 0) {
                    argv1_val[p++] = (uint32)-1;
                }
                else {
                    argv1_val[p++] = (uint32)strtoul(argv[i], &endptr, 0);
                }
                break;
            }

            default:
            {
                bh_assert(0);
                break;
            }
        }

        if (endptr && *endptr != '\0' && *endptr != '_') {
            snprintf(buf, sizeof(buf), "invalid input argument %" PRId32 ": %s",
                     i, argv[i]);
            wasm_component_set_exception(component_inst, buf);
            goto fail;
        }
    }

    wasm_component_set_exception(component_inst, NULL);

    /* Convert argv1_val cells to wasm_val_t array for wasm_runtime_call_wasm_a
     */
    wasm_val_t wasm_args[MAX_FLAT_TYPES];
    for (i = 0, p = 0; i < argc; i++) {
        switch (type->types[i]) {
            case VALUE_TYPE_I32:
            {
                wasm_args[i].kind = WASM_I32;
                wasm_args[i].of.i32 = (int32)argv1_val[p++];
                break;
            }

            case VALUE_TYPE_I64:
            {
                union {
                    uint64 val;
                    uint32 parts[2];
                } u;
                u.parts[0] = argv1_val[p++];
                u.parts[1] = argv1_val[p++];
                wasm_args[i].kind = WASM_I64;
                wasm_args[i].of.i64 = u.val;
                break;
            }

            case VALUE_TYPE_F32:
            {
                wasm_args[i].kind = WASM_F32;
                memcpy(&wasm_args[i].of.f32, &argv1_val[p++], sizeof(float));
                break;
            }

            case VALUE_TYPE_F64:
            {
                union {
                    float64 val;
                    uint32 parts[2];
                } u;
                u.parts[0] = argv1_val[p++];
                u.parts[1] = argv1_val[p++];
                wasm_args[i].kind = WASM_F64;
                wasm_args[i].of.f64 = u.val;
                break;
            }

            default:
            {
                wasm_args[i].kind = WASM_I32;
                wasm_args[i].of.i32 = (int32)argv1_val[p++];
                break;
            }
        }
    }

    uint32 num_results = type->result_count;
    wasm_val_t wasm_results[MAX_FLAT_TYPES];

    if (!wasm_runtime_call_wasm_a(
            exec_env, (WASMFunctionInstanceCommon *)target_func->core_func,
            num_results, wasm_results, type->param_count, wasm_args)) {
        // Propagate exception from module instance to component instance
        const char *exception = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)
                target_func->core_func->module_instance);

        if (exception) {
            wasm_component_set_exception(component_inst, exception);
        }
        goto fail;
    }

    /* Copy results back into argv1_val for backward compatibility */
    for (i = 0, p = 0; i < (int32)num_results; i++) {
        switch (wasm_results[i].kind) {
            case WASM_I32:
            {
                argv1_val[p++] = (uint32)wasm_results[i].of.i32;
                break;
            }

            case WASM_I64:
            {
                union {
                    uint64 val;
                    uint32 parts[2];
                } u;
                u.val = (uint64)wasm_results[i].of.i64;
                argv1_val[p++] = u.parts[0];
                argv1_val[p++] = u.parts[1];
                break;
            }

            case WASM_F32:
            {
                memcpy(&argv1_val[p++], &wasm_results[i].of.f32, sizeof(float));
                break;
            }

            case WASM_F64:
            {
                union {
                    float64 val;
                    uint32 parts[2];
                } u;
                u.val = wasm_results[i].of.f64;
                argv1_val[p++] = u.parts[0];
                argv1_val[p++] = u.parts[1];
                break;
            }

            default:
            {
                argv1_val[p++] = (uint32)wasm_results[i].of.i32;
                break;
            }
        }
    }

    /* Set output parameters if provided */
    if (argc1) {
        *argc1 = argc1_val;
    }
    if (argv1) {
        *argv1 = argv1_val;
    }

    /* Lift results to WIT values */
    CanonicalOptions *lift_opts = target_func->canon_options;
    WASMComponentFuncTypeInstance *ft = target_func->func_type;

    if (ft && ft->results) {
        LiftLowerContext cx_lift;
        memset(&cx_lift, 0, sizeof(cx_lift));
        cx_lift.canonical_opts = lift_opts;
        cx_lift.inst = target_func->core_func->module_instance->comp_instance;
        cx_lift.borrow_scope_type = BORROW_SCOPE_NONE;

        CoreValue core_results[MAX_FLAT_TYPES];
        for (uint32 j = 0; j < num_results; j++) {
            switch (wasm_results[j].kind) {
                case WASM_I32:
                    core_results[j].type = CORE_TYPE_I32;
                    core_results[j].val.i32 = wasm_results[j].of.i32;
                    break;
                case WASM_I64:
                    core_results[j].type = CORE_TYPE_I64;
                    core_results[j].val.i64 = wasm_results[j].of.i64;
                    break;
                case WASM_F32:
                    core_results[j].type = CORE_TYPE_F32;
                    core_results[j].val.f32 = wasm_results[j].of.f32;
                    break;
                case WASM_F64:
                    core_results[j].type = CORE_TYPE_F64;
                    core_results[j].val.f64 = wasm_results[j].of.f64;
                    break;
                default:
                    break;
            }
        }

        CoreValueIter result_vi;
        vi_init(&result_vi, core_results, num_results);

        wit_value_t lifted_results = NULL;
        if (!lift_flat_values(&cx_lift, MAX_FLAT_RESULTS, &result_vi, NULL,
                              ft->results, &lifted_results)) {
            wasm_component_set_exception(component_inst,
                                         "failed to lift return values");
            if (lifted_results)
                free_wit_value(lifted_results);
            goto fail;
        }

        print_return_values(lifted_results);

        /* Call post-return to let the callee free temporary allocations */
        if (lift_opts && lift_opts->post_return_func) {
            if (!wasm_runtime_call_wasm_a(
                    exec_env,
                    (WASMFunctionInstanceCommon *)lift_opts->post_return_func,
                    0, NULL, num_results, wasm_results)) {
                const char *ex = wasm_runtime_get_exception(
                    (WASMModuleInstanceCommon *)
                        lift_opts->post_return_func->module_instance);
                wasm_component_set_exception(
                    component_inst, ex ? ex : "component: post-return failed");

                if (lifted_results) {
                    free_wit_value(lifted_results);
                }
                goto fail;
            }
        }

        if (lifted_results) {
            free_wit_value(lifted_results);
        }
    }

    /* Only free if we allocated it internally (not pre-allocated by caller) */
    if (argv1_allocated) {
        wasm_runtime_free(argv1_val);
    }

    return true;

fail:
    /* Only free if we allocated it internally */
    if (argv1_allocated && argv1_val) {
        wasm_runtime_free(argv1_val);
    }
    bh_assert(wasm_component_runtime_get_exception(component_inst));
    return false;
}

static bool
execute_component_main(WASMComponentInstance *component_inst, int32 argc,
                       char *argv[])
{
    WASMComponentFunctionInstance *func = NULL;
    WASMFuncType *func_type = NULL;
    WASMExecEnv *exec_env = NULL;
    uint32 argc1 = 0, argv1[3] = { 0 };
    uint32 total_argv_size = 0;
    uint64 total_size = 0;
    uint64 argv_buf_offset = 0;
    int32 i = 0;
    char *argv_buf = NULL, *p = NULL, *p_end = NULL;
    uint32 *argv_offsets = NULL, module_type = 0;
    bool ret = false, is_import_func = true, is_memory64 = false;

    func = wasm_component_lookup_function(
        component_inst,
        "run"); // SEEME: TBD wasi:cli interface is needed for lookup
    if (!func) {
        wasm_component_set_exception(
            component_inst,
            "lookup the entry point symbol (like run, wasi:cli/run) failed");
        return false;
    }

    exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)func->core_func->module_instance);
    if (!exec_env) {
        wasm_component_set_exception(component_inst,
                                     "create singleton exec_env failed");
        return false;
    }

#if WASM_ENABLE_MEMORY64 != 0
    if (func->core_func->module_instance->memory_count > 0)
        is_memory64 =
            func->core_func->module_instance->memories[0]->is_memory64;
#endif

#if WASM_ENABLE_INTERP
    is_import_func = func->core_func->is_import_func;
#endif

    if (is_import_func) {
        wasm_component_set_exception(component_inst,
                                     "lookup main function failed");
        return false;
    }

    module_type = func->core_func->module_instance->module_type;
    func_type = wasm_runtime_get_function_type(func->core_func, module_type);

    if (!func_type) {
        LOG_ERROR("invalid module instance type");
        return false;
    }

    if (!check_main_func_type(func_type, is_memory64)) {
        wasm_component_set_exception(component_inst,
                                     "invalid function type of main function");
        return false;
    }

    if (func_type->param_count) {
        for (i = 0; i < argc; i++) {
            total_argv_size += (uint32)(strlen(argv[i]) + 1);
        }
#if WASM_ENABLE_MEMORY64 != 0
        if (is_memory64)
            /* `char **argv` is an array of 64-bit elements in memory64 */
            total_argv_size = align_uint(total_argv_size, 8);
        else
#endif
            total_argv_size = (uint32)align_uint(total_argv_size, 4);

#if WASM_ENABLE_MEMORY64 != 0
        if (is_memory64)
            /* `char **argv` is an array of 64-bit elements in memory64 */
            total_size = (uint64)total_argv_size
                         + (uint64)(sizeof(uint64) * (uint64)argc);
        else
#endif
            total_size = (uint64)total_argv_size
                         + (uint64)(sizeof(uint32) * (uint64)argc);

        if (total_size >= UINT32_MAX) {
            wasm_component_set_exception(component_inst,
                                         "allocate memory failed");
            return false;
        }
        argv_buf_offset = (uint64)wasm_runtime_module_malloc(
            (WASMModuleInstanceCommon *)func->core_func->module_instance,
            total_size, (void **)&argv_buf);
        if (!argv_buf_offset) {
            wasm_component_set_exception(component_inst,
                                         "allocate memory failed");
            return false;
        }

        p = argv_buf;
        argv_offsets = (uint32 *)(p + total_argv_size);
        p_end = p + total_size;

        for (i = 0; i < argc; i++) {
            bh_memcpy_s(p, (uint32)(p_end - p), argv[i],
                        (uint32)(strlen(argv[i]) + 1));
#if WASM_ENABLE_MEMORY64 != 0
            if (is_memory64)
                /* `char **argv` is an array of 64-bit elements in memory64 */
                ((uint64 *)argv_offsets)[i] =
                    (uint32)argv_buf_offset + (uint32)(p - argv_buf);
            else
#endif
                argv_offsets[i] =
                    (uint32)argv_buf_offset + (uint32)(p - argv_buf);
            p += strlen(argv[i]) + 1;
        }

        argv1[0] = (uint32)argc;
#if WASM_ENABLE_MEMORY64 != 0
        if (is_memory64) {
            argc1 = 3;
            uint64 app_addr = wasm_runtime_addr_native_to_app(
                (WASMModuleInstanceCommon *)func->core_func->module_instance,
                argv_offsets);
            PUT_I64_TO_ADDR(&argv1[1], app_addr);
        }
        else
#endif
        {
            argc1 = 2;
            argv1[1] = (uint32)wasm_runtime_addr_native_to_app(
                (WASMModuleInstanceCommon *)func->core_func->module_instance,
                argv_offsets);
        }
    }

    ret = wasm_runtime_call_wasm(exec_env, func->core_func, argc1, argv1);
    if (ret && func_type->result_count > 0 && argc > 0 && argv)
        /* copy the return value */
        *(int *)argv = (int)argv1[0];

    if (argv_buf_offset)
        wasm_runtime_module_free(
            (WASMModuleInstanceCommon *)func->core_func->module_instance,
            argv_buf_offset);

    return ret;
}

bool
wasm_component_application_execute_main(WASMComponentInstance *component_inst,
                                        int32 argc, char *argv[])
{
    bool ret = false;
    ret = execute_component_main(component_inst, argc, argv);
    return (ret && !wasm_component_runtime_get_exception(component_inst))
               ? true
               : false;
}

bool
wasm_component_application_execute_func(WASMComponentInstance *component_inst,
                                        const char *name, int32 argc,
                                        char *argv[])
{
    bool ret;
    ret = execute_component_func(component_inst, name, argc, argv, NULL, NULL);
    return (ret && !wasm_component_runtime_get_exception(component_inst))
               ? true
               : false;
}

bool
wasm_component_application_execute_func_ex(
    WASMComponentInstance *component_inst, const char *name, int32 argc,
    char *argv[], uint32 *argc1, uint32 **argv1)
{
    bool ret;
    ret =
        execute_component_func(component_inst, name, argc, argv, argc1, argv1);
    return (ret && !wasm_component_runtime_get_exception(component_inst))
               ? true
               : false;
}
#endif /* WASM_ENABLE_COMPONENT_MODEL != 0*/
