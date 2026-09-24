//===--- SampleFeatureModule.cpp - Example Clangd Feature Module ----------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file implements an example FeatureModule for clangd that can be
// dynamically loaded into clangd at runtime via the `-load` option:
//
//   clangd -load=/path/to/SampleFeatureModule.so
//
// When loaded, the module registers itself with clangd's FeatureModuleRegistry
// and demonstrates:
//   1. Contributing custom code action tweaks (Tweak).
//   2. Observing AST builds and diagnostics via ASTListener.
//   3. Registering custom LSP method/command handlers via LSPBinder.
//
//===----------------------------------------------------------------------===//

#include "FeatureModule.h"
#include "LSPBinder.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"

namespace clang::clangd {
namespace {

/// An example code action tweak contributed by the feature module.
class SampleTweak final : public Tweak {
public:
  const char *id() const override { return "SampleModuleTweak"; }

  bool prepare(const Selection &Sel) override {
    // This tweak is available anywhere in the file.
    return true;
  }

  Expected<Effect> apply(const Selection &Sel) override {
    return Effect::showMessage("Sample FeatureModule tweak executed!");
  }

  std::string title() const override {
    return "Execute Sample FeatureModule action";
  }

  llvm::StringLiteral kind() const override {
    return llvm::StringLiteral("quickfix");
  }
};

/// An example AST listener that observes AST construction events.
class SampleASTListener final : public FeatureModule::ASTListener {
public:
  void beforeExecute(CompilerInstance &CI) override {
    vlog("SampleFeatureModule: before AST execution");
  }

  void afterExecute(CompilerInstance &CI) override {
    vlog("SampleFeatureModule: after AST execution");
  }

  void sawDiagnostic(const clang::Diagnostic &Diag, clangd::Diag &D) override {
    vlog("SampleFeatureModule: observed diagnostic '{0}'", D.Message);
  }
};

/// The main feature module class.
class SampleFeatureModule final : public FeatureModule {
public:
  /// Contribute custom tweaks to clangd's code action list.
  void contributeTweaks(std::vector<std::unique_ptr<Tweak>> &Out) override {
    Out.emplace_back(new SampleTweak);
  }

  /// Hook into AST creation to observe AST events and diagnostics.
  std::unique_ptr<ASTListener> astListeners() override {
    return std::make_unique<SampleASTListener>();
  }

  /// Register custom LSP endpoints or update server capabilities.
  void initializeLSP(LSPBinder &Bind,
                     const llvm::json::Object &ClientCaps,
                     llvm::json::Object &ServerCaps) override {
    vlog("SampleFeatureModule: initializeLSP called");
    // Example: Bind a custom LSP method or notification if needed:
    // Bind.method("sample/customMethod", this, &SampleFeatureModule::onCustomMethod);
  }
};

// Register the module with clangd's FeatureModuleRegistry.
// When clangd loads this shared library via -load, this static initializer
// will run and add the module to clangd's registry.
static FeatureModuleRegistry::Add<SampleFeatureModule>
    X("sample-feature-module",
      "Example feature module demonstrating dynamic loading into clangd");

} // namespace
} // namespace clang::clangd
