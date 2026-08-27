//===--- FeatureModulesForceLinker.h - Force-link feature modules ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_FEATUREMODULESFORCELINKER_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_FEATUREMODULESFORCELINKER_H

namespace clang {
namespace clangd {

// This anchor is used to force the linker to link the ClangTidyFeatureModule.
extern volatile int ClangTidyFeatureModuleAnchorSource;
[[maybe_unused]] static int ClangTidyFeatureModuleAnchorDestination =
    ClangTidyFeatureModuleAnchorSource;

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_FEATUREMODULESFORCELINKER_H
