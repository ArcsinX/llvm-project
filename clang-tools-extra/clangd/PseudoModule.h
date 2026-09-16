//===--- PseudoModule.h - Pseudo-parser fallback feature module ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDOMODULE_H
#define LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDOMODULE_H

#include "FeatureModule.h"
#include "GlobalCompilationDatabase.h"
#include "Protocol.h"
#include "XRefs.h"
#include "support/ThreadsafeFS.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/VirtualFileSystem.h"
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

// Anchor used to force the linker to link PseudoModule into executables.
extern volatile int PseudoModuleAnchorSource;
[[maybe_unused]] static int PseudoModuleAnchorDestination =
    PseudoModuleAnchorSource;

/// Feature module that uses clang-pseudo (GLR C++ pseudo-parser) to provide
/// syntax-based tooling features (such as document symbols, semantic selection,
/// and folding ranges) without requiring valid compilation commands, headers, or
/// complete types.
///
/// This serves as an approximate / fallback parser when standard Clang AST
/// cannot be built (e.g. missing/incorrect compilation flags, or severely broken code).
class PseudoModule final : public FeatureModule {
public:
  PseudoModule();
  ~PseudoModule() override;

  void *typeId() const override;

  void initializeLSP(LSPBinder &Bind,
                     const llvm::json::Object &ClientCaps,
                     llvm::json::Object &ServerCaps) override;

  std::unique_ptr<ASTListener> astListeners() override;

  bool blockASTBuild(llvm::StringRef File) const override { return PseudoOnly; }

  void setEnabled(bool E) { Enabled = E; }
  bool isEnabled() const { return Enabled; }

  void setPseudoOnly(bool P) {
    PseudoOnly = P;
    if (P)
      Enabled = true;
  }
  bool isPseudoOnly() const { return PseudoOnly; }

  /// Parse C++ code using clang-pseudo and extract hierarchical DocumentSymbols.
  llvm::Expected<std::vector<DocumentSymbol>>
  getDocumentSymbols(llvm::StringRef Code);

  /// Parse C++ code using clang-pseudo and compute semantic SelectionRanges.
  llvm::Expected<std::vector<SelectionRange>>
  getSemanticRanges(llvm::StringRef Code,
                    llvm::ArrayRef<Position> Positions);

  /// Parse C++ code using clang-pseudo and compute folding ranges.
  llvm::Expected<std::vector<FoldingRange>>
  getFoldingRanges(llvm::StringRef Code, bool LineFoldingOnly);

  /// Locate the definition / declaration of the symbol at \p Pos within \p Code.
  llvm::Expected<std::vector<LocatedSymbol>>
  locateSymbolAt(PathRef File, llvm::StringRef Code, Position Pos);

  /// Find all references to the symbol at \p Pos within \p Code.
  llvm::Expected<ReferencesResult>
  findReferences(PathRef File, llvm::StringRef Code, Position Pos,
                 uint32_t Limit = 0);

  std::string getDocument(PathRef File);

  void onGoToDefinition(const TextDocumentPositionParams &Params,
                        Callback<std::vector<Location>> Reply);
  void onGoToDeclaration(const TextDocumentPositionParams &Params,
                         Callback<std::vector<Location>> Reply);
  void onReference(const ReferenceParams &Params,
                   Callback<std::vector<ReferenceLocation>> Reply);
  void onDocumentHighlight(const TextDocumentPositionParams &Params,
                           Callback<std::vector<DocumentHighlight>> Reply);
  void onDocumentSymbol(const DocumentSymbolParams &Params,
                        Callback<llvm::json::Value> Reply);
  void onSelectionRange(const SelectionRangeParams &Params,
                        Callback<std::vector<SelectionRange>> Reply);
  void onFoldingRange(const FoldingRangeParams &Params,
                      Callback<std::vector<FoldingRange>> Reply);
  void onHover(const TextDocumentPositionParams &Params,
               Callback<std::optional<Hover>> Reply);

  struct IncludeDirective {
    std::string Written;
    bool IsAngled = false;
    int HashLine = -1;
  };

  enum class DeclKind : uint8_t {
    Unknown,
    Variable,
    Parameter,
    Function,
    Constructor,
    Class,
    Enum,
    EnumValue,
    TypeAlias,
    Namespace,
    TemplateParam,
    Concept,
  };

  static inline bool isTypeDecl(DeclKind K) {
    return K == DeclKind::Class || K == DeclKind::Enum ||
           K == DeclKind::TypeAlias || K == DeclKind::TemplateParam ||
           K == DeclKind::Namespace || K == DeclKind::Concept;
  }

  static inline bool isValueDecl(DeclKind K) {
    return K == DeclKind::Variable || K == DeclKind::Parameter ||
           K == DeclKind::EnumValue;
  }

  static inline bool isFunctionDecl(DeclKind K) {
    return K == DeclKind::Function || K == DeclKind::Constructor;
  }

  struct HeaderDecl {
    std::string Name;
    Range NameRange;
    Range ScopeRange;
    std::string EnclosingScope;
    DeclKind Kind = DeclKind::Unknown;
  };

  struct HeaderInfo {
    std::string Path;
    std::vector<HeaderDecl> Decls;
    std::vector<IncludeDirective> Includes;
  };

  void setCompilationDatabaseForTesting(const GlobalCompilationDatabase *CDB) {
    TestCDB = CDB;
  }
  void setFSForTesting(const ThreadsafeFS *FS) { TestFS = FS; }

  std::vector<std::string> getIncludeDirectories(PathRef File) const;
  static std::vector<IncludeDirective> extractIncludes(llvm::StringRef Code);
  std::string resolveHeader(const IncludeDirective &Inc,
                            llvm::StringRef CurrentDir,
                            llvm::ArrayRef<std::string> IncludeDirs,
                            llvm::vfs::FileSystem &FS) const;

  std::shared_ptr<const HeaderInfo>
  getHeaderInfo(llvm::StringRef HeaderPath, llvm::vfs::FileSystem &FS);

private:
  const GlobalCompilationDatabase *getCDB() const;
  llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> getFS() const;

  void onCustomPseudoSymbols(const DocumentSymbolParams &Params,
                             Callback<std::vector<DocumentSymbol>> Reply);

  bool Enabled = true;
  bool PseudoOnly = false;
  bool SupportsHierarchicalDocumentSymbol = true;
  SymbolKindBitset SupportedSymbolKinds;

  const GlobalCompilationDatabase *TestCDB = nullptr;
  const ThreadsafeFS *TestFS = nullptr;
  mutable std::mutex HeaderCacheMutex;
  llvm::StringMap<std::shared_ptr<const HeaderInfo>> HeaderCache;
};

} // namespace clangd
} // namespace clang

#endif // LLVM_CLANG_TOOLS_EXTRA_CLANGD_PSEUDOMODULE_H
