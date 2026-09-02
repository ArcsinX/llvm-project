//===--- ClangTidyFeatureModule.h - clang-tidy integration -----*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGTIDYFEATUREMODULE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGTIDYFEATUREMODULE_H

#include "FeatureModule.h"
#include "TidyProvider.h"

namespace clang {
namespace clangd {

/// Runs clang-tidy checks as part of clangd's AST build.
class ClangTidyFeatureModule final : public FeatureModule {
public:
  ClangTidyFeatureModule() = default;
  explicit ClangTidyFeatureModule(TidyProviderRef Provider)
      : Provider(Provider) {}

  void setProvider(TidyProviderRef Provider);

  std::unique_ptr<ASTListener> astListeners() override;

private:
  TidyProviderRef Provider;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_CLANGTIDYFEATUREMODULE_H
