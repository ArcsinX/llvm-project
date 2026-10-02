# Ascend C support in clangd

This checkout provides an Ascend C **editor compatibility mode**. It supports
parsing and ordinary C++ editor features for kernels; it does not implement an
Ascend compiler or claim complete conformance to Huawei's language extensions.

## Specification and assessment

The public specification material is Huawei's CANN Ascend C Operator Development
Guide, particularly its language extension layer. No separate AscendC language
specification documentation or complete formal language standard was found. The
documentation is versioned and some properties also depend on processor
architecture and the installed compiler headers.

The reviewed CANN 9.0.X and 9.1.X pages are:

- [SIMD built-in keywords](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/900/programug/Ascendcopdevg/atlas_ascendc_10_10053.html):
  function qualifiers, distinct address spaces, SIMD/SIMT VF annotations,
  predefined architecture macros and built-in variables.
- [SIMT built-in keywords](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/900/programug/Ascendcopdevg/atlas_ascendc_10_10054.html):
  thread/grid variables, scalar and short vector types, supported operations,
  four-argument kernel launches and launch bounds.
- [Kernel function](https://www.hiascend.com/doc_center/source/en/CANNCommunityEdition/900/programug/Ascendcopdevg/atlas_ascendc_10_0014.html):
  entry functions, GM pointers and heterogeneous launch syntax.

- [CANN 9.1.X SIMD keywords](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/programug/Ascendcopdevg/docs/en/guide/programming_guide/language_extension/SIMD-BuiltIn_keyword.md):
  current three-argument launch syntax takes a dynamic UB byte count, with
  defaults for the byte count and stream; older compiler wrappers can instead
  expose an SM descriptor pointer.
- [CANN 9.1.X SIMD/SIMT hybrid keywords](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/programug/Ascendcopdevg/docs/en/guide/programming_guide/language_extension/simd_and_simt_hybrid_programming_builtin_keyword.md):
  VF function call and parameter restrictions.
- [CANN 9.1.X function restrictions](https://www.hiascend.com/document/detail/en/CANNCommunityEdition/910/programug/Ascendcopdevg/docs/en/guide/technical_appendix/cpp_standard_support/syntax_restrictions/function.md):
  kernel parameter constraints and SIMT restrictions. Some restrictions differ
  between documentation pages, reinforcing the need to select an SDK version.

These specify more than the
[DeepSeek reference](https://github.com/deepseek-ai/clangd-ascend) implements.
The reference is a useful starting point for frontend compatibility and typed
intrinsic declarations. Its shipped configuration erases execution annotations,
replaces many compiler intrinsics with no-op macros, hardcodes a C310/3510
architecture profile and enables frontend extensions by default. Consequently it
cannot validate all Ascend rules or execute kernels correctly. Its build script
pins a different LLVM revision; the patches cannot be applied verbatim here.

## Implementation and differences from the reference

- `LangOptions::CceExt` defaults to **false**. No Clang driver or cc1 option
  enables it. clangd enables it through its invocation API only.
- clangd recognizes `.asc`, selects C++, and adds an internal marker that also
  survives command inference for headers. Define `__CLANGD_ASCENDC__` in a clangd
  compile command to opt in for other filenames, such as legacy `.cpp` kernels.
- The compatibility headers are embedded in clangd and injected as virtual
  headers through a virtual filesystem overlay. This covers preambles,
  completions and index parses without a separate installation of editor stub
  headers, and avoids remapped-buffer ownership problems when Clang copies
  invocations for preamble construction.
- The parser recognizes `<<<...>>>` only in CUDA or Ascend editor mode.
  Execution configurations resolve the **overload set** of the SDK's global
  `__cce_rtConfigureCall` rather than a single cached declaration. Kernel and
  configuration arguments remain in the AST and are type checked. Invalid
  configurations preserve diagnostics without passing recovery expressions to
  the kernel-call AST constructor. Standalone
  fallback declarations cover SIMD dynamic UB byte counts (including defaults),
  legacy descriptor pointers, and four-argument SIMT forms;
  SDK declarations determine supported forms when SDK headers are available.
- Eight distinct builtin types support printing, overload resolution, type
  traits, vectors, symbol identifiers and preamble serialization:
  `__hif8`, `__hif4x2`, `__fp8e4m3`, `__fp8e5m2`, `__fp8e6m2`, `__fp8e8m0`,
  `__fp4e2m1x2`, `__fp4e1m2x2`. `__cce_half` aliases Clang's half type.
  The keywords are registered only in Ascend editor mode. E4M3 uses the finite
  format with maximum value 448 specified by CANN; the reference used a different
  APFloat format.
- New serialized type IDs are appended after the existing IDs, preserving
  existing predefined type numbering in this newer LLVM checkout.
- Complete `[aicore]`, `[aicpu]`, `[host]`, and comma-separated combinations
  are accepted in declaration specifiers only in Ascend mode. Lookahead checks
  the entire annotation before consuming it, protecting C++ lambda captures.
- Unknown `clang_builtin_alias(__builtin_cce_...)` annotations are ignored
  only in Ascend mode, preserving the typed function declaration without
  attaching an unresolved builtin alias to the AST.
- Address spaces use distinct Clang `address_space(N)` types. Automatic
  address-qualified buffers and string literal conversion to target address
  space pointers are permitted only in Ascend mode. Ordinary C++ keeps its
  previous diagnostics.
- Ascend builtin type names are offered in type keyword completion only in
  Ascend mode; the reference did not add completion candidates for these types.
- The shared printf diagnostic type switch also handles the added types; this
  was missing from the reference patch.
- AArch64 is not advertised as an Ascend target. Target/core macros come from
  the user's command/configuration rather than an assumed device architecture.
- Focused clangd tests exercise parsing, errors, scope isolation, preamble
  serialization, navigation, hover and completion.

The MIT editor headers and the Apache-2.0 WITH LLVM-exception frontend patches
retain their respective provenance. See `AscendC/NOTICE` and `AscendC/LICENSE.txt`.
No CANN SDK snapshot is included.

## Coverage

| Feature | Editor support and limits |
| --- | --- |
| C++ syntax, templates, classes | Clang's existing support, including inside kernels |
| Kernel launch syntax | Three-argument SIMD and four-argument SIMT parsing; SDK overloads can supply additional forms |
| Launch/kernel arguments | Ordinary overload resolution and type/count diagnostics; hardware limits are not checked |
| Function qualifiers | `__global__`, `__aicore__`, `__aicpu__`, `__host__`, `__cube__`, `__vector__`, `__mix__` parse through compatibility macros |
| Scheduling and VF annotations | `__schedmode__`, `__simd_vf__`, `__simd_callee__`, `__simt_vf__`, `__simt_callee__`, `__launch_bounds__`, `__maxnreg__` accepted syntactically |
| Address spaces | `__gm__`, `__ubuf__`, `__ca__`, `__cb__`, `__cc__`, `__cbuf__`, `__fbuf__`, `__ssbuf__`, `__biasbuf__`; `__private__` maps to default space |
| Small floating formats | Distinct types and byte sizes; several formats retain the reference's approximate constant evaluation semantics |
| Half and bfloat16 | Existing Clang half/bfloat types, with native half arguments enabled in editor mode |
| SIMT dimensions | `dim3`, `gridDim`, `blockDim`, `blockIdx`, `threadIdx`, `warpSize`; fallback declarations when no SDK is available |
| Tensor/pipeline/operator APIs | Supplied by the installed CANN SDK, then handled as C++ declarations |
| Typed low-level intrinsics | Reference compatibility declarations plus SDK declarations provide signatures and normal editor features |
| Unknown compiler intrinsics | The reference no-op macro catalogue is retained for SDK compatibility, without intrinsic semantics or argument checking |
| Navigation, hover, completion, indexing | Standard clangd features operate on the resulting AST |

Not implemented: host/device call restrictions, all kernel declaration rules
(including AI CPU's special return and parameter rules), exact special floating
format arithmetic and packed FP4 operations, complete architecture-dependent
pointer layouts/casts, synthesized architecture resource constants such as
`ASC_UB_SIZE`, register/instruction semantics, scheduling/synchronization
validation, NPU code generation, runtime lowering, linkage or execution. Erased
qualifiers and no-op intrinsic macros mean some invalid Ascend programs may be
accepted, and macro arguments may be absent from the AST. Native CANN APIs still
need the correct SDK version, include directories and architecture definitions.

## Configuration

Use the rebuilt `build/bin/clangd`. `.asc` files are enabled automatically.
For SDK projects, retain normal project include paths and provide CANN compiler
headers on the include path. This example assumes compilation commands run from
the project root and an SDK header tree is available under `third_party/cann`:

```yaml
If:
  PathMatch: .*\.asc
CompileFlags:
  Add:
    - -std=c++20
    - -Ithird_party/cann/include
    - -Ithird_party/cann/asc/include
    - -isystemthird_party/cann/tools/bisheng_compiler/lib/clang/15.0.5/include
```

Include paths are relative to each compilation command's working directory,
which can differ from the directory containing `.clangd`. Adjust these relative
paths for your build layout. Paths, compiler version and architecture definitions
must match the actual SDK and selected processor. The reference installation
script lists further include paths used by its C310 setup. Put compiler-only unsupported flags in
`CompileFlags.Remove` as needed. In particular, an NPU target triple is not
recognized by this host frontend; remove `--target`/`-target` from the clangd
command if the compilation database supplies one. clangd does not infer a
complete SDK setup from a `bisheng` executable.

For C++ kernels or headers opened without an inferred Ascend command:

```yaml
If:
  PathMatch: (kernels/.*\.(cpp|h|hpp))
CompileFlags:
  Add: [-D__CLANGD_ASCENDC__=1]
```

The marker is interpreted by clangd only; passing it to `clang` does not enable
Ascend frontend extensions. It does not select an NPU architecture.

## Why Clang source changes are needed

clangd shares Clang's lexer, parser, semantic analyzer, AST and serialization.
There is no separate clangd C++ parser. Macros can make many annotations and API
headers parseable with unmodified Clang, and CUDA mode can approximate launch
syntax, but CUDA has different execution rules and launch signatures. Macros
cannot introduce new builtin type identities or change C++ `<<<...>>>` grammar.
Rewriting source also compromises diagnostics, navigation and source ranges.

Accurate native syntax/type support therefore needs changes in the shared
frontend. Those changes can be dormant outside clangd, as in this implementation.
Changing these libraries does **not** supply an Ascend compiler: a working
compiler would also need target/ABI support, intrinsic definitions and lowering,
code generation and the CANN runtime/toolchain integration.

## Validation

The build and checks completed on this checkout:

- `ninja -C build -j 6 clangd ClangdTests`: succeeded.
- All **13** `AscendC.*` tests passed.
- **441** existing clangd regression tests passed across command handling,
  invocation construction, preambles, parsing, diagnostics, completion, hover,
  background indexing, symbol collection, references and navigation. Two tests
  in the selected suites are disabled upstream and were not run.
- The rebuilt `clangd --check` completed with **0 errors** on a standalone
  `.asc` fixture combining templates, serialized types, the E4M3 maximum, VF
  annotations, address-qualified buffers and SIMD/SIMT launch forms.
- `git diff --check`: clean.

Run the focused tests with:

```sh
build/tools/clang/tools/extra/clangd/unittests/ClangdTests --gtest_filter='AscendC.*'
```

See the focused `AscendC.*` tests in `unittests/AscendCTests.cpp`. They include a
preamble round trip, both launch forms, all eight small floating types, positive
and negative address space cases, ordinary C++ isolation, explicit opt-in,
navigation/hover, symbol indexing, preamble reuse and builtin type completion.

A full CANN SDK is not present on this machine. Compatibility with its actual
headers and real Ascend projects must be validated on a CANN installation;
passing the standalone tests is not evidence of complete SDK or specification
coverage.
