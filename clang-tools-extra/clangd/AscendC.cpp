//===--- AscendC.cpp - Ascend C editor support --------------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "AscendC.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "clang/Lex/PreprocessorOptions.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/Support/MemoryBuffer.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <initializer_list>
#include <string>

namespace clang::clangd {
bool isAscendCFile(llvm::StringRef File) {
  return llvm::sys::path::extension(File) == ".asc";
}

void enableAscendC(CompilerInvocation &CI) {
  // All consumers (preamble, completion, foreground and background ASTs) use
  // the same invocation. No driver flag enables this mode in clang.
  auto &Lang = CI.getLangOpts();
  if (!Lang.CPlusPlus || Lang.CUDA || Lang.OpenCL || Lang.HLSL)
    return;
  Lang.CceExt = true;
  Lang.NativeHalfType = true;
  Lang.NativeHalfArgsAndReturns = true;
  llvm::SmallString<128> Header(CI.getFileSystemOpts().WorkingDir);
  llvm::sys::path::append(Header, "__clangd_ascendc__", "cce_stubs.h");
  auto &Includes = CI.getPreprocessorOpts().Includes;
  Includes.insert(Includes.begin(), Header.str().str());
}

llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem>
addAscendCHeaders(const CompilerInvocation &CI,
                  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> VFS) {
  if (!CI.getLangOpts().CceExt)
    return VFS;
  auto HeadersFS = llvm::makeIntrusiveRefCnt<llvm::vfs::InMemoryFileSystem>();
  auto Overlay = llvm::makeIntrusiveRefCnt<llvm::vfs::OverlayFileSystem>(VFS);
  // Synchronize working directories before adding relative paths.
  Overlay->pushOverlay(HeadersFS);
  static const struct {
    const char *Name;
    std::initializer_list<const char *> Parts;
  } Headers[] = {
#include "AscendCHeaders.inc"
  };
  // Keep buffers in a VFS: preamble builds copy the invocation and take
  // ownership of remapped buffers. A VFS safely shares their contents across
  // preamble construction, reuse checks, completion and foreground parsing.
  llvm::SmallString<128> Base(CI.getFileSystemOpts().WorkingDir);
  llvm::sys::path::append(Base, "__clangd_ascendc__");
  for (const auto &Header : Headers) {
    llvm::SmallString<128> Path(Base);
    llvm::sys::path::append(Path, Header.Name);
    std::string Contents;
    for (const char *Part : Header.Parts)
      Contents += Part;
    HeadersFS->addFile(
        Path, 0, llvm::MemoryBuffer::getMemBufferCopy(Contents, Path.str()));
  }
  return Overlay;
}
} // namespace clang::clangd
