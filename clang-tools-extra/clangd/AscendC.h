//===--- AscendC.h - Ascend C editor support ------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASCENDC_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_ASCENDC_H
#include "llvm/ADT/IntrusiveRefCntPtr.h"
#include "llvm/ADT/StringRef.h"
namespace llvm::vfs {
class FileSystem;
}
namespace clang {
class CompilerInvocation;
namespace clangd {
bool isAscendCFile(llvm::StringRef File);
void enableAscendC(CompilerInvocation &CI);
llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
addAscendCHeaders(const CompilerInvocation &CI,
                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS);
} // namespace clangd
} // namespace clang
#endif
