# WAMR Component Model support

## Introduction

A WebAssembly (Wasm) component is an abstraction layer built over [standard WebAssembly](https://webassembly.github.io/spec/core/index.html) (often called WebAssembly Core). It is designed to enrich the exposed type system and improve interoperability across different programming languages and libraries. More details can be found [here](https://component-model.bytecodealliance.org/).

To distinguish between the two layers, the Component Model specification refers to standard WebAssembly entities by prefixing them with the word "core" (e.g., core modules, core functions, core types).

In short, a Wasm component uses WIT—an [Interface Definition Language](https://en.wikipedia.org/wiki/Interface_description_language) (IDL)—to define its public-facing interfaces. It then bundles one or more Wasm core modules to implement the underlying logic behind those interfaces.

WAMR implements binary parsing for the WebAssembly [Component Model](https://github.com/WebAssembly/component-model) proposal. The parser handles the component binary format as defined in the [Binary.md](https://github.com/WebAssembly/component-model/blob/main/design/mvp/Binary.md) specification, covering all 13 section types with validation and error reporting.

References:
- [Component Model design repository](https://github.com/WebAssembly/component-model)
- [Component Model binary format specification](https://github.com/WebAssembly/component-model/blob/main/design/mvp/Binary.md)
- [Canonical ABI specification](https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md)
- [Build WAMR vmcore](./build_wamr.md) for build flag reference

## Build

Enable the feature by setting the CMake flag `WAMR_BUILD_COMPONENT_MODEL` (enabled by default). This defines the C preprocessor macro `WASM_ENABLE_COMPONENT_MODEL=1`.

```cmake
set (WAMR_BUILD_COMPONENT_MODEL 1)
include (${WAMR_ROOT_DIR}/build-scripts/runtime_lib.cmake)
add_library(vmlib ${WAMR_RUNTIME_LIB_SOURCE})
```

Or pass it on the cmake command line:

```bash
cmake -DWAMR_BUILD_COMPONENT_MODEL=1 ..
```

## Component vs core module

A component binary shares the same magic number (`\0asm`) as a core WebAssembly module but is distinguished by its header fields:

|               | Core module   | Component     |
|---------------|---------------|---------------|
| magic         | `\0asm`       | `\0asm`       |
| version       | `0x0001`      | `0x000d`      |
| layer         | `0x0000`      | `0x0001`      |

WAMR uses `wasm_decode_header()` to read the 8-byte header and `is_wasm_component()` to determine whether a binary is a component or a core module.

## Binary parsing overview

The binary parser takes a raw component binary buffer and produces a `WASMComponent` structure that holds the decoded header and an array of parsed sections. The diagram below illustrates the high-level parsing flow:

<center><img src="./pics/binary_parser_hld.png" width="85%" height="85%"></img></center>

The parsing proceeds as follows:

1. **Header decode** -- `wasm_decode_header()` reads the 8-byte header (magic, version, layer) and populates a `WASMHeader` struct.
2. **Section loop** -- the parser iterates over the binary, reading a 1-byte section ID and a LEB128-encoded payload length for each section.
3. **Section dispatch** -- based on the section ID, the parser delegates to a dedicated per-section parser. Each parser decodes the section payload into a typed struct, validates its contents (UTF-8 names, index bounds, canonical options), and reports how many bytes were consumed.
4. **Core module delegation** -- when a Core Module section (0x01) is encountered, the parser delegates to the existing WAMR core module loader (`wasm_runtime_load_ex()`) to parse the embedded module.
5. **Recursive nesting** -- when a Component section (0x04) is encountered, the parser calls itself recursively with an incremented depth counter. Recursion depth is capped at 100.
6. **Result** -- on success, the `WASMComponent` struct holds the header and a dynamically-sized array of `WASMComponentSection` entries, each containing the raw payload pointer and a typed union with the parsed result.

### Section types

The component binary format defines 13 section types:

| ID   | Section          | Spec reference                    |
|------|------------------|-----------------------------------|
| 0x00 | Core Custom      | custom section (name + data)      |
| 0x01 | Core Module      | embedded core wasm module         |
| 0x02 | Core Instance    | core module instantiation         |
| 0x03 | Core Type        | core func types, module types     |
| 0x04 | Component        | nested component (recursive)      |
| 0x05 | Instance         | component instance definitions    |
| 0x06 | Alias            | export, core export, outer aliases|
| 0x07 | Type             | component type definitions        |
| 0x08 | Canon            | canonical lift/lower/resource ops |
| 0x09 | Start            | component start function          |
| 0x0A | Import           | component imports                 |
| 0x0B | Export           | component exports                 |
| 0x0C | Value            | value definitions                 |

### Current limitations

- **Core Type (0x03)**: only `moduletype` is supported; WebAssembly GC types (`rectype`, `subtype`) are rejected.
- **Canon (0x08)**: `async` and `callback` canonical options are rejected; all other canonical operations are supported.

## Source layout

All component model sources reside in `core/iwasm/common/component-model/`:

```
component-model/
  iwasm_component.cmake              # CMake build configuration
  wasm_component.h                   # type definitions, enums, struct declarations
  wasm_component.c                   # entry point: section dispatch and free
  wasm_component_helpers.c           # shared utilities: LEB128, names, value types
  wasm_component_validate.c          # validation: UTF-8, index bounds, canon options
  wasm_component_validate.h          # validation declarations
  wasm_component_core_custom_section.c   # section 0x00
  wasm_component_core_module_section.c   # section 0x01
  wasm_component_core_instance_section.c # section 0x02
  wasm_component_core_type_section.c     # section 0x03
  wasm_component_component_section.c     # section 0x04
  wasm_component_instances_section.c     # section 0x05
  wasm_component_alias_section.c         # section 0x06
  wasm_component_types_section.c         # section 0x07
  wasm_component_canons_section.c        # section 0x08
  wasm_component_start_section.c         # section 0x09
  wasm_component_imports_section.c       # section 0x0A
  wasm_component_exports_section.c       # section 0x0B
  wasm_component_values_section.c        # section 0x0C
  wasm_component_export.c               # export runtime helpers
  wasm_component_export.h               # export declarations
```

## Component instantiation overview

The component instantiation takes a `WASMComponent` structure produced by the binary parser and outputs a `WASMComponentInstance` recursive structure that contains 12 fixed size arrays of component sort index spaces, as defined in [Explainer.md](https://github.com/WebAssembly/component-model/blob/main/design/mvp/Explainer.md):


|      | Index space              | Data type                       |
|------|--------------------------|---------------------------------|
| 1    | component functions      | `WASMComponentFunctionInstance` |
| 2    | component values         | `WASMComponentValue`            |
| 3    | component types          | `WASMComponentTypeInstance`     |
| 4    | component instances      | `WASMComponentInstance`         |
| 5    | components               | `WASMComponentFunctionInstance` |
| 6    | core functions           | `WASMFunctionInstance`          |
| 7    | core tables              | `WASMTableInstance`             |
| 8    | core memories            | `WASMMemoryInstance`            |
| 9    | core globals             | `WASMGlobalInstance`            |
| 10   | core types               | `WASMType`                      |
| 11   | core module instaces     | `WASMModuleInstance`            |
| 12   | core modules             | `WASMModule`                    |

In adition, `WASMComponentInstance` also keeps track of component exports and WASI arguments information.

These index spaces are populated by iterating over the `WASMComponent` sections in order and resolving them based on section type as follows:

1. **Core module section** -- adds definition of a `WASMModule` to the **core modules** index space
2. **Core instance section** -- adds a `WASMModuleInstance` to the **core module instances** index space by either:
      - instantiating a previously defined `WASMModule` (by supplying a set of named arguments which satisfy all of its named imports) OR 
      - creating one from a predefined list of exports
3. **Core type section** -- adds a standart `WASMType` defintion to the **core types** index space
4. **Component section** -- adds a `WASMComponent` definition to the **components** index spaces
5. **Instance section** -- adds a `WASMComponentInstance` to the **component instances** index spaces by either:
      - instantiating a previously defined  `WASMComponent`(by supplying a set of named arguments which satisfy all of its named imports) OR 
      - creating one from a predefined list of exports
6. **Alias section** -- populates a new index space entry from a another previusly defined one, as either:
      - export alias -- adds an index space entry for a `WASMComponentFunctionInstance`, `WASMComponentTypeInstance` or `WASMComponentValue` from the exports of a previously defined `WASMComponentInstance`
      - core export alias -- adds an index space entry for a `WASMFunctionInstance`, `WASMType`, `WASMTableInstance` or `WASMGlobalInstance` from the exports of a previously defined `WASMModuleInstance`
      - outer alias -- adds a new index space entry to a nested inner `WASMComponentInstance` from a previously defined index space entry from an enclosing outer `WASMComponentInstance` 
7. **Type section** -- adds a `WASMComponentTypeInstance` component type, as defined by [Explainer.md type definitions](https://github.com/WebAssembly/component-model/blob/main/design/mvp/Explainer.md#type-definiti)
8. **Canon section** -- adds a `WASMFunctionInstance` or `WASMComponentFunctionInstance` based on a canonical lift or lower definition, as defined in [CanonicalABI.md](https://github.com/WebAssembly/component-model/blob/main/design/mvp/CanonicalABI.md)
9. **Start section** -- UNSUPORTED FOR NOW
10. **Import section** -- fills index space entry by resolving the required imports of the component, either:
      - from the exports of previously defined `WASMComponentInstance`s
      - from WASI_P2 libraries, as defined by the [interfaces](https://github.com/WebAssembly/WASI/tree/main/proposals):
11. **Export section** -- add elements to a list of `WASMComponentExportInstance` from a defined index space entry element
12. **Value section** -- UNSUPORTED FOR NOW

## Canonical ABI overview

Canonical ABI defines the rules to convert between the values and functions of components in the `Component Model` and the values and functions of modules in `Core WebAssembly`.
The Canonical ABI specifies, for each component function signature, a corresponding core function signature and the process for reading component-level values into and out of linear memory.

Most Canonical ABI definitions depend on some ambient information which is established by the `canon lift` or `canon lower`.

- `canon lift`: Upgrades a core WebAssembly function into a component-level function, mapping core Wasm types (like i32, f64) into higher-level WIT types (like strings, records, lists).
- `canon lower`: Downgrades a component-level function into a core WebAssembly function, translating high-level WIT types back into low-level core Wasm representations and managing the associated memory allocation.

Intermediate Representation: wit_value_t
To manage the complex translation process between high-level WIT types and low-level Wasm memory arrays, WAMR introduces an intermediate representation (IR) abstraction known as wit_value_t.

Instead of directly translating bytes from Wasm memory into native C variables and vice versa, Canonical ABI lifting and lowering operations interact strictly with wit_value_t data structures.

During a lower operation (Component -> Core), host-provided arguments are packaged as wit_value_t structures, which WAMR then flattens and copies into the core Wasm linear memory.

During a lift operation (Core -> Component), the raw memory segments returned by the Wasm module are parsed and wrapped into wit_value_t structures before being handed over to the host environment.

This C-based struct acts as a universal container capable of representing any defined WIT type (e.g., primitives, variants, records, lists), heavily simplifying the type-checking and memory-parsing logic within the runtime.

### Execution Model: Tasks and Subtasks

Currently, the Canonical ABI operations in WAMR are implemented to run strictly synchronously. Threads are not utilized in the translation or execution pipeline.

However, to align with the Canonical ABI specifications and future-proof the architecture for the WebAssembly `async` proposal, WAMR manages ABI operations using a **Task** and **Subtask** abstraction to describe the behavior of the embedder (the runtime):

* **Tasks:** Created each time a component's **exported** function is called by the host. According to the specification, this theoretically spawns a new thread. In WAMR's current synchronous mode, this simply executes on the current thread, but the task boundary is maintained.
* **Subtasks:** Created symmetrically when a component calls an **imported** function (a host function or another component's function), representing a nested execution context within the parent task.

In the current implementation, all tasks and subtasks are executed immediately in a blocking, synchronous fashion. The infrastructure is intentionally designed this way so that state machines can easily be introduced later, allowing tasks to yield and resume without requiring a full structural rewrite when asynchronous Component Model features are officially supported.

The WebAssembly Component Model bridges two distinct type systems: the rich, high-level Component Model (strings, records, variants) and the low-level Core WebAssembly (restricted to `i32`, `i64`, `f32`, `f64`, and linear memory). The Canonical ABI defines the exact rules for translating between these two worlds.

**Core Concepts: Lifting and Lowering**
To convert data across the ABI boundary, WAMR relies on two fundamental operations:
* **Lifting (Memory → Component):** Reads values from the core Wasm linear memory and converts them into rich component types. For example, lifting translates a memory pointer and a length integer into a high-level string.
* **Lowering (Component → Memory):** Writes rich component values into the core Wasm linear memory. For example, lowering takes a high-level string, writes its bytes into memory, and passes the resulting pointer and length to the core module.

**Four-Layer Architecture**
WAMR implements the Canonical ABI using a strict, bottom-up four-layer architecture to ensure type safety and separation of concerns:
1.  **Layer 1: Raw Memory Operations (`load` / `store`):** The lowest level handles direct byte reads and writes against the `WASMMemoryInstance`. It performs strict bounds checking and manages primitive integer/float conversions.
2.  **Layer 2: Individual Value Conversion (`lift_flat` / `lower_flat`):** Converts single values. It processes primitive types directly and delegates complex types (like strings or records) down to Layer 1.
3.  **Layer 3: Multi-value Orchestration (`lift_flat_values` / `lower_flat_values`):** Manages multiple arguments or return values simultaneously. This layer is responsible for determining whether to pass data via registers or memory (the flattening optimization) and triggers memory allocation (`realloc`) when required.
4.  **Layer 4: Function Wrappers (`canon_lift` / `canon_lower`):** The highest level intercepts cross-boundary function calls. It orchestrates the full lifecycle: lowering parameters, executing the target function, lifting the results, and returning them.

**The Flattening Optimization**
To maximize execution speed, WAMR implements the Canonical ABI's "flattening" optimization rule.
* **Small payloads** (up to 16 parameters or 1 result in sync mode) are "flattened" and passed directly as individual core Wasm function parameters (e.g., `i32`, `i64`, `f32`, or `f64` values on the Wasm stack). This avoids the overhead of linear memory allocation. While WebAssembly itself is a stack-based machine without registers, passing flat parameters allows AOT or JIT compilers to efficiently optimize and map these values directly into native machine registers during execution.
* **Large payloads** (like records exceeding the parameter limit) bypass this flattening process. Instead, they are written to linear memory, and only a single `i32` pointer to that memory block is passed on the stack.

**Context and Memory Management**
To keep the layers modular and future-proof for asynchronous execution, configuration is passed through a tiered context structure (`LiftLowerContext`). Operations that only read memory use a base `LiftOptions` context, while operations requiring memory modification use `LiftLowerOptions` (which includes the `realloc` allocator). The topmost function wrappers use a full `CanonicalOptions` context, which manages execution state (sync/async) and callbacks.
