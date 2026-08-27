//===--- TidyCheckInfo.h - Utility queries for clang-tidy checks -*-C++-*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_TIDYCHECKINFO_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_TIDYCHECKINFO_H

#include "llvm/ADT/StringRef.h"
#include <optional>

namespace clang {
namespace clangd {

/// Returns if \p Check is a registered clang-tidy check.
/// \pre \p must not be empty, must not contain '*' or ',' or start with '-'.
bool isRegisteredTidyCheck(llvm::StringRef Check);

/// Returns if \p Check is known-fast, known-slow, or its speed is unknown.
/// By default, only fast checks will run in clangd.
std::optional<bool> isFastTidyCheck(llvm::StringRef Check);

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_TIDYCHECKINFO_H
