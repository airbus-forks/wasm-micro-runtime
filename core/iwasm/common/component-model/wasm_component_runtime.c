/*
 * Copyright (C) 2026 Airbus Defence and Space Romania SRL. All rights reserved.
 * SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
 */

#if WASM_ENABLE_COMPONENT_MODEL != 0
#include "wasm.h"
#include "wasm_runtime.h"
#include "wasm_component.h"
#include "wasm_loader.h"
#include "wasm_component_runtime.h"
#include "bh_log.h"
#include "stdio.h"
#include "wasm_component_canonical.h"
#include "wasm_component_task.h"
#include "bh_assert.h"

/// @brief Compute discriminant alignment based on number of cases
uint32_t
compute_discriminant_alignment(uint32_t num_cases)
{
    if (num_cases <= 256)
        return 1; // u8
    else if (num_cases <= 65536)
        return 2; // u16
    else
        return 4; // u32
}

uint32_t
compute_max_case_alignment(WASMComponentVariantInstance *type)
{
    uint32_t max_case_align = 1;
    for (uint32_t i = 0; i < type->count; i++) {
        if (type->cases[i].value_type) {
            uint32_t case_align = compute_alignment(type->cases[i].value_type);
            if (case_align > max_case_align) {
                max_case_align = case_align;
            }
        }
    }

    return max_case_align;
}

/// @brief Compute alignment for primitive values
uint32_t
compute_alignment_primitive_value(WASMComponentPrimValType primval)
{
    switch (primval) {
        case WASM_COMP_PRIMVAL_BOOL:
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            return 1;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_U16:
            return 2;
        case WASM_COMP_PRIMVAL_S32:
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_F32:
        case WASM_COMP_PRIMVAL_CHAR:
        case WASM_COMP_PRIMVAL_STRING:
        case WASM_COMP_PRIMVAL_ERROR_CONTEXT:
            return 4;
        case WASM_COMP_PRIMVAL_S64:
        case WASM_COMP_PRIMVAL_U64:
        case WASM_COMP_PRIMVAL_F64:
            return 8;
        default:
            return 1;
    }
}

/// @brief Calculate alignment size type
/// @param type
/// @return uint32_t alignment computed
uint32_t
compute_alignment(WASMComponentTypeInstance *type)
{
    switch (type->type) {
        case COMPONENT_VAL_TYPE_PRIMVAL:
            // Look at which primval it is
            return compute_alignment_primitive_value(
                type->type_specific.primval);

        case COMPONENT_VAL_TYPE_LIST:
        {
            return 4;
        }

        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
        {
            WASMComponentListLenInstance *list_len =
                type->type_specific.list_len;
            return compute_alignment(list_len->element_type);
        }

        case COMPONENT_VAL_TYPE_RECORD:
        {
            // For records, alignment = max alignment of all fields
            uint32_t max_align = 1;
            WASMComponentTypeInstance *val_type = NULL;
            for (uint32_t index = 0; index < type->type_specific.record->count;
                 index++) {
                val_type = type->type_specific.record->fields[index].type;
                uint32_t field_align = compute_alignment(val_type);
                if (field_align > max_align) {
                    max_align = field_align;
                }
            }

            return max_align;
        }

        case COMPONENT_VAL_TYPE_TUPLE:
        {
            // For tuples, alignment = max alignment of all elements
            uint32_t max_align = 1;
            WASMComponentTupleInstance *tuple = type->type_specific.tuple;
            for (uint32_t i = 0; i < tuple->count; i++) {
                uint32_t elem_align =
                    compute_alignment(tuple->element_types[i]);
                if (elem_align > max_align) {
                    max_align = elem_align;
                }
            }
            return max_align;
        }

        case COMPONENT_VAL_TYPE_FLAGS:
        {
            uint32_t n = type->type_specific.flag->count;

            bh_assert((0 < n) && (n <= 32));

            if (n <= 8)
                return 1;
            if (n <= 16)
                return 2;

            return 4;
        }

        case COMPONENT_VAL_TYPE_VARIANT:
        {
            uint32_t n = type->type_specific.variant->count;

            // Compute discriminant alignment
            uint32_t disc_align = compute_discriminant_alignment(n);

            // Compute max case alignment
            uint32_t max_case_align =
                compute_max_case_alignment(type->type_specific.variant);

            return (disc_align > max_case_align) ? disc_align : max_case_align;
        }

        case COMPONENT_VAL_TYPE_OPTION:
        {
            // Option = variant with 2 cases: none (empty), some (element_type)
            WASMComponentOptionInstance *option = type->type_specific.option;

            // Discriminant for 2 cases = u8 = alignment 1
            uint32_t disc_align = 1;

            // Max case alignment = max(none, some)
            // none is empty (align 1), some has element_type
            uint32_t some_align = compute_alignment(option->element_type);

            return (disc_align > some_align) ? disc_align : some_align;
        }

        case COMPONENT_VAL_TYPE_RESULT:
        {
            // Result = variant with 2 cases: ok (result_type), error
            // (error_type)
            WASMComponentResultInstance *result = type->type_specific.result;

            // Discriminant for 2 cases = u8 = alignment 1
            uint32_t disc_align = 1;

            // Max case alignment = max(ok, error)
            uint32_t ok_align = 1;
            uint32_t err_align = 1;

            if (result->result_type) {
                ok_align = compute_alignment(result->result_type);
            }
            if (result->error_type) {
                err_align = compute_alignment(result->error_type);
            }

            uint32_t max_case_align =
                (ok_align > err_align) ? ok_align : err_align;

            return (disc_align > max_case_align) ? disc_align : max_case_align;
        }

        case COMPONENT_VAL_TYPE_ENUM:
        {
            // Enum = variant where all cases are empty
            // So alignment = discriminant alignment only
            WASMComponentEnumType *enum_type = type->type_specific.enum_type;
            return compute_discriminant_alignment(enum_type->count);
        }

        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
        case COMPONENT_VAL_TYPE_STREAM:
        case COMPONENT_VAL_TYPE_FUTURE:
        {
            return 4; // All are i32 handles
        }

        default:
            return 0;
    }
}

/// @brief Compute elem size for primitive values
uint32_t
compute_elem_size_primitive_value(WASMComponentPrimValType primval)
{
    switch (primval) {
        case WASM_COMP_PRIMVAL_BOOL:
        case WASM_COMP_PRIMVAL_S8:
        case WASM_COMP_PRIMVAL_U8:
            return 1;
        case WASM_COMP_PRIMVAL_S16:
        case WASM_COMP_PRIMVAL_U16:
            return 2;
        case WASM_COMP_PRIMVAL_S32:
        case WASM_COMP_PRIMVAL_U32:
        case WASM_COMP_PRIMVAL_F32:
        case WASM_COMP_PRIMVAL_CHAR:
        case WASM_COMP_PRIMVAL_ERROR_CONTEXT:
            return 4;
        case WASM_COMP_PRIMVAL_S64:
        case WASM_COMP_PRIMVAL_U64:
        case WASM_COMP_PRIMVAL_F64:
        case WASM_COMP_PRIMVAL_STRING:
            return 8;
        default:
            return 1;
    }
}

/// @brief Calculate element size
/// @param type
/// @return uint32_t elem size computed
uint32_t
compute_elem_size(WASMComponentTypeInstance *type)
{
    switch (type->type) {
        case COMPONENT_VAL_TYPE_PRIMVAL:
            // Look at which primval it is
            return compute_elem_size_primitive_value(
                type->type_specific.primval);

        case COMPONENT_VAL_TYPE_LIST:
        {
            return 8;
        }

        case COMPONENT_VAL_TYPE_FIXED_SIZE_LIST:
        {
            WASMComponentListLenInstance *list_len =
                type->type_specific.list_len;
            return list_len->len * compute_elem_size(list_len->element_type);
        }

        case COMPONENT_VAL_TYPE_RECORD:
        {
            // For records, alignment = max alignment of all fields
            uint32_t s = 0;
            WASMComponentTypeInstance *val_type = NULL;
            for (uint32_t index = 0; index < type->type_specific.record->count;
                 index++) {
                val_type = type->type_specific.record->fields[index].type;
                s = align_to(s, compute_alignment(val_type));
                s += compute_elem_size(val_type);
            }

            assert(s > 0);
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_TUPLE:
        {
            // Tuple despecializes to record, same elem_size algorithm
            uint32_t s = 0;
            WASMComponentTupleInstance *tuple = type->type_specific.tuple;

            for (uint32_t i = 0; i < tuple->count; i++) {
                WASMComponentTypeInstance *elem = tuple->element_types[i];
                s = align_to(
                    s, compute_alignment(elem)); // Align to element's alignment
                s += compute_elem_size(elem);    // Add element's size
            }

            bh_assert(s > 0); // Tuples can't be empty
            return align_to(
                s,
                type->alignment); // Final padding to tuple's overall alignment
        }

        case COMPONENT_VAL_TYPE_FLAGS:
        {
            uint32_t n = type->type_specific.flag->count;

            bh_assert((0 < n) && (n <= 32));

            if (n <= 8)
                return 1;
            if (n <= 16)
                return 2;

            return 4;
        }

        case COMPONENT_VAL_TYPE_VARIANT:
        {
            WASMComponentVariantInstance *variant = type->type_specific.variant;

            // 1. Discriminant size (depends on number of cases)
            uint32_t s = compute_discriminant_alignment(
                variant->count); // Returns size (1, 2, or 4)

            // 2. Find max case alignment and max case size
            uint32_t max_case_align = 1;
            uint32_t max_case_size = 0;

            for (uint32_t i = 0; i < variant->count; i++) {
                if (variant->cases[i].value_type) {
                    uint32_t case_align =
                        compute_alignment(variant->cases[i].value_type);
                    uint32_t case_size =
                        compute_elem_size(variant->cases[i].value_type);

                    if (case_align > max_case_align) {
                        max_case_align = case_align;
                    }
                    if (case_size > max_case_size) {
                        max_case_size = case_size;
                    }
                }
            }

            // 3. Align discriminant to max case alignment
            s = align_to(s, max_case_align);

            // 4. Add max case size (largest payload among all cases)
            s += max_case_size;

            // 5. Align to variant's overall alignment
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_OPTION:
        {
            // Option = variant with 2 cases: none (empty), some (element_type)
            WASMComponentOptionInstance *option = type->type_specific.option;

            // 1. Discriminant size (2 cases = u8 = 1 byte)
            uint32_t s = 1;

            // 2. Max case alignment: max(none=1, some=element_align)
            uint32_t some_align = compute_alignment(option->element_type);
            uint32_t max_case_align = (some_align > 1) ? some_align : 1;

            // 3. Align discriminant to max case alignment
            s = align_to(s, max_case_align);

            // 4. Add max case size (only "some" has payload)
            uint32_t some_size = compute_elem_size(option->element_type);
            s += some_size;

            // 5. Align to variant's overall alignment
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_RESULT:
        {
            // Result = variant with 2 cases: ok (result_type), error
            // (error_type)
            WASMComponentResultInstance *result = type->type_specific.result;

            // 1. Discriminant size (2 cases = u8 = 1 byte)
            uint32_t s = 1;

            // 2. Max case alignment: max(ok_align, error_align)
            uint32_t ok_align = 1, err_align = 1;
            uint32_t ok_size = 0, err_size = 0;

            if (result->result_type) {
                ok_align = compute_alignment(result->result_type);
                ok_size = compute_elem_size(result->result_type);
            }
            if (result->error_type) {
                err_align = compute_alignment(result->error_type);
                err_size = compute_elem_size(result->error_type);
            }

            uint32_t max_case_align =
                (ok_align > err_align) ? ok_align : err_align;
            uint32_t max_case_size = (ok_size > err_size) ? ok_size : err_size;

            // 3. Align discriminant to max case alignment
            s = align_to(s, max_case_align);

            // 4. Add max case size
            s += max_case_size;

            // 5. Align to variant's overall alignment
            return align_to(s, type->alignment);
        }

        case COMPONENT_VAL_TYPE_ENUM:
        {
            // Enum = variant with all cases empty
            // elem_size = discriminant size only (no payloads)
            WASMComponentEnumType *enum_type = type->type_specific.enum_type;
            return compute_discriminant_alignment(enum_type->count);
        }

        case COMPONENT_VAL_TYPE_OWN:
        case COMPONENT_VAL_TYPE_BORROW:
        case COMPONENT_VAL_TYPE_STREAM:
        case COMPONENT_VAL_TYPE_FUTURE:
        {
            return 4; // All are i32 handles
        }

        default:
            return 0;
    }
}

/// @brief Calculate size needed for the default value types defined in this
/// component
/// @param def_val_type
/// @return
uint32
wasm_get_def_val_type_size(WASMComponentDefValType *def_val_type)
{
    if (!def_val_type) {
        return 0;
    }
    uint32 size = sizeof(WASMComponentTypeInstance);
    switch (def_val_type->tag) {
        case WASM_COMP_DEF_VAL_PRIMVAL: // pvt:<primvaltype>
        // Flags type
        case WASM_COMP_DEF_VAL_FLAGS: // 0x6e l*:vec(<label'>)
        // Enum type
        case WASM_COMP_DEF_VAL_ENUM: // 0x6d l*:vec(<label'>)
            // Flags and Enums are fully defined in Component binary, require no
            // additional linking in Instance
            break;
        // Record type (labeled fields)
        case WASM_COMP_DEF_VAL_RECORD: // 0x72 lt*:vec(<labelvaltype>)
            size += (uint32)(sizeof(WASMComponentRecordInstance)
                             + (def_val_type->def_val.record->count
                                * sizeof(WASMComponentLabelValTypeInstance)));
            LOG_DEBUG("Detected size of Record is %d", size);
            break;
        // Variant type (labeled cases)
        case WASM_COMP_DEF_VAL_VARIANT: // 0x71 case*:vec(<case>)
            size += (uint32)(sizeof(WASMComponentVariantInstance)
                             + (def_val_type->def_val.variant->count
                                * sizeof(WASMComponentCaseValInstance)));
            LOG_DEBUG("Detected size of Variant is %d", size);
            break;
        // List types
        case WASM_COMP_DEF_VAL_LIST: // 0x70 t:<valtype>
            size += sizeof(WASMComponentListInstance);
            LOG_DEBUG("Detected size of list is %d", size);
            break;
        case WASM_COMP_DEF_VAL_LIST_LEN: // 0x67 t:<valtype> len:<u32>
            size += sizeof(WASMComponentListLenInstance);
            LOG_DEBUG("Detected size of fixed size list is %d", size);
            break;
        // Tuple type
        case WASM_COMP_DEF_VAL_TUPLE: // 0x6f t*:vec(<valtype>)
            size += (uint32)(sizeof(WASMComponentTupleInstance)
                             + (def_val_type->def_val.tuple->count
                                * sizeof(WASMComponentTypeInstance *)));
            LOG_DEBUG("Detected size of Tuple is %d", size);
            break;
        // Option type
        case WASM_COMP_DEF_VAL_OPTION: // 0x6b t:<valtype>
            size += sizeof(WASMComponentOptionInstance);
            LOG_DEBUG("Detected size of option is %d", size);
            break;
        // Result type
        case WASM_COMP_DEF_VAL_RESULT: // 0x6a t?:<valtype>? u?:<valtype>?
            size += sizeof(WASMComponentResultInstance);
            LOG_DEBUG("Detected size of result is %d", size);
            break;
        // Handle types
        case WASM_COMP_DEF_VAL_OWN:    // 0x69 i:<typeidx>
        case WASM_COMP_DEF_VAL_BORROW: // 0x68 i:<typeidx>
            size += sizeof(WASMComponentResourceInstance);
            break;
        // Async types
        case WASM_COMP_DEF_VAL_STREAM: // 0x66 t?:<valtype>?
        case WASM_COMP_DEF_VAL_FUTURE: // 0x65 t?:<valtype>?
            LOG_WARNING("Not yet supported\n");
            break;
    }
    return size;
}

/// @brief Calculate size needed for the function types defined in this
/// component
/// @param func_type
/// @return
uint32
wasm_get_func_type_size(WASMComponentFuncType *func_type)
{
    if (!func_type) {
        return 0;
    }
    uint32 size = (uint32)(sizeof(WASMComponentFuncTypeInstance)
                           + sizeof(WASMComponentParamListInstance)
                           + (func_type->params->count
                              * sizeof(WASMComponentLabelValTypeInstance))
                           + sizeof(WASMComponentResultListInstance)
                           + sizeof(WASMComponentTypeInstance));
    return size;
}

/// @brief Calculates total size required by component instance type (including
/// index spaces and defined types)
/// @param instance_type instance type declaration from binary section
/// @param instance_type_size OUT: structure that will hold the index counts +
/// total defined types memory size
/// @return
uint32
wasm_get_inst_decl_size(WASMComponentInstType *instance_type,
                        WASMComponentInstanceDeclTypeSize *instance_type_size)
{
    if (!instance_type) {
        return 0;
    }
    uint32 size = 0, types_size = 0, funcs_count = 0, types_count = 0,
           exports_count = 0, resource_count = 0, idx = 0;
    for (idx = 0; idx < instance_type->count; idx++) {
        WASMComponentInstDecl *instance_decl =
            &instance_type->instance_decls[idx];
        switch (instance_decl->tag) {
            case WASM_COMP_COMPONENT_DECL_INSTANCE_CORE_TYPE:
                // TODO: not present in current test binaries
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_TYPE:
                // The new types defined in this instance type
                switch (instance_decl->decl.type->tag) {
                    case WASM_COMP_DEF_TYPE:
                        types_size += wasm_get_def_val_type_size(
                            instance_decl->decl.type->type.def_val_type);
                        break;
                    case WASM_COMP_FUNC_TYPE:
                        types_size += wasm_get_func_type_size(
                            instance_decl->decl.type->type.func_type);
                        break;
                    default:
                        LOG_WARNING("Instance declaration type only supports "
                                    "types and functions for now \n");
                        break;
                }
                types_count++;
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_ALIAS:
                // Increase index count for the aliased sort
                switch (instance_decl->decl.alias->sort->sort) {
                    case WASM_COMP_SORT_FUNC:
                        funcs_count++;
                        break;
                    case WASM_COMP_SORT_TYPE:
                        types_count++;
                        break;
                    default:
                        LOG_WARNING("Instance declaration Aliases should only "
                                    "support types and functions for now\n");
                        break;
                }
                break;
            case WASM_COMP_COMPONENT_DECL_INSTANCE_EXPORTDECL:
                // Increase index count + export count for export definition
                exports_count++;
                switch (instance_decl->decl.export_decl->extern_desc->type) {
                    case WASM_COMP_EXTERN_FUNC:
                        funcs_count++;
                        break;
                    case WASM_COMP_EXTERN_TYPE:
                        types_count++;
                        if (instance_decl->decl.export_decl->extern_desc
                                ->extern_desc.type.type_bound->tag
                            == WASM_COMP_TYPEBOUND_TYPE) {
                            types_size +=
                                sizeof(WASMComponentTypeInstance)
                                + sizeof(WASMComponentResourceInstance)
                                + 3
                                      * sizeof(
                                          WASMFunctionInstance); // sub resource
                                                                 // definition
                            resource_count++;
                        }
                        break;
                    default:
                        LOG_WARNING("Instance declaration Exports should only "
                                    "support types and functions for now\n");
                        break;
                }
                break;
            default:
                LOG_WARNING("Unsupported Component instance declaration type");
                break;
        }
    }

    if (instance_type_size) {
        // Needed for instantiating the WASMComponentInstTypeInstance structure
        instance_type_size->exports_count = exports_count;
        instance_type_size->func_count = funcs_count;
        instance_type_size->types_count = types_count;
        instance_type_size->types_size = types_size;
        instance_type_size->resource_count = resource_count;
    }
    size = (uint32)(sizeof(WASMComponentTypeInstance)
                    + sizeof(WASMComponentInstTypeInstance) + types_size
                    + (types_count * sizeof(WASMComponentTypeInstance))
                    + (funcs_count * sizeof(WASMComponentFunctionInstance))
                    + (exports_count * sizeof(WASMComponentExportInstance))
                    + ((funcs_count + (resource_count * 3))
                       * sizeof(WASMFunctionInstance))
                    + ((funcs_count + (resource_count * 3))
                       * sizeof(WASMFunctionImport)));
    return size;
}

/// @brief Calculate total size that needs to be allocated for the instance ==
/// size of index spaces + total size necessary for defined types
/// @param component
/// @return index count structure
WASMComponentIndexCount *
wasm_component_get_index_count(WASMComponent *component, char *error_buf,
                               uint32 error_buf_size)
{
    if (!component) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: component ptr is NULL\n");
        return NULL;
    }
    // From the component binary section extract the count of each index space
    // of the component, for memory allocation and indexing
    WASMComponentIndexCount *index_count =
        (WASMComponentIndexCount *)wasm_runtime_malloc(
            sizeof(WASMComponentIndexCount));
    if (!index_count) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: alloc index_count failed\n");
        return NULL;
    }
    memset(index_count, 0, sizeof(WASMComponentIndexCount));
    uint32 section_idx = 0, idx = 0;
    WASMComponentSection *section = NULL;

    LOG_DEBUG(
        "Calculate size of component instance index spaces to be allocated");
    for (section_idx = 0; section_idx < component->section_count;
         section_idx++) {
        if (!component->sections) {
            set_error_buf_ex(error_buf, error_buf_size,
                             "ERROR: component section NULL value\n");
            return NULL;
        }
        section = &component->sections[section_idx];
        switch (section->id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
                // TODO: Core custom section not in scope for now
                break;
            case WASM_COMP_SECTION_CORE_MODULE:
                index_count->core_modules++;
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                index_count->core_instances +=
                    (uint32)section->parsed.core_instance_section->count;
                index_count->defined_core_instances +=
                    (uint32)section->parsed.core_instance_section->count;
                break;
            case WASM_COMP_SECTION_CORE_TYPE:
                index_count->core_types +=
                    (uint32)section->parsed.core_type_section->count;
                break;
            case WASM_COMP_SECTION_COMPONENT:
                index_count->components++;
                break;
            case WASM_COMP_SECTION_INSTANCES:
                index_count->instances +=
                    (uint32)section->parsed.instance_section->count;
                index_count->defined_instances +=
                    (uint32)section->parsed.instance_section->count;
                break;
            case WASM_COMP_SECTION_ALIASES:
                uint32 aliases_count =
                    (uint32)section->parsed.alias_section->count;
                for (idx = 0; idx < aliases_count; idx++) {
                    WASMComponentAliasDefinition *alias =
                        &section->parsed.alias_section->aliases[idx];
                    if (!alias || !alias->sort) {
                        set_error_buf_ex(error_buf, error_buf_size,
                                         "ERROR: Invalid alias section");
                        return NULL;
                    }
                    switch (alias->sort->sort) {
                        case WASM_COMP_SORT_CORE_SORT:
                            switch (alias->sort->core_sort) {
                                case WASM_COMP_CORE_SORT_FUNC:
                                    index_count->core_functions++;
                                    index_count->defined_core_functions++;
                                    break;
                                case WASM_COMP_CORE_SORT_TABLE:
                                    index_count->core_tables++;
                                    break;
                                case WASM_COMP_CORE_SORT_MEMORY:
                                    index_count->core_memories++;
                                    break;
                                case WASM_COMP_CORE_SORT_GLOBAL:
                                    index_count->core_globals++;
                                    break;
                                case WASM_COMP_CORE_SORT_TYPE:
                                    index_count->core_types++;
                                    break;
                                case WASM_COMP_CORE_SORT_MODULE:
                                    index_count->core_modules++;
                                    break;
                                case WASM_COMP_CORE_SORT_INSTANCE:
                                    index_count->core_instances++;
                                default:
                                    break;
                            }
                            break;
                        case WASM_COMP_SORT_FUNC:
                            index_count->functions++;
                            break;
                        case WASM_COMP_SORT_VALUE:
                            index_count->values++;
                            break;
                        case WASM_COMP_SORT_TYPE:
                            index_count->types++;
                            break;
                        case WASM_COMP_SORT_COMPONENT:
                            index_count->components++;
                            break;
                        case WASM_COMP_SORT_INSTANCE:
                            index_count->instances++;
                            break;
                        default:
                            break;
                    }
                }
                break;
            case WASM_COMP_SECTION_TYPE:
                index_count->types +=
                    (uint32)section->parsed.type_section->count;
                for (idx = 0; idx < section->parsed.type_section->count;
                     idx++) {
                    WASMComponentTypes *type =
                        &section->parsed.type_section->types[idx];
                    if (!type) {
                        set_error_buf_ex(error_buf, error_buf_size,
                                         "ERROR: Invalid type section");
                        break;
                    }
                    switch (type->tag) {
                        case WASM_COMP_DEF_TYPE:
                            index_count->types_total_size +=
                                wasm_get_def_val_type_size(
                                    type->type.def_val_type);
                            break;
                        case WASM_COMP_FUNC_TYPE:
                            index_count->types_total_size +=
                                wasm_get_func_type_size(type->type.func_type);
                            break;
                        case WASM_COMP_INSTANCE_TYPE:
                            index_count->types_total_size +=
                                wasm_get_inst_decl_size(
                                    type->type.instance_type, NULL);
                            break;
                        case WASM_COMP_RESOURCE_TYPE_SYNC:
                            index_count->types_total_size +=
                                sizeof(WASMComponentTypeInstance)
                                + sizeof(WASMComponentResourceInstance)
                                + 3 * sizeof(WASMFunctionInstance);
                            break;
                        default:
                            LOG_WARNING("Other types not supported for now\n");
                            break;
                    }
                }
                break;
            case WASM_COMP_SECTION_IMPORTS:
                index_count->imports +=
                    (uint32)section->parsed.import_section->count;
                for (idx = 0; idx < section->parsed.import_section->count;
                     idx++) {
                    WASMComponentImport *import_section =
                        &section->parsed.import_section->imports[idx];
                    if (!import_section || !import_section->extern_desc)
                        continue;
                    switch (import_section->extern_desc->type) {
                        case WASM_COMP_EXTERN_CORE_MODULE:
                            index_count->core_modules++;
                            break;
                        case WASM_COMP_EXTERN_FUNC:
                            index_count->functions++;
                            break;
                        case WASM_COMP_EXTERN_VALUE:
                            index_count->values++;
                            break;
                        case WASM_COMP_EXTERN_TYPE:
                            index_count->types++;
                            break;
                        case WASM_COMP_EXTERN_COMPONENT:
                            index_count->components++;
                            break;
                        case WASM_COMP_EXTERN_INSTANCE:
                            index_count->instances++;
                            index_count->defined_instances++;
                            break;
                        default:
                            set_error_buf_ex(
                                error_buf, error_buf_size,
                                "ERROR: Unrecognised import type\n");
                            break;
                    }
                }
                break;
            case WASM_COMP_SECTION_EXPORTS:
                index_count->exports +=
                    (uint32)section->parsed.export_section->count;
                for (idx = 0; idx < section->parsed.export_section->count;
                     idx++) {
                    WASMComponentExport *export_section =
                        &section->parsed.export_section->exports[idx];
                    if (!export_section)
                        continue;
                    switch (export_section->sort_idx->sort->sort) {
                        case WASM_COMP_EXTERN_CORE_MODULE:
                            if (export_section->sort_idx->sort->core_sort
                                == WASM_COMP_CORE_SORT_MODULE)
                                index_count->core_modules++;
                            break;
                        case WASM_COMP_EXTERN_FUNC:
                            index_count->functions++;
                            break;
                        case WASM_COMP_EXTERN_VALUE:
                            index_count->values++;
                            break;
                        case WASM_COMP_EXTERN_TYPE:
                            index_count->types++;
                            break;
                        case WASM_COMP_EXTERN_COMPONENT:
                            index_count->components++;
                            break;
                        case WASM_COMP_EXTERN_INSTANCE:
                            index_count->instances++;
                            break;
                        default:
                            set_error_buf_ex(
                                error_buf, error_buf_size,
                                "ERROR: Unrecognised export type\n");
                            break;
                    }
                }
                break;
            case WASM_COMP_SECTION_VALUES:
                // TODO: not in scope for now
                break;
            case WASM_COMP_SECTION_CANONS:
                WASMComponentCanon *canon = NULL;
                for (idx = 0; idx < section->parsed.canon_section->count;
                     idx++) {
                    canon = &section->parsed.canon_section->canons[idx];

                    switch (canon->tag) {
                        case WASM_COMP_CANON_LIFT:
                            index_count->functions++;
                            index_count->defined_functions++;
                            if (canon->canon_data.lift.canon_opts
                                    ->canon_opts_count) {
                                index_count->canon_options_funcs++;
                                index_count->canon_options +=
                                    (uint32)canon->canon_data.lift.canon_opts
                                        ->canon_opts_count;
                            }
                            break;
                        case WASM_COMP_CANON_LOWER:
                            index_count->core_functions++;
                            index_count->defined_core_functions++;
                            if (canon->canon_data.lower.canon_opts
                                    ->canon_opts_count) {
                                index_count->canon_options_funcs++;
                                index_count->canon_options +=
                                    (uint32)canon->canon_data.lift.canon_opts
                                        ->canon_opts_count;
                            }
                            break;
                        case WASM_COMP_CANON_RESOURCE_DROP:
                        case WASM_COMP_CANON_RESOURCE_NEW:
                        case WASM_COMP_CANON_RESOURCE_REP:
                            index_count->core_functions++;
                            break;
                        default:
                            break;
                    }
                }
                break;
            default:
                set_error_buf_ex(
                    error_buf, error_buf_size,
                    "FATAL ERROR: Unknown/unsupported section (id=%u)\n",
                    section->id);
                break;
        }
    }
    return index_count;
}

WASMComponentInstance *
wasm_component_instance_allocate(WASMComponentIndexCount *index_count,
                                 char *error_buf, uint32 error_buf_size)
{
    LOG_DEBUG("Allocate memory for component instance with:");
    LOG_DEBUG(" %d components", index_count->components);
    LOG_DEBUG(" %d instances", index_count->instances);
    LOG_DEBUG(" %d core modules", index_count->core_modules);
    LOG_DEBUG(" %d core instances", index_count->core_instances);
    LOG_DEBUG(" %d core functions", index_count->core_functions);
    LOG_DEBUG(" %d core memories", index_count->core_memories);
    LOG_DEBUG(" %d core tables", index_count->core_tables);
    LOG_DEBUG(" %d core globals", index_count->core_globals);
    LOG_DEBUG(" %d core types", index_count->core_types);
    LOG_DEBUG(" %d functions", index_count->functions);
    LOG_DEBUG(" %d types", index_count->types);
    LOG_DEBUG(" %d values", index_count->values);
    LOG_DEBUG(" %d exports", index_count->exports);
    // Allocate memory for the Component instance + defined types
    uint32 total_size =
        (uint32)(sizeof(WASMComponentInstance)
                 + (index_count->functions
                    * sizeof(WASMComponentFunctionInstance *))
                 + (index_count->values * sizeof(WASMComponentValue *))
                 + (index_count->types * sizeof(WASMComponentTypeInstance *))
                 + (index_count->instances * sizeof(WASMComponentInstance *))
                 + (index_count->components * sizeof(WASMComponent *))
                 + (index_count->core_functions
                    * sizeof(WASMFunctionInstance *))
                 + (index_count->core_tables * sizeof(WASMTableInstance *))
                 + (index_count->core_memories * sizeof(WASMMemoryInstance *))
                 + (index_count->core_globals * sizeof(WASMGlobalInstance *))
                 + (index_count->core_types * sizeof(WASMType *))
                 + (index_count->core_instances * sizeof(WASMModuleInstance *))
                 + (index_count->core_modules * sizeof(WASMModule *))
                 + (index_count->exports * sizeof(WASMComponentExportInstance))
                 + index_count->types_total_size
                 + (index_count->defined_core_functions
                    * sizeof(WASMFunctionInstance))
                 + (index_count->defined_functions
                    * sizeof(WASMComponentFunctionInstance))
                 + (index_count->defined_core_instances
                    * sizeof(WASMModuleInstance *))
                 + (index_count->defined_instances
                    * sizeof(WASMComponentInstance *))
                 + (index_count->canon_options_funcs
                    * sizeof(WASMComponentCanonOptsInstance))
                 + (index_count->canon_options
                    * sizeof(WASMComponentCanonOptInstance)));
    WASMComponentInstance *comp_instance = wasm_runtime_malloc(total_size);
    memset(comp_instance, 0, total_size);
    // The types defined in this component will be stored in the memory region
    // right after the component instance, similar to Wasm Module implementation

    comp_instance->defined_types_size = index_count->types_total_size;
    comp_instance->defined_canon_opts_size =
        (uint32)((index_count->canon_options_funcs
                  * sizeof(WASMComponentCanonOptsInstance))
                 + (index_count->canon_options
                    * sizeof(WASMComponentCanonOptInstance)));
    comp_instance->functions =
        (WASMComponentFunctionInstance **)((uint8_t *)comp_instance
                                           + sizeof(WASMComponentInstance));
    comp_instance->values =
        (WASMComponentValue **)((uint8_t *)comp_instance->functions
                                + (index_count->functions
                                   * sizeof(WASMComponentFunctionInstance *)));
    comp_instance->types =
        (WASMComponentTypeInstance **)((uint8_t *)comp_instance->values
                                       + (index_count->values
                                          * sizeof(WASMComponentValue *)));
    comp_instance->component_instances =
        (WASMComponentInstance **)((uint8_t *)comp_instance->types
                                   + (index_count->types
                                      * sizeof(WASMComponentTypeInstance *)));
    comp_instance->components =
        (WASMComponent **)((uint8_t *)comp_instance->component_instances
                           + (index_count->instances
                              * sizeof(WASMComponentInstance *)));
    comp_instance->core_functions =
        (WASMFunctionInstance **)((uint8_t *)comp_instance->components
                                  + (index_count->components
                                     * sizeof(WASMComponent *)));
    comp_instance->core_tables =
        (WASMTableInstance **)((uint8_t *)comp_instance->core_functions
                               + (index_count->core_functions
                                  * sizeof(WASMFunctionInstance *)));
    comp_instance->core_memories =
        (WASMMemoryInstance **)((uint8_t *)comp_instance->core_tables
                                + (index_count->core_tables
                                   * sizeof(WASMTableInstance *)));
    comp_instance->core_globals =
        (WASMGlobalInstance **)((uint8_t *)comp_instance->core_memories
                                + (index_count->core_memories
                                   * sizeof(WASMMemoryInstance *)));
    comp_instance->core_types =
        (WASMType **)((uint8_t *)comp_instance->core_globals
                      + (index_count->core_globals
                         * sizeof(WASMGlobalInstance *)));
    comp_instance->core_module_instances =
        (WASMModuleInstance **)((uint8_t *)comp_instance->core_types
                                + (index_count->core_types
                                   * sizeof(WASMType *)));
    comp_instance->core_modules =
        (WASMModule **)((uint8_t *)comp_instance->core_module_instances
                        + (index_count->core_instances
                           * sizeof(WASMModuleInstance *)));
    comp_instance->exports =
        (WASMComponentExportInstance *)((uint8_t *)comp_instance->core_modules
                                        + (index_count->core_modules
                                           * sizeof(WASMModule *)));
    comp_instance->defined_types =
        (void *)((uint8_t *)comp_instance->exports
                 + (index_count->exports
                    * sizeof(WASMComponentExportInstance)));
    comp_instance->defined_core_functions =
        (WASMFunctionInstance *)((uint8_t *)comp_instance->defined_types
                                 + comp_instance->defined_types_size);
    comp_instance->defined_functions =
        (WASMComponentFunctionInstance *)((uint8_t *)comp_instance
                                              ->defined_core_functions
                                          + (index_count->defined_core_functions
                                             * sizeof(WASMFunctionInstance)));
    comp_instance->defined_instances =
        (WASMComponentInstance **)((uint8_t *)comp_instance->defined_functions
                                   + (index_count->defined_functions
                                      * sizeof(WASMComponentFunctionInstance)));
    comp_instance->defined_core_instances =
        (WASMModuleInstance **)((uint8_t *)comp_instance->defined_instances
                                + (index_count->defined_instances
                                   * sizeof(WASMComponentInstance *)));
    comp_instance->defined_canon_opts =
        (void *)((uint8_t *)comp_instance->defined_core_instances
                 + (index_count->defined_core_instances
                    * sizeof(WASMModuleInstance *)));

    /*
    Component instance and all dependant memory areas are allocated in a
    contignuous memory area, as described below

    +----------------------------------+
    | WASMComponentInstance            |
    +----------------------------------+ --> comp_instance->functions
    | < functions >                    |
    +----------------------------------+ --> comp_instance->values
    | < values >                       |
    +----------------------------------+ --> comp_instance->types
    | < types >                        |
    +----------------------------------+ --> comp_instance->component_instances
    | < component instances >          |
    +----------------------------------+ --> comp_instance->components
    | < components >                   |
    +----------------------------------+ --> comp_instance->core_functions
    | < core fuctions >                |
    +----------------------------------+ --> comp_instance->core_tables
    | < core tables >                  |
    +----------------------------------+ --> comp_instance->core_memories
    | < core memories >                |
    +----------------------------------+ --> comp_instance->core_globals
    | < core globals >                 |
    +----------------------------------+ --> comp_instance->core_types
    | < core types >                   |
    +----------------------------------+ -->
    comp_instance->core_module_instances | < core module instances >        |
    +----------------------------------+ --> comp_instance->core_modules
    | < core modules >                 |
    +----------------------------------+ --> comp_instance->exports
    | < exports >                      |
    +----------------------------------+ --> comp_instance->defined_types
    | < defined types >                |
    +----------------------------------+

*/

    // Start with initial size of 10, grow by 50% when needed
    comp_instance->table = wasm_component_table_init(10, 50);
    if (!comp_instance->table) {
        wasm_runtime_free(comp_instance);
        set_error_buf_ex(
            error_buf, error_buf_size,
            "ERROR: Failed to initialize component instance table\n");
        return NULL;
    }

    LOG_DEBUG(
        "Component instance memory allocation successfull, total size is %d\n",
        total_size);
    return comp_instance;
}

uint64
get_core_index_count(WASMInstExpr *instance_expression)
{
    uint32 idx = 0;
    uint64 total_size = 0;
    WASMComponentInlineExport *inline_expr = NULL;
    uint32 func_count = 0, table_count = 0, mem_count = 0, global_count = 0;
    for (idx = 0; idx < instance_expression->without_args.inline_expr_len;
         idx++) {
        inline_expr = instance_expression->without_args.inline_expr;
        switch (inline_expr->sort_idx->sort->core_sort) {
            case WASM_COMP_CORE_SORT_FUNC:
                func_count++;
                break;
            case WASM_COMP_CORE_SORT_TABLE:
                table_count++;
                break;
            case WASM_COMP_CORE_SORT_MEMORY:
                mem_count++;
                break;
            case WASM_COMP_CORE_SORT_GLOBAL:
                global_count++;
                break;
            default:
                break;
        }
    }
    total_size =
        offsetof(WASMModuleInstance, global_table_data.bytes)
        + (uint64)sizeof(table_elem_type_t) * table_count
        + sizeof(WASMMemoryInstance) * (uint64)mem_count
        + sizeof(WASMModuleInstanceExtra); /* Consider global data size == 0 */

    return total_size;
}

WASMComponentInstance *
wasm_component_instantiate_internal(
    WASMComponent *component,
    WASMComponentInstArgInstances *instance_expression, char *error_buf,
    uint32 error_buf_size)
{
    LOG_DEBUG("Instantiate internal");
    if (!component) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: Invalid component\n");
        return NULL;
    }
    WASMComponentIndexCount *index_count =
        wasm_component_get_index_count(component, error_buf, error_buf_size);
    if (!index_count) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: failed to retrieve component index count\n");
        return NULL;
    }

    WASMComponentInstance *comp_instance = wasm_component_instance_allocate(
        index_count, error_buf, error_buf_size);
    if (!comp_instance) {
        set_error_buf_ex(error_buf, error_buf_size,
                         "ERROR: component instance allocation failed\n");
        goto done;
    }
    if (instance_expression) {
        comp_instance->parent = instance_expression->parent;
    }
    comp_instance->component = component;
    uint32 section_idx = 0;
    WASMComponentSection *section = NULL;

    for (section_idx = 0; section_idx < component->section_count;
         section_idx++) {
        section = &component->sections[section_idx];
        switch (section->id) {
            case WASM_COMP_SECTION_CORE_CUSTOM:
                // TODO: not in scope for now
                break;
            case WASM_COMP_SECTION_CORE_MODULE:
                LOG_DEBUG("%d : --Core module section", section_idx);
                comp_instance->core_modules[comp_instance->core_modules_count] =
                    (WASMModule *)section->parsed.core_module->module_handle;
                comp_instance->core_modules_count++;
                if (comp_instance->core_modules_count
                    > index_count->core_modules) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: Core modules count exceeded in "
                                     "section %d, expected only %d modules",
                                     section_idx, index_count->core_modules);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                LOG_DEBUG("Added core module %d",
                          comp_instance->core_modules_count - 1);
                break;
            case WASM_COMP_SECTION_CORE_INSTANCE:
                LOG_DEBUG("%d : --Core instance section\n", section_idx);
                if (!wasm_resolve_core_instance(
                        section->parsed.core_instance_section, comp_instance,
                        error_buf, error_buf_size)) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: Core instance section %d "
                                     "instantiation failed\n",
                                     section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_CORE_TYPE:
                // TODO: need more info on this section (not found in any of the
                // test binaries)
                break;
            case WASM_COMP_SECTION_COMPONENT:
                LOG_DEBUG("%d : --Component section", section_idx);
                if (!section->parsed.component) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "WARNING: parsed component empty!\n");
                }
                comp_instance->components[comp_instance->components_count] =
                    section->parsed.component;
                comp_instance->components_count++;
                if (comp_instance->components_count > index_count->components) {
                    set_error_buf_ex(error_buf, error_buf_size,
                                     "ERROR: Components count exceeded in "
                                     "section %d, expected only %d components",
                                     section_idx, index_count->components);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                LOG_DEBUG("Added component %d",
                          comp_instance->components_count - 1);
                break;
            case WASM_COMP_SECTION_INSTANCES:
                LOG_DEBUG("%d : --Instance section\n", section_idx);
                if (!wasm_resolve_instance(section->parsed.instance_section,
                                           comp_instance, error_buf,
                                           error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Instance section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_ALIASES:
                LOG_DEBUG("%d : --Alias section", section_idx);
                if (!wasm_resolve_alias(section->parsed.alias_section,
                                        comp_instance, error_buf,
                                        error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Alias section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_TYPE:
                LOG_DEBUG("%d : --Type section %d", section_idx,
                          comp_instance->types_count);
                if (!wasm_resolve_types(section->parsed.type_section,
                                        comp_instance, error_buf,
                                        error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: type section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_CANONS:
                LOG_DEBUG("%d : --Canon section", section_idx);
                if (!wasm_resolve_canon(section->parsed.canon_section,
                                        comp_instance, error_buf,
                                        error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Canonical section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_START:
                // TODO: Not in scope for now
                break;
            case WASM_COMP_SECTION_IMPORTS:
                LOG_DEBUG("%d : --Import section", section_idx);
                bool import_failed = false;
                if (comp_instance->parent && instance_expression)
                    import_failed = !wasm_resolve_imports(
                        section->parsed.import_section, comp_instance,
                        instance_expression, error_buf, error_buf_size);
                else if (!comp_instance->parent)
                    import_failed = !wasm_resolve_imports_WASI(
                        section->parsed.import_section, comp_instance,
                        error_buf, error_buf_size);
                if (import_failed) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: Import section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_EXPORTS:
                LOG_DEBUG("%d : --Export section", section_idx);
                if (!wasm_resolve_exports(section->parsed.export_section,
                                          comp_instance, error_buf,
                                          error_buf_size)) {
                    set_error_buf_ex(
                        error_buf, error_buf_size,
                        "ERROR: export section %d instantiation failed\n",
                        section->id);
                    wasm_component_deinstantiate(comp_instance);
                    comp_instance = NULL;
                    goto done;
                }
                break;
            case WASM_COMP_SECTION_VALUES:
                // TODO, not in scope for now
                break;
            default:
                set_error_buf_ex(
                    error_buf, error_buf_size,
                    "FATAL ERROR: Unknown/unsupported section (id=%u)\n",
                    section->id);
                break;
        }
    }
    comp_instance->may_leave = true;
    LOG_DEBUG("Instantiation done\n");
done:
    wasm_runtime_free(index_count);
    return comp_instance;
}

WASMComponentInstance *
wasm_component_instantiate(WASMComponent *component, char *error_buf,
                           uint32 error_buf_size)
{

    return wasm_component_instantiate_internal(component, NULL, error_buf,
                                               error_buf_size);
}

void
wasm_component_deinstantiate(WASMComponentInstance *comp_instance)
{
    if (!comp_instance) {
        return;
    }
    uint32 idx = 0;

    // Free core instances
    for (idx = 0; idx < comp_instance->defined_core_instances_count; idx++) {
        if (!comp_instance->defined_core_instances[idx]->e) {
            wasm_runtime_free(
                comp_instance->defined_core_instances
                    [idx]); // This instance was generated from exports list,
                            // not from a core module
        }
        else
            wasm_deinstantiate(comp_instance->defined_core_instances[idx],
                               false);
        comp_instance->defined_core_instances[idx] = NULL;
    }

    // Free nested component instances
    for (idx = 0; idx < comp_instance->defined_instances_count; idx++) {
        if (comp_instance->defined_instances[idx]) {
            wasm_component_deinstantiate(comp_instance->defined_instances[idx]);
            comp_instance->defined_instances[idx] = NULL;
        }
    }

    // Free runtime canonical options for component functions (canon.lift)
    for (idx = 0; idx < comp_instance->defined_functions_count; idx++) {
        WASMComponentFunctionInstance *func =
            &comp_instance->defined_functions[idx];
        if (func && func->canon_options) {
            free_canonical_options(func->canon_options);
            func->canon_options = NULL;
        }
    }

    // Free runtime canonical options for core functions (canon.lower)
    for (idx = 0; idx < comp_instance->defined_core_functions_count; idx++) {
        WASMFunctionInstance *func =
            &comp_instance->defined_core_functions[idx];
        if (func && func->canon_options) {
            free_canonical_options(func->canon_options);
            func->canon_options = NULL;
        }
    }

    // Destroy the component instance table
    if (comp_instance->table) {
        wasm_component_table_destroy(comp_instance->table);
        comp_instance->table = NULL;
    }

    wasm_runtime_free(comp_instance);
    comp_instance = NULL;
}

WASMComponentFunctionInstance *
wasm_component_lookup_function(const WASMComponentInstance *component_inst,
                               const char *name)
{
    if (!component_inst || !component_inst->exports || !name)
        return NULL;

    // Iterate through exports
    uint32 index = 0;
    const char *exported_name = NULL;

    for (index = 0; index < component_inst->exports_count; index++) {
        WASMComponentExportInstance *export_inst =
            &component_inst->exports[index];

        if (export_inst->type == WASM_COMP_EXTERN_FUNC) {
            WASMComponentExportName *export_name = export_inst->export_name;

            if (export_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
                exported_name = export_name->exported.simple.name->name;
            }
            else if (export_name->tag == WASM_COMP_IMPORTNAME_VERSIONED) {
                exported_name = export_name->exported.versioned.name->name;
            }

            if (exported_name && strcmp(exported_name, name) == 0) {
                return export_inst->exp.function;
            }
        }

        if (export_inst->type == WASM_COMP_EXTERN_INSTANCE) {
            uint32 index2 = 0;
            WASMComponentInstance *extern_inst = export_inst->exp.instance;

            for (index2 = 0; index2 < extern_inst->exports_count; index2++) {
                WASMComponentExportInstance *export_inst2 =
                    &extern_inst->exports[index2];

                if (export_inst2->type == WASM_COMP_EXTERN_FUNC) {
                    WASMComponentExportName *export_name =
                        export_inst2->export_name;

                    if (export_name->tag == WASM_COMP_IMPORTNAME_SIMPLE) {
                        exported_name = export_name->exported.simple.name->name;
                    }
                    else if (export_name->tag
                             == WASM_COMP_IMPORTNAME_VERSIONED) {
                        exported_name =
                            export_name->exported.versioned.name->name;
                    }

                    if (exported_name && strcmp(exported_name, name) == 0) {
                        return export_inst2->exp.function;
                    }
                }
            }
        }
    }

    return NULL; // Function not found
}

WASMMemoryInstance *
canon_get_memory(CanonicalOptions *canon_opts)
{
    if (!canon_opts || !canon_opts->lift_lower_opts
        || !canon_opts->lift_lower_opts->lift_opts) {
        return NULL;
    }
    else {
        return canon_opts->lift_lower_opts->lift_opts->memory;
    }
}

uint32_t
wasm_runtime_call_realloc(LiftLowerContext *cx, int32_t old_ptr,
                          int32_t old_size, int32_t align, int32_t new_size)
{
    WASMFunctionInstance *realloc_func = get_realloc_func(cx);
    if (!realloc_func) {
        set_component_exception(cx, "realloc function not provided");
        return 0;
    }

    WASMExecEnv *exec_env = wasm_runtime_get_exec_env_singleton(
        (WASMModuleInstanceCommon *)realloc_func->module_instance);
    if (!exec_env) {
        set_component_exception(cx, "create singleton exec_env failed");
        return 0;
    }

    // The singleton exec_env may be changed by a nested import call, thus we
    // need to save and restore it
    WASMModuleInstanceCommon *saved_module_inst =
        wasm_runtime_get_module_inst(exec_env);
    wasm_exec_env_set_module_inst(
        exec_env, (WASMModuleInstanceCommon *)realloc_func->module_instance);

    wasm_val_t args[4] = { { .kind = WASM_I32, .of.i32 = old_ptr },
                           { .kind = WASM_I32, .of.i32 = old_size },
                           { .kind = WASM_I32, .of.i32 = align },
                           { .kind = WASM_I32, .of.i32 = new_size } };
    wasm_val_t results[1];

#ifdef OS_ENABLE_HW_BOUND_CHECK
    WASMExecEnv *saved_tls = wasm_runtime_get_exec_env_tls();
    wasm_runtime_set_exec_env_tls(NULL);
#endif
    if (!wasm_runtime_call_wasm_a(exec_env,
                                  (WASMFunctionInstanceCommon *)realloc_func, 1,
                                  results, 4, args)) {
        const char *ex = wasm_runtime_get_exception(
            (WASMModuleInstanceCommon *)realloc_func->module_instance);
#ifdef OS_ENABLE_HW_BOUND_CHECK
        wasm_runtime_set_exec_env_tls(saved_tls);
#endif
        wasm_exec_env_restore_module_inst(exec_env, saved_module_inst);
        set_component_exception(cx, ex ? ex : "realloc call failed");
        return 0;
    }
#ifdef OS_ENABLE_HW_BOUND_CHECK
    wasm_runtime_set_exec_env_tls(saved_tls);
#endif
    wasm_exec_env_restore_module_inst(exec_env, saved_module_inst);
    return results[0].of.i32;
}

WASMComponentFuncTypeInstance *
wasm_get_component_func_type(WASMExecEnv *exec_env)
{
    if (!exec_env || !exec_env->core_func
        || !exec_env->core_func->component_function
        || !exec_env->core_func->component_function->func_type) {
        return NULL;
    }
    return exec_env->core_func->component_function->func_type;
}

#endif /* WASM_ENABLE_COMPONENT_MODEL != 0*/
