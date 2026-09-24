# Sample Dynamic FeatureModule for Clangd

This example demonstrates how to implement, build, and load a **dynamic `FeatureModule`** plugin into `clangd`.

## Background

Clangd supports vertical extensions via `FeatureModule`s. Previously, `FeatureModule`s could only be linked statically into the `clangd` binary. With dynamic loading support, feature modules can be compiled into independent shared libraries (`.so` / `.dylib` / `.dll`) and loaded at runtime into `clangd` via the standard LLVM `-load` command-line option.

When a plugin shared library is loaded:
1. Static initializers in the shared library run.
2. `FeatureModuleRegistry::Add<MyModule> X("name", "description")` registers the module with `FeatureModuleRegistry`.
3. Clangd queries `FeatureModuleSet::fromRegistry()`, which instantiates all registered feature modules and integrates them into the LSP server.

## Features demonstrated in this example

- **Tweaks (Code Actions)**: Contributes a custom code action tweak (`SampleModuleTweak`) that appears in editors.
- **AST Listeners**: Observes AST building lifecycle events (`beforeExecute`, `afterExecute`) and diagnostics (`sawDiagnostic`).
- **LSP Bindings**: Demonstrates how to register custom LSP methods, notifications, or commands via `LSPBinder` and modify server capabilities in `initializeLSP`.

## How to Build

### In-tree Build

To build this example as part of LLVM / Clang:

1. Configure LLVM with examples enabled:
   ```bash
   cmake -B build -S llvm -DCLANGD_BUILD_EXAMPLES=ON [other options...]
   ```
2. Build the plugin:
   ```bash
   ninja -C build SampleFeatureModule
   ```
   The shared library will be built in `build/lib/SampleFeatureModule.so` (or `.dylib` on macOS).

### Out-of-tree Build

You can also build feature modules against an installed LLVM / Clang toolchain:

```bash
mkdir build && cd build
cmake -DCMAKE_PREFIX_PATH=/path/to/llvm/installation ..
cmake --build .
```

## How to Use with Clangd

### Check Mode

You can test that your module is loaded and functioning using `clangd --check`:

```bash
clangd -check=test.cpp -load=/path/to/SampleFeatureModule.so -log=verbose
```

Output will show:
```
Loaded plugin: /path/to/SampleFeatureModule.so
Adding feature module 'sample-feature-module' (Example feature module demonstrating dynamic loading into clangd)
...
    tweak: SampleModuleTweak
```

### Editor Integration

Pass `-load=/path/to/SampleFeatureModule.so` to the `clangd` arguments in your editor configuration:

- **VS Code** (`settings.json`):
  ```json
  "clangd.arguments": [
    "--load=/path/to/SampleFeatureModule.so"
  ]
  ```
- **Environment Variable**:
  You can also pass arguments via the `CLANGD_FLAGS` environment variable:
  ```bash
  export CLANGD_FLAGS="--load=/path/to/SampleFeatureModule.so"
  ```
