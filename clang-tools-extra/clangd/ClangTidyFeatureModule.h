//===--- ClangTidyFeatureModule.h - Clang-Tidy feature module --*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGTIDYFEATUREMODULE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGTIDYFEATUREMODULE_H

#include "../clang-tidy/ClangTidyOptions.h"
#include "FeatureModule.h"
#include "TidyCheckInfo.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/FunctionExtras.h"
#include "llvm/ADT/StringRef.h"
#include <memory>
#include <vector>

namespace clang {
class ASTContext;
namespace clangd {

/// A factory to modify a \ref tidy::ClangTidyOptions.
using TidyProvider =
    llvm::unique_function<void(tidy::ClangTidyOptions &,
                               /*Filename=*/llvm::StringRef) const>;

/// A factory to modify a \ref tidy::ClangTidyOptions that doesn't hold any
/// state.
using TidyProviderRef = llvm::function_ref<void(tidy::ClangTidyOptions &,
                                                /*Filename=*/llvm::StringRef)>;

TidyProvider combine(std::vector<TidyProvider> Providers);

/// Provider that just sets the defaults.
TidyProvider provideEnvironment();

/// Provider that will enable a nice set of default checks if none are
/// specified.
TidyProvider provideDefaultChecks();

/// Provider the enables a specific set of checks and warnings as errors.
TidyProvider addTidyChecks(llvm::StringRef Checks,
                           llvm::StringRef WarningsAsErrors = {});

/// Provider that will disable checks known to not work with clangd. \p
/// ExtraBadChecks specifies any other checks that should be always
/// disabled.
TidyProvider
disableUnusableChecks(llvm::ArrayRef<std::string> ExtraBadChecks = {});

/// Provider that searches for .clang-tidy configuration files in the directory
/// tree.
TidyProvider provideClangTidyFiles(const ThreadsafeFS &);

// Provider that uses clangd configuration files.
TidyProvider provideClangdConfig();

tidy::ClangTidyOptions getTidyOptionsForFile(TidyProviderRef Provider,
                                             llvm::StringRef Filename);

/// A FeatureModule that integrates clang-tidy into clangd.
///
/// This module provides clang-tidy checks by setting up a MatchFinder
/// via beforeExecute(), and handles diagnostic filtering (NOLINT, system
/// headers, warning-as-error) in sawDiagnostic().
///
/// The module is self-configuring: it sets up providers in initialize()
/// using facilities and config. For testing, setProvider() can override
/// the defaults.
class ClangTidyFeatureModule final : public FeatureModule {
public:
  class TidyListener;

  /// Sets up default providers using facilities and config.
  /// Can be overridden after construction for testing.
  void initialize(const Facilities &F) override;

  /// Override the tidy provider (e.g. for testing or check tool).
  void setProvider(TidyProvider Provider);
  TidyProviderRef provider() const;

  std::unique_ptr<ASTListener> astListeners() override;

private:
  TidyProvider Provider;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGTIDYFEATUREMODULE_H
