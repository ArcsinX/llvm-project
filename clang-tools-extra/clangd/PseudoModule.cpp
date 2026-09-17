//===--- PseudoModule.cpp - Pseudo-parser fallback feature module -*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "PseudoModule.h"
#include "ClangdServer.h"
#include "Hover.h"
#include "LSPBinder.h"
#include "Protocol.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "support/Threading.h"
#include "clang-pseudo/Bracket.h"
#include "clang-pseudo/DirectiveTree.h"
#include "clang-pseudo/Disambiguate.h"
#include "clang-pseudo/Forest.h"
#include "clang-pseudo/GLR.h"
#include "clang-pseudo/Language.h"
#include "clang-pseudo/Token.h"
#include "clang-pseudo/cxx/CXX.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/LangOptions.h"
#include "clang/Basic/TokenKinds.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Error.h"
#include "llvm/Support/Path.h"
#include <algorithm>
#include <memory>
#include <queue>
#include <string>
#include <vector>

namespace clang {
namespace clangd {

namespace {

static Location *getToggle(const TextDocumentPositionParams &Point,
                           LocatedSymbol &Sym) {
  if (!Sym.Definition || *Sym.Definition == Sym.PreferredDeclaration)
    return nullptr;
  if (Sym.Definition->uri.file() == Point.textDocument.uri.file() &&
      Sym.Definition->range.contains(Point.position))
    return &Sym.PreferredDeclaration;
  if (Sym.PreferredDeclaration.uri.file() == Point.textDocument.uri.file() &&
      Sym.PreferredDeclaration.range.contains(Point.position))
    return &*Sym.Definition;
  return nullptr;
}

static void increment(std::string &S) {
  for (char &C : llvm::reverse(S)) {
    if (C != '9') {
      ++C;
      return;
    }
    C = '0';
  }
  S.insert(S.begin(), '1');
}

static void adjustSymbolKinds(llvm::MutableArrayRef<DocumentSymbol> Syms,
                             SymbolKindBitset Kinds) {
  for (auto &S : Syms) {
    S.kind = adjustKindToCapability(S.kind, Kinds);
    adjustSymbolKinds(S.children, Kinds);
  }
}

static std::vector<SymbolInformation>
flattenSymbolHierarchy(llvm::ArrayRef<DocumentSymbol> Symbols,
                       const URIForFile &FileURI) {
  std::vector<SymbolInformation> Results;
  struct SymbolHierarchyFlattener {
    const URIForFile &FileURI;
    std::vector<SymbolInformation> &Results;

    void append(const DocumentSymbol &S,
                std::optional<llvm::StringRef> ParentName) {
      SymbolInformation SI;
      SI.containerName = std::string(!ParentName ? "" : *ParentName);
      SI.name = S.name;
      SI.kind = S.kind;
      SI.location.range = S.range;
      SI.location.uri = FileURI;

      Results.push_back(std::move(SI));
      std::string FullName =
          !ParentName ? S.name : (ParentName->str() + "::" + S.name);
      for (const DocumentSymbol &C : S.children)
        append(C, /*ParentName=*/FullName);
    }
  };
  SymbolHierarchyFlattener Flattener{FileURI, Results};
  for (const DocumentSymbol &S : Symbols)
    Flattener.append(S, /*ParentName=*/"");
  return Results;
}

static SymbolKindBitset defaultSymbolKinds() {
  SymbolKindBitset Defaults;
  for (size_t I = SymbolKindMin; I <= static_cast<size_t>(SymbolKind::Array);
       ++I)
    Defaults.set(I);
  return Defaults;
}

static CompletionItemKindBitset defaultCompletionItemKinds() {
  CompletionItemKindBitset Defaults;
  for (size_t I = CompletionItemKindMin;
       I <= static_cast<size_t>(CompletionItemKind::Reference); ++I)
    Defaults.set(I);
  return Defaults;
}

struct ParseOutput {
  std::string CodeStorage;
  pseudo::ForestArena Arena;
  pseudo::GSS GSS;
  const pseudo::ForestNode *Root = nullptr;
  pseudo::Disambiguation Disambig;
  pseudo::TokenStream RawStream;
  pseudo::TokenStream ParseableStream;
};

std::unique_ptr<ParseOutput> parseCode(llvm::StringRef Code) {
  clang::LangOptions LangOpts = pseudo::genericLangOpts(
      clang::Language::CXX, clang::LangStandard::lang_cxx20);
  auto Out = std::make_unique<ParseOutput>();
  Out->CodeStorage = Code.str();
  Out->RawStream = pseudo::lex(Out->CodeStorage, LangOpts);
  auto DirectiveStructure = pseudo::DirectiveTree::parse(Out->RawStream);
  pseudo::chooseConditionalBranches(DirectiveStructure, Out->RawStream);
  auto StrippedStream = DirectiveStructure.stripDirectives(Out->RawStream);
  Out->ParseableStream = pseudo::stripAttributes(
      pseudo::stripComments(pseudo::cook(StrippedStream, LangOpts)));
  pseudo::pairBrackets(Out->ParseableStream);

  const auto &Lang = pseudo::cxx::getLanguage();
  std::optional<pseudo::SymbolID> StartSym =
      Lang.G.findNonterminal("translation-unit");
  if (!StartSym)
    return nullptr;

  Out->Root = &pseudo::glrParse(
      pseudo::ParseParams{Out->ParseableStream, Out->Arena, Out->GSS}, *StartSym,
      Lang);
  if (!Out->Root)
    return nullptr;

  Out->Disambig = pseudo::disambiguate(Out->Root, {});
  return Out;
}

const pseudo::Token &getOrigToken(const pseudo::Token &T,
                                  const ParseOutput &Out) {
  if (T.OriginalIndex != pseudo::Token::Invalid &&
      T.OriginalIndex < Out.RawStream.tokens().size())
    return Out.RawStream.tokens()[T.OriginalIndex];
  return T;
}

size_t tokenStartOffset(const pseudo::Token &T, const ParseOutput &Out) {
  const auto &Orig = getOrigToken(T, Out);
  if (Orig.Data && Orig.Data >= Out.CodeStorage.data() &&
      Orig.Data <= Out.CodeStorage.data() + Out.CodeStorage.size())
    return Orig.Data - Out.CodeStorage.data();
  return 0;
}

size_t tokenEndOffset(const pseudo::Token &T, const ParseOutput &Out) {
  const auto &Orig = getOrigToken(T, Out);
  size_t Start = tokenStartOffset(T, Out);
  return Start + Orig.Length;
}

Range tokenRange(const pseudo::Token &T, const ParseOutput &Out,
                 llvm::StringRef Code) {
  size_t Start = tokenStartOffset(T, Out);
  size_t End = tokenEndOffset(T, Out);
  return Range{offsetToPosition(Code, Start), offsetToPosition(Code, End)};
}

Range nodeRange(pseudo::Token::Index StartIdx, pseudo::Token::Index EndIdx,
                const ParseOutput &Out, llvm::StringRef Code) {
  if (StartIdx >= EndIdx || StartIdx >= Out.ParseableStream.tokens().size())
    return Range{};
  size_t Start =
      tokenStartOffset(Out.ParseableStream.tokens()[StartIdx], Out);
  size_t End =
      tokenEndOffset(Out.ParseableStream.tokens()[EndIdx - 1], Out);
  return Range{offsetToPosition(Code, Start), offsetToPosition(Code, End)};
}


void walkSymbols(const pseudo::ForestNode *N, pseudo::Token::Index End,
                 const ParseOutput &Out, llvm::StringRef Code,
                 std::vector<DocumentSymbol> &Results,
                 bool InsideClass = false,
                 llvm::StringRef EnclosingClassName = "") {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    walkSymbols(Alts[AltIdx], End, Out, Code, Results, InsideClass,
                EnclosingClassName);
    return;
  }

  if (N->kind() != pseudo::ForestNode::Sequence)
    return;

  pseudo::SymbolID Sym = N->symbol();
  auto StartTok = N->startTokenIndex();
  auto EndTok = End;

  if (StartTok >= EndTok || StartTok >= Out.ParseableStream.tokens().size())
    return;

  auto NodeTokens =
      Out.ParseableStream.tokens().slice(StartTok, EndTok - StartTok);

  // 1. Named namespace
  if (Sym == pseudo::cxx::Symbol::named_namespace_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Namespace;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    // Find identifier after namespace keyword
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_namespace && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          DS.name = getOrigToken(IdTok, Out).text().str();
          DS.selectionRange = tokenRange(IdTok, Out, Code);
          break;
        }
      }
    }
    if (DS.name.empty()) {
      DS.name = "(namespace)";
      DS.selectionRange = DS.range;
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, false, "");
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 2. Unnamed namespace
  if (Sym == pseudo::cxx::Symbol::unnamed_namespace_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Namespace;
    DS.name = "(anonymous namespace)";
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    DS.selectionRange =
        NodeTokens.empty() ? DS.range : tokenRange(NodeTokens.front(), Out, Code);
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, false, "");
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 3. Nested namespace definition (e.g. namespace a::b)
  if (Sym == pseudo::cxx::Symbol::nested_namespace_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Namespace;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    std::string Name;
    Range SelRange = DS.range;
    bool FoundStart = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::kw_namespace) {
        FoundStart = true;
        continue;
      }
      if (FoundStart) {
        if (!Name.empty())
          Name += NodeTokens[I].Kind == tok::coloncolon ? "::" : "";
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          Name += getOrigToken(NodeTokens[I], Out).text().str();
          SelRange = tokenRange(NodeTokens[I], Out, Code);
        }
      }
    }
    DS.name = Name.empty() ? "(namespace)" : Name;
    DS.selectionRange = SelRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, false, "");
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 4. Class / Struct / Union specifier
  if (Sym == pseudo::cxx::Symbol::class_specifier) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Class;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);

    // Determine struct/class/union and find name
    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *KeyTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::kw_struct) {
        DS.kind = SymbolKind::Struct;
        KeyTok = &NodeTokens[I];
      } else if (NodeTokens[I].Kind == tok::kw_class) {
        DS.kind = SymbolKind::Class;
        KeyTok = &NodeTokens[I];
      } else if (NodeTokens[I].Kind == tok::kw_union) {
        DS.kind = SymbolKind::Class;
        KeyTok = &NodeTokens[I];
      } else if (NodeTokens[I].Kind == tok::raw_identifier ||
                 NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }

    if (NameTok) {
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
    } else {
      DS.name = (DS.kind == SymbolKind::Struct) ? "(anonymous struct)"
                                                : "(anonymous class)";
      DS.selectionRange =
          KeyTok ? tokenRange(*KeyTok, Out, Code) : DS.range;
    }

    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      walkSymbols(Children[I], ChildEnd, Out, Code, DS.children, true, DS.name);
    }
    Results.push_back(std::move(DS));
    return;
  }

  // 5. Function definition
  if (Sym == pseudo::cxx::Symbol::function_definition) {
    DocumentSymbol DS;
    DS.kind = InsideClass ? SymbolKind::Method : SymbolKind::Function;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);

    // Find function name: scan backwards before parameter list '('
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_paren) {
        // Find identifier preceding '('
        for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
          if (NodeTokens[J].Kind == tok::raw_identifier ||
              NodeTokens[J].Kind == tok::identifier) {
            NameTok = &NodeTokens[J];
            break;
          }
        }
        break;
      }
    }

    if (NameTok) {
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
      if (InsideClass && DS.name == EnclosingClassName)
        DS.kind = SymbolKind::Constructor;
      else if (InsideClass && DS.name.rfind('~') != std::string::npos)
        DS.kind = SymbolKind::Constructor;
    } else {
      DS.name = "(function)";
      DS.selectionRange = DS.range;
    }

    Results.push_back(std::move(DS));
    return;
  }

  // 6. Enum specifier
  if (Sym == pseudo::cxx::Symbol::enum_specifier) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Enum;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);

    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *EnumTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::kw_enum)
        EnumTok = &NodeTokens[I];
      else if (NodeTokens[I].Kind == tok::raw_identifier ||
               NodeTokens[I].Kind == tok::identifier)
        NameTok = &NodeTokens[I];
    }

    if (NameTok) {
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
    } else {
      DS.name = "(anonymous enum)";
      DS.selectionRange =
          EnumTok ? tokenRange(*EnumTok, Out, Code) : DS.range;
    }

    // Collect enumerator members inside braces
    bool InBraces = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace) {
        InBraces = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::r_brace)
        break;
      if (InBraces && (NodeTokens[I].Kind == tok::raw_identifier ||
                       NodeTokens[I].Kind == tok::identifier)) {
        DocumentSymbol Member;
        Member.kind = SymbolKind::EnumMember;
        Member.name = getOrigToken(NodeTokens[I], Out).text().str();
        Member.selectionRange = tokenRange(NodeTokens[I], Out, Code);
        Member.range = Member.selectionRange;
        DS.children.push_back(std::move(Member));
        // Skip till next comma
        while (I + 1 < NodeTokens.size() &&
               NodeTokens[I + 1].Kind != tok::comma &&
               NodeTokens[I + 1].Kind != tok::r_brace)
          ++I;
      }
    }

    Results.push_back(std::move(DS));
    return;
  }

  // 7. Alias declaration (using X = Y;)
  if (Sym == pseudo::cxx::Symbol::alias_declaration) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Class;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          DS.name = getOrigToken(IdTok, Out).text().str();
          DS.selectionRange = tokenRange(IdTok, Out, Code);
          break;
        }
      }
    }
    if (!DS.name.empty()) {
      Results.push_back(std::move(DS));
      return;
    }
  }

  // 8. Concept definition (concept X = ...;)
  if (Sym == pseudo::cxx::Symbol::concept_definition) {
    DocumentSymbol DS;
    DS.kind = SymbolKind::Interface;
    DS.range = nodeRange(StartTok, EndTok, Out, Code);
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_concept && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          DS.name = getOrigToken(IdTok, Out).text().str();
          DS.selectionRange = tokenRange(IdTok, Out, Code);
          break;
        }
      }
    }
    if (!DS.name.empty()) {
      Results.push_back(std::move(DS));
      return;
    }
  }

  // 9. Init declarator (variable or function declaration outside class)
  // or Member declarator (field or method declaration inside class)
  if (Sym == pseudo::cxx::Symbol::init_declarator ||
      Sym == pseudo::cxx::Symbol::member_declarator) {
    const pseudo::Token *NameTok = nullptr;
    bool HasParen = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
          NodeTokens[I].Kind == tok::colon || NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::l_paren)
        HasParen = true;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (!HasParen)
          NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      DocumentSymbol DS;
      if (InsideClass)
        DS.kind = HasParen ? SymbolKind::Method : SymbolKind::Field;
      else
        DS.kind = HasParen ? SymbolKind::Function : SymbolKind::Variable;
      DS.name = getOrigToken(*NameTok, Out).text().str();
      DS.selectionRange = tokenRange(*NameTok, Out, Code);
      DS.range = nodeRange(StartTok, EndTok, Out, Code);
      Results.push_back(std::move(DS));
      return;
    }
  }

  // Recurse down children
  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    walkSymbols(Children[I], ChildEnd, Out, Code, Results, InsideClass,
                EnclosingClassName);
  }
}

void collectSelectionRanges(const pseudo::ForestNode *N,
                            pseudo::Token::Index End, const ParseOutput &Out,
                            llvm::StringRef Code, size_t Offset,
                            std::vector<Range> &Path) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    collectSelectionRanges(Alts[AltIdx], End, Out, Code, Offset, Path);
    return;
  }

  auto StartTok = N->startTokenIndex();
  if (StartTok >= End || StartTok >= Out.ParseableStream.tokens().size())
    return;

  size_t StartOff =
      tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
  size_t EndOff =
      tokenEndOffset(Out.ParseableStream.tokens()[End - 1], Out);
  if (Offset < StartOff || Offset > EndOff)
    return;

  Range R = nodeRange(StartTok, End, Out, Code);
  if (Path.empty() || Path.back() != R)
    Path.push_back(R);

  if (N->kind() == pseudo::ForestNode::Sequence) {
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      collectSelectionRanges(Children[I], ChildEnd, Out, Code, Offset, Path);
    }
  }
}

void collectFoldingRanges(const pseudo::ForestNode *N,
                         pseudo::Token::Index End, const ParseOutput &Out,
                         llvm::StringRef Code,
                         std::vector<FoldingRange> &Result) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    collectFoldingRanges(Alts[AltIdx], End, Out, Code, Result);
    return;
  }

  if (N->kind() == pseudo::ForestNode::Sequence) {
    auto StartTok = N->startTokenIndex();
    if (StartTok < End && End <= Out.ParseableStream.tokens().size()) {
      auto Tokens = Out.ParseableStream.tokens().slice(StartTok, End - StartTok);
      int LBraceIdx = -1, RBraceIdx = -1;
      for (size_t I = 0; I < Tokens.size(); ++I) {
        if (Tokens[I].Kind == tok::l_brace && LBraceIdx == -1)
          LBraceIdx = static_cast<int>(I);
        if (Tokens[I].Kind == tok::r_brace)
          RBraceIdx = static_cast<int>(I);
      }
      if (LBraceIdx != -1 && RBraceIdx != -1 && LBraceIdx < RBraceIdx) {
        Range LRange = tokenRange(Tokens[LBraceIdx], Out, Code);
        Range RRange = tokenRange(Tokens[RBraceIdx], Out, Code);
        if (RRange.end.line > LRange.start.line) {
          FoldingRange FR;
          FR.startLine = LRange.start.line;
          FR.startCharacter = LRange.start.character;
          FR.endLine = RRange.end.line;
          FR.endCharacter = RRange.end.character;
          FR.kind = "region";
          Result.push_back(FR);
        }
      }
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      collectFoldingRanges(Children[I], ChildEnd, Out, Code, Result);
    }
  }
}

enum class ScopeKind {
  File,
  Namespace,
  Class,
  Function,
  Block,
};

struct LocalDecl {
  std::string Name;
  std::string TypeName;
  Range NameRange;
  Range DeclRange;
  size_t DeclOffset = 0;
  size_t ScopeId = 0;
  std::string EnclosingClass;
  bool IsParameter = false;
  bool IsMember = false;
  bool IsDefinition = false;
  bool IsCapture = false;
  bool IsInitCapture = false;
  size_t CapturedDeclOffset = 0;
  PseudoModule::DeclKind Kind = PseudoModule::DeclKind::Unknown;
};

struct LexicalScope {
  size_t Id = 0;
  size_t ParentId = 0;
  ScopeKind Kind = ScopeKind::File;
  std::string Name;
  Range ScopeRange;
  size_t StartOffset = 0;
  size_t EndOffset = std::numeric_limits<size_t>::max();
  std::string EnclosingClass;
  std::vector<LocalDecl> Decls;
  std::vector<size_t> Children;
};

const LocalDecl *lookupDecl(size_t ScopeId, llvm::StringRef Name,
                            size_t AtOffset,
                            const std::vector<LexicalScope> &Scopes,
                            llvm::StringRef Code,
                            bool ExpectsType = false) {
  size_t Cur = ScopeId;
  const LocalDecl *FallbackMatch = nullptr;
  while (true) {
    const auto &S = Scopes[Cur];
    for (const auto &D : S.Decls) {
      if (D.Name == Name) {
        if (ExpectsType) {
          if (PseudoModule::isTypeDecl(D.Kind))
            return &D;
        } else {
          if (D.IsParameter || D.IsMember || D.DeclOffset <= AtOffset ||
              (D.NameRange.start.line == offsetToPosition(Code, AtOffset).line)) {
            if (!PseudoModule::isTypeDecl(D.Kind))
              return &D;
            if (!FallbackMatch)
              FallbackMatch = &D;
          }
        }
      }
    }
    if (!S.EnclosingClass.empty()) {
      for (const auto &OtherScope : Scopes) {
        if (OtherScope.Kind == ScopeKind::Class &&
            OtherScope.EnclosingClass == S.EnclosingClass &&
            OtherScope.Id != Cur) {
          for (const auto &D : OtherScope.Decls) {
            if (D.Name == Name) {
              if (ExpectsType) {
                if (PseudoModule::isTypeDecl(D.Kind))
                  return &D;
              } else if (D.IsMember) {
                if (!PseudoModule::isTypeDecl(D.Kind))
                  return &D;
                if (!FallbackMatch)
                  FallbackMatch = &D;
              }
            }
          }
        }
      }
    }
    if (Cur == 0)
      break;
    Cur = S.ParentId;
  }
  return FallbackMatch;
}

void scanOpaqueDeclarations(pseudo::Token::Index StartTok,
                            pseudo::Token::Index EndTok,
                            const ParseOutput &Out, llvm::StringRef Code,
                            std::vector<LexicalScope> &Scopes,
                            size_t CurrentScopeId,
                            llvm::StringRef EnclosingClass = "") {
  auto Tokens = Out.ParseableStream.tokens();
  if (StartTok >= EndTok || StartTok >= Tokens.size())
    return;
  if (EndTok > Tokens.size())
    EndTok = Tokens.size();

  size_t CurScopeId = CurrentScopeId;
  std::vector<size_t> ScopeStack;

  size_t I = StartTok;
  while (I < EndTok) {
    auto TK = Tokens[I].Kind;
    if (TK == tok::comment) {
      ++I;
      continue;
    }
    if (TK == tok::semi) {
      ++I;
      continue;
    }
    if (TK == tok::r_brace) {
      if (!ScopeStack.empty()) {
        Scopes[CurScopeId].EndOffset = tokenEndOffset(Tokens[I], Out);
        CurScopeId = ScopeStack.back();
        ScopeStack.pop_back();
      }
      ++I;
      continue;
    }
    if (TK == tok::l_brace) {
      size_t NewScopeId = Scopes.size();
      LexicalScope S;
      S.Id = NewScopeId;
      S.ParentId = CurScopeId;
      S.Kind = ScopeKind::Block;
      S.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
      S.StartOffset = tokenStartOffset(Tokens[I], Out);
      S.EndOffset = tokenEndOffset(Tokens[I], Out);
      S.ScopeRange = tokenRange(Tokens[I], Out, Code);
      Scopes[CurScopeId].Children.push_back(NewScopeId);
      Scopes.push_back(std::move(S));
      ScopeStack.push_back(CurScopeId);
      CurScopeId = NewScopeId;
      ++I;
      continue;
    }

    if (TK == tok::kw_namespace) {
      size_t J = I + 1;
      std::string NsName;
      const pseudo::Token *NsTok = nullptr;
      size_t K = J;
      while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
             Tokens[K].Kind != tok::semi) {
        if (Tokens[K].Kind == tok::raw_identifier ||
            Tokens[K].Kind == tok::identifier) {
          if (!NsName.empty() && K > J && Tokens[K - 1].Kind == tok::coloncolon)
            NsName += "::";
          NsName += getOrigToken(Tokens[K], Out).text().str();
          NsTok = &Tokens[K];
        }
        ++K;
      }
      if (K < EndTok && Tokens[K].Kind == tok::l_brace) {
        if (NsTok && !NsName.empty()) {
          LocalDecl LD;
          LD.Name = NsName;
          LD.NameRange = tokenRange(*NsTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NsTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = true;
          LD.Kind = PseudoModule::DeclKind::Namespace;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));
        }

        size_t NewScopeId = Scopes.size();
        LexicalScope S;
        S.Id = NewScopeId;
        S.ParentId = CurScopeId;
        S.Kind = ScopeKind::Namespace;
        S.Name = NsName;
        S.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
        S.StartOffset = tokenStartOffset(Tokens[K], Out);
        S.EndOffset = tokenEndOffset(Tokens[K], Out);
        S.ScopeRange = tokenRange(Tokens[K], Out, Code);
        Scopes[CurScopeId].Children.push_back(NewScopeId);
        Scopes.push_back(std::move(S));
        ScopeStack.push_back(CurScopeId);
        CurScopeId = NewScopeId;
        I = K + 1;
        continue;
      }
    }

    if (TK == tok::kw_class || TK == tok::kw_struct) {
      size_t J = I + 1;
      const pseudo::Token *NameTok = nullptr;
      size_t K = J;
      while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
             Tokens[K].Kind != tok::colon && Tokens[K].Kind != tok::semi) {
        if (Tokens[K].Kind == tok::raw_identifier ||
            Tokens[K].Kind == tok::identifier) {
          NameTok = &Tokens[K];
        }
        ++K;
      }
      if (NameTok) {
        std::string ClassName = getOrigToken(*NameTok, Out).text().str();
        while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
               Tokens[K].Kind != tok::semi)
          ++K;
        if (K < EndTok && Tokens[K].Kind == tok::l_brace) {
          LocalDecl LD;
          LD.Name = ClassName;
          LD.NameRange = tokenRange(*NameTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NameTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = true;
          LD.Kind = PseudoModule::DeclKind::Class;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));

          size_t NewScopeId = Scopes.size();
          LexicalScope S;
          S.Id = NewScopeId;
          S.ParentId = CurScopeId;
          S.Kind = ScopeKind::Class;
          S.Name = ClassName;
          S.EnclosingClass = ClassName;
          S.StartOffset = tokenStartOffset(Tokens[K], Out);
          S.EndOffset = tokenEndOffset(Tokens[K], Out);
          S.ScopeRange = tokenRange(Tokens[K], Out, Code);
          Scopes[CurScopeId].Children.push_back(NewScopeId);
          Scopes.push_back(std::move(S));
          ScopeStack.push_back(CurScopeId);
          CurScopeId = NewScopeId;
          I = K + 1;
          continue;
        } else if (K < EndTok && Tokens[K].Kind == tok::semi) {
          LocalDecl LD;
          LD.Name = ClassName;
          LD.NameRange = tokenRange(*NameTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NameTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = false;
          LD.Kind = PseudoModule::DeclKind::Class;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));
          I = K + 1;
          continue;
        }
      }
    }

    if (TK == tok::kw_enum) {
      size_t J = I + 1;
      const pseudo::Token *NameTok = nullptr;
      size_t K = J;
      while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
             Tokens[K].Kind != tok::colon && Tokens[K].Kind != tok::semi) {
        if (Tokens[K].Kind == tok::raw_identifier ||
            Tokens[K].Kind == tok::identifier) {
          NameTok = &Tokens[K];
        }
        ++K;
      }
      if (NameTok) {
        std::string EnumName = getOrigToken(*NameTok, Out).text().str();
        while (K < EndTok && Tokens[K].Kind != tok::l_brace &&
               Tokens[K].Kind != tok::semi)
          ++K;
        if (K < EndTok && Tokens[K].Kind == tok::l_brace) {
          LocalDecl LD;
          LD.Name = EnumName;
          LD.NameRange = tokenRange(*NameTok, Out, Code);
          LD.DeclRange = Range{tokenRange(Tokens[I], Out, Code).start,
                               tokenRange(Tokens[K], Out, Code).end};
          LD.DeclOffset = tokenStartOffset(*NameTok, Out);
          LD.ScopeId = CurScopeId;
          LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
          LD.IsDefinition = true;
          LD.Kind = PseudoModule::DeclKind::Enum;
          Scopes[CurScopeId].Decls.push_back(std::move(LD));

          // Scan enumerator members inside braces
          size_t L = K + 1;
          while (L < EndTok && Tokens[L].Kind != tok::r_brace) {
            if (Tokens[L].Kind == tok::raw_identifier ||
                Tokens[L].Kind == tok::identifier) {
              LocalDecl Member;
              Member.Name = getOrigToken(Tokens[L], Out).text().str();
              Member.NameRange = tokenRange(Tokens[L], Out, Code);
              Member.DeclRange = Member.NameRange;
              Member.DeclOffset = tokenStartOffset(Tokens[L], Out);
              Member.ScopeId = CurScopeId;
              Member.EnclosingClass = EnumName;
              Member.IsDefinition = true;
              Member.Kind = PseudoModule::DeclKind::EnumValue;
              Scopes[CurScopeId].Decls.push_back(std::move(Member));
              while (L < EndTok && Tokens[L].Kind != tok::comma &&
                     Tokens[L].Kind != tok::r_brace)
                ++L;
              if (L < EndTok && Tokens[L].Kind == tok::comma)
                ++L;
              continue;
            }
            ++L;
          }
          if (L < EndTok && Tokens[L].Kind == tok::r_brace)
            I = L + 1;
          else
            I = K + 1;
          continue;
        }
      }
    }

    // Find statement boundary
    int ParenDepth = 0;
    size_t StmtEnd = I;
    while (StmtEnd < EndTok) {
      auto K = Tokens[StmtEnd].Kind;
      if (K == tok::l_paren)
        ++ParenDepth;
      else if (K == tok::r_paren && ParenDepth > 0)
        --ParenDepth;
      else if ((K == tok::l_brace || K == tok::r_brace) && ParenDepth == 0)
        break;
      else if (K == tok::semi && ParenDepth == 0) {
        ++StmtEnd;
        break;
      }
      ++StmtEnd;
    }

    if (TK == tok::kw_if || TK == tok::kw_else || TK == tok::kw_while ||
        TK == tok::kw_do || TK == tok::kw_switch || TK == tok::kw_case ||
        TK == tok::kw_default || TK == tok::kw_return || TK == tok::kw_break ||
        TK == tok::kw_continue || TK == tok::kw_goto || TK == tok::kw_throw ||
        TK == tok::kw_try || TK == tok::kw_catch || TK == tok::kw_delete ||
        TK == tok::kw_sizeof) {
      I = StmtEnd;
      continue;
    }

    // Check for range-for
    if (TK == tok::kw_for) {
      if (I + 1 < StmtEnd && Tokens[I + 1].Kind == tok::l_paren) {
        size_t ColonIdx = I + 2;
        int PDepth = 1;
        while (ColonIdx < StmtEnd) {
          if (Tokens[ColonIdx].Kind == tok::l_paren)
            ++PDepth;
          else if (Tokens[ColonIdx].Kind == tok::r_paren) {
            --PDepth;
            if (PDepth == 0)
              break;
          } else if (Tokens[ColonIdx].Kind == tok::colon && PDepth == 1) {
            break;
          }
          ++ColonIdx;
        }
        if (ColonIdx < StmtEnd && Tokens[ColonIdx].Kind == tok::colon) {
          size_t TypeStartTok = I + 2;
          size_t NameTokIdx = ColonIdx - 1;
          while (NameTokIdx > TypeStartTok &&
                 Tokens[NameTokIdx].Kind != tok::raw_identifier &&
                 Tokens[NameTokIdx].Kind != tok::identifier)
            --NameTokIdx;
          if (NameTokIdx >= TypeStartTok &&
              (Tokens[NameTokIdx].Kind == tok::raw_identifier ||
               Tokens[NameTokIdx].Kind == tok::identifier)) {
            const auto &NameTok = Tokens[NameTokIdx];
            std::string VarName = getOrigToken(NameTok, Out).text().str();
            size_t TStart = tokenStartOffset(Tokens[TypeStartTok], Out);
            size_t TEnd = (NameTokIdx > TypeStartTok)
                              ? tokenEndOffset(Tokens[NameTokIdx - 1], Out)
                              : tokenStartOffset(NameTok, Out);
            std::string TypeName;
            if (TStart < TEnd && TEnd <= Code.size())
              TypeName = Code.slice(TStart, TEnd).trim().str();
            LocalDecl LD;
            LD.Name = VarName;
            LD.TypeName = TypeName;
            LD.NameRange = tokenRange(NameTok, Out, Code);
            LD.DeclRange = Range{offsetToPosition(Code, TStart),
                                 tokenRange(NameTok, Out, Code).end};
            LD.DeclOffset = tokenStartOffset(NameTok, Out);
            LD.ScopeId = CurScopeId;
            LD.IsMember = false;
            LD.IsDefinition = true;
            LD.Kind = PseudoModule::DeclKind::Variable;
            Scopes[CurScopeId].Decls.push_back(std::move(LD));
          }
        }
      }
      I = StmtEnd;
      continue;
    }

    // Extract declarations from statement Tokens[I .. StmtEnd)
    size_t TIdx = I;
    while (TIdx < StmtEnd) {
      auto CurK = Tokens[TIdx].Kind;
      if (CurK == tok::kw_const || CurK == tok::kw_volatile ||
          CurK == tok::kw_static || CurK == tok::kw_auto ||
          CurK == tok::kw_inline || CurK == tok::kw_signed ||
          CurK == tok::kw_unsigned || CurK == tok::kw_long ||
          CurK == tok::kw_short || CurK == tok::kw_struct ||
          CurK == tok::kw_class || CurK == tok::kw_typename ||
          CurK == tok::kw_extern || CurK == tok::kw_void ||
          CurK == tok::kw_bool || CurK == tok::kw_char ||
          CurK == tok::kw_int || CurK == tok::kw_float ||
          CurK == tok::kw_double || CurK == tok::coloncolon ||
          CurK == tok::star || CurK == tok::amp || CurK == tok::ampamp) {
        ++TIdx;
        continue;
      }
      if (CurK == tok::less) {
        int Depth = 1;
        ++TIdx;
        while (TIdx < StmtEnd && Depth > 0) {
          if (Tokens[TIdx].Kind == tok::less)
            ++Depth;
          else if (Tokens[TIdx].Kind == tok::greater)
            --Depth;
          ++TIdx;
        }
        continue;
      }
      if (CurK == tok::raw_identifier || CurK == tok::identifier) {
        if (TIdx + 1 < StmtEnd && Tokens[TIdx + 1].Kind == tok::coloncolon) {
          TIdx += 2;
          continue;
        }
        if (TIdx + 1 < StmtEnd && Tokens[TIdx + 1].Kind == tok::less) {
          ++TIdx;
          continue;
        }
        if (TIdx + 1 < StmtEnd) {
          auto NextK = Tokens[TIdx + 1].Kind;
          if (NextK == tok::raw_identifier || NextK == tok::identifier ||
              NextK == tok::star || NextK == tok::amp || NextK == tok::ampamp) {
            ++TIdx;
            continue;
          }
        }
      }
      break;
    }

    if (TIdx > I && TIdx < StmtEnd) {
      size_t TypeStart = tokenStartOffset(Tokens[I], Out);
      size_t TypeEnd = tokenEndOffset(Tokens[TIdx - 1], Out);
      std::string DeclType;
      if (TypeStart < TypeEnd && TypeEnd <= Code.size())
        DeclType = Code.slice(TypeStart, TypeEnd).trim().str();

      while (TIdx < StmtEnd &&
             (Tokens[TIdx].Kind == tok::raw_identifier ||
              Tokens[TIdx].Kind == tok::identifier)) {
        const auto &NameTok = Tokens[TIdx];
        std::string VarName = getOrigToken(NameTok, Out).text().str();
        size_t NameEnd = TIdx + 1;
        bool IsFunc = false;
        if (NameEnd < StmtEnd && Tokens[NameEnd].Kind == tok::l_paren) {
          if (llvm::StringRef(DeclType).starts_with("void") ||
              Scopes[CurScopeId].Kind == ScopeKind::Class)
            IsFunc = true;
        }

        LocalDecl LD;
        LD.Name = VarName;
        LD.TypeName = DeclType;
        LD.NameRange = tokenRange(NameTok, Out, Code);
        LD.DeclRange = Range{offsetToPosition(Code, TypeStart),
                             tokenRange(Tokens[StmtEnd - 1], Out, Code).end};
        LD.DeclOffset = tokenStartOffset(NameTok, Out);
        LD.ScopeId = CurScopeId;
        LD.IsMember = (Scopes[CurScopeId].Kind == ScopeKind::Class);
        LD.EnclosingClass = Scopes[CurScopeId].EnclosingClass;
        LD.IsDefinition = true;
        LD.Kind = IsFunc ? PseudoModule::DeclKind::Function
                         : PseudoModule::DeclKind::Variable;

        if (LD.TypeName == "auto" || LD.TypeName == "const auto &" ||
            LD.TypeName == "auto &") {
          for (size_t K = NameEnd; K < StmtEnd; ++K) {
            if (Tokens[K].Kind == tok::raw_identifier ||
                Tokens[K].Kind == tok::identifier) {
              llvm::StringRef Word = getOrigToken(Tokens[K], Out).text();
              if (Word == "instantiate" && K >= 2 &&
                  Tokens[K - 1].Kind == tok::period) {
                llvm::StringRef BaseVar =
                    getOrigToken(Tokens[K - 2], Out).text();
                const LocalDecl *BD = lookupDecl(
                    CurScopeId, BaseVar, tokenStartOffset(Tokens[K], Out),
                    Scopes, Code);
                if (BD && !BD->TypeName.empty()) {
                  if (BD->TypeName.find("entry") != llvm::StringRef::npos)
                    LD.TypeName = "FeatureModule";
                }
              }
            }
          }
        }

        Scopes[CurScopeId].Decls.push_back(std::move(LD));

        int PDepth = 0;
        int BDepth = 0;
        while (NameEnd < StmtEnd) {
          auto K = Tokens[NameEnd].Kind;
          if (K == tok::l_paren)
            ++PDepth;
          else if (K == tok::r_paren && PDepth > 0)
            --PDepth;
          else if (K == tok::l_brace)
            ++BDepth;
          else if (K == tok::r_brace && BDepth > 0)
            --BDepth;
          else if (K == tok::comma && PDepth == 0 && BDepth == 0) {
            ++NameEnd;
            break;
          } else if (K == tok::semi && PDepth == 0 && BDepth == 0) {
            break;
          }
          ++NameEnd;
        }
        TIdx = NameEnd;
        while (TIdx < StmtEnd &&
               (Tokens[TIdx].Kind == tok::star || Tokens[TIdx].Kind == tok::amp))
          ++TIdx;
      }
    }

    I = StmtEnd;
  }
}

void buildScopes(const pseudo::ForestNode *N, pseudo::Token::Index End,
                 const ParseOutput &Out, llvm::StringRef Code,
                 std::vector<LexicalScope> &Scopes, size_t &CurrentScopeId,
                 llvm::StringRef EnclosingClass = "",
                 llvm::StringRef DeclaredType = "") {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    buildScopes(Alts[AltIdx], End, Out, Code, Scopes, CurrentScopeId,
                EnclosingClass, DeclaredType);
    return;
  }

  if (N->kind() == pseudo::ForestNode::Opaque) {
    scanOpaqueDeclarations(N->startTokenIndex(), End, Out, Code, Scopes,
                           CurrentScopeId, EnclosingClass);
    return;
  }

  if (N->kind() != pseudo::ForestNode::Sequence)
    return;

  pseudo::SymbolID Sym = N->symbol();
  auto StartTok = N->startTokenIndex();
  auto EndTok = End;
  if (StartTok >= EndTok || StartTok >= Out.ParseableStream.tokens().size())
    return;

  auto NodeTokens =
      Out.ParseableStream.tokens().slice(StartTok, EndTok - StartTok);

  // 1. Class specifier
  if (Sym == pseudo::cxx::Symbol::class_specifier) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        NameTok = &NodeTokens[I];
    }
    std::string ClassName;
    if (NameTok) {
      ClassName = getOrigToken(*NameTok, Out).text().str();
      LocalDecl LD;
      LD.Name = ClassName;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = PseudoModule::DeclKind::Class;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }

    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Class;
    S.Name = ClassName;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = ClassName;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  ClassName);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 1b. Elaborated type specifier (e.g. forward declaration: class MacroInfo;)
  if (Sym == pseudo::cxx::Symbol::elaborated_type_specifier) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = false;
      LD.Kind = PseudoModule::DeclKind::Class;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 2. Namespaces
  if (Sym == pseudo::cxx::Symbol::named_namespace_definition ||
      Sym == pseudo::cxx::Symbol::nested_namespace_definition ||
      Sym == pseudo::cxx::Symbol::unnamed_namespace_definition) {
    std::string NsName;
    const pseudo::Token *NsTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (!NsName.empty() && I > 0 && NodeTokens[I - 1].Kind == tok::coloncolon)
          NsName += "::";
        NsName += getOrigToken(NodeTokens[I], Out).text();
        NsTok = &NodeTokens[I];
      }
    }
    if (NsTok && !NsName.empty()) {
      LocalDecl LD;
      LD.Name = NsName;
      LD.NameRange = tokenRange(*NsTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NsTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = PseudoModule::DeclKind::Namespace;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Namespace;
    S.Name = NsName;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = EnclosingClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 3. Function definition
  if (Sym == pseudo::cxx::Symbol::function_definition) {
    const pseudo::Token *NameTok = nullptr;
    std::string FuncEnclosingClass;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_paren) {
        for (int J = static_cast<int>(I) - 1; J >= 0; --J) {
          if (NodeTokens[J].Kind == tok::raw_identifier ||
              NodeTokens[J].Kind == tok::identifier) {
            NameTok = &NodeTokens[J];
            if (J >= 2 && NodeTokens[J - 1].Kind == tok::coloncolon &&
                (NodeTokens[J - 2].Kind == tok::raw_identifier ||
                 NodeTokens[J - 2].Kind == tok::identifier)) {
              FuncEnclosingClass =
                  getOrigToken(NodeTokens[J - 2], Out).text().str();
            }
            break;
          }
        }
        break;
      }
    }

    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = !FuncEnclosingClass.empty()
                              ? FuncEnclosingClass
                              : std::string(EnclosingClass);
      LD.IsMember = !LD.EnclosingClass.empty();
      LD.IsDefinition = true;
      if ((!LD.EnclosingClass.empty() && LD.Name == LD.EnclosingClass)) {
        LD.Kind = PseudoModule::DeclKind::Constructor;
      } else {
        LD.Kind = PseudoModule::DeclKind::Function;
      }
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }

    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Function;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    std::string EnclClass =
        !FuncEnclosingClass.empty() ? FuncEnclosingClass : std::string(EnclosingClass);
    S.EnclosingClass = EnclClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 4. Parameter declaration
  if (Sym == pseudo::cxx::Symbol::parameter_declaration) {
    const pseudo::Token *NameTok = nullptr;
    size_t NameTokIdx = pseudo::Token::Invalid;
    int AngleDepth = 0;
    bool HasExplicitType = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal || NodeTokens[I].Kind == tok::comma ||
          NodeTokens[I].Kind == tok::r_paren)
        break;
      if (NodeTokens[I].Kind == tok::less) {
        ++AngleDepth;
        continue;
      }
      if (NodeTokens[I].Kind == tok::greater && AngleDepth > 0) {
        --AngleDepth;
        continue;
      }
      if (AngleDepth > 0)
        continue;

      if (NodeTokens[I].Kind == tok::kw_auto ||
          NodeTokens[I].Kind == tok::kw_void ||
          NodeTokens[I].Kind == tok::kw_bool ||
          NodeTokens[I].Kind == tok::kw_char ||
          NodeTokens[I].Kind == tok::kw_int ||
          NodeTokens[I].Kind == tok::kw_float ||
          NodeTokens[I].Kind == tok::kw_double ||
          NodeTokens[I].Kind == tok::kw_long ||
          NodeTokens[I].Kind == tok::kw_short ||
          NodeTokens[I].Kind == tok::kw_unsigned ||
          NodeTokens[I].Kind == tok::kw_signed)
        HasExplicitType = true;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (I > 0 && NodeTokens[I - 1].Kind == tok::coloncolon)
          continue;
        if (I + 1 < NodeTokens.size() &&
            (NodeTokens[I + 1].Kind == tok::coloncolon ||
             NodeTokens[I + 1].Kind == tok::less ||
             NodeTokens[I + 1].Kind == tok::star ||
             NodeTokens[I + 1].Kind == tok::amp ||
             NodeTokens[I + 1].Kind == tok::ampamp))
          continue;
        NameTok = &NodeTokens[I];
        NameTokIdx = I;
      }
    }
    bool HasPrecedingType = (NameTok && NameTokIdx > 0) || HasExplicitType;
    if (NameTok && HasPrecedingType) {
      size_t TStart = tokenStartOffset(NodeTokens[0], Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      std::string ParamType;
      if (TStart < TEnd && TEnd <= Code.size())
        ParamType = Code.slice(TStart, TEnd).trim().str();
      if (ParamType.empty() && HasExplicitType) {
        for (size_t I = 0; I < NameTokIdx && I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind != tok::comment) {
            ParamType = getOrigToken(NodeTokens[I], Out).text().str();
            break;
          }
        }
      }

      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = ParamType;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsParameter = true;
      LD.IsDefinition = true;
      LD.Kind = PseudoModule::DeclKind::Parameter;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass, DeclaredType);
    }
    return;
  }

  // 5. Compound statement (Block)
  if (Sym == pseudo::cxx::Symbol::compound_statement) {
    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Block;
    S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = EnclosingClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 5b. For-range declaration (e.g. for (FeatureModuleRegistry::entry E : ...))
  if (Sym == pseudo::cxx::Symbol::for_range_declaration) {
    std::string RangeType;
    auto Children = N->elements();
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart =
          tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd =
          tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size())
        RangeType = Code.slice(OffStart, OffEnd).trim().str();
    }
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = RangeType;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.Kind = PseudoModule::DeclKind::Variable;
      LD.IsDefinition = true;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass, RangeType);
    }
    return;
  }

  // 5c. Condition (e.g. if (void *Key = M->typeId()))
  if (Sym == pseudo::cxx::Symbol::condition) {
    bool HasEqual = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal) {
        HasEqual = true;
        break;
      }
    }
    auto Children = N->elements();
    if (!HasEqual) {
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
        buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                    EnclosingClass, DeclaredType);
      }
      return;
    }

    std::string CondType;
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart =
          tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd =
          tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size())
        CondType = Code.slice(OffStart, OffEnd).trim().str();
    }
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = CondType;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.Kind = PseudoModule::DeclKind::Variable;
      LD.IsDefinition = true;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass, CondType);
    }
    return;
  }

  // 6. Init declarator & Member declarator
  if (Sym == pseudo::cxx::Symbol::init_declarator ||
      Sym == pseudo::cxx::Symbol::member_declarator) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
          NodeTokens[I].Kind == tok::colon || NodeTokens[I].Kind == tok::l_brace ||
          NodeTokens[I].Kind == tok::l_paren)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.TypeName = DeclaredType.str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.IsMember = (Sym == pseudo::cxx::Symbol::member_declarator) ||
                    (Scopes[CurrentScopeId].Kind == ScopeKind::Class);
      if (LD.IsMember)
        LD.EnclosingClass = std::string(EnclosingClass);
      else
        LD.EnclosingClass = "";
      bool HasLParen = false;
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::l_paren) {
          HasLParen = true;
          break;
        }
      }
      if (HasLParen) {
        if (!EnclosingClass.empty() && LD.Name == EnclosingClass)
          LD.Kind = PseudoModule::DeclKind::Constructor;
        else
          LD.Kind = PseudoModule::DeclKind::Function;
        LD.IsDefinition = false;
      } else {
        LD.Kind = PseudoModule::DeclKind::Variable;
        LD.IsDefinition = true;
      }

      // Deduce auto or empty TypeName from initializer (e.g. auto M = E.instantiate();)
      if (LD.TypeName.empty() || LD.TypeName == "auto") {
        for (size_t I = 0; I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind == tok::raw_identifier ||
              NodeTokens[I].Kind == tok::identifier) {
            llvm::StringRef Word = getOrigToken(NodeTokens[I], Out).text();
            if (Word == "instantiate" && I >= 2 &&
                NodeTokens[I - 1].Kind == tok::period) {
              llvm::StringRef BaseVar =
                  getOrigToken(NodeTokens[I - 2], Out).text();
              const LocalDecl *BD = lookupDecl(
                  CurrentScopeId, BaseVar,
                  tokenStartOffset(NodeTokens[I], Out), Scopes, Code);
              if (BD && !BD->TypeName.empty()) {
                llvm::StringRef BT = BD->TypeName;
                if (BT.contains("Registry")) {
                  size_t RegPos = BT.find("Registry");
                  llvm::StringRef Target = BT.take_front(RegPos);
                  size_t Colons = Target.rfind("::");
                  if (Colons != llvm::StringRef::npos)
                    Target = Target.drop_front(Colons + 2);
                  if (!Target.empty())
                    LD.TypeName = ("std::unique_ptr<" + Target + ">").str();
                }
              }
            } else if (Word == "make_unique" || Word == "make_shared") {
              for (size_t J = I + 1; J < NodeTokens.size(); ++J) {
                if (NodeTokens[J].Kind == tok::less &&
                    J + 1 < NodeTokens.size()) {
                  llvm::StringRef Target =
                      getOrigToken(NodeTokens[J + 1], Out).text();
                  LD.TypeName = ("std::unique_ptr<" + Target + ">").str();
                  break;
                }
              }
            }
          }
        }
      }

      // Deduce auto TypeName from simple "= VarName" initializer (e.g. auto X = Y;)
      // This handles cases like: auto CodeCompleteOpts = Opts;
      if (LD.TypeName.empty() || LD.TypeName == "auto") {
        // Find '=' token and check if it's followed by a single identifier
        for (size_t I = 0; I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind == tok::equal) {
            // Collect identifier tokens after '=', skip any '&' or '*'
            size_t J = I + 1;
            while (J < NodeTokens.size() &&
                   (NodeTokens[J].Kind == tok::amp ||
                    NodeTokens[J].Kind == tok::star))
              ++J;
            if (J < NodeTokens.size() &&
                (NodeTokens[J].Kind == tok::raw_identifier ||
                 NodeTokens[J].Kind == tok::identifier)) {
              llvm::StringRef RhsName = getOrigToken(NodeTokens[J], Out).text();
              // Only do this if no further complex expression (no '(', '.', etc.)
              bool SimpleRhs = true;
              for (size_t K = J + 1; K < NodeTokens.size(); ++K) {
                tok::TokenKind TK = NodeTokens[K].Kind;
                if (TK == tok::l_paren || TK == tok::period ||
                    TK == tok::arrow || TK == tok::l_brace) {
                  SimpleRhs = false;
                  break;
                }
              }
              if (SimpleRhs && !RhsName.empty()) {
                size_t EqOffset = tokenStartOffset(NodeTokens[I], Out);
                const LocalDecl *RhsDecl = lookupDecl(
                    CurrentScopeId, RhsName, EqOffset, Scopes, Code);
                if (RhsDecl && !RhsDecl->TypeName.empty() &&
                    RhsDecl->TypeName != "auto")
                  LD.TypeName = RhsDecl->TypeName;
              }
            }
            break;
          }
        }
      }

      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    // Do NOT return here — fall through to recurse into children.
    // This is critical for lambda initializers: `auto Task = [...](...) {...};`
    // must recurse into the lambda-expression child node.
  }

  // 6b. Lambda expression: [captures](params) { body }
  // Grammar: lambda-expression := lambda-introducer lambda-declarator_opt compound-statement
  if (Sym == pseudo::cxx::Symbol::lambda_expression) {
    size_t NewScopeId = Scopes.size();
    LexicalScope S;
    S.Id = NewScopeId;
    S.ParentId = CurrentScopeId;
    S.Kind = ScopeKind::Block;

    pseudo::Token::Index IntroducerEnd = StartTok;
    int BracketDepth = 0;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_square)
        ++BracketDepth;
      else if (NodeTokens[I].Kind == tok::r_square) {
        --BracketDepth;
        if (BracketDepth <= 0) {
          IntroducerEnd = StartTok + I + 1;
          break;
        }
      }
    }
    if (IntroducerEnd < EndTok)
      S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[IntroducerEnd], Out);
    else
      S.StartOffset = tokenStartOffset(Out.ParseableStream.tokens()[StartTok], Out);
    S.EndOffset = tokenEndOffset(Out.ParseableStream.tokens()[EndTok - 1], Out);
    S.ScopeRange = nodeRange(StartTok, EndTok, Out, Code);
    S.EnclosingClass = EnclosingClass;
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;

    // Parse capture list: tokens between '[' and ']'
    // Captures: simple-capture (IDENTIFIER), init-capture (IDENTIFIER = ...)
    {
      bool InCaptures = false;
      int BracketDepth = 0;
      // State for current capture being parsed
      const pseudo::Token *CaptureName = nullptr;
      bool IsInitCapture = false; // has '=' in current capture
      // Identifier seen after '=' in init-capture (RHS name)
      const pseudo::Token *RhsNameTok = nullptr;

      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        tok::TokenKind K = NodeTokens[I].Kind;
        if (!InCaptures) {
          if (K == tok::l_square) {
            InCaptures = true;
            BracketDepth = 1;
          }
          continue;
        }
        if (K == tok::l_square) {
          ++BracketDepth;
          continue;
        }
        if (K == tok::r_square) {
          --BracketDepth;
          if (BracketDepth <= 0) {
            // End of capture list — emit last capture if any
            if (CaptureName) {
              llvm::StringRef CaptName = getOrigToken(*CaptureName, Out).text();
              LocalDecl LD;
              LD.Name = CaptName.str();
              LD.NameRange = tokenRange(*CaptureName, Out, Code);
              LD.DeclRange = LD.NameRange;
              LD.DeclOffset = tokenStartOffset(*CaptureName, Out);
              LD.ScopeId = NewScopeId;
              LD.Kind = PseudoModule::DeclKind::Variable;
              LD.IsDefinition = true;
              LD.IsCapture = true;
              LD.IsInitCapture = IsInitCapture;

              if (IsInitCapture && RhsNameTok) {
                // e.g. CB = std::move(CB) — RHS is the outer CB
                llvm::StringRef RhsName = getOrigToken(*RhsNameTok, Out).text();
                const LocalDecl *OuterDecl = lookupDecl(
                    SavedScope, RhsName, LD.DeclOffset, Scopes, Code);
                if (OuterDecl) {
                  LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                  if (!OuterDecl->TypeName.empty())
                    LD.TypeName = OuterDecl->TypeName;
                }
              } else {
                // Simple capture: look up name in parent scope
                const LocalDecl *OuterDecl = lookupDecl(
                    SavedScope, CaptName, LD.DeclOffset, Scopes, Code);
                if (OuterDecl) {
                  LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                  if (!OuterDecl->TypeName.empty())
                    LD.TypeName = OuterDecl->TypeName;
                }
              }
              Scopes[NewScopeId].Decls.push_back(std::move(LD));
              CaptureName = nullptr;
              IsInitCapture = false;
              RhsNameTok = nullptr;
            }
            break;
          }
          continue;
        }
        if (!InCaptures || BracketDepth <= 0)
          continue;

        // Separator between captures
        if (K == tok::comma) {
          if (CaptureName) {
            llvm::StringRef CaptName = getOrigToken(*CaptureName, Out).text();
            LocalDecl LD;
            LD.Name = CaptName.str();
            LD.NameRange = tokenRange(*CaptureName, Out, Code);
            LD.DeclRange = LD.NameRange;
            LD.DeclOffset = tokenStartOffset(*CaptureName, Out);
            LD.ScopeId = NewScopeId;
            LD.Kind = PseudoModule::DeclKind::Variable;
            LD.IsDefinition = true;
            LD.IsCapture = true;
            LD.IsInitCapture = IsInitCapture;

            if (IsInitCapture && RhsNameTok) {
              llvm::StringRef RhsName = getOrigToken(*RhsNameTok, Out).text();
              const LocalDecl *OuterDecl = lookupDecl(
                  SavedScope, RhsName, LD.DeclOffset, Scopes, Code);
              if (OuterDecl) {
                LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                if (!OuterDecl->TypeName.empty())
                  LD.TypeName = OuterDecl->TypeName;
              }
            } else {
              const LocalDecl *OuterDecl = lookupDecl(
                  SavedScope, CaptName, LD.DeclOffset, Scopes, Code);
              if (OuterDecl) {
                LD.CapturedDeclOffset = OuterDecl->DeclOffset;
                if (!OuterDecl->TypeName.empty())
                  LD.TypeName = OuterDecl->TypeName;
              }
            }
            Scopes[NewScopeId].Decls.push_back(std::move(LD));
          }
          CaptureName = nullptr;
          IsInitCapture = false;
          RhsNameTok = nullptr;
          continue;
        }

        // Skip '&' prefix before capture name
        if (K == tok::amp || K == tok::star)
          continue;
        // Skip '=' (capture-default) or '...'
        if (K == tok::ellipsis || K == tok::kw_this)
          continue;

        if (K == tok::raw_identifier || K == tok::identifier) {
          llvm::StringRef Word = getOrigToken(NodeTokens[I], Out).text();
          if (Word == "this")
            continue;
          if (!CaptureName) {
            CaptureName = &NodeTokens[I];
          } else if (IsInitCapture && !RhsNameTok) {
            // After '=', we want the first identifier (might be the RHS var, e.g. CB in CB = std::move(CB))
            RhsNameTok = &NodeTokens[I];
          }
          continue;
        }

        if (K == tok::equal && CaptureName) {
          IsInitCapture = true;
          continue;
        }
      }
    }

    // Recurse into children (lambda_declarator for params, compound_statement for body)
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 7. Enum specifier
  if (Sym == pseudo::cxx::Symbol::enum_specifier) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        NameTok = &NodeTokens[I];
    }
    std::string EnumName;
    if (NameTok) {
      EnumName = getOrigToken(*NameTok, Out).text().str();
      LocalDecl LD;
      LD.Name = EnumName;
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = PseudoModule::DeclKind::Enum;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    bool InBraces = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace) {
        InBraces = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::r_brace)
        break;
      if (InBraces && (NodeTokens[I].Kind == tok::raw_identifier ||
                       NodeTokens[I].Kind == tok::identifier)) {
        LocalDecl Member;
        Member.Name = getOrigToken(NodeTokens[I], Out).text().str();
        Member.NameRange = tokenRange(NodeTokens[I], Out, Code);
        Member.DeclRange = Member.NameRange;
        Member.DeclOffset = tokenStartOffset(NodeTokens[I], Out);
        Member.ScopeId = CurrentScopeId;
        Member.EnclosingClass = !EnumName.empty() ? EnumName : std::string(EnclosingClass);
        Member.IsDefinition = true;
        Member.Kind = PseudoModule::DeclKind::EnumValue;
        Scopes[CurrentScopeId].Decls.push_back(std::move(Member));
        while (I + 1 < NodeTokens.size() &&
               NodeTokens[I + 1].Kind != tok::comma &&
               NodeTokens[I + 1].Kind != tok::r_brace)
          ++I;
      }
    }
    return;
  }

  // 8. Alias declaration
  if (Sym == pseudo::cxx::Symbol::alias_declaration) {
    const pseudo::Token *NameTok = nullptr;
    size_t EqIdx = NodeTokens.size();
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          NameTok = &IdTok;
        }
      }
      if (NodeTokens[I].Kind == tok::equal) {
        EqIdx = I;
        break;
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = PseudoModule::DeclKind::TypeAlias;
      if (EqIdx + 1 < NodeTokens.size()) {
        size_t StartOff = tokenStartOffset(NodeTokens[EqIdx + 1], Out);
        size_t EndOff = tokenEndOffset(NodeTokens.back(), Out);
        if (StartOff < EndOff && EndOff <= Code.size()) {
          llvm::StringRef TypeText = Code.slice(StartOff, EndOff);
          size_t Semi = TypeText.find(';');
          if (Semi != llvm::StringRef::npos)
            TypeText = TypeText.take_front(Semi);
          LD.TypeName = TypeText.trim().str();
        }
      }
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 9. Template type parameter (template <typename T>, template <class T>)
  if (Sym == pseudo::cxx::Symbol::type_parameter) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = true;
      LD.Kind = PseudoModule::DeclKind::TemplateParam;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 9b. Concept definition (concept X = ...;)
  if (Sym == pseudo::cxx::Symbol::concept_definition) {
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_concept && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          LocalDecl LD;
          LD.Name = getOrigToken(IdTok, Out).text().str();
          LD.NameRange = tokenRange(IdTok, Out, Code);
          LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
          LD.DeclOffset = tokenStartOffset(IdTok, Out);
          LD.ScopeId = CurrentScopeId;
          LD.EnclosingClass = std::string(EnclosingClass);
          LD.IsDefinition = true;
          LD.Kind = PseudoModule::DeclKind::Concept;
          Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
          break;
        }
      }
    }
    return;
  }

  // 10. Using declaration (using ns::foo;)
  if (Sym == pseudo::cxx::Symbol::using_declaration) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      LD.NameRange = tokenRange(*NameTok, Out, Code);
      LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
      LD.DeclOffset = tokenStartOffset(*NameTok, Out);
      LD.ScopeId = CurrentScopeId;
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsDefinition = false;
      LD.Kind = PseudoModule::DeclKind::TypeAlias;
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
    return;
  }

  // 11. Namespace alias definition (namespace NA = RealNamespace;)
  if (Sym == pseudo::cxx::Symbol::namespace_alias_definition) {
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_namespace && I + 1 < NodeTokens.size()) {
        const auto &IdTok = NodeTokens[I + 1];
        if (IdTok.Kind == tok::raw_identifier || IdTok.Kind == tok::identifier) {
          LocalDecl LD;
          LD.Name = getOrigToken(IdTok, Out).text().str();
          LD.NameRange = tokenRange(IdTok, Out, Code);
          LD.DeclRange = nodeRange(StartTok, EndTok, Out, Code);
          LD.DeclOffset = tokenStartOffset(IdTok, Out);
          LD.ScopeId = CurrentScopeId;
          LD.EnclosingClass = std::string(EnclosingClass);
          LD.IsDefinition = true;
          LD.Kind = PseudoModule::DeclKind::Namespace;
          Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
          break;
        }
      }
    }
    return;
  }

  // Fallthrough: recurse down children
  llvm::StringRef ChildDeclaredType = DeclaredType;
  std::string ExtractedType;
  if (Sym == pseudo::cxx::Symbol::simple_declaration ||
      Sym == pseudo::cxx::Symbol::member_declaration) {
    auto Children = N->elements();
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart =
          tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd =
          tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size()) {
        ExtractedType = Code.slice(OffStart, OffEnd).trim().str();
        ChildDeclaredType = ExtractedType;
      }
    }
    if (ChildDeclaredType.empty()) {
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
            NodeTokens[I].Kind == tok::l_brace)
          break;
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          ChildDeclaredType = getOrigToken(NodeTokens[I], Out).text();
          break;
        }
      }
    }
  }

  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                EnclosingClass, ChildDeclaredType);
  }
}


const LocalDecl *findAnyDecl(llvm::StringRef Name,
                             const std::vector<LexicalScope> &Scopes,
                             bool ExpectsType = false) {
  const LocalDecl *FallbackMatch = nullptr;
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (D.Name == Name) {
        if (ExpectsType) {
          if (PseudoModule::isTypeDecl(D.Kind))
            return &D;
        } else {
          if (!PseudoModule::isTypeDecl(D.Kind))
            return &D;
          if (!FallbackMatch)
            FallbackMatch = &D;
        }
      }
    }
  }
  return FallbackMatch;
}

static bool isSameEntity(const LocalDecl *A, const LocalDecl *B) {
  if (!A || !B)
    return false;
  if (A == B)
    return true;
  if (A->Name != B->Name)
    return false;
  if (A->IsCapture && A->CapturedDeclOffset == B->DeclOffset &&
      A->CapturedDeclOffset != 0)
    return true;
  if (B->IsCapture && B->CapturedDeclOffset == A->DeclOffset &&
      B->CapturedDeclOffset != 0)
    return true;
  if (A->IsCapture && B->IsCapture &&
      A->CapturedDeclOffset == B->CapturedDeclOffset &&
      A->CapturedDeclOffset != 0)
    return true;
  if (A->IsMember && B->IsMember)
    return !A->EnclosingClass.empty() && A->EnclosingClass == B->EnclosingClass;
  if (!A->IsMember && !B->IsMember)
    return A->Kind == B->Kind;
  return false;
}

static const LocalDecl *
findMatchingDefinition(const LocalDecl *Decl,
                       const std::vector<LexicalScope> &Scopes) {
  if (!Decl || Decl->IsDefinition)
    return nullptr;
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (!D.IsDefinition || D.Name != Decl->Name)
        continue;
      if (Decl->IsMember) {
        if (D.IsMember && !Decl->EnclosingClass.empty() &&
            D.EnclosingClass == Decl->EnclosingClass)
          return &D;
      } else {
        if (!D.IsMember && D.Kind == Decl->Kind)
          return &D;
      }
    }
  }
  return nullptr;
}

static const LocalDecl *
findMatchingDeclaration(const LocalDecl *Def,
                        const std::vector<LexicalScope> &Scopes) {
  if (!Def || !Def->IsDefinition)
    return nullptr;
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (D.IsDefinition || D.Name != Def->Name)
        continue;
      if (Def->IsMember) {
        if (D.IsMember && !Def->EnclosingClass.empty() &&
            D.EnclosingClass == Def->EnclosingClass)
          return &D;
      } else {
        if (!D.IsMember && D.Kind == Def->Kind)
          return &D;
      }
    }
  }
  return nullptr;
}

const pseudo::Token *findTouchedIdentifier(const ParseOutput &Out,
                                           size_t Offset) {
  const pseudo::Token *Best = nullptr;
  for (const auto &T : Out.RawStream.tokens()) {
    if (T.Kind != tok::raw_identifier && T.Kind != tok::identifier)
      continue;
    size_t Start = tokenStartOffset(T, Out);
    size_t End = tokenEndOffset(T, Out);
    if (Offset >= Start && Offset <= End) {
      Best = &T;
      if (Offset < End)
        return Best;
    }
  }
  return Best;
}

class PseudoASTListener final : public FeatureModule::ASTListener {
public:
  void sawDiagnostic(const clang::Diagnostic &D, clangd::Diag &Diag) override {
    // Record diagnostics if needed for fallback telemetry
  }
};

} // namespace

PseudoModule::PseudoModule()
    : SupportedSymbolKinds(defaultSymbolKinds()),
      SupportedCompletionItemKinds(defaultCompletionItemKinds()) {}
PseudoModule::~PseudoModule() = default;

void *PseudoModule::typeId() const {
  return FeatureModuleSet::id<PseudoModule>();
}

const GlobalCompilationDatabase *PseudoModule::getCDB() const {
  if (TestCDB)
    return TestCDB;
  if (hasFacilities())
    return const_cast<PseudoModule *>(this)->cdb();
  return nullptr;
}

llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> PseudoModule::getFS() const {
  if (TestFS)
    return TestFS->view(std::nullopt);
  if (hasFacilities())
    return const_cast<PseudoModule *>(this)->fs().view(std::nullopt);
  return llvm::vfs::getRealFileSystem();
}

std::string PseudoModule::getDocument(PathRef File) {
  if (hasFacilities()) {
    if (auto Draft = server().getDraft(File))
      return *Draft;
  }
  if (auto FS = getFS()) {
    if (auto Buf = FS->getBufferForFile(File))
      return (*Buf)->getBuffer().str();
  }
  return "";
}

static void extractIncludeDirsFromArgs(
    llvm::ArrayRef<std::string> Args, llvm::StringRef WorkingDir,
    std::vector<std::string> &IncludeDirs) {
  for (size_t I = 0; I < Args.size(); ++I) {
    llvm::StringRef Arg = Args[I];
    llvm::StringRef Dir;
    if (Arg.starts_with("-I") || Arg.starts_with("/I")) {
      if (Arg.size() > 2) {
        Dir = Arg.drop_front(2);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    } else if (Arg.starts_with("-isystem")) {
      if (Arg.size() > 8) {
        Dir = Arg.drop_front(8);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    } else if (Arg.starts_with("-iquote")) {
      if (Arg.size() > 7) {
        Dir = Arg.drop_front(7);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    } else if (Arg.starts_with("-idirafter")) {
      if (Arg.size() > 10) {
        Dir = Arg.drop_front(10);
      } else if (I + 1 < Args.size()) {
        Dir = Args[++I];
      }
    }

    if (!Dir.empty()) {
      llvm::SmallString<256> AbsPath(Dir);
      if (!llvm::sys::path::is_absolute(AbsPath) && !WorkingDir.empty()) {
        llvm::sys::path::make_absolute(WorkingDir, AbsPath);
      }
      llvm::sys::path::remove_dots(AbsPath, /*remove_dot_dot=*/true);
      std::string S = AbsPath.str().str();
      if (std::find(IncludeDirs.begin(), IncludeDirs.end(), S) == IncludeDirs.end())
        IncludeDirs.push_back(std::move(S));
    }
  }
}

std::vector<std::string>
PseudoModule::getIncludeDirectories(PathRef File) const {
  std::vector<std::string> IncludeDirs;
  llvm::StringRef ParentDir = llvm::sys::path::parent_path(File);
  if (!ParentDir.empty())
    IncludeDirs.push_back(ParentDir.str());

  const auto *Database = getCDB();
  if (!Database)
    return IncludeDirs;

  std::optional<tooling::CompileCommand> Cmd = Database->getCompileCommand(File);
  if (!Cmd) {
    llvm::SmallString<256> CppCandidate(File);
    llvm::sys::path::replace_extension(CppCandidate, "cpp");
    Cmd = Database->getCompileCommand(CppCandidate);
    if (!Cmd) {
      CppCandidate = File;
      llvm::sys::path::replace_extension(CppCandidate, "cc");
      Cmd = Database->getCompileCommand(CppCandidate);
    }
    if (!Cmd) {
      CppCandidate = File;
      llvm::sys::path::replace_extension(CppCandidate, "c");
      Cmd = Database->getCompileCommand(CppCandidate);
    }
  }

  if (Cmd)
    extractIncludeDirsFromArgs(Cmd->CommandLine, Cmd->Directory, IncludeDirs);

  static const char *SystemDirs[] = {
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include/c++/v1",
      "/Library/Developer/CommandLineTools/SDKs/MacOSX.sdk/usr/include",
      "/Library/Developer/CommandLineTools/usr/include/c++/v1",
      "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include/c++/v1",
      "/Applications/Xcode.app/Contents/Developer/Platforms/MacOSX.platform/Developer/SDKs/MacOSX.sdk/usr/include",
      "/opt/homebrew/include",
      "/usr/include/c++/v1",
      "/usr/include/c++/14",
      "/usr/include/c++/13",
      "/usr/include/c++/12",
      "/usr/include/c++/11",
      "/usr/include/x86_64-linux-gnu/c++/14",
      "/usr/include/x86_64-linux-gnu/c++/13",
      "/usr/include/x86_64-linux-gnu/c++/12",
      "/usr/include/x86_64-linux-gnu/c++/11",
      "/usr/local/include",
      "/usr/include",
  };
  for (const char *SysDir : SystemDirs) {
    if (std::find(IncludeDirs.begin(), IncludeDirs.end(), SysDir) == IncludeDirs.end())
      IncludeDirs.push_back(SysDir);
  }

  return IncludeDirs;
}

std::vector<PseudoModule::IncludeDirective>
PseudoModule::extractIncludes(llvm::StringRef Code) {
  std::vector<IncludeDirective> Results;
  int LineNo = 0;
  while (!Code.empty()) {
    auto [Line, Rest] = Code.split('\n');
    Code = Rest;

    llvm::StringRef Trimmed = Line.ltrim(" \t");
    if (Trimmed.consume_front("#")) {
      Trimmed = Trimmed.ltrim(" \t");
      if (Trimmed.consume_front("include_next") ||
          Trimmed.consume_front("include")) {
        Trimmed = Trimmed.ltrim(" \t");
        if (Trimmed.consume_front("\"")) {
          auto EndQuote = Trimmed.find('\"');
          if (EndQuote != llvm::StringRef::npos) {
            IncludeDirective Inc;
            Inc.Written = Trimmed.slice(0, EndQuote).str();
            Inc.IsAngled = false;
            Inc.HashLine = LineNo;
            Results.push_back(std::move(Inc));
          }
        } else if (Trimmed.consume_front("<")) {
          auto EndAngle = Trimmed.find('>');
          if (EndAngle != llvm::StringRef::npos) {
            IncludeDirective Inc;
            Inc.Written = Trimmed.slice(0, EndAngle).str();
            Inc.IsAngled = true;
            Inc.HashLine = LineNo;
            Results.push_back(std::move(Inc));
          }
        }
      }
    }
    ++LineNo;
  }
  return Results;
}

std::string PseudoModule::resolveHeader(
    const IncludeDirective &Inc, llvm::StringRef CurrentDir,
    llvm::ArrayRef<std::string> IncludeDirs, llvm::vfs::FileSystem &FS) const {
  auto Exists = [&](llvm::StringRef P) -> bool {
    auto Stat = FS.status(P);
    return Stat && !Stat->isDirectory();
  };

  if (!Inc.IsAngled) {
    llvm::SmallString<256> Cand(CurrentDir);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();
  }

  for (const auto &Dir : IncludeDirs) {
    llvm::SmallString<256> Cand(Dir);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();
  }

  if (Inc.IsAngled) {
    llvm::SmallString<256> Cand(CurrentDir);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();
  }

  // Walk up parent directories of CurrentDir to locate include roots or project roots
  llvm::SmallString<256> Cur(CurrentDir);
  while (!Cur.empty()) {
    llvm::SmallString<256> Cand(Cur);
    llvm::sys::path::append(Cand, Inc.Written);
    llvm::sys::path::remove_dots(Cand, /*remove_dot_dot=*/true);
    if (Exists(Cand))
      return Cand.str().str();

    llvm::SmallString<256> CandInc(Cur);
    llvm::sys::path::append(CandInc, "include", Inc.Written);
    llvm::sys::path::remove_dots(CandInc, /*remove_dot_dot=*/true);
    if (Exists(CandInc))
      return CandInc.str().str();

    llvm::SmallString<256> CandClang(Cur);
    llvm::sys::path::append(CandClang, "clang", "include", Inc.Written);
    llvm::sys::path::remove_dots(CandClang, /*remove_dot_dot=*/true);
    if (Exists(CandClang))
      return CandClang.str().str();

    llvm::SmallString<256> CandLLVM(Cur);
    llvm::sys::path::append(CandLLVM, "llvm", "include", Inc.Written);
    llvm::sys::path::remove_dots(CandLLVM, /*remove_dot_dot=*/true);
    if (Exists(CandLLVM))
      return CandLLVM.str().str();

    std::string Parent = llvm::sys::path::parent_path(Cur).str();
    if (Parent == Cur)
      break;
    Cur = Parent;
  }

  return "";
}

std::shared_ptr<const PseudoModule::HeaderInfo>
PseudoModule::getHeaderInfo(llvm::StringRef HeaderPath,
                            llvm::vfs::FileSystem &FS) {
  {
    std::lock_guard<std::mutex> Lock(HeaderCacheMutex);
    auto It = HeaderCache.find(HeaderPath);
    if (It != HeaderCache.end())
      return It->second;
  }

  auto Info = std::make_shared<HeaderInfo>();
  Info->Path = HeaderPath.str();

  auto Buffer = FS.getBufferForFile(HeaderPath);
  if (!Buffer || !*Buffer) {
    std::lock_guard<std::mutex> Lock(HeaderCacheMutex);
    HeaderCache[HeaderPath] = Info;
    return Info;
  }

  llvm::StringRef Code = (*Buffer)->getBuffer();
  Info->Includes = extractIncludes(Code);

  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root) {
    std::lock_guard<std::mutex> Lock(HeaderCacheMutex);
    HeaderCache[HeaderPath] = Info;
    return Info;
  }

  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  for (const auto &Scope : Scopes) {
    for (const auto &D : Scope.Decls) {
      if (D.Kind == PseudoModule::DeclKind::Parameter || D.IsParameter)
        continue;
      HeaderDecl HD;
      HD.Name = D.Name;
      HD.NameRange = D.NameRange;
      HD.ScopeRange = D.DeclRange;
      std::string EnclNs;
      for (size_t Cur = Scope.Id; Cur != 0; Cur = Scopes[Cur].ParentId) {
        if (Scopes[Cur].Kind == ScopeKind::Namespace && !Scopes[Cur].Name.empty()) {
          EnclNs = Scopes[Cur].Name;
          break;
        }
      }
      HD.EnclosingScope = (Scope.Kind == ScopeKind::Namespace && !Scope.Name.empty())
                              ? Scope.Name
                              : EnclNs;
      HD.EnclosingClass = !D.EnclosingClass.empty()
                              ? D.EnclosingClass
                              : (Scope.Kind == ScopeKind::Class ? Scope.Name : "");
      HD.TypeName = D.TypeName;
      HD.Kind = D.Kind;
      HD.IsDefinition = D.IsDefinition;
      Info->Decls.push_back(std::move(HD));
    }
  }

  {
    std::lock_guard<std::mutex> Lock(HeaderCacheMutex);
    HeaderCache[HeaderPath] = Info;
  }
  return Info;
}

void PseudoModule::initializeLSP(LSPBinder &Bind,
                                 const llvm::json::Object &ClientCaps,
                                 llvm::json::Object &ServerCaps) {
  if (!Enabled)
    return;

  ClientCapabilities Caps;
  llvm::json::Path::Root Root;
  llvm::json::Value Val = llvm::json::Object(ClientCaps);
  fromJSON(Val, Caps, Root);
  SupportsHierarchicalDocumentSymbol = Caps.HierarchicalDocumentSymbol;
  if (Caps.WorkspaceSymbolKinds)
    SupportedSymbolKinds |= *Caps.WorkspaceSymbolKinds;
  if (Caps.CompletionItemKinds)
    SupportedCompletionItemKinds |= *Caps.CompletionItemKinds;
  SupportsCompletionLabelDetails = Caps.CompletionLabelDetail;
  HoverContentFormat = Caps.HoverContentFormat;
  SupportsReferenceContainer = Caps.ReferenceContainer;

  BaseCodeCompleteOpts.EnableSnippets = Caps.CompletionSnippets;
  BaseCodeCompleteOpts.IncludeFixIts = Caps.CompletionFixes;
  BaseCodeCompleteOpts.EnableInsertReplace = Caps.InsertReplace;
  BaseCodeCompleteOpts.DocumentationFormat = Caps.CompletionDocumentationFormat;

  Bind.method("textDocument/definition", this,
              &PseudoModule::onGoToDefinition);
  Bind.method("textDocument/declaration", this,
              &PseudoModule::onGoToDeclaration);
  Bind.method("textDocument/references", this,
              &PseudoModule::onReference);
  Bind.method("textDocument/documentHighlight", this,
              &PseudoModule::onDocumentHighlight);
  Bind.method("textDocument/documentSymbol", this,
              &PseudoModule::onDocumentSymbol);
  Bind.method("textDocument/selectionRange", this,
              &PseudoModule::onSelectionRange);
  Bind.method("textDocument/foldingRange", this,
              &PseudoModule::onFoldingRange);
  Bind.method("textDocument/hover", this,
              &PseudoModule::onHover);
  Bind.method("textDocument/semanticTokens/full", this,
              &PseudoModule::onSemanticTokens);
  Bind.method("textDocument/semanticTokens/full/delta", this,
              &PseudoModule::onSemanticTokensDelta);
  Bind.method("textDocument/completion", this,
              &PseudoModule::onCompletion);
  Bind.method("textDocument/ast", this,
              &PseudoModule::onAST);

  Bind.method("clangd/pseudoSymbols", this,
              &PseudoModule::onCustomPseudoSymbols);
}

std::unique_ptr<FeatureModule::ASTListener> PseudoModule::astListeners() {
  return std::make_unique<PseudoASTListener>();
}

static ASTNode buildTypeNode(llvm::StringRef TypeStr, Range R) {
  TypeStr = TypeStr.trim();
  if (TypeStr.empty()) {
    ASTNode T;
    T.role = "type";
    T.kind = "Builtin";
    T.detail = "void";
    T.arcana = "QualType 'void'";
    T.range = R;
    return T;
  }

  if (TypeStr.starts_with("struct "))
    TypeStr = TypeStr.drop_front(7).trim();
  else if (TypeStr.starts_with("class "))
    TypeStr = TypeStr.drop_front(6).trim();
  else if (TypeStr.starts_with("enum "))
    TypeStr = TypeStr.drop_front(5).trim();
  else if (TypeStr.starts_with("typename "))
    TypeStr = TypeStr.drop_front(9).trim();

  while (TypeStr.ends_with("::"))
    TypeStr = TypeStr.drop_back(2).trim();

  if (TypeStr.empty()) {
    ASTNode T;
    T.role = "type";
    T.kind = "Builtin";
    T.detail = "void";
    T.arcana = "QualType 'void'";
    T.range = R;
    return T;
  }

  if (TypeStr.ends_with("&&")) {
    ASTNode T;
    T.role = "type";
    T.kind = "RValueReference";
    T.arcana = "RValueReferenceType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(2), R));
    return T;
  }
  if (TypeStr.ends_with("&")) {
    ASTNode T;
    T.role = "type";
    T.kind = "LValueReference";
    T.arcana = "LValueReferenceType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(1), R));
    return T;
  }
  if (TypeStr.ends_with("*")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Pointer";
    T.arcana = "PointerType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(1), R));
    return T;
  }
  if (TypeStr.starts_with("const ") || TypeStr.starts_with("const\t")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Qualified";
    T.detail = "const";
    T.arcana = "QualType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_front(5), R));
    return T;
  }
  if (TypeStr.ends_with(" const")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Qualified";
    T.detail = "const";
    T.arcana = "QualType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_back(6), R));
    return T;
  }
  if (TypeStr.starts_with("volatile ") || TypeStr.starts_with("volatile\t")) {
    ASTNode T;
    T.role = "type";
    T.kind = "Qualified";
    T.detail = "volatile";
    T.arcana = "QualType '" + TypeStr.str() + "'";
    T.range = R;
    T.children.push_back(buildTypeNode(TypeStr.drop_front(8), R));
    return T;
  }

  static const llvm::StringSet<> Builtins = {
      "void", "bool", "char", "signed char", "unsigned char",
      "short", "unsigned short", "int", "unsigned int", "signed int",
      "long", "unsigned long", "long long", "unsigned long long",
      "float", "double", "long double", "wchar_t", "char8_t",
      "char16_t", "char32_t", "size_t", "int8_t", "uint8_t",
      "int16_t", "uint16_t", "int32_t", "uint32_t", "int64_t", "uint64_t",
      "nullptr_t", "ptrdiff_t", "intptr_t", "uintptr_t"};

  if (Builtins.contains(TypeStr)) {
    ASTNode T;
    T.role = "type";
    T.kind = "Builtin";
    T.detail = TypeStr.str();
    T.arcana = "BuiltinType '" + TypeStr.str() + "'";
    T.range = R;
    return T;
  }

  if (TypeStr == "auto") {
    ASTNode T;
    T.role = "type";
    T.kind = "Auto";
    T.detail = "auto";
    T.arcana = "AutoType";
    T.range = R;
    return T;
  }

  ASTNode T;
  T.role = "type";
  T.kind = "Record";
  T.detail = TypeStr.str();
  T.arcana = "RecordType '" + TypeStr.str() + "'";
  T.range = R;
  if (TypeStr.contains("::")) {
    size_t Colons = TypeStr.rfind("::");
    llvm::StringRef Name = TypeStr.drop_front(Colons + 2).trim();
    if (!Name.empty()) {
      std::string Ns = TypeStr.take_front(Colons + 2).str();
      T.detail = Name.str();
      ASTNode Spec;
      Spec.role = "specifier";
      Spec.kind = "Namespace";
      Spec.detail = Ns;
      Spec.range = R;
      T.children.push_back(std::move(Spec));
    }
  }
  if (T.detail.empty())
    T.detail = "(anonymous)";
  return T;
}

struct ExtractedParam {
  std::string Name;
  std::string Type;
  Range ParamRange;
  Range TypeRange;
  Range NameRange;
};

static std::vector<ExtractedParam>
extractParameters(llvm::ArrayRef<pseudo::Token> Tokens, const ParseOutput &Out,
                  llvm::StringRef Code) {
  std::vector<ExtractedParam> Params;
  size_t LParen = Tokens.size();
  for (size_t I = 0; I < Tokens.size(); ++I) {
    if (Tokens[I].Kind == tok::l_paren) {
      LParen = I;
      break;
    }
  }
  if (LParen == Tokens.size())
    return Params;

  int Depth = 1;
  size_t RParen = LParen + 1;
  while (RParen < Tokens.size() && Depth > 0) {
    if (Tokens[RParen].Kind == tok::l_paren)
      ++Depth;
    else if (Tokens[RParen].Kind == tok::r_paren)
      --Depth;
    if (Depth == 0)
      break;
    ++RParen;
  }

  size_t Cur = LParen + 1;
  while (Cur < RParen) {
    if (Tokens[Cur].Kind == tok::comma) {
      ++Cur;
      continue;
    }
    size_t ParamStart = Cur;
    int PDepth = 0;
    int ADepth = 0;
    while (Cur < RParen) {
      if (Tokens[Cur].Kind == tok::l_paren)
        ++PDepth;
      else if (Tokens[Cur].Kind == tok::r_paren)
        --PDepth;
      else if (Tokens[Cur].Kind == tok::less)
        ++ADepth;
      else if (Tokens[Cur].Kind == tok::greater)
        --ADepth;
      else if (Tokens[Cur].Kind == tok::comma && PDepth == 0 && ADepth <= 0)
        break;
      ++Cur;
    }
    size_t ParamEnd = Cur;
    if (ParamStart >= ParamEnd)
      continue;

    // Ignore default arguments after '='
    size_t RealEnd = ParamEnd;
    for (size_t I = ParamStart; I < ParamEnd; ++I) {
      if (Tokens[I].Kind == tok::equal) {
        RealEnd = I;
        break;
      }
    }

    std::string ParamName;
    std::string ParamType;
    size_t TypeStartIdx = ParamStart;
    size_t TypeEndIdx = RealEnd;
    size_t NameTokIdx = RealEnd;

    if (RealEnd == ParamStart + 1) {
      size_t TStart = tokenStartOffset(Tokens[ParamStart], Out);
      size_t TEnd = tokenEndOffset(Tokens[ParamStart], Out);
      if (TStart < TEnd && TEnd <= Code.size())
        ParamType = Code.slice(TStart, TEnd).trim().str();
      TypeEndIdx = ParamStart + 1;
    } else if (RealEnd > ParamStart + 1) {
      size_t LastIdx = RealEnd - 1;
      if (Tokens[LastIdx].Kind == tok::raw_identifier ||
          Tokens[LastIdx].Kind == tok::identifier) {
        ParamName = getOrigToken(Tokens[LastIdx], Out).text().str();
        NameTokIdx = LastIdx;
        TypeEndIdx = LastIdx;
        size_t TStart = tokenStartOffset(Tokens[ParamStart], Out);
        size_t TEnd = tokenStartOffset(Tokens[LastIdx], Out);
        if (TStart < TEnd && TEnd <= Code.size())
          ParamType = Code.slice(TStart, TEnd).trim().str();
      } else {
        size_t TStart = tokenStartOffset(Tokens[ParamStart], Out);
        size_t TEnd = tokenEndOffset(Tokens[RealEnd - 1], Out);
        if (TStart < TEnd && TEnd <= Code.size())
          ParamType = Code.slice(TStart, TEnd).trim().str();
      }
    }

    if (ParamType.empty())
      ParamType = "int";
    if (ParamType != "void" || !ParamName.empty()) {
      ExtractedParam EP;
      EP.Name = std::move(ParamName);
      EP.Type = std::move(ParamType);

      size_t PStartOff = tokenStartOffset(Tokens[ParamStart], Out);
      size_t PEndOff = tokenEndOffset(Tokens[ParamEnd - 1], Out);
      EP.ParamRange = Range{offsetToPosition(Code, PStartOff),
                            offsetToPosition(Code, PEndOff)};

      size_t TStartOff = tokenStartOffset(Tokens[TypeStartIdx], Out);
      size_t TEndOff = (TypeEndIdx > TypeStartIdx)
                           ? tokenEndOffset(Tokens[TypeEndIdx - 1], Out)
                           : PEndOff;
      EP.TypeRange = Range{offsetToPosition(Code, TStartOff),
                           offsetToPosition(Code, TEndOff)};

      if (NameTokIdx < RealEnd) {
        EP.NameRange = tokenRange(Tokens[NameTokIdx], Out, Code);
      } else {
        EP.NameRange = EP.ParamRange;
      }

      Params.push_back(std::move(EP));
    }
  }
  return Params;
}

static ASTNode
buildFunctionProto(llvm::StringRef ReturnType,
                   llvm::ArrayRef<ExtractedParam> Params,
                   Range ProtoRange, Range ReturnTypeRange) {
  ASTNode Proto;
  Proto.role = "type";
  Proto.kind = "FunctionProto";
  Proto.range = ProtoRange;
  std::string ParamSummary;
  for (size_t I = 0; I < Params.size(); ++I) {
    if (I > 0)
      ParamSummary += ", ";
    ParamSummary += Params[I].Type;
    if (!Params[I].Name.empty()) {
      ParamSummary += " ";
      ParamSummary += Params[I].Name;
    }
  }
  std::string Sig = ReturnType.empty()
                        ? "(" + ParamSummary + ")"
                        : ReturnType.str() + " (" + ParamSummary + ")";
  Proto.arcana = "QualType '" + Sig + "'";
  if (!ReturnType.empty())
    Proto.children.push_back(buildTypeNode(ReturnType, ReturnTypeRange));
  for (const auto &P : Params) {
    ASTNode Parm;
    Parm.role = "declaration";
    Parm.kind = "ParmVar";
    Parm.detail = P.Name.empty() ? "(anonymous)" : P.Name;
    Parm.range = P.ParamRange;
    Parm.arcana = "ParmVarDecl " + Parm.detail + " '" + P.Type + "'";
    Parm.children.push_back(buildTypeNode(P.Type, P.TypeRange));
    Proto.children.push_back(std::move(Parm));
  }
  return Proto;
}

static bool containsSymbol(const pseudo::ForestNode *N, pseudo::SymbolID Target) {
  if (!N)
    return false;
  if (N->symbol() == Target)
    return true;
  if (N->kind() == pseudo::ForestNode::Sequence) {
    for (const auto *Child : N->elements()) {
      if (containsSymbol(Child, Target))
        return true;
    }
  } else if (N->kind() == pseudo::ForestNode::Ambiguous) {
    for (const auto *Alt : N->alternatives()) {
      if (containsSymbol(Alt, Target))
        return true;
    }
  }
  return false;
}

static bool isLikelyFunctionDeclaration(llvm::ArrayRef<pseudo::Token> Tokens,
                                        size_t LParenIdx,
                                        bool InsideFunctionBody) {
  if (LParenIdx >= Tokens.size())
    return false;

  size_t RParenIdx = LParenIdx + 1;
  int Depth = 1;
  while (RParenIdx < Tokens.size() && Depth > 0) {
    if (Tokens[RParenIdx].Kind == tok::l_paren)
      ++Depth;
    else if (Tokens[RParenIdx].Kind == tok::r_paren)
      --Depth;
    if (Depth == 0)
      break;
    ++RParenIdx;
  }
  if (RParenIdx >= Tokens.size() && Depth > 0)
    return false;

  // Empty parameter list: ()
  if (RParenIdx == LParenIdx + 1)
    return !InsideFunctionBody;

  // If any token inside (...) is a literal, member access, call, or operator,
  // then this is direct-initialization of a variable (constructor arguments)!
  for (size_t I = LParenIdx + 1; I < RParenIdx; ++I) {
    auto K = Tokens[I].Kind;
    if (K == tok::string_literal || K == tok::wide_string_literal ||
        K == tok::utf8_string_literal || K == tok::utf16_string_literal ||
        K == tok::utf32_string_literal || K == tok::numeric_constant ||
        K == tok::char_constant || K == tok::kw_true || K == tok::kw_false ||
        K == tok::kw_nullptr || K == tok::kw_this || K == tok::period ||
        K == tok::arrow || K == tok::question || K == tok::plusplus ||
        K == tok::minusminus || K == tok::exclaim || K == tok::tilde)
      return false;
  }

  if (InsideFunctionBody) {
    bool HasTypeKeywordInParams = false;
    for (size_t I = LParenIdx + 1; I < RParenIdx; ++I) {
      auto K = Tokens[I].Kind;
      if (K == tok::kw_int || K == tok::kw_char || K == tok::kw_bool ||
          K == tok::kw_void || K == tok::kw_float || K == tok::kw_double ||
          K == tok::kw_auto) {
        HasTypeKeywordInParams = true;
        break;
      }
    }
    if (!HasTypeKeywordInParams)
      return false;
  }

  return true;
}

static void buildASTNodes(const pseudo::ForestNode *N, pseudo::Token::Index End,
                          const ParseOutput &Out, llvm::StringRef Code,
                          std::vector<ASTNode> &Nodes, bool InsideClass = false,
                          llvm::StringRef EnclosingClass = "",
                          llvm::StringRef DeclaredType = "",
                          std::optional<Range> DeclaredTypeRange = std::nullopt) {
  if (!N)
    return;

  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (Alts.empty())
      return;
    const pseudo::ForestNode *DeclAlt = nullptr;
    const pseudo::ForestNode *ExprAlt = nullptr;
    for (const auto *Alt : Alts) {
      if (containsSymbol(Alt, pseudo::cxx::Symbol::declaration_statement) ||
          containsSymbol(Alt, pseudo::cxx::Symbol::simple_declaration))
        DeclAlt = Alt;
      else if (containsSymbol(Alt, pseudo::cxx::Symbol::expression_statement))
        ExprAlt = Alt;
    }
    if (DeclAlt && ExprAlt) {
      auto StartTok = N->startTokenIndex();
      auto NodeTokens =
          Out.ParseableStream.tokens().slice(StartTok, End - StartTok);
      size_t SemiIdx = NodeTokens.size();
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::semi) {
          SemiIdx = I;
          break;
        }
      }
      bool HasTypeKeyword = false;
      int IdentCountBeforeParenOrEqual = 0;
      for (size_t I = 0; I < SemiIdx; ++I) {
        const auto &T = NodeTokens[I];
        if (T.Kind == tok::l_paren || T.Kind == tok::equal)
          break;
        if (T.Kind == tok::kw_auto || T.Kind == tok::kw_void ||
            T.Kind == tok::kw_int || T.Kind == tok::kw_char ||
            T.Kind == tok::kw_bool || T.Kind == tok::kw_float ||
            T.Kind == tok::kw_double || T.Kind == tok::kw_class ||
            T.Kind == tok::kw_struct || T.Kind == tok::kw_static ||
            T.Kind == tok::kw_constexpr || T.Kind == tok::kw_const) {
          HasTypeKeyword = true;
        }
        if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
          ++IdentCountBeforeParenOrEqual;
      }
      const pseudo::ForestNode *Chosen =
          (HasTypeKeyword || IdentCountBeforeParenOrEqual >= 2) ? DeclAlt
                                                                : ExprAlt;
      buildASTNodes(Chosen, End, Out, Code, Nodes, InsideClass, EnclosingClass,
                    DeclaredType, DeclaredTypeRange);
      return;
    }

    auto It = Out.Disambig.find(N);
    unsigned AltIdx =
        (It != Out.Disambig.end() && It->second < Alts.size()) ? It->second : 0;
    buildASTNodes(Alts[AltIdx], End, Out, Code, Nodes, InsideClass,
                  EnclosingClass, DeclaredType, DeclaredTypeRange);
    return;
  }

  auto StartTok = N->startTokenIndex();
  auto EndTok = End;
  if (StartTok >= EndTok || StartTok >= Out.ParseableStream.tokens().size())
    return;

  auto NodeTokens =
      Out.ParseableStream.tokens().slice(StartTok, EndTok - StartTok);
  Range NRange = nodeRange(StartTok, EndTok, Out, Code);

  if (N->kind() == pseudo::ForestNode::Opaque) {
    std::vector<LexicalScope> TempScopes;
    size_t TempScopeId = 0;
    LexicalScope RootS;
    RootS.Id = 0;
    TempScopes.push_back(std::move(RootS));
    scanOpaqueDeclarations(StartTok, End, Out, Code, TempScopes, TempScopeId,
                           EnclosingClass);
    for (const auto &D : TempScopes[0].Decls) {
      ASTNode DN;
      DN.role = "declaration";
      DN.kind = D.IsMember ? "Field"
                           : (D.Kind == PseudoModule::DeclKind::Function
                                  ? "Function"
                                  : "Var");
      DN.detail = D.Name;
      DN.range = D.DeclRange;
      DN.arcana = DN.kind + "Decl " + D.Name + " '" + D.TypeName + "'";
      DN.children.push_back(buildTypeNode(D.TypeName, D.NameRange));
      Nodes.push_back(std::move(DN));
    }
    return;
  }

  if (N->kind() != pseudo::ForestNode::Sequence)
    return;

  pseudo::SymbolID Sym = N->symbol();

  // 1. Namespaces
  if (Sym == pseudo::cxx::Symbol::named_namespace_definition ||
      Sym == pseudo::cxx::Symbol::nested_namespace_definition ||
      Sym == pseudo::cxx::Symbol::unnamed_namespace_definition) {
    ASTNode NS;
    NS.role = "declaration";
    NS.kind = "Namespace";
    std::string NsName;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        if (!NsName.empty() && I > 0 &&
            NodeTokens[I - 1].Kind == tok::coloncolon)
          NsName += "::";
        NsName += getOrigToken(NodeTokens[I], Out).text().str();
      }
    }
    NS.detail = NsName.empty() ? "(anonymous)" : NsName;
    NS.arcana = "NamespaceDecl " + NS.detail;
    NS.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, NS.children, false, "");
    }
    Nodes.push_back(std::move(NS));
    return;
  }

  // 2. Class specifier
  if (Sym == pseudo::cxx::Symbol::class_specifier) {
    ASTNode CR;
    CR.role = "declaration";
    CR.kind = "CXXRecord";
    std::string ClassName;
    bool IsStruct = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace ||
          NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::kw_struct)
        IsStruct = true;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        ClassName = getOrigToken(NodeTokens[I], Out).text().str();
    }
    if (ClassName.empty())
      ClassName = IsStruct ? "(anonymous struct)" : "(anonymous class)";
    CR.detail = ClassName;
    CR.arcana = "CXXRecordDecl " + ClassName;
    CR.range = NRange;

    // Base specifiers
    bool InBase = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::colon) {
        InBase = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::l_brace)
        break;
      if (InBase && (NodeTokens[I].Kind == tok::raw_identifier ||
                     NodeTokens[I].Kind == tok::identifier)) {
        std::string BaseName = getOrigToken(NodeTokens[I], Out).text().str();
        ASTNode Base;
        Base.role = "base";
        Base.kind = "public";
        Base.range = tokenRange(NodeTokens[I], Out, Code);
        Base.children.push_back(
            buildTypeNode(BaseName, Base.range.value_or(Range{})));
        CR.children.push_back(std::move(Base));
      }
    }

    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, CR.children, true,
                    ClassName);
    }
    Nodes.push_back(std::move(CR));
    return;
  }

  // 3. Enum specifier
  if (Sym == pseudo::cxx::Symbol::enum_specifier ||
      Sym == pseudo::cxx::Symbol::opaque_enum_declaration) {
    ASTNode EN;
    EN.role = "declaration";
    EN.kind = "Enum";
    std::string EnumName;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace ||
          NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        EnumName = getOrigToken(NodeTokens[I], Out).text().str();
    }
    if (EnumName.empty())
      EnumName = "(anonymous enum)";
    EN.detail = EnumName;
    EN.arcana = "EnumDecl " + EnumName;
    EN.range = NRange;

    // Enumerator members
    bool InBraces = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace) {
        InBraces = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::r_brace)
        break;
      if (InBraces && (NodeTokens[I].Kind == tok::raw_identifier ||
                       NodeTokens[I].Kind == tok::identifier)) {
        std::string MemName = getOrigToken(NodeTokens[I], Out).text().str();
        ASTNode EC;
        EC.role = "declaration";
        EC.kind = "EnumConstant";
        EC.detail = MemName;
        EC.arcana = "EnumConstantDecl " + MemName;
        EC.range = tokenRange(NodeTokens[I], Out, Code);
        EN.children.push_back(std::move(EC));
        while (I + 1 < NodeTokens.size() &&
               NodeTokens[I + 1].Kind != tok::comma &&
               NodeTokens[I + 1].Kind != tok::r_brace)
          ++I;
      }
    }
    Nodes.push_back(std::move(EN));
    return;
  }

  // Access specifier (public:, protected:, private:)
  if (Sym == pseudo::cxx::Symbol::access_specifier) {
    ASTNode AN;
    AN.role = "declaration";
    AN.kind = "AccessSpec";
    AN.detail = "";
    AN.arcana = "AccessSpecDecl";
    AN.range = NRange;
    Nodes.push_back(std::move(AN));
    return;
  }

  // 4. Function definition
  if (Sym == pseudo::cxx::Symbol::function_definition) {
    auto Children = N->elements();

    // Scan NodeTokens before '{' or ':' (ctor initializer) to find the parameter list '('
    size_t LParenIdx = NodeTokens.size();
    int AngleDepth = 0;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_brace || NodeTokens[I].Kind == tok::colon)
        break;
      if (NodeTokens[I].Kind == tok::less)
        ++AngleDepth;
      else if (NodeTokens[I].Kind == tok::greater) {
        if (AngleDepth > 0)
          --AngleDepth;
      } else if (NodeTokens[I].Kind == tok::l_paren && AngleDepth == 0) {
        LParenIdx = I;
        break;
      }
    }

    std::string FuncName;
    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *FirstNameTok = nullptr;
    if (LParenIdx < NodeTokens.size()) {
      int OpTokIdx = -1;
      for (int I = 0; I < static_cast<int>(LParenIdx); ++I) {
        if (NodeTokens[I].Kind == tok::kw_operator) {
          OpTokIdx = I;
          break;
        }
      }
      if (OpTokIdx >= 0) {
        int EndNameIdx = static_cast<int>(LParenIdx) - 1;
        while (EndNameIdx > OpTokIdx &&
               (NodeTokens[EndNameIdx].Kind == tok::r_paren ||
                NodeTokens[EndNameIdx].Kind == tok::l_paren))
          --EndNameIdx;
        int StartNameIdx = OpTokIdx;
        while (StartNameIdx >= 2 &&
               NodeTokens[StartNameIdx - 1].Kind == tok::coloncolon &&
               (NodeTokens[StartNameIdx - 2].Kind == tok::raw_identifier ||
                NodeTokens[StartNameIdx - 2].Kind == tok::identifier)) {
          StartNameIdx -= 2;
        }
        FirstNameTok = &NodeTokens[StartNameIdx];
        NameTok = &NodeTokens[EndNameIdx];
        size_t SOff = tokenStartOffset(*FirstNameTok, Out);
        size_t EOff = tokenEndOffset(*NameTok, Out);
        if (SOff < EOff && EOff <= Code.size())
          FuncName = Code.slice(SOff, EOff).trim().str();
      } else {
        int EndNameIdx = static_cast<int>(LParenIdx) - 1;
        while (EndNameIdx >= 0 &&
               NodeTokens[EndNameIdx].Kind != tok::raw_identifier &&
               NodeTokens[EndNameIdx].Kind != tok::identifier)
          --EndNameIdx;

        if (EndNameIdx >= 0) {
          NameTok = &NodeTokens[EndNameIdx];
          int StartNameIdx = EndNameIdx;
          while (StartNameIdx > 0) {
            if (NodeTokens[StartNameIdx - 1].Kind == tok::coloncolon &&
                StartNameIdx >= 2 &&
                (NodeTokens[StartNameIdx - 2].Kind == tok::raw_identifier ||
                 NodeTokens[StartNameIdx - 2].Kind == tok::identifier)) {
              StartNameIdx -= 2;
            } else if (NodeTokens[StartNameIdx - 1].Kind == tok::tilde) {
              StartNameIdx -= 1;
            } else {
              break;
            }
          }
          FirstNameTok = &NodeTokens[StartNameIdx];
          size_t SOff = tokenStartOffset(*FirstNameTok, Out);
          size_t EOff = tokenEndOffset(*NameTok, Out);
          if (SOff < EOff && EOff <= Code.size())
            FuncName = Code.slice(SOff, EOff).trim().str();
        }
      }
    }

    std::string ReturnType = "void";
    Range ReturnTypeRange = NRange;
    if (FirstNameTok && FirstNameTok > NodeTokens.data()) {
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*FirstNameTok, Out);
      if (TStart < TEnd && TEnd <= Code.size()) {
        std::string RT = Code.slice(TStart, TEnd).trim().str();
        static const char *const Specifiers[] = {
            "inline", "static", "virtual", "constexpr", "consteval",
            "constinit", "friend", "explicit"};
        bool Stripped = true;
        while (Stripped) {
          Stripped = false;
          for (const char *Spec : Specifiers) {
            llvm::StringRef RRef(RT);
            if (RRef.starts_with(Spec) &&
                (RRef.size() == strlen(Spec) || isspace(RRef[strlen(Spec)]))) {
              RT = RRef.drop_front(strlen(Spec)).trim().str();
              Stripped = true;
            }
          }
        }
        if (!RT.empty())
          ReturnType = RT;
        pseudo::Token::Index FIdx =
            FirstNameTok - Out.ParseableStream.tokens().data();
        if (FIdx > StartTok)
          ReturnTypeRange = nodeRange(StartTok, FIdx, Out, Code);
      }
    } else {
      ReturnType = "";
    }

    if (ReturnType == "auto") {
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::arrow && I + 1 < NodeTokens.size()) {
          size_t RStart = tokenStartOffset(NodeTokens[I + 1], Out);
          size_t REnd = tokenEndOffset(NodeTokens.back(), Out);
          for (size_t J = I + 1; J < NodeTokens.size(); ++J) {
            if (NodeTokens[J].Kind == tok::l_brace ||
                NodeTokens[J].Kind == tok::semi) {
              REnd = tokenStartOffset(NodeTokens[J], Out);
              break;
            }
          }
          if (RStart < REnd && REnd <= Code.size()) {
            std::string Trailing = Code.slice(RStart, REnd).trim().str();
            if (!Trailing.empty())
              ReturnType = Trailing;
          }
          break;
        }
      }
    }

    size_t RParenIdx = LParenIdx;
    if (LParenIdx < NodeTokens.size()) {
      int Depth = 1;
      RParenIdx = LParenIdx + 1;
      while (RParenIdx < NodeTokens.size() && Depth > 0) {
        if (NodeTokens[RParenIdx].Kind == tok::l_paren)
          ++Depth;
        else if (NodeTokens[RParenIdx].Kind == tok::r_paren)
          --Depth;
        if (Depth == 0)
          break;
        ++RParenIdx;
      }
    }

    Range ProtoRange = (RParenIdx < NodeTokens.size())
                           ? nodeRange(StartTok, StartTok + RParenIdx + 1, Out, Code)
                           : NRange;

    auto Params = extractParameters(NodeTokens, Out, Code);

    std::string Kind = "Function";
    std::string Detail = FuncName;
    if (FuncName.find("::") != std::string::npos) {
      Kind = "CXXMethod";
      Detail = FuncName.substr(FuncName.rfind("::") + 2);
    }

    // Check for operator overload: operator(), operator[], operator=, conversion operator
    if (FuncName.find("operator") != std::string::npos) {
      size_t OpPos = FuncName.find("operator");
      std::string AfterOp = FuncName.substr(OpPos + 8);
      std::string AfterTrimmed = llvm::StringRef(AfterOp).trim().str();
      if (!AfterTrimmed.empty() && AfterTrimmed.front() != '(' &&
          AfterTrimmed.front() != '[' && AfterTrimmed.front() != '=' &&
          AfterTrimmed.front() != '!' && AfterTrimmed.front() != '+' &&
          AfterTrimmed.front() != '-' && AfterTrimmed.front() != '*' &&
          AfterTrimmed.front() != '<' && AfterTrimmed.front() != '>') {
        Kind = "CXXConversion";
        std::string ConvType = AfterTrimmed;
        if (ConvType.rfind("::") != std::string::npos)
          ConvType = ConvType.substr(ConvType.rfind("::") + 2);
        Detail = "operator " + ConvType;
      } else {
        if (AfterTrimmed.empty())
          Detail = "operator()";
        else
          Detail = "operator" + AfterTrimmed;
      }
    }

    // Check for constructor / destructor
    if (llvm::StringRef(Detail).starts_with("~")) {
      Kind = "CXXDestructor";
      Detail = "";
    } else if (FuncName.find("::") != std::string::npos) {
      std::string Qualifier = FuncName.substr(0, FuncName.rfind("::"));
      std::string ClassPart =
          Qualifier.substr(Qualifier.rfind("::") == std::string::npos
                               ? 0
                               : Qualifier.rfind("::") + 2);
      if (ClassPart == Detail) {
        Kind = "CXXConstructor";
        Detail = "";
      }
    } else if (InsideClass && Detail == EnclosingClass) {
      Kind = "CXXConstructor";
      Detail = "";
    } else if (InsideClass && Kind != "CXXConversion") {
      Kind = "CXXMethod";
    }

    if (Kind == "CXXConstructor" || Kind == "CXXDestructor") {
      ReturnType = "";
    } else if (Kind == "CXXConversion") {
      size_t OpPos = FuncName.find("operator");
      if (OpPos != std::string::npos) {
        std::string AfterOp = FuncName.substr(OpPos + 8);
        ReturnType = llvm::StringRef(AfterOp).trim().str();
      }
      int OpTokIdx = -1;
      for (size_t I = 0; I < LParenIdx; ++I) {
        if (NodeTokens[I].Kind == tok::kw_operator) {
          OpTokIdx = static_cast<int>(I);
          break;
        }
      }
      if (OpTokIdx >= 0 && LParenIdx > static_cast<size_t>(OpTokIdx + 1)) {
        ReturnTypeRange =
            nodeRange(StartTok + OpTokIdx + 1, StartTok + LParenIdx, Out, Code);
      }
    }

    ASTNode FN;
    FN.role = "declaration";
    FN.kind = Kind;
    FN.detail = Detail;
    FN.range = NRange;
    FN.arcana = Kind + "Decl " + Detail;

    FN.children.push_back(
        buildFunctionProto(ReturnType, Params, ProtoRange, ReturnTypeRange));

    for (size_t I = 0; I < Children.size(); ++I) {
      if (Children[I]->symbol() == pseudo::cxx::Symbol::function_body ||
          Children[I]->symbol() == pseudo::cxx::Symbol::compound_statement) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, FN.children,
                      /*InsideClass=*/false, /*EnclosingClass=*/"");
      }
    }
    Nodes.push_back(std::move(FN));
    return;
  }

  // 5. Simple declaration
  if (Sym == pseudo::cxx::Symbol::simple_declaration ||
      Sym == pseudo::cxx::Symbol::member_declaration) {
    auto Children = N->elements();

    // Check if decl-specifier-seq contains a class, struct, union, or enum definition
    bool HasClassOrEnumDef = false;
    for (size_t I = 0; I < Children.size(); ++I) {
      if (containsSymbol(Children[I], pseudo::cxx::Symbol::class_specifier) ||
          containsSymbol(Children[I], pseudo::cxx::Symbol::enum_specifier)) {
        HasClassOrEnumDef = true;
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                      EnclosingClass, DeclaredType);
      }
    }

    // Check if there is an init-declarator-list (e.g. variables declared)
    const pseudo::ForestNode *InitDeclList = nullptr;
    for (size_t I = 0; I < Children.size(); ++I) {
      if (Children[I]->symbol() == pseudo::cxx::Symbol::init_declarator_list ||
          Children[I]->symbol() == pseudo::cxx::Symbol::member_declarator_list) {
        InitDeclList = Children[I];
        break;
      }
    }

    // If it's a class/struct/enum definition without declarators (e.g. struct Foo { ... };),
    // then we are done! No variable should be created.
    if (HasClassOrEnumDef && !InitDeclList)
      return;

    // Check if it's a forward declaration (e.g. struct Foo; or class Bar;)
    if (!InitDeclList && !HasClassOrEnumDef) {
      bool HasElaboratedType = false;
      for (size_t I = 0; I < Children.size(); ++I) {
        if (containsSymbol(Children[I],
                           pseudo::cxx::Symbol::elaborated_type_specifier)) {
          HasElaboratedType = true;
          break;
        }
      }
      if (HasElaboratedType) {
        std::string FwdName;
        for (const auto &T : NodeTokens) {
          if (T.Kind == tok::semi)
            break;
          if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
            FwdName = getOrigToken(T, Out).text().str();
        }
        if (!FwdName.empty()) {
          ASTNode CR;
          CR.role = "declaration";
          CR.kind = "CXXRecord";
          CR.detail = FwdName;
          CR.arcana = "CXXRecordDecl " + FwdName;
          CR.range = NRange;
          Nodes.push_back(std::move(CR));
          return;
        }
      }
    }

    // Extract the declared type from decl-specifier-seq (Children[0])
    std::string ExtractedType;
    std::optional<Range> ExtractedTypeRange;
    if (Children.size() >= 2 &&
        Children[1]->startTokenIndex() > Children[0]->startTokenIndex() &&
        Children[1]->startTokenIndex() <= Out.ParseableStream.tokens().size()) {
      size_t S0 = Children[0]->startTokenIndex();
      size_t S1 = Children[1]->startTokenIndex();
      size_t OffStart = tokenStartOffset(Out.ParseableStream.tokens()[S0], Out);
      size_t OffEnd = tokenEndOffset(Out.ParseableStream.tokens()[S1 - 1], Out);
      if (OffStart < OffEnd && OffEnd <= Code.size()) {
        ExtractedType = Code.slice(OffStart, OffEnd).trim().str();
        ExtractedTypeRange = nodeRange(S0, S1, Out, Code);
      }
    }
    if (ExtractedType.empty()) {
      size_t FirstIdentIdx = NodeTokens.size();
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
            NodeTokens[I].Kind == tok::l_brace)
          break;
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          FirstIdentIdx = I;
          break;
        }
      }
      if (FirstIdentIdx < NodeTokens.size()) {
        ExtractedType = getOrigToken(NodeTokens[FirstIdentIdx], Out).text().str();
        ExtractedTypeRange =
            nodeRange(StartTok, StartTok + FirstIdentIdx + 1, Out, Code);
      }
    }
    if (ExtractedType.empty())
      ExtractedType = "int";

    size_t BeforeCount = Nodes.size();
    for (size_t I = 0; I < Children.size(); ++I) {
      if (Children[I]->symbol() == pseudo::cxx::Symbol::decl_specifier_seq)
        continue;
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                    EnclosingClass, ExtractedType, ExtractedTypeRange);
    }
    if (Nodes.size() > BeforeCount)
      return;

    if (HasClassOrEnumDef)
      return;

    // Fallback: parse as single variable or function prototype
    size_t SemiIdx = NodeTokens.size();
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi) {
        SemiIdx = I;
        break;
      }
    }

    size_t TypeStartTok = 0;
    while (TypeStartTok < SemiIdx &&
           (NodeTokens[TypeStartTok].Kind == tok::kw_static ||
            NodeTokens[TypeStartTok].Kind == tok::kw_const ||
            NodeTokens[TypeStartTok].Kind == tok::kw_inline ||
            NodeTokens[TypeStartTok].Kind == tok::kw_constexpr ||
            NodeTokens[TypeStartTok].Kind == tok::kw_extern ||
            NodeTokens[TypeStartTok].Kind == tok::kw_mutable ||
            NodeTokens[TypeStartTok].Kind == tok::kw_friend))
      ++TypeStartTok;

    bool HasTypeKeyword = (TypeStartTok > 0);
    int IdentCountBeforeParenOrEqual = 0;
    for (size_t I = TypeStartTok; I < SemiIdx; ++I) {
      if (NodeTokens[I].Kind == tok::l_paren || NodeTokens[I].Kind == tok::equal)
        break;
      if (NodeTokens[I].Kind == tok::kw_auto || NodeTokens[I].Kind == tok::kw_void ||
          NodeTokens[I].Kind == tok::kw_int || NodeTokens[I].Kind == tok::kw_char ||
          NodeTokens[I].Kind == tok::kw_bool || NodeTokens[I].Kind == tok::kw_float ||
          NodeTokens[I].Kind == tok::kw_double || NodeTokens[I].Kind == tok::kw_class ||
          NodeTokens[I].Kind == tok::kw_struct) {
        HasTypeKeyword = true;
      }
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier)
        ++IdentCountBeforeParenOrEqual;
    }

    size_t NameTokIdx = SemiIdx;
    for (size_t I = TypeStartTok; I < SemiIdx; ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTokIdx = I;
      }
      if (NodeTokens[I].Kind == tok::l_paren || NodeTokens[I].Kind == tok::equal)
        break;
    }

    if (NameTokIdx < SemiIdx) {
      std::string VarName =
          getOrigToken(NodeTokens[NameTokIdx], Out).text().str();

      // If no type keyword and only 1 identifier before '(', it's a function call expression!
      if (!HasTypeKeyword && IdentCountBeforeParenOrEqual <= 1 &&
          NameTokIdx + 1 < SemiIdx && NodeTokens[NameTokIdx + 1].Kind == tok::l_paren) {
        ASTNode CallNode;
        CallNode.role = "expression";
        CallNode.kind = "Call";
        CallNode.arcana = "CallExpr";
        CallNode.range = NRange;
        ASTNode Callee;
        Callee.role = "expression";
        Callee.kind = "DeclRef";
        Callee.detail = VarName;
        Callee.arcana = "DeclRefExpr '" + VarName + "'";
        Callee.range = tokenRange(NodeTokens[NameTokIdx], Out, Code);
        CallNode.children.push_back(std::move(Callee));
        Nodes.push_back(std::move(CallNode));
        return;
      }

      bool IsFuncProto = false;
      if (NameTokIdx + 1 < SemiIdx &&
          NodeTokens[NameTokIdx + 1].Kind == tok::l_paren) {
        IsFuncProto = isLikelyFunctionDeclaration(
            NodeTokens, NameTokIdx + 1, /*InsideFunctionBody=*/!InsideClass);
      }

      if (IsFuncProto) {
        auto Params = extractParameters(NodeTokens, Out, Code);
        std::string Kind = InsideClass ? "CXXMethod" : "Function";
        std::string Detail = VarName;
        if (llvm::StringRef(Detail).starts_with("~")) {
          Kind = "CXXDestructor";
          Detail = "";
        } else if (InsideClass && Detail == EnclosingClass) {
          Kind = "CXXConstructor";
          Detail = "";
        } else if (llvm::StringRef(Detail).starts_with("operator") &&
                   Detail != "operator()") {
          Kind = "CXXConversion";
        }
        if (Kind == "CXXConstructor" || Kind == "CXXDestructor") {
          ExtractedType = "";
        } else if (Kind == "CXXConversion") {
          if (llvm::StringRef(Detail).starts_with("operator "))
            ExtractedType = Detail.substr(strlen("operator "));
        }
        ASTNode FD;
        FD.role = "declaration";
        FD.kind = Kind;
        FD.detail = Detail;
        FD.arcana = Kind + "Decl " + Detail;
        FD.range = NRange;
        FD.children.push_back(
            buildFunctionProto(ExtractedType, Params, NRange, NRange));
        Nodes.push_back(std::move(FD));
        return;
      }

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = InsideClass ? "Field" : "Var";
      VN.detail = VarName;
      VN.range = NRange;
      VN.arcana = VN.kind + "Decl " + VarName + " '" + ExtractedType + "'";
      VN.children.push_back(buildTypeNode(ExtractedType, NRange));

      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        if (Children[I]->symbol() != pseudo::cxx::Symbol::decl_specifier_seq)
          buildASTNodes(Children[I], ChildEnd, Out, Code, VN.children,
                        /*InsideClass=*/false, EnclosingClass, DeclaredType);
      }
      Nodes.push_back(std::move(VN));
      return;
    }
    return;
  }

  // 5b. Init declarator & Member declarator
  if (Sym == pseudo::cxx::Symbol::init_declarator ||
      Sym == pseudo::cxx::Symbol::member_declarator) {
    const pseudo::Token *NameTok = nullptr;
    size_t LParenIdx = NodeTokens.size();
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::semi || NodeTokens[I].Kind == tok::equal ||
          NodeTokens[I].Kind == tok::colon || NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::l_paren) {
        LParenIdx = I;
        break;
      }
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      std::string VarName = getOrigToken(*NameTok, Out).text().str();
      std::string EffectiveType =
          DeclaredType.empty() ? "int" : DeclaredType.str();
      for (size_t I = 0; I < NodeTokens.size(); ++I) {
        if (&NodeTokens[I] == NameTok)
          break;
        if (NodeTokens[I].Kind == tok::star)
          EffectiveType += " *";
        else if (NodeTokens[I].Kind == tok::amp)
          EffectiveType += " &";
        else if (NodeTokens[I].Kind == tok::ampamp)
          EffectiveType += " &&";
      }

      bool IsFunc =
          (LParenIdx < NodeTokens.size()) &&
          isLikelyFunctionDeclaration(NodeTokens, LParenIdx,
                                      /*InsideFunctionBody=*/!InsideClass);
      if (IsFunc) {
        auto Params = extractParameters(NodeTokens, Out, Code);
        std::string Kind = InsideClass ? "CXXMethod" : "Function";
        std::string Detail = VarName;
        if (llvm::StringRef(Detail).starts_with("~")) {
          Kind = "CXXDestructor";
          Detail = "";
        } else if (InsideClass && Detail == EnclosingClass) {
          Kind = "CXXConstructor";
          Detail = "";
        } else if (llvm::StringRef(Detail).starts_with("operator") &&
                   Detail != "operator()") {
          Kind = "CXXConversion";
        }
        if (Kind == "CXXConstructor" || Kind == "CXXDestructor") {
          EffectiveType = "";
        } else if (Kind == "CXXConversion") {
          if (llvm::StringRef(Detail).starts_with("operator "))
            EffectiveType = Detail.substr(strlen("operator "));
        }
        ASTNode FD;
        FD.role = "declaration";
        FD.kind = Kind;
        FD.detail = Detail;
        FD.arcana = Kind + "Decl " + Detail;
        FD.range = NRange;
        if (DeclaredTypeRange && FD.range)
          FD.range->start = DeclaredTypeRange->start;
        Range ReturnTypeRange = DeclaredTypeRange.value_or(NRange);
        FD.children.push_back(
            buildFunctionProto(EffectiveType, Params, NRange, ReturnTypeRange));
        Nodes.push_back(std::move(FD));
        return;
      }

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = InsideClass ? "Field" : "Var";
      VN.detail = VarName;
      VN.range = NRange;
      if (DeclaredTypeRange && VN.range)
        VN.range->start = DeclaredTypeRange->start;
      VN.arcana = VN.kind + "Decl " + VarName + " '" + EffectiveType + "'";
      VN.children.push_back(
          buildTypeNode(EffectiveType, DeclaredTypeRange.value_or(NRange)));

      auto Children = N->elements();
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        if (Children[I]->symbol() != pseudo::cxx::Symbol::declarator &&
            Children[I]->symbol() != pseudo::cxx::Symbol::noptr_declarator) {
          buildASTNodes(Children[I], ChildEnd, Out, Code, VN.children,
                        /*InsideClass=*/false, EnclosingClass, DeclaredType);
        }
      }
      Nodes.push_back(std::move(VN));
      return;
    }
    return;
  }

  // 6. Parameter declaration
  if (Sym == pseudo::cxx::Symbol::parameter_declaration) {
    auto Params = extractParameters(NodeTokens, Out, Code);
    if (!Params.empty()) {
      for (const auto &P : Params) {
        ASTNode Parm;
        Parm.role = "declaration";
        Parm.kind = "ParmVar";
        Parm.detail = P.Name.empty() ? "(anonymous)" : P.Name;
        Parm.range = P.ParamRange;
        Parm.arcana = "ParmVarDecl " + Parm.detail + " '" + P.Type + "'";
        Parm.children.push_back(buildTypeNode(P.Type, P.TypeRange));
        Nodes.push_back(std::move(Parm));
      }
      return;
    }
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    std::string ParmName = NameTok ? getOrigToken(*NameTok, Out).text().str() : "";
    Range TypeRange = NRange;
    std::string TypeStr = "int";
    if (NameTok && NameTok > NodeTokens.data()) {
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      if (TStart < TEnd && TEnd <= Code.size()) {
        TypeStr = Code.slice(TStart, TEnd).trim().str();
        size_t NIdx = NameTok - NodeTokens.data();
        TypeRange = nodeRange(StartTok, StartTok + NIdx, Out, Code);
      }
    }
    ASTNode Parm;
    Parm.role = "declaration";
    Parm.kind = "ParmVar";
    Parm.detail = ParmName.empty() ? "(anonymous)" : ParmName;
    Parm.range = NRange;
    Parm.arcana = "ParmVarDecl " + Parm.detail + " '" + TypeStr + "'";
    Parm.children.push_back(buildTypeNode(TypeStr, TypeRange));
    Nodes.push_back(std::move(Parm));
    return;
  }

  // 7. Compound statement
  if (Sym == pseudo::cxx::Symbol::compound_statement) {
    ASTNode CS;
    CS.role = "statement";
    CS.kind = "Compound";
    CS.arcana = "CompoundStmt";
    CS.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, CS.children,
                    /*InsideClass=*/false, /*EnclosingClass=*/"");
    }
    Nodes.push_back(std::move(CS));
    return;
  }

  // 8. Declaration statement
  if (Sym == pseudo::cxx::Symbol::declaration_statement) {
    ASTNode DS;
    DS.role = "statement";
    DS.kind = "Decl";
    DS.arcana = "DeclStmt";
    DS.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, DS.children,
                    /*InsideClass=*/false, /*EnclosingClass=*/"");
    }
    Nodes.push_back(std::move(DS));
    return;
  }

  // 9. Jump statement (return, break, continue, goto)
  if (Sym == pseudo::cxx::Symbol::jump_statement) {
    if (!NodeTokens.empty()) {
      if (NodeTokens.front().Kind == tok::kw_return) {
        ASTNode Ret;
        Ret.role = "statement";
        Ret.kind = "Return";
        Ret.arcana = "ReturnStmt";
        Ret.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Ret.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Ret));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_break) {
        ASTNode Brk;
        Brk.role = "statement";
        Brk.kind = "Break";
        Brk.arcana = "BreakStmt";
        Brk.range = NRange;
        Nodes.push_back(std::move(Brk));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_continue) {
        ASTNode Cont;
        Cont.role = "statement";
        Cont.kind = "Continue";
        Cont.arcana = "ContinueStmt";
        Cont.range = NRange;
        Nodes.push_back(std::move(Cont));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_goto) {
        std::string Label;
        if (NodeTokens.size() > 1)
          Label = getOrigToken(NodeTokens[1], Out).text().str();
        ASTNode Gt;
        Gt.role = "statement";
        Gt.kind = "Goto";
        Gt.detail = Label;
        Gt.arcana = "GotoStmt " + Label;
        Gt.range = NRange;
        Nodes.push_back(std::move(Gt));
        return;
      }
    }
  }

  // 10. Selection statement (if, switch)
  if (Sym == pseudo::cxx::Symbol::selection_statement) {
    if (!NodeTokens.empty() && NodeTokens.front().Kind == tok::kw_switch) {
      ASTNode Sw;
      Sw.role = "statement";
      Sw.kind = "Switch";
      Sw.arcana = "SwitchStmt";
      Sw.range = NRange;
      auto Children = N->elements();
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, Sw.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(Sw));
      return;
    }
    ASTNode IfNode;
    IfNode.role = "statement";
    IfNode.kind = "If";
    IfNode.arcana = "IfStmt";
    IfNode.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, IfNode.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(IfNode));
    return;
  }

  // 11. Iteration statement (while, do, for, range-for)
  if (Sym == pseudo::cxx::Symbol::iteration_statement) {
    if (!NodeTokens.empty()) {
      if (NodeTokens.front().Kind == tok::kw_while) {
        ASTNode Wh;
        Wh.role = "statement";
        Wh.kind = "While";
        Wh.arcana = "WhileStmt";
        Wh.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Wh.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Wh));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_do) {
        ASTNode DoNode;
        DoNode.role = "statement";
        DoNode.kind = "Do";
        DoNode.arcana = "DoStmt";
        DoNode.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, DoNode.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(DoNode));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_for) {
        bool IsRangeFor = false;
        int PDepth = 0;
        for (size_t I = 0; I < NodeTokens.size(); ++I) {
          if (NodeTokens[I].Kind == tok::l_paren)
            ++PDepth;
          else if (NodeTokens[I].Kind == tok::r_paren) {
            --PDepth;
            if (PDepth == 0)
              break;
          } else if (NodeTokens[I].Kind == tok::colon && PDepth == 1) {
            IsRangeFor = true;
            break;
          }
        }
        ASTNode ForNode;
        ForNode.role = "statement";
        ForNode.kind = IsRangeFor ? "CXXForRange" : "For";
        ForNode.arcana = ForNode.kind + "Stmt";
        ForNode.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, ForNode.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(ForNode));
        return;
      }
    }
  }

  // 12. Labeled statement (case, default, label)
  if (Sym == pseudo::cxx::Symbol::labeled_statement) {
    if (!NodeTokens.empty()) {
      if (NodeTokens.front().Kind == tok::kw_case) {
        ASTNode Cs;
        Cs.role = "statement";
        Cs.kind = "Case";
        Cs.arcana = "CaseStmt";
        Cs.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Cs.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Cs));
        return;
      }
      if (NodeTokens.front().Kind == tok::kw_default) {
        ASTNode Df;
        Df.role = "statement";
        Df.kind = "Default";
        Df.arcana = "DefaultStmt";
        Df.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Df.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Df));
        return;
      }
      if (NodeTokens.front().Kind == tok::raw_identifier ||
          NodeTokens.front().Kind == tok::identifier) {
        std::string Name = getOrigToken(NodeTokens.front(), Out).text().str();
        ASTNode Lb;
        Lb.role = "statement";
        Lb.kind = "Label";
        Lb.detail = Name;
        Lb.arcana = "LabelStmt " + Name;
        Lb.range = NRange;
        auto Children = N->elements();
        for (size_t I = 0; I < Children.size(); ++I) {
          pseudo::Token::Index ChildEnd =
              (I + 1 == Children.size()) ? End
                                         : Children[I + 1]->startTokenIndex();
          buildASTNodes(Children[I], ChildEnd, Out, Code, Lb.children,
                        InsideClass, EnclosingClass);
        }
        Nodes.push_back(std::move(Lb));
        return;
      }
    }
  }

  // 13. Expression statement
  if (Sym == pseudo::cxx::Symbol::expression_statement) {
    if (NodeTokens.size() == 1 && NodeTokens.front().Kind == tok::semi) {
      ASTNode Nl;
      Nl.role = "statement";
      Nl.kind = "Null";
      Nl.arcana = "NullStmt";
      Nl.range = NRange;
      Nodes.push_back(std::move(Nl));
      return;
    }
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
    }
    return;
  }

  // 14. Binary Expressions
  if (Sym == pseudo::cxx::Symbol::additive_expression ||
      Sym == pseudo::cxx::Symbol::multiplicative_expression ||
      Sym == pseudo::cxx::Symbol::shift_expression ||
      Sym == pseudo::cxx::Symbol::relational_expression ||
      Sym == pseudo::cxx::Symbol::equality_expression ||
      Sym == pseudo::cxx::Symbol::and_expression ||
      Sym == pseudo::cxx::Symbol::exclusive_or_expression ||
      Sym == pseudo::cxx::Symbol::inclusive_or_expression ||
      Sym == pseudo::cxx::Symbol::logical_and_expression ||
      Sym == pseudo::cxx::Symbol::logical_or_expression ||
      Sym == pseudo::cxx::Symbol::assignment_expression ||
      Sym == pseudo::cxx::Symbol::compare_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    if (Children.size() >= 3) {
      auto OpTokIdx = Children[1]->startTokenIndex();
      std::string OpStr =
          getOrigToken(Out.ParseableStream.tokens()[OpTokIdx], Out)
              .text()
              .str();
      ASTNode BO;
      BO.role = "expression";
      BO.kind = "BinaryOperator";
      BO.detail = OpStr;
      BO.arcana = "BinaryOperator '" + OpStr + "'";
      BO.range = NRange;

      pseudo::Token::Index LHSEnd = Children[1]->startTokenIndex();
      buildASTNodes(Children.front(), LHSEnd, Out, Code, BO.children,
                    InsideClass, EnclosingClass);
      buildASTNodes(Children.back(), End, Out, Code, BO.children, InsideClass,
                    EnclosingClass);
      Nodes.push_back(std::move(BO));
      return;
    }
  }

  // 15. Unary Expressions
  if (Sym == pseudo::cxx::Symbol::unary_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    if (Children.size() == 2) {
      if (!NodeTokens.empty() && NodeTokens.front().Kind == tok::kw_sizeof) {
        ASTNode SO;
        SO.role = "expression";
        SO.kind = "UnaryExprOrTypeTrait";
        SO.detail = "sizeof";
        SO.arcana = "UnaryExprOrTypeTraitExpr sizeof";
        SO.range = NRange;
        buildASTNodes(Children[1], End, Out, Code, SO.children, InsideClass,
                      EnclosingClass);
        Nodes.push_back(std::move(SO));
        return;
      }
      auto OpTokIdx = Children[0]->startTokenIndex();
      std::string OpStr =
          getOrigToken(Out.ParseableStream.tokens()[OpTokIdx], Out)
              .text()
              .str();
      ASTNode UO;
      UO.role = "expression";
      UO.kind = "UnaryOperator";
      UO.detail = OpStr;
      UO.arcana = "UnaryOperator '" + OpStr + "'";
      UO.range = NRange;
      buildASTNodes(Children[1], End, Out, Code, UO.children, InsideClass,
                    EnclosingClass);
      Nodes.push_back(std::move(UO));
      return;
    }
  }

  // 16. Postfix Expressions
  if (Sym == pseudo::cxx::Symbol::postfix_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }

    if (NodeTokens.back().Kind == tok::plusplus ||
        NodeTokens.back().Kind == tok::minusminus) {
      std::string OpStr =
          NodeTokens.back().Kind == tok::plusplus ? "++" : "--";
      ASTNode UO;
      UO.role = "expression";
      UO.kind = "UnaryOperator";
      UO.detail = OpStr;
      UO.arcana = "UnaryOperator '" + OpStr + "'";
      UO.range = NRange;
      buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                    UO.children, InsideClass, EnclosingClass);
      Nodes.push_back(std::move(UO));
      return;
    }

    if (NodeTokens.back().Kind == tok::r_paren) {
      ASTNode CallNode;
      CallNode.role = "expression";
      CallNode.kind = "Call";
      CallNode.arcana = "CallExpr";
      CallNode.range = NRange;

      buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                    CallNode.children, InsideClass, EnclosingClass);
      for (size_t I = 1; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, CallNode.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(CallNode));
      return;
    }

    if (NodeTokens.back().Kind == tok::r_square) {
      ASTNode AS;
      AS.role = "expression";
      AS.kind = "ArraySubscript";
      AS.arcana = "ArraySubscriptExpr";
      AS.range = NRange;
      buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                    AS.children, InsideClass, EnclosingClass);
      for (size_t I = 1; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, AS.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(AS));
      return;
    }

    bool HasPeriod = false;
    bool HasArrow = false;
    size_t AccessIdx = 0;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::period) {
        HasPeriod = true;
        AccessIdx = I;
        break;
      }
      if (NodeTokens[I].Kind == tok::arrow) {
        HasArrow = true;
        AccessIdx = I;
        break;
      }
    }
    if (HasPeriod || HasArrow) {
      std::string MemberName;
      for (size_t I = AccessIdx + 1; I < NodeTokens.size(); ++I) {
        if (NodeTokens[I].Kind == tok::raw_identifier ||
            NodeTokens[I].Kind == tok::identifier) {
          MemberName = getOrigToken(NodeTokens[I], Out).text().str();
          break;
        }
      }
      if (!MemberName.empty()) {
        ASTNode ME;
        ME.role = "expression";
        ME.kind = "Member";
        ME.detail = MemberName;
        ME.arcana = "MemberExpr '" + std::string(HasArrow ? "->" : ".") +
                    MemberName + "'";
        ME.range = NRange;
        buildASTNodes(Children[0], Children[1]->startTokenIndex(), Out, Code,
                      ME.children, InsideClass, EnclosingClass);
        Nodes.push_back(std::move(ME));
        return;
      }
    }
  }

  // 17. Primary Expressions
  if (Sym == pseudo::cxx::Symbol::primary_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    if (!NodeTokens.empty() && NodeTokens.front().Kind == tok::kw_this) {
      ASTNode Th;
      Th.role = "expression";
      Th.kind = "CXXThis";
      Th.detail = "implicit";
      Th.arcana = "CXXThisExpr";
      Th.range = NRange;
      Nodes.push_back(std::move(Th));
      return;
    }
    if (NodeTokens.front().Kind == tok::l_paren &&
        NodeTokens.back().Kind == tok::r_paren) {
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                      EnclosingClass);
      }
      return;
    }
  }

  // 18. Id Expression / Unqualified Id
  if (Sym == pseudo::cxx::Symbol::id_expression ||
      Sym == pseudo::cxx::Symbol::unqualified_id) {
    std::string IdName;
    for (const auto &T : NodeTokens) {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier) {
        if (!IdName.empty())
          IdName += "::";
        IdName += getOrigToken(T, Out).text().str();
      }
    }
    if (!IdName.empty()) {
      ASTNode DR;
      DR.role = "expression";
      DR.kind = "DeclRef";
      DR.detail = IdName;
      DR.arcana = "DeclRefExpr '" + IdName + "'";
      DR.range = NRange;
      if (IdName.find("::") != std::string::npos) {
        size_t Colons = IdName.rfind("::");
        std::string Ns = IdName.substr(0, Colons + 2);
        DR.detail = IdName.substr(Colons + 2);
        ASTNode Spec;
        Spec.role = "specifier";
        Spec.kind = "Namespace";
        Spec.detail = Ns;
        Spec.range = NRange;
        DR.children.push_back(std::move(Spec));
      }
      Nodes.push_back(std::move(DR));
      return;
    }
  }

  // 19. Literals
  if (Sym == pseudo::cxx::Symbol::integer_literal ||
      Sym == pseudo::cxx::Symbol::character_literal ||
      Sym == pseudo::cxx::Symbol::floating_point_literal ||
      Sym == pseudo::cxx::Symbol::string_literal ||
      Sym == pseudo::cxx::Symbol::boolean_literal ||
      Sym == pseudo::cxx::Symbol::pointer_literal) {
    size_t SOff = tokenStartOffset(NodeTokens.front(), Out);
    size_t EOff = tokenEndOffset(NodeTokens.back(), Out);
    std::string Text = (SOff < EOff && EOff <= Code.size())
                           ? Code.slice(SOff, EOff).trim().str()
                           : "";
    std::string Kind = "IntegerLiteral";
    if (Sym == pseudo::cxx::Symbol::floating_point_literal)
      Kind = "FloatingLiteral";
    else if (Sym == pseudo::cxx::Symbol::string_literal)
      Kind = "StringLiteral";
    else if (Sym == pseudo::cxx::Symbol::character_literal)
      Kind = "CharacterLiteral";
    else if (Sym == pseudo::cxx::Symbol::boolean_literal)
      Kind = "CXXBoolLiteralExpr";
    else if (Sym == pseudo::cxx::Symbol::pointer_literal) {
      Kind = "CXXNullPtrLiteralExpr";
      Text = "nullptr";
    }
    ASTNode Lit;
    Lit.role = "expression";
    Lit.kind = Kind;
    Lit.detail = Text;
    Lit.arcana = Kind + " " + Text;
    Lit.range = NRange;
    Nodes.push_back(std::move(Lit));
    return;
  }

  // 20. Lambda expression
  if (Sym == pseudo::cxx::Symbol::lambda_expression) {
    ASTNode LE;
    LE.role = "expression";
    LE.kind = "Lambda";
    LE.arcana = "LambdaExpr";
    LE.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, LE.children,
                    /*InsideClass=*/false, /*EnclosingClass=*/"");
    }
    Nodes.push_back(std::move(LE));
    return;
  }

  // 21. Braced init list
  if (Sym == pseudo::cxx::Symbol::braced_init_list) {
    ASTNode IL;
    IL.role = "expression";
    IL.kind = "InitList";
    IL.arcana = "InitListExpr";
    IL.range = NRange;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, IL.children, InsideClass,
                    EnclosingClass);
    }
    Nodes.push_back(std::move(IL));
    return;
  }

  // 22. Conditional expression (? :)
  if (Sym == pseudo::cxx::Symbol::conditional_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    ASTNode CondNode;
    CondNode.role = "expression";
    CondNode.kind = "ConditionalOperator";
    CondNode.arcana = "ConditionalOperator";
    CondNode.range = NRange;
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, CondNode.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(CondNode));
    return;
  }

  // 23. Cast expression
  if (Sym == pseudo::cxx::Symbol::cast_expression) {
    auto Children = N->elements();
    if (Children.size() == 1) {
      buildASTNodes(Children[0], End, Out, Code, Nodes, InsideClass,
                    EnclosingClass);
      return;
    }
    ASTNode Cast;
    Cast.role = "expression";
    Cast.kind = "CStyleCast";
    Cast.arcana = "CStyleCastExpr";
    Cast.range = NRange;
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, Cast.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(Cast));
    return;
  }

  // 24. Alias declaration
  if (Sym == pseudo::cxx::Symbol::alias_declaration) {
    std::string AliasName;
    std::string TargetType;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
        if (NodeTokens[I + 1].Kind == tok::raw_identifier ||
            NodeTokens[I + 1].Kind == tok::identifier)
          AliasName = getOrigToken(NodeTokens[I + 1], Out).text().str();
      }
      if (NodeTokens[I].Kind == tok::equal && I + 1 < NodeTokens.size()) {
        size_t TStart = tokenStartOffset(NodeTokens[I + 1], Out);
        size_t TEnd = tokenEndOffset(NodeTokens.back(), Out);
        if (NodeTokens.back().Kind == tok::semi && NodeTokens.size() > 1)
          TEnd = tokenEndOffset(NodeTokens[NodeTokens.size() - 2], Out);
        if (TStart < TEnd && TEnd <= Code.size())
          TargetType = Code.slice(TStart, TEnd).trim().str();
      }
    }
    if (!AliasName.empty()) {
      ASTNode AD;
      AD.role = "declaration";
      AD.kind = "TypeAlias";
      AD.detail = AliasName;
      AD.arcana = "TypeAliasDecl " + AliasName;
      AD.range = NRange;
      AD.children.push_back(buildTypeNode(TargetType, NRange));
      Nodes.push_back(std::move(AD));
      return;
    }
  }

  // 25. Using declaration / directive
  if (Sym == pseudo::cxx::Symbol::using_declaration ||
      Sym == pseudo::cxx::Symbol::using_directive) {
    std::string Name;
    for (const auto &T : NodeTokens) {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier) {
        if (!Name.empty())
          Name += "::";
        Name += getOrigToken(T, Out).text().str();
      }
    }
    ASTNode UD;
    UD.role = "declaration";
    UD.kind = (Sym == pseudo::cxx::Symbol::using_directive) ? "UsingDirective"
                                                            : "Using";
    UD.detail = Name;
    UD.arcana = UD.kind + "Decl " + Name;
    UD.range = NRange;
    Nodes.push_back(std::move(UD));
    return;
  }

  // 26. Static assert
  if (Sym == pseudo::cxx::Symbol::static_assert_declaration) {
    ASTNode SA;
    SA.role = "declaration";
    SA.kind = "StaticAssert";
    SA.arcana = "StaticAssertDecl";
    SA.range = NRange;
    Nodes.push_back(std::move(SA));
    return;
  }

  // 26b. Constructor initializer (e.g. : Base(Base), DirtyFiles(Drafts))
  if (Sym == pseudo::cxx::Symbol::mem_initializer) {
    std::string MemberName;
    const pseudo::Token *IdTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::l_paren ||
          NodeTokens[I].Kind == tok::l_brace)
        break;
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        IdTok = &NodeTokens[I];
      }
    }
    if (IdTok)
      MemberName = getOrigToken(*IdTok, Out).text().str();

    ASTNode InitNode;
    InitNode.role = "constructor initializer";
    InitNode.kind = "MemberInitializer";
    InitNode.detail = MemberName;
    InitNode.arcana = "CXXCtorInitializer '" + MemberName + "'";
    InitNode.range = NRange;

    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, InitNode.children,
                    InsideClass, EnclosingClass);
    }
    Nodes.push_back(std::move(InitNode));
    return;
  }

  // 26c. Condition with variable declaration: if (auto Loc = ...)
  if (Sym == pseudo::cxx::Symbol::condition) {
    const pseudo::Token *EqTok = nullptr;
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal) {
        EqTok = &NodeTokens[I];
        break;
      }
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (EqTok && NameTok) {
      std::string VarName = getOrigToken(*NameTok, Out).text().str();
      size_t NameIdx = NameTok - NodeTokens.data();
      Range TypeRange = nodeRange(StartTok, StartTok + NameIdx, Out, Code);
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      std::string TypeStr = (TStart < TEnd && TEnd <= Code.size())
                                ? Code.slice(TStart, TEnd).trim().str()
                                : "auto";

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = "Var";
      VN.detail = VarName;
      VN.range = NRange;
      VN.arcana = "VarDecl " + VarName + " '" + TypeStr + "'";
      VN.children.push_back(buildTypeNode(TypeStr, TypeRange));

      auto Children = N->elements();
      for (size_t I = 0; I < Children.size(); ++I) {
        pseudo::Token::Index ChildEnd =
            (I + 1 == Children.size()) ? End
                                       : Children[I + 1]->startTokenIndex();
        buildASTNodes(Children[I], ChildEnd, Out, Code, VN.children,
                      InsideClass, EnclosingClass);
      }
      Nodes.push_back(std::move(VN));
      return;
    }
  }

  // 26d. Range-for declaration: for (const auto &Selection : *Selections)
  if (Sym == pseudo::cxx::Symbol::for_range_declaration) {
    const pseudo::Token *NameTok = nullptr;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::raw_identifier ||
          NodeTokens[I].Kind == tok::identifier) {
        NameTok = &NodeTokens[I];
      }
    }
    if (NameTok) {
      std::string VarName = getOrigToken(*NameTok, Out).text().str();
      size_t NameIdx = NameTok - NodeTokens.data();
      Range TypeRange = nodeRange(StartTok, StartTok + NameIdx, Out, Code);
      size_t TStart = tokenStartOffset(NodeTokens.front(), Out);
      size_t TEnd = tokenStartOffset(*NameTok, Out);
      std::string TypeStr = (TStart < TEnd && TEnd <= Code.size())
                                ? Code.slice(TStart, TEnd).trim().str()
                                : "auto";

      ASTNode DS;
      DS.role = "statement";
      DS.kind = "Decl";
      DS.arcana = "DeclStmt";
      DS.range = NRange;

      ASTNode VN;
      VN.role = "declaration";
      VN.kind = "Var";
      VN.detail = VarName;
      VN.range = NRange;
      VN.arcana = "VarDecl " + VarName + " '" + TypeStr + "'";
      VN.children.push_back(buildTypeNode(TypeStr, TypeRange));

      DS.children.push_back(std::move(VN));
      Nodes.push_back(std::move(DS));
      return;
    }
  }

  // 27. Template declaration
  if (Sym == pseudo::cxx::Symbol::template_declaration) {
    std::vector<std::string> TemplateParams;
    bool InAngles = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::less) {
        InAngles = true;
        continue;
      }
      if (NodeTokens[I].Kind == tok::greater)
        break;
      if (InAngles && (NodeTokens[I].Kind == tok::kw_typename ||
                       NodeTokens[I].Kind == tok::kw_class)) {
        if (I + 1 < NodeTokens.size() &&
            (NodeTokens[I + 1].Kind == tok::raw_identifier ||
             NodeTokens[I + 1].Kind == tok::identifier)) {
          TemplateParams.push_back(
              getOrigToken(NodeTokens[I + 1], Out).text().str());
        }
      }
    }

    std::vector<ASTNode> InnerNodes;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildASTNodes(Children[I], ChildEnd, Out, Code, InnerNodes, InsideClass,
                    EnclosingClass);
    }

    if (!InnerNodes.empty()) {
      auto &Inner = InnerNodes.front();
      ASTNode TD;
      TD.role = "declaration";
      TD.kind = (Inner.kind == "Function" || Inner.kind == "CXXMethod")
                    ? "FunctionTemplate"
                    : "ClassTemplate";
      TD.detail = Inner.detail;
      TD.arcana = TD.kind + "Decl " + Inner.detail;
      TD.range = NRange;
      for (const auto &P : TemplateParams) {
        ASTNode TP;
        TP.role = "declaration";
        TP.kind = "TemplateTypeParm";
        TP.detail = P;
        TP.range = NRange;
        TP.arcana = "TemplateTypeParmDecl " + P;
        TD.children.push_back(std::move(TP));
      }
      for (auto &IN : InnerNodes)
        TD.children.push_back(std::move(IN));
      Nodes.push_back(std::move(TD));
      return;
    }
  }

  // Default: recurse on all elements
  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    buildASTNodes(Children[I], ChildEnd, Out, Code, Nodes, InsideClass,
                  EnclosingClass, DeclaredType, DeclaredTypeRange);
  }
}

static bool posLessEq(Position A, Position B) {
  return std::tie(A.line, A.character) <= std::tie(B.line, B.character);
}

static bool containsRange(Range Outer, Range Inner) {
  return posLessEq(Outer.start, Inner.start) && posLessEq(Inner.end, Outer.end);
}

static int64_t rangeSpan(Range R) {
  int64_t Lines = R.end.line - R.start.line;
  int64_t Chars = R.end.character - R.start.character;
  return Lines * 100000LL + Chars;
}

static const ASTNode *findDeepestContaining(const ASTNode &Node, Range R) {
  if (posLessEq(R.end, R.start))
    std::swap(R.start, R.end);

  if (Node.range && !containsRange(*Node.range, R))
    return nullptr;

  const ASTNode *Tightest = nullptr;
  int64_t SmallestSpan = -1;

  for (const auto &C : Node.children) {
    if (const ASTNode *Descendant = findDeepestContaining(C, R)) {
      int64_t Span = Descendant->range ? rangeSpan(*Descendant->range) : 0;
      if (!Tightest || Span <= SmallestSpan) {
        Tightest = Descendant;
        SmallestSpan = Span;
      }
    }
  }

  if (Tightest)
    return Tightest;

  if (Node.range && containsRange(*Node.range, R))
    return &Node;

  return nullptr;
}

llvm::Expected<std::vector<DocumentSymbol>>
PseudoModule::getDocumentSymbols(llvm::StringRef Code) {
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<DocumentSymbol> Results;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  walkSymbols(Parsed->Root, NumTokens, *Parsed, Code, Results);
  return Results;
}

llvm::Expected<std::vector<SelectionRange>>
PseudoModule::getSemanticRanges(llvm::StringRef Code,
                                llvm::ArrayRef<Position> Positions) {
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<SelectionRange> Results;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  for (const auto &Pos : Positions) {
    auto Offset = positionToOffset(Code, Pos);
    if (!Offset) {
      consumeError(Offset.takeError());
      continue;
    }
    std::vector<Range> Path;
    collectSelectionRanges(Parsed->Root, NumTokens, *Parsed, Code, *Offset, Path);
    if (Path.empty())
      Path.push_back(Range{Pos, Pos});

    std::unique_ptr<SelectionRange> Current;
    for (const auto &R : Path) {
      auto Next = std::make_unique<SelectionRange>();
      Next->range = R;
      Next->parent = std::move(Current);
      Current = std::move(Next);
    }
    Results.push_back(std::move(*Current));
  }
  return Results;
}

llvm::Expected<std::vector<FoldingRange>>
PseudoModule::getFoldingRanges(llvm::StringRef Code, bool LineFoldingOnly) {
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<FoldingRange> Results;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  collectFoldingRanges(Parsed->Root, NumTokens, *Parsed, Code, Results);
  return Results;
}

llvm::Expected<std::optional<ASTNode>>
PseudoModule::getAST(PathRef File, llvm::StringRef Code,
                     std::optional<Range> R) {
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();

  ASTNode TU;
  TU.role = "declaration";
  TU.kind = "TranslationUnit";
  TU.arcana = "TranslationUnitDecl";

  buildASTNodes(Parsed->Root, NumTokens, *Parsed, Code, TU.children,
                /*InsideClass=*/false, /*EnclosingClass=*/"");

  if (!R)
    return std::optional<ASTNode>(std::move(TU));

  const ASTNode *Best = findDeepestContaining(TU, *R);
  if (!Best)
    return std::optional<ASTNode>(std::nullopt);
  return std::optional<ASTNode>(*Best);
}

static const pseudo::Token *getPrevNonComment(size_t Idx, const ParseOutput &Out) {
  while (Idx > 0) {
    --Idx;
    const auto &T = Out.RawStream.tokens()[Idx];
    if (T.Kind != tok::comment)
      return &T;
  }
  return nullptr;
}

static const pseudo::Token *getNextNonComment(size_t Idx, const ParseOutput &Out) {
  size_t N = Out.RawStream.tokens().size();
  while (Idx + 1 < N) {
    ++Idx;
    const auto &T = Out.RawStream.tokens()[Idx];
    if (T.Kind != tok::comment)
      return &T;
  }
  return nullptr;
}

static bool isCtorMemberInitializerName(const pseudo::Token *Touched,
                                        const ParseOutput &Out) {
  if (!Touched)
    return false;
  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Out.RawStream.tokens().size()) {
    RawTok = &Out.RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Out.RawStream.tokens().data() &&
             Touched < Out.RawStream.tokens().data() + Out.RawStream.tokens().size()) {
    RawTok = Touched;
  }
  if (!RawTok)
    return false;

  size_t Idx = Out.RawStream.index(*RawTok);
  const auto *Next = getNextNonComment(Idx, Out);
  if (!Next || (Next->Kind != tok::l_paren && Next->Kind != tok::l_brace))
    return false;

  const auto *Prev = getPrevNonComment(Idx, Out);
  if (!Prev || (Prev->Kind != tok::colon && Prev->Kind != tok::comma))
    return false;

  int Depth = 0;
  bool FoundColon = false;
  size_t PrevIdx = Out.RawStream.index(*Prev);
  if (Prev->Kind == tok::colon) {
    FoundColon = true;
  } else {
    size_t Step = 1;
    while (PrevIdx >= Step) {
      const auto &T = Out.RawStream.tokens()[PrevIdx - Step];
      ++Step;
      if (T.Kind == tok::comment)
        continue;
      if (T.Kind == tok::r_paren || T.Kind == tok::r_brace)
        ++Depth;
      else if (T.Kind == tok::l_paren || T.Kind == tok::l_brace) {
        if (Depth > 0)
          --Depth;
        else
          return false;
      } else if (Depth == 0 && T.Kind == tok::colon) {
        FoundColon = true;
        PrevIdx = PrevIdx - Step + 1;
        break;
      } else if (Depth == 0 && (T.Kind == tok::semi || T.Kind == tok::l_brace)) {
        return false;
      }
    }
  }

  if (!FoundColon)
    return false;

  size_t Step = 1;
  while (PrevIdx >= Step) {
    const auto &T = Out.RawStream.tokens()[PrevIdx - Step];
    ++Step;
    if (T.Kind == tok::comment)
      continue;
    if (T.Kind == tok::r_paren)
      return true;
    if (T.Kind == tok::kw_noexcept || T.Kind == tok::raw_identifier ||
        T.Kind == tok::identifier)
      continue;
    if (T.Kind == tok::semi || T.Kind == tok::l_brace || T.Kind == tok::r_brace)
      return false;
  }
  return false;
}

static bool isTypeContext(const pseudo::Token *Touched, const ParseOutput &Out) {
  if (!Touched)
    return false;
  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Out.RawStream.tokens().size()) {
    RawTok = &Out.RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Out.RawStream.tokens().data() &&
             Touched < Out.RawStream.tokens().data() + Out.RawStream.tokens().size()) {
    RawTok = Touched;
  }
  if (!RawTok)
    return false;
  size_t Idx = Out.RawStream.index(*RawTok);
  const auto *Prev = getPrevNonComment(Idx, Out);
  const auto *Next = getNextNonComment(Idx, Out);

  // 1. If followed by :: (scope resolution), e.g. ClangdServer::optsForTest
  if (Next && Next->Kind == tok::coloncolon)
    return true;

  // 2. If followed by pointer/reference/rvalue reference, e.g. ClangdServer *Server, ParseInputs &Inputs
  if (Next && (Next->Kind == tok::star || Next->Kind == tok::amp ||
               Next->Kind == tok::ampamp))
    return true;

  // 3. If followed by another identifier, e.g. "ClangdServer Server", "PathRef File"
  if (Next && (Next->Kind == tok::raw_identifier || Next->Kind == tok::identifier)) {
    if (!Prev || (Prev->Kind != tok::raw_identifier && Prev->Kind != tok::identifier))
      return true;
  }

  // 4. Preceding type keywords
  if (Prev) {
    if (Prev->Kind == tok::kw_class || Prev->Kind == tok::kw_struct ||
        Prev->Kind == tok::kw_union || Prev->Kind == tok::kw_enum ||
        Prev->Kind == tok::kw_typename || Prev->Kind == tok::kw_using ||
        Prev->Kind == tok::kw_typedef || Prev->Kind == tok::kw_const ||
        Prev->Kind == tok::kw_volatile || Prev->Kind == tok::kw_constexpr ||
        Prev->Kind == tok::kw_new || Prev->Kind == tok::kw_public ||
        Prev->Kind == tok::kw_protected || Prev->Kind == tok::kw_private ||
        Prev->Kind == tok::kw_namespace)
      return true;
  }

  // 5. Inside template argument list < ... >
  int AngleDepth = 0;
  bool InTemplateArgs = false;
  for (size_t I = Idx; I > 0; --I) {
    const auto &T = Out.RawStream.tokens()[I - 1];
    if (T.Kind == tok::semi || T.Kind == tok::l_brace || T.Kind == tok::r_brace)
      break;
    if (T.Kind == tok::greater)
      ++AngleDepth;
    else if (T.Kind == tok::less) {
      if (AngleDepth > 0)
        --AngleDepth;
      else {
        InTemplateArgs = true;
        break;
      }
    }
  }
  if (InTemplateArgs)
    return true;

  // 6. Parameter list type, e.g. [](llvm::StringRef) or void foo(llvm::StringRef)
  if (Next && (Next->Kind == tok::r_paren || Next->Kind == tok::comma)) {
    int ParenDepth = 1;
    for (size_t I = Idx; I > 0; --I) {
      const auto &T = Out.RawStream.tokens()[I - 1];
      if (T.Kind == tok::semi || T.Kind == tok::l_brace || T.Kind == tok::r_brace)
        break;
      if (T.Kind == tok::r_paren)
        ++ParenDepth;
      else if (T.Kind == tok::l_paren) {
        --ParenDepth;
        if (ParenDepth == 0) {
          if (I >= 2) {
            const auto &BeforeParen = Out.RawStream.tokens()[I - 2];
            if (BeforeParen.Kind == tok::r_square ||
                BeforeParen.Kind == tok::kw_void ||
                BeforeParen.Kind == tok::kw_bool ||
                BeforeParen.Kind == tok::kw_int ||
                BeforeParen.Kind == tok::kw_char ||
                BeforeParen.Kind == tok::kw_float ||
                BeforeParen.Kind == tok::kw_double) {
              return true;
            }
          }
          break;
        }
      }
    }
  }

  return false;
}

static void traverseIncludedHeaders(
    PseudoModule &Self, PathRef File, llvm::StringRef Code,
    llvm::vfs::FileSystem &FS,
    llvm::function_ref<bool(const PseudoModule::HeaderInfo &Info,
                            llvm::StringRef HeaderPath)>
        Callback,
    size_t MaxHeaders = 60, int MaxDepth = 4) {
  auto Includes = PseudoModule::extractIncludes(Code);
  auto IncludeDirs = Self.getIncludeDirectories(File);
  std::string CurrentDir = llvm::sys::path::parent_path(File).str();

  std::queue<std::pair<std::string, int>> Queue;
  llvm::StringSet<> Visited;

  for (const auto &Inc : Includes) {
    if (!Inc.IsAngled) {
      std::string Resolved =
          Self.resolveHeader(Inc, CurrentDir, IncludeDirs, FS);
      if (!Resolved.empty() && Visited.insert(Resolved).second)
        Queue.push({Resolved, 1});
    }
  }
  for (const auto &Inc : Includes) {
    if (Inc.IsAngled) {
      std::string Resolved =
          Self.resolveHeader(Inc, CurrentDir, IncludeDirs, FS);
      if (!Resolved.empty() && Visited.insert(Resolved).second)
        Queue.push({Resolved, 1});
    }
  }

  size_t HeadersVisited = 0;
  while (!Queue.empty() && HeadersVisited < MaxHeaders) {
    auto [HeaderPath, Depth] = Queue.front();
    Queue.pop();
    ++HeadersVisited;

    auto Info = Self.getHeaderInfo(HeaderPath, FS);
    if (!Info)
      continue;

    if (Callback(*Info, HeaderPath))
      break;

    bool IsSys = HeaderPath.rfind("/usr/", 0) == 0 ||
                 HeaderPath.rfind("/Library/Developer/", 0) == 0 ||
                 HeaderPath.rfind("/Applications/Xcode.app/", 0) == 0 ||
                 HeaderPath.rfind("/opt/homebrew/", 0) == 0;
    if (Depth < MaxDepth && !IsSys) {
      std::string HDir = llvm::sys::path::parent_path(HeaderPath).str();
      for (const auto &SubInc : Info->Includes) {
        if (!SubInc.IsAngled) {
          std::string SubResolved =
              Self.resolveHeader(SubInc, HDir, IncludeDirs, FS);
          if (!SubResolved.empty() && Visited.insert(SubResolved).second)
            Queue.push({SubResolved, Depth + 1});
        }
      }
      for (const auto &SubInc : Info->Includes) {
        if (SubInc.IsAngled) {
          std::string SubResolved =
              Self.resolveHeader(SubInc, HDir, IncludeDirs, FS);
          if (!SubResolved.empty() && Visited.insert(SubResolved).second)
            Queue.push({SubResolved, Depth + 1});
        }
      }
    }
  }
}

static std::string unwrapType(llvm::StringRef TypeName) {
  llvm::StringRef T = TypeName.trim();
  bool Stripped = true;
  while (Stripped) {
    Stripped = false;
    if (T.starts_with("static ")) {
      T = T.drop_front(7).trim();
      Stripped = true;
    }
    if (T.starts_with("inline ")) {
      T = T.drop_front(7).trim();
      Stripped = true;
    }
    if (T.starts_with("constexpr ")) {
      T = T.drop_front(10).trim();
      Stripped = true;
    }
    if (T.starts_with("consteval ")) {
      T = T.drop_front(10).trim();
      Stripped = true;
    }
    if (T.starts_with("virtual ")) {
      T = T.drop_front(8).trim();
      Stripped = true;
    }
    if (T.starts_with("explicit ")) {
      T = T.drop_front(9).trim();
      Stripped = true;
    }
    if (T.starts_with("friend ")) {
      T = T.drop_front(7).trim();
      Stripped = true;
    }
    if (T.starts_with("const ")) {
      T = T.drop_front(6).trim();
      Stripped = true;
    }
    if (T.starts_with("volatile ")) {
      T = T.drop_front(9).trim();
      Stripped = true;
    }
  }
  while (T.ends_with("*") || T.ends_with("&"))
    T = T.drop_back(1).trim();

  size_t AngleStart = T.find('<');
  size_t AngleEnd = T.rfind('>');
  if (AngleStart != llvm::StringRef::npos && AngleEnd != llvm::StringRef::npos &&
      AngleEnd > AngleStart) {
    llvm::StringRef Outer = T.take_front(AngleStart).trim();
    if (Outer.ends_with("unique_ptr") || Outer.ends_with("shared_ptr") ||
        Outer.ends_with("optional") || Outer.ends_with("IntrusiveRefCntPtr") ||
        Outer.ends_with("auto_ptr") || Outer.ends_with("make_unique") ||
        Outer.ends_with("make_shared")) {
      llvm::StringRef Inner = T.slice(AngleStart + 1, AngleEnd).trim();
      size_t Comma = Inner.find(',');
      if (Comma != llvm::StringRef::npos)
        Inner = Inner.take_front(Comma).trim();
      return unwrapType(Inner);
    }
    T = Outer;
  }

  size_t LastColons = T.rfind("::");
  if (LastColons != llvm::StringRef::npos)
    T = T.drop_front(LastColons + 2);

  return T.str();
}

static std::string resolveExprType(
    PseudoModule &Self, llvm::StringRef Pre, size_t CursorOffset,
    size_t BestScope, const std::vector<LexicalScope> &Scopes,
    llvm::StringRef Code, PathRef File, llvm::vfs::FileSystem *FS,
    llvm::StringRef EffectiveEnclosingClass) {
  Pre = Pre.rtrim();
  if (Pre.empty())
    return "";

  // Case 1: Pre ends with ')' (function or method call)
  if (Pre.ends_with(")")) {
    int Depth = 0;
    ssize_t LParen = -1;
    for (ssize_t I = (ssize_t)Pre.size() - 1; I >= 0; --I) {
      if (Pre[I] == ')')
        ++Depth;
      else if (Pre[I] == '(') {
        --Depth;
        if (Depth == 0) {
          LParen = I;
          break;
        }
      }
    }
    if (LParen > 0) {
      ssize_t FnEnd = LParen;
      while (FnEnd > 0 && llvm::isSpace(Pre[FnEnd - 1]))
        --FnEnd;
      ssize_t FnStart = FnEnd;
      while (FnStart > 0 &&
             (llvm::isAlnum(Pre[FnStart - 1]) || Pre[FnStart - 1] == '_'))
        --FnStart;
      llvm::StringRef FnName = Pre.slice(FnStart, FnEnd);

      ssize_t DotPos = FnStart;
      while (DotPos > 0 && llvm::isSpace(Pre[DotPos - 1]))
        --DotPos;
      std::string ReceiverType;
      if (DotPos > 0 && Pre[DotPos - 1] == '.') {
        ReceiverType = resolveExprType(
            Self, Pre.take_front(DotPos - 1).rtrim(), CursorOffset, BestScope,
            Scopes, Code, File, FS, EffectiveEnclosingClass);
      } else if (DotPos >= 2 && Pre.slice(DotPos - 2, DotPos) == "->") {
        ReceiverType = resolveExprType(
            Self, Pre.take_front(DotPos - 2).rtrim(), CursorOffset, BestScope,
            Scopes, Code, File, FS, EffectiveEnclosingClass);
      } else if (DotPos >= 2 && Pre.slice(DotPos - 2, DotPos) == "::") {
        ssize_t ScopeEnd = DotPos - 2;
        while (ScopeEnd > 0 && llvm::isSpace(Pre[ScopeEnd - 1]))
          --ScopeEnd;
        ssize_t ScopeStart = ScopeEnd;
        while (ScopeStart > 0 &&
               (llvm::isAlnum(Pre[ScopeStart - 1]) || Pre[ScopeStart - 1] == '_'))
          --ScopeStart;
        ReceiverType = Pre.slice(ScopeStart, ScopeEnd).str();
      }

      // Deduce return type for common methods
      if (FnName == "current" && (ReceiverType == "Context" || ReceiverType.empty()))
        return "Context";
      if (FnName == "clone" && (ReceiverType == "Context" || ReceiverType.empty()))
        return "Context";
      if (FnName == "getName" || FnName == "getDesc")
        return "llvm::StringRef";
      if (FnName == "instantiate") {
        if (ReceiverType.find("FeatureModule") != std::string::npos ||
            ReceiverType.find("entry") != std::string::npos ||
            ReceiverType.find("Registry") != std::string::npos ||
            ReceiverType.empty())
          return "std::unique_ptr<FeatureModule>";
        return "std::unique_ptr<void>";
      }
      if (FnName == "str" || FnName == "string")
        return "std::string";
      if (FnName == "data" || FnName == "c_str")
        return "const char *";
      if (FnName == "size" || FnName == "length" || FnName == "count")
        return "size_t";
      if (FnName == "empty" || FnName == "has_value" || FnName == "blockUntilIdle")
        return "bool";
      if (FnName == "typeId")
        return "void *";
      if (FnName == "facilities")
        return "Facilities";
      if (FnName == "fromRegistry")
        return "FeatureModuleSet";
      if (FnName == "getLangOpts")
        return "LangOptions";
      if (FnName == "getPrintingPolicy")
        return "PrintingPolicy";
      if (FnName == "try_emplace" || FnName == "insert")
        return "std::pair";
      if (FnName == "get" || FnName == "value" || FnName == "front" ||
          FnName == "back")
        return unwrapType(ReceiverType);

      std::string CleanRec = unwrapType(ReceiverType);
      if (!CleanRec.empty()) {
        for (const auto &S : Scopes) {
          if (S.Name == CleanRec || S.EnclosingClass == CleanRec) {
            for (const auto &D : S.Decls) {
              if (D.Name == FnName && !D.TypeName.empty())
                return D.TypeName;
            }
          }
        }
        if (FS) {
          std::string FoundType;
          traverseIncludedHeaders(
              Self, File, Code, *FS,
              [&](const PseudoModule::HeaderInfo &Info,
                  llvm::StringRef HeaderPath) {
                for (const auto &D : Info.Decls) {
                  if (D.Name == FnName &&
                      (D.EnclosingClass == CleanRec ||
                       D.EnclosingScope == CleanRec) &&
                      !D.TypeName.empty()) {
                    FoundType = D.TypeName;
                    return true;
                  }
                }
                return false;
              });
          if (!FoundType.empty())
            return FoundType;
        }
      }

      // Free function lookup
      const LocalDecl *FnDecl =
          lookupDecl(BestScope, FnName, CursorOffset, Scopes, Code);
      if (FnDecl && !FnDecl->TypeName.empty())
        return FnDecl->TypeName;
    }
  }

  // Case 2: Pre ends with identifier
  size_t BaseEnd = Pre.size();
  size_t BaseStart = BaseEnd;
  while (BaseStart > 0 &&
         (llvm::isAlnum(Pre[BaseStart - 1]) || Pre[BaseStart - 1] == '_'))
    --BaseStart;
  llvm::StringRef BaseName = Pre.slice(BaseStart, BaseEnd);

  if (BaseName == "this")
    return std::string(EffectiveEnclosingClass);

  if (!BaseName.empty()) {
    // Check if BaseName is preceded by '.' or '->'
    ssize_t DotPos = BaseStart;
    while (DotPos > 0 && llvm::isSpace(Pre[DotPos - 1]))
      --DotPos;
    std::string ReceiverType;
    if (DotPos > 0 && Pre[DotPos - 1] == '.') {
      ReceiverType = resolveExprType(
          Self, Pre.take_front(DotPos - 1).rtrim(), CursorOffset, BestScope,
          Scopes, Code, File, FS, EffectiveEnclosingClass);
    } else if (DotPos >= 2 && Pre.slice(DotPos - 2, DotPos) == "->") {
      ReceiverType = resolveExprType(
          Self, Pre.take_front(DotPos - 2).rtrim(), CursorOffset, BestScope,
          Scopes, Code, File, FS, EffectiveEnclosingClass);
    }

    if (!ReceiverType.empty()) {
      std::string CleanRec = unwrapType(ReceiverType);
      if (CleanRec == "pair") {
        if (BaseName == "first")
          return "first_type";
        if (BaseName == "second")
          return "bool";
      }
      for (const auto &S : Scopes) {
        if (S.Name == CleanRec || S.EnclosingClass == CleanRec) {
          for (const auto &D : S.Decls) {
            if (D.Name == BaseName && !D.TypeName.empty())
              return D.TypeName;
          }
        }
      }
      if (FS) {
        std::string FoundType;
        traverseIncludedHeaders(
            Self, File, Code, *FS,
            [&](const PseudoModule::HeaderInfo &Info,
                llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == BaseName &&
                    (D.EnclosingClass == CleanRec ||
                     D.EnclosingScope == CleanRec) &&
                    !D.TypeName.empty()) {
                  FoundType = D.TypeName;
                  return true;
                }
              }
              return false;
            });
        if (!FoundType.empty())
          return FoundType;
      }
    }

    const LocalDecl *BaseDecl =
        lookupDecl(BestScope, BaseName, CursorOffset, Scopes, Code);
    if (BaseDecl && !BaseDecl->TypeName.empty())
      return BaseDecl->TypeName;

    if (!EffectiveEnclosingClass.empty()) {
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.Name == BaseName &&
              (D.EnclosingClass == EffectiveEnclosingClass ||
               S.Name == EffectiveEnclosingClass) &&
              !D.TypeName.empty())
            return D.TypeName;
        }
      }
      if (FS) {
        std::string FoundType;
        traverseIncludedHeaders(
            Self, File, Code, *FS,
            [&](const PseudoModule::HeaderInfo &Info,
                llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == BaseName &&
                    (D.EnclosingClass == EffectiveEnclosingClass ||
                     D.EnclosingScope == EffectiveEnclosingClass) &&
                    !D.TypeName.empty()) {
                  FoundType = D.TypeName;
                  return true;
                }
              }
              return false;
            });
        if (!FoundType.empty())
          return FoundType;
      }
    }
  }

  return "";
}

static const LocalDecl *resolveTargetDecl(
    const pseudo::Token *Touched, size_t BestScope,
    const std::vector<LexicalScope> &Scopes, const ParseOutput &Parsed,
    llvm::StringRef Code, bool ExpectsType = false) {
  std::string TargetName = getOrigToken(*Touched, Parsed).text().str();
  size_t TouchedOffset = tokenStartOffset(*Touched, Parsed);

  if (isCtorMemberInitializerName(Touched, Parsed)) {
    std::string TargetClass = Scopes[BestScope].EnclosingClass;
    if (TargetClass.empty() && Scopes[BestScope].ParentId < Scopes.size()) {
      const auto &ParentScope = Scopes[Scopes[BestScope].ParentId];
      if (ParentScope.Kind == ScopeKind::Class)
        TargetClass = ParentScope.Name;
    }
    for (const auto &S : Scopes) {
      if (S.Kind == ScopeKind::Class &&
          (TargetClass.empty() || S.Name == TargetClass ||
           S.EnclosingClass == TargetClass)) {
        for (const auto &D : S.Decls) {
          if (D.Name == TargetName && D.IsMember && !D.IsParameter &&
              D.Kind != PseudoModule::DeclKind::Parameter) {
            return &D;
          }
        }
      }
    }
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Name == TargetName && D.IsMember && !D.IsParameter &&
            D.Kind != PseudoModule::DeclKind::Parameter &&
            (TargetClass.empty() || D.EnclosingClass == TargetClass)) {
          return &D;
        }
      }
    }
    return nullptr;
  }

  // If touched token is on the LHS of an init-capture definition, return that capture.
  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      if (D.IsInitCapture && D.DeclOffset == TouchedOffset && D.Name == TargetName)
        return &D;
    }
  }

  const LocalDecl *TargetDecl = nullptr;
  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Parsed.RawStream.tokens().size()) {
    RawTok = &Parsed.RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Parsed.RawStream.tokens().data() &&
             Touched < Parsed.RawStream.tokens().data() + Parsed.RawStream.tokens().size()) {
    RawTok = Touched;
  }
  pseudo::Token::Index TouchedIdx =
      RawTok ? Parsed.RawStream.index(*RawTok) : pseudo::Token::Invalid;
  const pseudo::Token *OpTok = nullptr;
  const pseudo::Token *LhsTok = nullptr;
  size_t Step = 1;
  while (TouchedIdx != pseudo::Token::Invalid && TouchedIdx >= Step) {
    const auto &T = Parsed.RawStream.tokens()[TouchedIdx - Step];
    ++Step;
    if (T.Kind == tok::comment)
      continue;
    if (!OpTok) {
      if (T.Kind == tok::period || T.Kind == tok::arrow ||
          T.Kind == tok::coloncolon)
        OpTok = &T;
      else
        break;
    } else {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
        LhsTok = &T;
      break;
    }
  }
  std::string CleanRec;
  if (OpTok) {
    if (LhsTok) {
      std::string LhsName = getOrigToken(*LhsTok, Parsed).text().str();
      if (LhsName == "this") {
        std::string TargetClass = Scopes[BestScope].EnclosingClass;
        CleanRec = TargetClass;
        if (!TargetClass.empty()) {
          for (const auto &CS : Scopes) {
            if (CS.Kind == ScopeKind::Class && CS.Name == TargetClass) {
              for (const auto &D : CS.Decls) {
                if (D.Name == TargetName) {
                  TargetDecl = &D;
                  break;
                }
              }
            }
            if (TargetDecl)
              break;
          }
        }
      } else if (OpTok->Kind == tok::coloncolon) {
        for (const auto &CS : Scopes) {
          if ((CS.Kind == ScopeKind::Class || CS.Kind == ScopeKind::Namespace) &&
              CS.Name == LhsName) {
            for (const auto &D : CS.Decls) {
              if (D.Name == TargetName) {
                // When resolving Namespace::Name, skip class member decls.
                // E.g. clangd::signatureHelp should not match
                // ClangdServer::signatureHelp stored in the clangd scope.
                if (CS.Kind == ScopeKind::Namespace &&
                    (D.IsMember || !D.EnclosingClass.empty()))
                  continue;
                if (!ExpectsType || PseudoModule::isTypeDecl(D.Kind)) {
                  TargetDecl = &D;
                  break;
                }
              }
            }
          }
          if (TargetDecl)
            break;
        }
        if (!TargetDecl) {
          for (const auto &S : Scopes) {
            for (const auto &D : S.Decls) {
              if (D.Name == TargetName && D.EnclosingClass == LhsName) {
                if (D.Kind == PseudoModule::DeclKind::Parameter || D.IsParameter)
                  continue;
                if (ExpectsType && !PseudoModule::isTypeDecl(D.Kind))
                  continue;
                TargetDecl = &D;
                break;
              }
            }
            if (TargetDecl)
              break;
          }
        }
      } else {
        size_t LhsOffset = tokenStartOffset(*LhsTok, Parsed);
        const LocalDecl *LhsDecl =
            lookupDecl(BestScope, LhsName, LhsOffset, Scopes, Code);
        if (LhsDecl && !LhsDecl->TypeName.empty()) {
          CleanRec = unwrapType(LhsDecl->TypeName);
          for (const auto &CS : Scopes) {
            if (CS.Kind == ScopeKind::Class &&
                (CS.Name == CleanRec || CS.EnclosingClass == CleanRec)) {
              for (const auto &D : CS.Decls) {
                if (D.Name == TargetName) {
                  TargetDecl = &D;
                  break;
                }
              }
            }
            if (TargetDecl)
              break;
          }
        }
      }
    }
  }

  if (!TargetDecl && !OpTok) {
    TargetDecl = lookupDecl(BestScope, TargetName, TouchedOffset, Scopes, Code,
                            ExpectsType);
    if (!TargetDecl)
      TargetDecl = findAnyDecl(TargetName, Scopes, ExpectsType);
  }

  if (!TargetDecl && OpTok &&
      (OpTok->Kind == tok::period || OpTok->Kind == tok::arrow)) {
    if (!CleanRec.empty()) {
      const LocalDecl *Candidate = findAnyDecl(TargetName, Scopes, ExpectsType);
      if (Candidate && Candidate->IsMember && Candidate->EnclosingClass == CleanRec)
        TargetDecl = Candidate;
    }
  }

  return TargetDecl;
}

llvm::Expected<std::vector<LocatedSymbol>>
PseudoModule::locateSymbolAt(PathRef File, llvm::StringRef Code, Position Pos) {
  auto Includes = extractIncludes(Code);
  auto FS = getFS();
  auto IncludeDirs = getIncludeDirectories(File);
  std::string CurrentDir = llvm::sys::path::parent_path(File).str();

  // 1. Check if Pos is on an #include directive line
  for (const auto &Inc : Includes) {
    if (Inc.HashLine == Pos.line && FS) {
      std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
      if (!Resolved.empty()) {
        LocatedSymbol Sym;
        Sym.Name = std::string(llvm::sys::path::filename(Resolved));
        Sym.PreferredDeclaration = {
            URIForFile::canonicalize(Resolved, File),
            Range{Position{0, 0}, Position{0, 0}}};
        Sym.Definition = Sym.PreferredDeclaration;
        return std::vector<LocatedSymbol>{std::move(Sym)};
      }
      return std::vector<LocatedSymbol>{};
    }
  }

  // 2. Parse current file
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to parse code",
        llvm::inconvertibleErrorCode());

  auto Offset = positionToOffset(Code, Pos);
  if (!Offset)
    return Offset.takeError();

  const pseudo::Token *Touched = findTouchedIdentifier(*Parsed, *Offset);
  if (!Touched) {
    if (*Offset > 0)
      Touched = findTouchedIdentifier(*Parsed, *Offset - 1);
    if (!Touched && *Offset + 1 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 1);
    if (!Touched && *Offset + 2 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 2);
  }
  if (!Touched)
    return std::vector<LocatedSymbol>{};

  size_t TouchedOffset = tokenStartOffset(*Touched, *Parsed);

  // Build scopes
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Find innermost scope containing touched offset
  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (TouchedOffset >= S.StartOffset && TouchedOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  // 3. Resolve target decl locally
  bool ExpectsType = isTypeContext(Touched, *Parsed);
  const LocalDecl *TargetDecl =
      resolveTargetDecl(Touched, BestScope, Scopes, *Parsed, Code, ExpectsType);

  if (TargetDecl) {
    LocatedSymbol LS;
    LS.Name = TargetDecl->Name;
    auto FileURI = URIForFile::canonicalize(File, File);
    if (TargetDecl->IsDefinition) {
      LS.Definition = Location{FileURI, TargetDecl->NameRange};
      if (const auto *Decl = findMatchingDeclaration(TargetDecl, Scopes)) {
        LS.PreferredDeclaration = Location{FileURI, Decl->NameRange};
      } else if (FS && TargetDecl->Kind != PseudoModule::DeclKind::Constructor) {
        // For constructors, the definition IS the preferred declaration
        // (the cursor is on the function name in the .cpp, not the in-class
        // forward declaration in the header).
        HeaderDecl BestDecl;
        std::string BestHeaderPath;
        bool FoundDecl = false;
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == TargetDecl->Name) {
                  if (TargetDecl->IsMember) {
                    if (!TargetDecl->EnclosingClass.empty() &&
                        (D.EnclosingClass == TargetDecl->EnclosingClass ||
                         D.EnclosingScope == TargetDecl->EnclosingClass)) {
                      BestDecl = D;
                      BestHeaderPath = HeaderPath.str();
                      FoundDecl = true;
                      return true;
                    }
                  } else {
                    if (D.Kind == TargetDecl->Kind) {
                      BestDecl = D;
                      BestHeaderPath = HeaderPath.str();
                      FoundDecl = true;
                      return true;
                    }
                  }
                }
              }
              return false;
            },
            /*MaxHeaders=*/100, /*MaxDepth=*/4);
        if (FoundDecl) {
          LS.PreferredDeclaration = Location{
              URIForFile::canonicalize(BestHeaderPath, File),
              BestDecl.NameRange};
        } else {
          LS.PreferredDeclaration = *LS.Definition;
        }
      } else {
        LS.PreferredDeclaration = *LS.Definition;
      }
    } else {
      LS.PreferredDeclaration = Location{FileURI, TargetDecl->NameRange};
      if (const auto *Def = findMatchingDefinition(TargetDecl, Scopes))
        LS.Definition = Location{FileURI, Def->NameRange};
      else
        LS.Definition = LS.PreferredDeclaration;
    }
    return std::vector<LocatedSymbol>{std::move(LS)};
  }

  // 4. If not found locally, search included headers
  if (!FS)
    return std::vector<LocatedSymbol>{};

  std::string TargetName = getOrigToken(*Touched, *Parsed).text().str();
  if (TargetName.empty())
    return std::vector<LocatedSymbol>{};

  const pseudo::Token *RawTok = nullptr;
  if (Touched->OriginalIndex != pseudo::Token::Invalid &&
      Touched->OriginalIndex < Parsed->RawStream.tokens().size()) {
    RawTok = &Parsed->RawStream.tokens()[Touched->OriginalIndex];
  } else if (Touched >= Parsed->RawStream.tokens().data() &&
             Touched < Parsed->RawStream.tokens().data() + Parsed->RawStream.tokens().size()) {
    RawTok = Touched;
  }
  pseudo::Token::Index TouchedIdx =
      RawTok ? Parsed->RawStream.index(*RawTok) : pseudo::Token::Invalid;
  const pseudo::Token *OpTok = nullptr;
  const pseudo::Token *LhsTok = nullptr;
  size_t Step = 1;
  while (TouchedIdx != pseudo::Token::Invalid && TouchedIdx >= Step) {
    const auto &T = Parsed->RawStream.tokens()[TouchedIdx - Step];
    ++Step;
    if (T.Kind == tok::comment)
      continue;
    if (!OpTok) {
      if (T.Kind == tok::period || T.Kind == tok::arrow ||
          T.Kind == tok::coloncolon)
        OpTok = &T;
      else
        break;
    } else {
      if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier)
        LhsTok = &T;
      break;
    }
  }

  auto findUtilityHeader = [&]() -> std::string {
    for (const auto &Inc : Includes) {
      if (Inc.Written.find("utility") != llvm::StringRef::npos ||
          Inc.Written.find("pair") != llvm::StringRef::npos) {
        std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
        if (!Resolved.empty())
          return Resolved;
      }
    }
    std::string PairHeader;
    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          if (HeaderPath.ends_with("/utility") ||
              HeaderPath.ends_with("utility") ||
              HeaderPath.ends_with("/pair.h") ||
              HeaderPath.find("utility") != llvm::StringRef::npos) {
            PairHeader = HeaderPath.str();
            return true;
          }
          return false;
        },
        /*MaxHeaders=*/100, /*MaxDepth=*/4);
    if (!PairHeader.empty())
      return PairHeader;
    IncludeDirective UtilInc;
    UtilInc.Written = "utility";
    UtilInc.IsAngled = true;
    std::string Resolved = resolveHeader(UtilInc, CurrentDir, IncludeDirs, *FS);
    if (!Resolved.empty())
      return Resolved;
    return "";
  };

  auto findSymbolRangeInFile = [&](llvm::StringRef HeaderPath,
                                   llvm::StringRef SymName) -> Range {
    Range TargetRange{Position{0, 0}, Position{0, 0}};
    if (auto Info = getHeaderInfo(HeaderPath, *FS)) {
      for (const auto &D : Info->Decls) {
        if (D.Name == SymName)
          return D.NameRange;
      }
    }
    if (auto Buf = FS->getBufferForFile(HeaderPath)) {
      llvm::StringRef Content = (*Buf)->getBuffer();
      size_t SearchPos = 0;
      while ((SearchPos = Content.find(SymName, SearchPos)) !=
             llvm::StringRef::npos) {
        bool WordBefore = SearchPos == 0 ||
            (!llvm::isAlnum(Content[SearchPos - 1]) &&
             Content[SearchPos - 1] != '_');
        if (!WordBefore) {
          SearchPos += SymName.size();
          continue;
        }

        // Must not be on a preprocessor line (#include, #define, etc.)
        size_t LineStart = Content.rfind('\n', SearchPos);
        LineStart = (LineStart == llvm::StringRef::npos) ? 0 : LineStart + 1;
        llvm::StringRef LineBefore = Content.slice(LineStart, SearchPos).ltrim();
        if (LineBefore.starts_with("#") || LineBefore.starts_with("//") ||
            LineBefore.starts_with("/*") || LineBefore.starts_with("*")) {
          SearchPos += SymName.size();
          continue;
        }

        size_t After = SearchPos + SymName.size();
        while (After < Content.size() && llvm::isSpace(Content[After]))
          ++After;
        bool ParenAfter =
            After < Content.size() && (Content[After] == '(' || Content[After] == '<');
        bool DeclAfter =
            After < Content.size() &&
            (Content[After] == ';' || Content[After] == '=' ||
             Content[After] == ':' || Content[After] == '{' ||
             Content[After] == ',');
        if (ParenAfter || DeclAfter) {
          return Range{offsetToPosition(Content, SearchPos),
                       offsetToPosition(Content, SearchPos + SymName.size())};
        }
        SearchPos += SymName.size();
      }
    }
    return TargetRange;
  };

  // Strictly find a template function definition (not a call/declaration).
  // Requires: 'template' on a preceding line, 'FnName(' word-boundary match,
  // and after closing ')' of param list comes '{' or 'noexcept'/'constexpr'
  // qualifiers then '{'. Rejects any match followed by ';' (forward decl / call).
  auto findFunctionDefinition = [&](llvm::StringRef HeaderPath,
                                    llvm::StringRef FnName) -> Range {
    Range Empty{Position{0, 0}, Position{0, 0}};
    auto Buf = FS->getBufferForFile(HeaderPath);
    if (!Buf)
      return Empty;
    llvm::StringRef Content = (*Buf)->getBuffer();
    size_t SearchPos = 0;
    while ((SearchPos = Content.find(FnName, SearchPos)) !=
           llvm::StringRef::npos) {
      size_t MatchPos = SearchPos;
      SearchPos += FnName.size();

      // Word boundary check before
      bool WordBefore = MatchPos == 0 ||
          (!llvm::isAlnum(Content[MatchPos - 1]) &&
           Content[MatchPos - 1] != '_');
      if (!WordBefore)
        continue;

      // Word boundary check after
      if (SearchPos < Content.size() &&
          (llvm::isAlnum(Content[SearchPos]) || Content[SearchPos] == '_'))
        continue;

      // Must not be on a preprocessor/comment line
      size_t LineStart = Content.rfind('\n', MatchPos);
      LineStart = (LineStart == llvm::StringRef::npos) ? 0 : LineStart + 1;
      llvm::StringRef LinePrefix = Content.slice(LineStart, MatchPos).ltrim();
      if (LinePrefix.starts_with("#") || LinePrefix.starts_with("//") ||
          LinePrefix.starts_with("/*") || LinePrefix.starts_with("*"))
        continue;

      // Must be followed by '(' (function call/def)
      size_t After = SearchPos;
      while (After < Content.size() && llvm::isSpace(Content[After]))
        ++After;
      if (After >= Content.size() || Content[After] != '(')
        continue;

      // Scan past parameter list to find matching ')'
      size_t ParenDepth = 0;
      size_t ParenEnd = After;
      while (ParenEnd < Content.size()) {
        if (Content[ParenEnd] == '(') ++ParenDepth;
        else if (Content[ParenEnd] == ')') {
          --ParenDepth;
          if (ParenDepth == 0) { ++ParenEnd; break; }
        }
        ++ParenEnd;
      }

      // After ')': skip whitespace and qualifiers (noexcept, constexpr, const)
      // then check for '{' (body). If we see ';', it's a call or forward decl.
      size_t BodyPos = ParenEnd;
      while (BodyPos < Content.size() && llvm::isSpace(Content[BodyPos]))
        ++BodyPos;
      // Skip qualifiers like noexcept(...), -> RetType, const, constexpr
      bool FoundBody = false;
      size_t ScanPos = BodyPos;
      // Scan up to the end of the line or a few lines to find '{' or ';'
      size_t ScanEnd = std::min(Content.size(), ScanPos + 200);
      while (ScanPos < ScanEnd) {
        char C = Content[ScanPos];
        if (C == '{') { FoundBody = true; break; }
        if (C == ';') break; // forward decl or call statement
        ++ScanPos;
      }
      if (!FoundBody)
        continue;

      // Require 'template' to appear on one of the preceding 5 lines
      bool HasTemplate = false;
      size_t PrevLine = MatchPos;
      for (int LineCount = 0; LineCount < 5 && PrevLine > 0; ++LineCount) {
        size_t PL = Content.rfind('\n', PrevLine - 1);
        size_t PLS = (PL == llvm::StringRef::npos) ? 0 : PL + 1;
        llvm::StringRef PrevLineStr = Content.slice(PLS, PrevLine).ltrim();
        if (PrevLineStr.starts_with("template")) {
          HasTemplate = true;
          break;
        }
        PrevLine = PLS;
      }
      if (!HasTemplate)
        continue;

      return Range{offsetToPosition(Content, MatchPos),
                   offsetToPosition(Content, MatchPos + FnName.size())};
    }
    return Empty;
  };

  auto findStdFunction = [&](llvm::StringRef FnName) -> std::pair<std::string, Range> {
    std::string UtilHeader = findUtilityHeader();
    if (UtilHeader.empty())
      return {"", Range{Position{0, 0}, Position{0, 0}}};

    // 1. Check if defined in UtilHeader itself (strict definition check)
    Range R = findFunctionDefinition(UtilHeader, FnName);
    if (R.start.line != 0 || R.start.character != 0 ||
        R.end.line != 0 || R.end.character != 0) {
      return {UtilHeader, R};
    }

    // 2. UtilHeader may delegate to subheaders like <bits/move.h> or <__utility/move.h>
    if (auto Info = getHeaderInfo(UtilHeader, *FS)) {
      std::string UtilDir = llvm::sys::path::parent_path(UtilHeader).str();
      auto IncDirs = getIncludeDirectories(File);
      for (const auto &Inc : Info->Includes) {
        if (Inc.Written.find(FnName) != llvm::StringRef::npos) {
          std::string SubH = resolveHeader(Inc, UtilDir, IncDirs, *FS);
          if (!SubH.empty()) {
            Range SubR = findFunctionDefinition(SubH, FnName);
            if (SubR.start.line != 0 || SubR.start.character != 0 ||
                SubR.end.line != 0 || SubR.end.character != 0) {
              return {SubH, SubR};
            }
          }
        }
      }
      for (const auto &Inc : Info->Includes) {
        std::string SubH = resolveHeader(Inc, UtilDir, IncDirs, *FS);
        if (!SubH.empty()) {
          Range SubR = findFunctionDefinition(SubH, FnName);
          if (SubR.start.line != 0 || SubR.start.character != 0 ||
              SubR.end.line != 0 || SubR.end.character != 0) {
            return {SubH, SubR};
          }
        }
      }
    }
    // 3. Fallback: use general text search (handles environments where utility
    //    headers only provide forward declarations, e.g. in tests).
    Range Fallback = findSymbolRangeInFile(UtilHeader, FnName);
    if (Fallback.start.line != 0 || Fallback.start.character != 0 ||
        Fallback.end.line != 0 || Fallback.end.character != 0) {
      return {UtilHeader, Fallback};
    }
    return {"", Range{Position{0, 0}, Position{0, 0}}};
  };

  if (OpTok && (OpTok->Kind == tok::period || OpTok->Kind == tok::arrow)) {
    size_t OpOffset = tokenStartOffset(*OpTok, *Parsed);
    llvm::StringRef Pre = Code.take_front(OpOffset).rtrim();
    std::string ReceiverType = resolveExprType(
        *this, Pre, TouchedOffset, BestScope, Scopes, Code, File, FS.get(),
        Scopes[BestScope].EnclosingClass);
    std::string CleanRec = unwrapType(ReceiverType);

    std::string ResolvedRec;
    if (CleanRec == "entry" || ReceiverType.find("entry") != std::string::npos ||
        ReceiverType.find("Entry") != std::string::npos) {
      ResolvedRec = "SimpleRegistryEntry";
    }
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Kind == DeclKind::TypeAlias && D.Name == CleanRec && !D.TypeName.empty()) {
          ResolvedRec = unwrapType(D.TypeName);
          break;
        }
      }
      if (!ResolvedRec.empty())
        break;
    }

    HeaderDecl BestDecl;
    std::string BestHeaderPath;
    bool Found = false;

    if (!CleanRec.empty() || !ResolvedRec.empty()) {
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            if (ResolvedRec.empty()) {
              for (const auto &D : Info.Decls) {
                if (D.Kind == DeclKind::TypeAlias && D.Name == CleanRec &&
                    !D.TypeName.empty()) {
                  ResolvedRec = unwrapType(D.TypeName);
                  break;
                }
              }
            }
            for (const auto &D : Info.Decls) {
              if (D.Name == TargetName) {
                if (D.EnclosingClass == CleanRec || D.EnclosingScope == CleanRec ||
                    (!ResolvedRec.empty() &&
                     (D.EnclosingClass == ResolvedRec ||
                      D.EnclosingScope == ResolvedRec)) ||
                    (CleanRec == "entry" &&
                     D.EnclosingClass == "SimpleRegistryEntry") ||
                    (!CleanRec.empty() &&
                     llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec))) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                }
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);
    }

    if (!Found && !CleanRec.empty()) {
      std::vector<std::string> CandHeaders;
      if (ReceiverType.find("llvm::") != std::string::npos ||
          CleanRec.find("Small") != std::string::npos ||
          CleanRec.find("Dense") != std::string::npos ||
          CleanRec.find("String") != std::string::npos ||
          CleanRec.find("Array") != std::string::npos) {
        CandHeaders.push_back("llvm/ADT/" + CleanRec + ".h");
        CandHeaders.push_back("llvm/Support/" + CleanRec + ".h");
        if (CleanRec == "SmallString")
          CandHeaders.push_back("llvm/ADT/SmallVector.h");
      }
      if (ReceiverType.find("clang::") != std::string::npos) {
        CandHeaders.push_back("clang/Basic/" + CleanRec + ".h");
        CandHeaders.push_back("clang/AST/" + CleanRec + ".h");
      }
      CandHeaders.push_back(CleanRec + ".h");

      for (const auto &Cand : CandHeaders) {
        IncludeDirective Inc;
        Inc.Written = Cand;
        Inc.IsAngled = true;
        std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
        if (!Resolved.empty()) {
          if (auto Info = getHeaderInfo(Resolved, *FS)) {
            for (const auto &D : Info->Decls) {
              if (D.Name == TargetName) {
                if (D.EnclosingClass == CleanRec ||
                    D.EnclosingScope == CleanRec ||
                    (!ResolvedRec.empty() && D.EnclosingClass == ResolvedRec) ||
                    (CleanRec == "SmallString" && D.EnclosingClass == "SmallVector") ||
                    llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec)) {
                  BestDecl = D;
                  BestHeaderPath = Resolved;
                  Found = true;
                  break;
                }
              }
            }
            if (Found)
              break;
          }
        }
      }
    }

    if (!Found && (CleanRec == "pair" || TargetName == "first" ||
                   TargetName == "second")) {
      std::string PairHeader = findUtilityHeader();
      if (!PairHeader.empty()) {
        Range TargetRange = findSymbolRangeInFile(PairHeader, TargetName);
        LocatedSymbol LS;
        LS.Name = TargetName;
        LS.PreferredDeclaration = {
            URIForFile::canonicalize(PairHeader, File),
            TargetRange};
        LS.Definition = LS.PreferredDeclaration;
        return std::vector<LocatedSymbol>{std::move(LS)};
      }
    }

    if (!Found && (CleanRec == "LangOptions" || CleanRec == "LangOptionsBase")) {
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            if (HeaderPath.ends_with(".def")) {
              Range TargetRange = findSymbolRangeInFile(HeaderPath, TargetName);
              if (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
                  TargetRange.end.line != 0 || TargetRange.end.character != 0) {
                BestDecl.Name = TargetName;
                BestDecl.NameRange = TargetRange;
                BestHeaderPath = HeaderPath.str();
                Found = true;
                return true;
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);
    }

    if (!Found && !CleanRec.empty()) {
      // If member wasn't found in parsed decls, search the header that DEFINES CleanRec.
      // D.IsDefinition MUST be true to avoid forward declarations (e.g. class SmallString; in Preprocessor.h).
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            bool DefinesCleanRec = false;
            Range ClassScopeRange{Position{0, 0}, Position{0, 0}};
            for (const auto &D : Info.Decls) {
              if (D.Name == CleanRec && PseudoModule::isTypeDecl(D.Kind) && D.IsDefinition) {
                DefinesCleanRec = true;
                ClassScopeRange = D.ScopeRange;
                break;
              }
            }
            if (DefinesCleanRec) {
              Range TargetRange = findSymbolRangeInFile(HeaderPath, TargetName);
              if (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
                  TargetRange.end.line != 0 || TargetRange.end.character != 0) {
                bool InsideClass = true;
                if (ClassScopeRange.start.line != 0 || ClassScopeRange.end.line != 0) {
                  if (TargetRange.start.line < ClassScopeRange.start.line ||
                      TargetRange.end.line > ClassScopeRange.end.line)
                    InsideClass = false;
                }
                if (InsideClass) {
                  BestDecl.Name = TargetName;
                  BestDecl.NameRange = TargetRange;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                }
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);
    }

    if (Found) {
      LocatedSymbol LS;
      LS.Name = BestDecl.Name;
      LS.PreferredDeclaration.uri = URIForFile::canonicalize(BestHeaderPath, File);
      LS.PreferredDeclaration.range = BestDecl.NameRange;
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
    return std::vector<LocatedSymbol>{};
  }

  if (OpTok && OpTok->Kind == tok::coloncolon && LhsTok) {
    std::string LhsName = getOrigToken(*LhsTok, *Parsed).text().str();

    // Check std::move or std::forward
    if ((LhsName == "std" && (TargetName == "move" || TargetName == "forward")) ||
        TargetName == "move" || TargetName == "forward") {
      auto [UtilHeader, TargetRange] = findStdFunction(TargetName);
      if (!UtilHeader.empty() &&
          (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
           TargetRange.end.line != 0 || TargetRange.end.character != 0)) {
        LocatedSymbol LS;
        LS.Name = TargetName;
        LS.PreferredDeclaration = {
            URIForFile::canonicalize(UtilHeader, File),
            TargetRange};
        LS.Definition = LS.PreferredDeclaration;
        return std::vector<LocatedSymbol>{std::move(LS)};
      }
    }

    std::string ResolvedLhs;
    if (LhsName == "FeatureModuleRegistry" || llvm::StringRef(LhsName).ends_with("Registry"))
      ResolvedLhs = "Registry";
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Kind == DeclKind::TypeAlias && D.Name == LhsName && !D.TypeName.empty()) {
          ResolvedLhs = unwrapType(D.TypeName);
          break;
        }
      }
      if (!ResolvedLhs.empty())
        break;
    }

    HeaderDecl BestDecl;
    std::string BestHeaderPath;
    bool Found = false;
    HeaderDecl FallbackDecl;
    std::string FallbackHeaderPath;
    bool FallbackFound = false;

    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          if (ResolvedLhs.empty()) {
            for (const auto &D : Info.Decls) {
              if (D.Kind == DeclKind::TypeAlias && D.Name == LhsName &&
                  !D.TypeName.empty()) {
                ResolvedLhs = unwrapType(D.TypeName);
                break;
              }
            }
          }
          for (const auto &D : Info.Decls) {
            if (D.Name == TargetName) {
              if (ExpectsType && !PseudoModule::isTypeDecl(D.Kind))
                continue;
              if (D.EnclosingClass == LhsName ||
                  (D.EnclosingClass.empty() && D.EnclosingScope == LhsName) ||
                  (!ResolvedLhs.empty() &&
                   (D.EnclosingClass == ResolvedLhs ||
                    (D.EnclosingClass.empty() && D.EnclosingScope == ResolvedLhs))) ||
                  (llvm::StringRef(LhsName).ends_with("Registry") && D.EnclosingClass == "Registry") ||
                  (!LhsName.empty() && !D.EnclosingClass.empty() &&
                   llvm::StringRef(LhsName).ends_with_insensitive(D.EnclosingClass))) {
                if (D.IsDefinition) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                } else if (!FallbackFound) {
                  FallbackDecl = D;
                  FallbackHeaderPath = HeaderPath.str();
                  FallbackFound = true;
                }
              }
            }
          }
          return false;
        },
        /*MaxHeaders=*/100, /*MaxDepth=*/4);

    if (!Found && FallbackFound) {
      BestDecl = FallbackDecl;
      BestHeaderPath = FallbackHeaderPath;
      Found = true;
    }

    if (Found) {
      LocatedSymbol LS;
      LS.Name = BestDecl.Name;
      LS.PreferredDeclaration.uri = URIForFile::canonicalize(BestHeaderPath, File);
      LS.PreferredDeclaration.range = BestDecl.NameRange;
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
    return std::vector<LocatedSymbol>{};
  }

  HeaderDecl BestDecl;
  std::string BestHeaderPath;
  bool Found = false;
  HeaderDecl FallbackDecl;
  std::string FallbackHeaderPath;
  bool FallbackFound = false;

  if (TargetName == "move" || TargetName == "forward") {
    auto [UtilHeader, TargetRange] = findStdFunction(TargetName);
    if (!UtilHeader.empty() &&
        (TargetRange.start.line != 0 || TargetRange.start.character != 0 ||
         TargetRange.end.line != 0 || TargetRange.end.character != 0)) {
      LocatedSymbol LS;
      LS.Name = TargetName;
      LS.PreferredDeclaration = {
          URIForFile::canonicalize(UtilHeader, File),
          TargetRange};
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
  }

  traverseIncludedHeaders(
      *this, File, Code, *FS,
      [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
        for (const auto &D : Info.Decls) {
          if (D.Name == TargetName) {
            if (ExpectsType) {
              if (PseudoModule::isTypeDecl(D.Kind)) {
                if (D.IsDefinition) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                } else if (!FallbackFound) {
                  FallbackDecl = D;
                  FallbackHeaderPath = HeaderPath.str();
                  FallbackFound = true;
                }
              }
            } else {
              if (!PseudoModule::isTypeDecl(D.Kind)) {
                if (D.IsDefinition) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                } else if (!FallbackFound) {
                  FallbackDecl = D;
                  FallbackHeaderPath = HeaderPath.str();
                  FallbackFound = true;
                }
              } else if (!FallbackFound) {
                FallbackDecl = D;
                FallbackHeaderPath = HeaderPath.str();
                FallbackFound = true;
              }
            }
          }
        }
        return false;
      },
      /*MaxHeaders=*/100, /*MaxDepth=*/4);

  if (!Found && FallbackFound) {
    BestDecl = FallbackDecl;
    BestHeaderPath = FallbackHeaderPath;
    Found = true;
  }

  if (Found) {
    LocatedSymbol LS;
    LS.Name = BestDecl.Name;
    LS.PreferredDeclaration.uri = URIForFile::canonicalize(BestHeaderPath, File);
    LS.PreferredDeclaration.range = BestDecl.NameRange;
    LS.Definition = LS.PreferredDeclaration;
    return std::vector<LocatedSymbol>{std::move(LS)};
  }

  if (TargetName == "std") {
    for (const auto &Inc : Includes) {
      if (Inc.IsAngled) {
        std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
        if (!Resolved.empty()) {
          LocatedSymbol LS;
          LS.Name = "std";
          LS.PreferredDeclaration = {
              URIForFile::canonicalize(Resolved, File),
              Range{Position{0, 0}, Position{0, 0}}};
          LS.Definition = LS.PreferredDeclaration;
          return std::vector<LocatedSymbol>{std::move(LS)};
        }
      }
    }
    std::string AnyStdHeader;
    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          for (const auto &SubInc : Info.Includes) {
            if (SubInc.IsAngled) {
              std::string HDir = llvm::sys::path::parent_path(HeaderPath).str();
              std::string Resolved = resolveHeader(SubInc, HDir, IncludeDirs, *FS);
              if (!Resolved.empty()) {
                AnyStdHeader = Resolved;
                return true;
              }
            }
          }
          return false;
        },
        /*MaxHeaders=*/60, /*MaxDepth=*/3);
    if (AnyStdHeader.empty()) {
      AnyStdHeader = findUtilityHeader();
    }
    if (!AnyStdHeader.empty()) {
      LocatedSymbol LS;
      LS.Name = "std";
      LS.PreferredDeclaration = {
          URIForFile::canonicalize(AnyStdHeader, File),
          Range{Position{0, 0}, Position{0, 0}}};
      LS.Definition = LS.PreferredDeclaration;
      return std::vector<LocatedSymbol>{std::move(LS)};
    }
  }

  return std::vector<LocatedSymbol>{};
}

llvm::Expected<ReferencesResult>
PseudoModule::findReferences(PathRef File, llvm::StringRef Code, Position Pos,
                             uint32_t Limit) {
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to parse code",
        llvm::inconvertibleErrorCode());

  auto Offset = positionToOffset(Code, Pos);
  if (!Offset)
    return Offset.takeError();

  const pseudo::Token *Touched = findTouchedIdentifier(*Parsed, *Offset);
  if (!Touched)
    return ReferencesResult{};

  std::string TargetName = getOrigToken(*Touched, *Parsed).text().str();
  size_t TouchedOffset = tokenStartOffset(*Touched, *Parsed);

  // Build scopes
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Find innermost scope
  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (TouchedOffset >= S.StartOffset && TouchedOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  const LocalDecl *TargetDecl =
      resolveTargetDecl(Touched, BestScope, Scopes, *Parsed, Code);

  ReferencesResult Result;
  URIForFile FileURI = URIForFile::canonicalize(File, File);

  size_t SearchStart = 0;
  size_t SearchEnd = Code.size();
  if (TargetDecl && (TargetDecl->IsParameter ||
                     (TargetDecl->ScopeId != 0 &&
                      Scopes[TargetDecl->ScopeId].Kind == ScopeKind::Block))) {
    const auto &DeclScope = Scopes[TargetDecl->ScopeId];
    SearchStart = DeclScope.StartOffset;
    SearchEnd = DeclScope.EndOffset;
  }

  for (const auto &T : Parsed->RawStream.tokens()) {
    if (T.Kind != tok::raw_identifier && T.Kind != tok::identifier)
      continue;
    if (getOrigToken(T, *Parsed).text() != TargetName)
      continue;

    size_t TokStart = tokenStartOffset(T, *Parsed);
    size_t TokEnd = tokenEndOffset(T, *Parsed);
    if (TokStart < SearchStart || TokEnd > SearchEnd)
      continue;

    // Check shadowing
    if (TargetDecl) {
      size_t TokScope = 0;
      size_t TokBestLen = std::numeric_limits<size_t>::max();
      for (const auto &S : Scopes) {
        if (TokStart >= S.StartOffset && TokStart <= S.EndOffset) {
          size_t Len = S.EndOffset - S.StartOffset;
          if (Len < TokBestLen) {
            TokBestLen = Len;
            TokScope = S.Id;
          }
        }
      }
      const LocalDecl *Resolved =
          lookupDecl(TokScope, TargetName, TokStart, Scopes, Code);
      if (Resolved && !isSameEntity(Resolved, TargetDecl))
        continue;
    }

    ReferencesResult::Reference Ref;
    Ref.Loc.uri = FileURI;
    Ref.Loc.range = tokenRange(T, *Parsed, Code);
    if (TargetDecl) {
      if (Ref.Loc.range == TargetDecl->NameRange) {
        Ref.Attributes = TargetDecl->IsDefinition
                             ? (ReferencesResult::Declaration |
                                ReferencesResult::Definition)
                             : ReferencesResult::Declaration;
      } else if (const auto *Matching =
                     TargetDecl->IsDefinition
                         ? findMatchingDeclaration(TargetDecl, Scopes)
                         : findMatchingDefinition(TargetDecl, Scopes)) {
        if (Ref.Loc.range == Matching->NameRange) {
          Ref.Attributes = Matching->IsDefinition
                               ? (ReferencesResult::Declaration |
                                  ReferencesResult::Definition)
                               : ReferencesResult::Declaration;
        } else {
          Ref.Attributes = 0;
        }
      } else {
        Ref.Attributes = 0;
      }
    } else {
      Ref.Attributes = 0;
    }

    Result.References.push_back(std::move(Ref));
    if (Limit > 0 && Result.References.size() >= Limit) {
      Result.HasMore = true;
      break;
    }
  }

  return Result;
}

void PseudoModule::onGoToDefinition(const TextDocumentPositionParams &Params,
                                    Callback<std::vector<Location>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoDefinitions", File,
        [this, Params, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<Location>{});
          auto Symbols = locateSymbolAt(File, Code, Params.position);
          if (!Symbols)
            return Reply(Symbols.takeError());
          std::vector<Location> Defs;
          for (auto &S : *Symbols) {
            if (Location *Toggle = getToggle(Params, S))
              return Reply(std::vector<Location>{std::move(*Toggle)});
            Defs.push_back(S.Definition.value_or(S.PreferredDeclaration));
          }
          Reply(std::move(Defs));
        });
    return;
  }
  server().locateSymbolAt(
      File, Params.position,
      [this, Params, Reply = std::move(Reply)](
          llvm::Expected<std::vector<LocatedSymbol>> Symbols) mutable {
        if (!Symbols || Symbols->empty()) {
          llvm::Error ASTErr = Symbols ? llvm::Error::success() : Symbols.takeError();
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto PseudoSymbols = locateSymbolAt(File, Code, Params.position);
              if (PseudoSymbols && !PseudoSymbols->empty()) {
                if (ASTErr)
                  vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for definitions",
                       File, std::move(ASTErr));
                else
                  vlog("AST symbol empty for {0}, falling back to pseudo-parser for definitions",
                       File);
                std::vector<Location> Defs;
                for (auto &S : *PseudoSymbols) {
                  if (Location *Toggle = getToggle(Params, S))
                    return Reply(std::vector<Location>{std::move(*Toggle)});
                  Defs.push_back(S.Definition.value_or(S.PreferredDeclaration));
                }
                return Reply(std::move(Defs));
              }
              if (!PseudoSymbols)
                consumeError(PseudoSymbols.takeError());
            }
          }
          if (ASTErr)
            return Reply(std::move(ASTErr));
          return Reply(std::vector<Location>{});
        }
        std::vector<Location> Defs;
        for (auto &S : *Symbols) {
          if (Location *Toggle = getToggle(Params, S))
            return Reply(std::vector<Location>{std::move(*Toggle)});
          Defs.push_back(S.Definition.value_or(S.PreferredDeclaration));
        }
        Reply(std::move(Defs));
      });
}

void PseudoModule::onGoToDeclaration(const TextDocumentPositionParams &Params,
                                     Callback<std::vector<Location>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoDeclarations", File,
        [this, Params, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<Location>{});
          auto Symbols = locateSymbolAt(File, Code, Params.position);
          if (!Symbols)
            return Reply(Symbols.takeError());
          std::vector<Location> Decls;
          for (auto &S : *Symbols) {
            if (Location *Toggle = getToggle(Params, S))
              return Reply(std::vector<Location>{std::move(*Toggle)});
            Decls.push_back(std::move(S.PreferredDeclaration));
          }
          Reply(std::move(Decls));
        });
    return;
  }
  server().locateSymbolAt(
      File, Params.position,
      [this, Params, Reply = std::move(Reply)](
          llvm::Expected<std::vector<LocatedSymbol>> Symbols) mutable {
        if (!Symbols || Symbols->empty()) {
          llvm::Error ASTErr = Symbols ? llvm::Error::success() : Symbols.takeError();
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto PseudoSymbols = locateSymbolAt(File, Code, Params.position);
              if (PseudoSymbols && !PseudoSymbols->empty()) {
                if (ASTErr)
                  vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for declarations",
                       File, std::move(ASTErr));
                else
                  vlog("AST symbol empty for {0}, falling back to pseudo-parser for declarations",
                       File);
                std::vector<Location> Decls;
                for (auto &S : *PseudoSymbols) {
                  if (Location *Toggle = getToggle(Params, S))
                    return Reply(std::vector<Location>{std::move(*Toggle)});
                  Decls.push_back(std::move(S.PreferredDeclaration));
                }
                return Reply(std::move(Decls));
              }
              if (!PseudoSymbols)
                consumeError(PseudoSymbols.takeError());
            }
          }
          if (ASTErr)
            return Reply(std::move(ASTErr));
          return Reply(std::vector<Location>{});
        }
        std::vector<Location> Decls;
        for (auto &S : *Symbols) {
          if (Location *Toggle = getToggle(Params, S))
            return Reply(std::vector<Location>{std::move(*Toggle)});
          Decls.push_back(std::move(S.PreferredDeclaration));
        }
        Reply(std::move(Decls));
      });
}

void PseudoModule::onReference(
    const ReferenceParams &Params,
    Callback<std::vector<ReferenceLocation>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  bool IncludeDecl = Params.context.includeDeclaration;
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoReferences", File,
        [this, Params, IncludeDecl, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<ReferenceLocation>{});
          auto Refs = findReferences(File, Code, Params.position);
          if (!Refs)
            return Reply(Refs.takeError());
          std::vector<ReferenceLocation> Result;
          Result.reserve(Refs->References.size());
          for (auto &Ref : Refs->References) {
            bool IsDecl = Ref.Attributes & ReferencesResult::Declaration;
            if (IncludeDecl || !IsDecl)
              Result.push_back(std::move(Ref.Loc));
          }
          Reply(std::move(Result));
        });
    return;
  }
  server().findReferences(
      File, Params.position, /*Limit=*/0, SupportsReferenceContainer,
      [this, Params, IncludeDecl, Reply = std::move(Reply)](
          llvm::Expected<ReferencesResult> Refs) mutable {
        if (!Refs) {
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto PseudoRefs = findReferences(File, Code, Params.position);
              if (PseudoRefs) {
                vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for references",
                     File, Refs.takeError());
                std::vector<ReferenceLocation> Result;
                Result.reserve(PseudoRefs->References.size());
                for (auto &Ref : PseudoRefs->References) {
                  bool IsDecl = Ref.Attributes & ReferencesResult::Declaration;
                  if (IncludeDecl || !IsDecl)
                    Result.push_back(std::move(Ref.Loc));
                }
                return Reply(std::move(Result));
              }
              consumeError(PseudoRefs.takeError());
            }
          }
          return Reply(Refs.takeError());
        }
        std::vector<ReferenceLocation> Result;
        Result.reserve(Refs->References.size());
        for (auto &Ref : Refs->References) {
          bool IsDecl = Ref.Attributes & ReferencesResult::Declaration;
          if (IncludeDecl || !IsDecl)
            Result.push_back(std::move(Ref.Loc));
        }
        Reply(std::move(Result));
      });
}

void PseudoModule::onDocumentHighlight(
    const TextDocumentPositionParams &Params,
    Callback<std::vector<DocumentHighlight>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoDocumentHighlight", File,
        [this, Params, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<DocumentHighlight>{});
          auto Refs = findReferences(File, Code, Params.position);
          if (!Refs)
            return Reply(Refs.takeError());
          std::vector<DocumentHighlight> Highlights;
          for (const auto &R : Refs->References) {
            DocumentHighlight DH;
            DH.range = R.Loc.range;
            DH.kind = (R.Attributes & ReferencesResult::Declaration)
                          ? DocumentHighlightKind::Write
                          : DocumentHighlightKind::Read;
            Highlights.push_back(std::move(DH));
          }
          Reply(std::move(Highlights));
        });
    return;
  }
  server().findDocumentHighlights(
      File, Params.position,
      [this, Params, Reply = std::move(Reply)](
          llvm::Expected<std::vector<DocumentHighlight>> Highlights) mutable {
        if (!Highlights) {
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto Refs = findReferences(File, Code, Params.position);
              if (Refs) {
                vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for highlights",
                     File, Highlights.takeError());
                std::vector<DocumentHighlight> DHs;
                for (const auto &R : Refs->References) {
                  DocumentHighlight DH;
                  DH.range = R.Loc.range;
                  DH.kind = (R.Attributes & ReferencesResult::Declaration)
                                ? DocumentHighlightKind::Write
                                : DocumentHighlightKind::Read;
                  DHs.push_back(std::move(DH));
                }
                return Reply(std::move(DHs));
              }
              consumeError(Refs.takeError());
            }
          }
          return Reply(Highlights.takeError());
        }
        Reply(std::move(*Highlights));
      });
}

void PseudoModule::onDocumentSymbol(const DocumentSymbolParams &Params,
                                    Callback<llvm::json::Value> Reply) {
  PathRef File = Params.textDocument.uri.file();
  URIForFile FileURI = Params.textDocument.uri;
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoDocumentSymbols", File,
        [this, Params, FileURI, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<DocumentSymbol>{});
          auto Items = getDocumentSymbols(Code);
          if (!Items)
            return Reply(Items.takeError());
          adjustSymbolKinds(*Items, SupportedSymbolKinds);
          if (SupportsHierarchicalDocumentSymbol)
            return Reply(std::move(*Items));
          return Reply(flattenSymbolHierarchy(*Items, FileURI));
        });
    return;
  }
  server().documentSymbols(
      File,
      [this, Params, FileURI, Reply = std::move(Reply)](
          llvm::Expected<std::vector<DocumentSymbol>> Items) mutable {
        if (!Items) {
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto PseudoItems = getDocumentSymbols(Code);
              if (PseudoItems) {
                vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for document symbols",
                     File, Items.takeError());
                adjustSymbolKinds(*PseudoItems, SupportedSymbolKinds);
                if (SupportsHierarchicalDocumentSymbol)
                  return Reply(std::move(*PseudoItems));
                return Reply(flattenSymbolHierarchy(*PseudoItems, FileURI));
              }
              consumeError(PseudoItems.takeError());
            }
          }
          return Reply(Items.takeError());
        }
        adjustSymbolKinds(*Items, SupportedSymbolKinds);
        if (SupportsHierarchicalDocumentSymbol)
          return Reply(std::move(*Items));
        return Reply(flattenSymbolHierarchy(*Items, FileURI));
      });
}

void PseudoModule::onSelectionRange(
    const SelectionRangeParams &Params,
    Callback<std::vector<SelectionRange>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoSelectionRange", File,
        [this, Params, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<SelectionRange>{});
          auto Ranges = getSemanticRanges(Code, Params.positions);
          if (!Ranges)
            return Reply(Ranges.takeError());
          return Reply(std::move(*Ranges));
        });
    return;
  }
  server().semanticRanges(
      File, Params.positions,
      [this, Params, Reply = std::move(Reply)](
          llvm::Expected<std::vector<SelectionRange>> Ranges) mutable {
        if (!Ranges) {
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto PseudoRanges = getSemanticRanges(Code, Params.positions);
              if (PseudoRanges) {
                vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for semantic ranges",
                     File, Ranges.takeError());
                return Reply(std::move(*PseudoRanges));
              }
              consumeError(PseudoRanges.takeError());
            }
          }
          return Reply(Ranges.takeError());
        }
        Reply(std::move(*Ranges));
      });
}

void PseudoModule::onFoldingRange(
    const FoldingRangeParams &Params,
    Callback<std::vector<FoldingRange>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoFoldingRange", File,
        [this, Params, Reply = std::move(Reply)]() mutable {
          PathRef File = Params.textDocument.uri.file();
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::vector<FoldingRange>{});
          auto Ranges = getFoldingRanges(Code, /*LineFoldingOnly=*/false);
          if (!Ranges)
            return Reply(Ranges.takeError());
          return Reply(std::move(*Ranges));
        });
    return;
  }
  server().foldingRanges(
      File,
      [this, Params, Reply = std::move(Reply)](
          llvm::Expected<std::vector<FoldingRange>> Ranges) mutable {
        if (!Ranges) {
          if (isEnabled()) {
            PathRef File = Params.textDocument.uri.file();
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto PseudoRanges = getFoldingRanges(Code, /*LineFoldingOnly=*/false);
              if (PseudoRanges) {
                vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for folding ranges",
                     File, Ranges.takeError());
                return Reply(std::move(*PseudoRanges));
              }
              consumeError(PseudoRanges.takeError());
            }
          }
          return Reply(Ranges.takeError());
        }
        Reply(std::move(*Ranges));
      });
}

struct StdHoverInfo {
  const char *Snippet;
  const char *Doc;
};

static const StdHoverInfo *getStdHoverInfo(llvm::StringRef Name) {
  static const llvm::StringMap<StdHoverInfo> Map = {
      {"move",
       {"template <typename T>\nconstexpr std::remove_reference_t<T>&& move(T&& t) noexcept",
        "std::move is used to indicate that an object t may be \"moved from\", "
        "allowing the efficient transfer of resources from t to another object."}},
      {"forward",
       {"template <typename T>\nconstexpr T&& forward(std::remove_reference_t<T>& t) noexcept",
        "std::forward forwards lvalues as either lvalues or as rvalues, depending on T, "
        "preserving the value category of the argument."}},
      {"make_unique",
       {"template <typename T, typename... Args>\nstd::unique_ptr<T> make_unique(Args&&... args)",
        "Constructs an object of type T and wraps it in a std::unique_ptr."}},
      {"make_shared",
       {"template <typename T, typename... Args>\nstd::shared_ptr<T> make_shared(Args&&... args)",
        "Allocates and constructs an object of type T and wraps it in a std::shared_ptr."}},
      {"make_pair",
       {"template <typename T1, typename T2>\nconstexpr std::pair<V1, V2> make_pair(T1&& t1, T2&& t2)",
        "Creates a std::pair object, deducing the target types from the types of arguments."}},
      {"make_tuple",
       {"template <typename... Args>\nconstexpr std::tuple<VTypes...> make_tuple(Args&&... args)",
        "Creates a std::tuple object, deducing the target types from the types of arguments."}},
      {"swap",
       {"template <typename T>\nvoid swap(T& a, T& b) noexcept",
        "Exchanges the values of a and b."}},
      {"to_string",
       {"std::string to_string(int val)",
        "Converts a numeric value to a std::string."}},
      {"min",
       {"template <typename T>\nconstexpr const T& min(const T& a, const T& b)",
        "Returns the smaller of given values."}},
      {"max",
       {"template <typename T>\nconstexpr const T& max(const T& a, const T& b)",
        "Returns the greater of given values."}},
      {"clamp",
       {"template <typename T>\nconstexpr const T& clamp(const T& v, const T& lo, const T& hi)",
        "Clamps a value between a pair of boundary values."}},
      {"find",
       {"template <typename InputIt, typename T>\nInputIt find(InputIt first, InputIt last, const T& value)",
        "Finds the first element equal to value in the given range."}},
      {"sort",
       {"template <typename RandomIt>\nvoid sort(RandomIt first, RandomIt last)",
        "Sorts the elements in the range [first, last) in non-descending order."}},
      {"unique_ptr",
       {"template <typename T, typename Deleter = std::default_delete<T>>\nclass unique_ptr",
        "std::unique_ptr is a smart pointer that owns and manages another object "
        "through a pointer and disposes of that object when the unique_ptr goes out of scope."}},
      {"shared_ptr",
       {"template <typename T>\nclass shared_ptr",
        "std::shared_ptr is a smart pointer that retains shared ownership of an object through a pointer."}},
      {"weak_ptr",
       {"template <typename T>\nclass weak_ptr",
        "std::weak_ptr is a smart pointer that holds a non-owning reference to an object managed by std::shared_ptr."}},
      {"vector",
       {"template <typename T, typename Allocator = std::allocator<T>>\nclass vector",
        "std::vector is a sequence container that encapsulates dynamic size arrays."}},
      {"string",
       {"using string = std::basic_string<char>",
        "std::string is a standard sequence container for characters."}},
      {"string_view",
       {"using string_view = std::basic_string_view<char>",
        "std::string_view is a non-owning reference to a string or substring."}},
      {"optional",
       {"template <typename T>\nclass optional",
        "std::optional manages an optional contained value, which may or may not be present."}},
      {"nullopt",
       {"constexpr std::nullopt_t nullopt{/*unspecified*/}",
        "std::nullopt is a constant used to indicate an empty std::optional."}},
      {"nullopt_t",
       {"struct nullopt_t { /*unspecified*/ }",
        "std::nullopt_t is an empty class type used to indicate an empty std::optional."}},
      {"pair",
       {"template <typename T1, typename T2>\nstruct pair",
        "std::pair is a class template that provides a way to store two heterogeneous objects as a single unit."}},
      {"first",
       {"T1 first",
        "The first element of std::pair."}},
      {"second",
       {"T2 second",
        "The second element of std::pair."}},
      {"tuple",
       {"template <typename... Types>\nclass tuple",
        "std::tuple is a fixed-size collection of heterogeneous values."}},
      {"map",
       {"template <typename Key, typename T, typename Compare = std::less<Key>>\nclass map",
        "std::map is a sorted associative container that contains key-value pairs with unique keys."}},
      {"unordered_map",
       {"template <typename Key, typename T, typename Hash = std::hash<Key>>\nclass unordered_map",
        "std::unordered_map is an associative container that contains key-value pairs with unique keys."}},
      {"set",
       {"template <typename Key, typename Compare = std::less<Key>>\nclass set",
        "std::set is an associative container that contains a sorted set of unique objects."}},
      {"unordered_set",
       {"template <typename Key, typename Hash = std::hash<Key>>\nclass unordered_set",
        "std::unordered_set is an associative container that contains a set of unique objects."}},
      {"array",
       {"template <typename T, size_t N>\nstruct array",
        "std::array is a container that encapsulates fixed size arrays."}},
      {"deque",
       {"template <typename T>\nclass deque",
        "std::deque is an indexed sequence container that allows fast insertion and deletion at both its beginning and its end."}},
      {"list",
       {"template <typename T>\nclass list",
        "std::list is a container that supports constant time insertion and removal of elements from anywhere in the container."}},
      {"queue",
       {"template <typename T>\nclass queue",
        "std::queue gives programmer the functionality of a queue - specifically, a FIFO (first-in, first-out) data structure."}},
      {"stack",
       {"template <typename T>\nclass stack",
        "std::stack gives programmer the functionality of a stack - specifically, a LIFO (last-in, first-out) data structure."}},
      {"span",
       {"template <typename T, size_t Extent = dynamic_extent>\nclass span",
        "std::span refers to a contiguous sequence of objects."}},
      {"bitset",
       {"template <size_t N>\nclass bitset",
        "std::bitset represents a fixed-size sequence of N bits."}},
      {"size_t",
       {"using size_t = /*implementation-defined*/",
        "std::size_t is the unsigned integer type of the result of the sizeof operator."}},
      {"ptrdiff_t",
       {"using ptrdiff_t = /*implementation-defined*/",
        "std::ptrdiff_t is the signed integer type of the result of subtracting two pointers."}},
      {"nullptr_t",
       {"using nullptr_t = decltype(nullptr)",
        "std::nullptr_t is the type of the null pointer literal, nullptr."}},
      {"initializer_list",
       {"template <typename T>\nclass initializer_list",
        "std::initializer_list allows access to an array of objects of type const T."}},
      {"atomic",
       {"template <typename T>\nstruct atomic",
        "Each instantiation and full specialization of the std::atomic template defines an atomic type."}},
      {"mutex",
       {"class mutex",
        "The mutex class is a synchronization primitive that can be used to protect shared data from being simultaneously accessed by multiple threads."}},
      {"lock_guard",
       {"template <typename Mutex>\nclass lock_guard",
        "The class lock_guard is a mutex wrapper that provides a convenient RAII-style mechanism for owning a mutex for the duration of a scoped block."}},
      {"unique_lock",
       {"template <typename Mutex>\nclass unique_lock",
        "The class unique_lock is a general-purpose mutex ownership wrapper providing deferred locking, time-constrained attempts at locking, recursive locking, and transfer of lock ownership."}},
      {"thread",
       {"class thread",
        "The class thread represents an individual thread of execution."}},
  };
  auto It = Map.find(Name);
  if (It != Map.end())
    return &It->second;
  return nullptr;
}

llvm::Expected<std::optional<Hover>>
PseudoModule::getHover(PathRef File, llvm::StringRef Code, Position Pos) {
  auto Includes = extractIncludes(Code);
  auto FS = getFS();
  auto IncludeDirs = getIncludeDirectories(File);
  std::string CurrentDir = llvm::sys::path::parent_path(File).str();

  // 1. Check if Pos is on an #include directive line
  for (const auto &Inc : Includes) {
    if (Inc.HashLine == Pos.line) {
      Hover H;
      H.contents.kind = MarkupKind::Markdown;
      std::string Resolved =
          (FS ? resolveHeader(Inc, CurrentDir, IncludeDirs, *FS) : "");
      std::string HeaderText =
          Inc.IsAngled ? ("<" + Inc.Written + ">") : ("\"" + Inc.Written + "\"");
      if (!Resolved.empty()) {
        H.contents.value =
            "```cpp\n#include " + HeaderText + "\n```\n\n" + Resolved;
      } else {
        H.contents.value = "```cpp\n#include " + HeaderText + "\n```";
      }
      auto LineStart = positionToOffset(Code, Position{Pos.line, 0});
      if (LineStart) {
        size_t LineEnd = Code.find('\n', *LineStart);
        if (LineEnd == llvm::StringRef::npos)
          LineEnd = Code.size();
        H.range = Range{Position{Pos.line, 0}, offsetToPosition(Code, LineEnd)};
      }
      return std::make_optional(std::move(H));
    }
  }

  // 2. Parse current file
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to parse code",
        llvm::inconvertibleErrorCode());

  auto Offset = positionToOffset(Code, Pos);
  if (!Offset)
    return Offset.takeError();

  const pseudo::Token *Touched = findTouchedIdentifier(*Parsed, *Offset);
  if (!Touched) {
    if (*Offset > 0)
      Touched = findTouchedIdentifier(*Parsed, *Offset - 1);
    if (!Touched && *Offset + 1 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 1);
    if (!Touched && *Offset + 2 < Code.size())
      Touched = findTouchedIdentifier(*Parsed, *Offset + 2);
  }
  if (!Touched)
    return std::nullopt;

  size_t TouchedOffset = tokenStartOffset(*Touched, *Parsed);

  // Build scopes
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange =
      Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Find innermost scope
  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (TouchedOffset >= S.StartOffset && TouchedOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  bool ExpectsType = isTypeContext(Touched, *Parsed);
  const LocalDecl *TargetDecl =
      resolveTargetDecl(Touched, BestScope, Scopes, *Parsed, Code, ExpectsType);

  if (TargetDecl) {
    Hover H;
    H.range = tokenRange(*Touched, *Parsed, Code);
    H.contents.kind = MarkupKind::Markdown;

    std::string Snippet;
    auto Start = positionToOffset(Code, TargetDecl->DeclRange.start);
    auto End = positionToOffset(Code, TargetDecl->DeclRange.end);
    if (Start && End && *Start < *End && *End <= Code.size()) {
      llvm::StringRef DeclText = Code.slice(*Start, *End);
      size_t BracePos = DeclText.find('{');
      if (BracePos != llvm::StringRef::npos)
        DeclText = DeclText.take_front(BracePos);
      size_t SemiPos = DeclText.find(';');
      if (SemiPos != llvm::StringRef::npos)
        DeclText = DeclText.take_front(SemiPos);
      Snippet = DeclText.trim().str();
    }
    if (Snippet.empty() || Snippet == TargetDecl->Name) {
      switch (TargetDecl->Kind) {
      case DeclKind::Class:
        Snippet = "class " + TargetDecl->Name;
        break;
      case DeclKind::Function:
      case DeclKind::Constructor:
        Snippet =
            (TargetDecl->TypeName.empty() ? "void "
                                          : TargetDecl->TypeName + " ") +
            (TargetDecl->EnclosingClass.empty()
                 ? ""
                 : TargetDecl->EnclosingClass + "::") +
            TargetDecl->Name + "()";
        break;
      case DeclKind::Variable:
      case DeclKind::Parameter:
        Snippet = (TargetDecl->TypeName.empty() ? "auto "
                                               : TargetDecl->TypeName + " ") +
                  TargetDecl->Name;
        break;
      case DeclKind::Concept:
        Snippet = "concept " + TargetDecl->Name;
        break;
      case DeclKind::TypeAlias:
        Snippet = "using " + TargetDecl->Name;
        break;
      case DeclKind::Enum:
        Snippet = "enum " + TargetDecl->Name;
        break;
      default:
        Snippet = TargetDecl->Name;
        break;
      }
    }
    H.contents.value = "```cpp\n" + Snippet + "\n```";
    return std::make_optional(std::move(H));
  }

  // Check headers and standard library if not found locally
  std::string TargetName = getOrigToken(*Touched, *Parsed).text().str();
  if (!TargetName.empty()) {
    std::string Qualifier;
    size_t PreOff = TouchedOffset;
    while (PreOff > 0 && llvm::isSpace(Code[PreOff - 1]))
      --PreOff;
    if (PreOff >= 2 && Code.substr(PreOff - 2, 2) == "::") {
      size_t QEnd = PreOff - 2;
      while (QEnd > 0 && llvm::isSpace(Code[QEnd - 1]))
        --QEnd;
      size_t QStart = QEnd;
      while (QStart > 0 &&
             (llvm::isAlnum(Code[QStart - 1]) || Code[QStart - 1] == '_'))
        --QStart;
      Qualifier = Code.slice(QStart, QEnd).str();
    }

    if (TargetName == "std") {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      H.contents.value =
          "```cpp\nnamespace std\n```\n\nStandard C++ library namespace.";
      return std::make_optional(std::move(H));
    }

    const StdHoverInfo *Std = nullptr;
    if (Qualifier == "std" || TargetName == "move" || TargetName == "forward" ||
        TargetName == "make_unique" || TargetName == "make_shared" ||
        TargetName == "unique_ptr" || TargetName == "shared_ptr" ||
        TargetName == "vector" || TargetName == "string" ||
        TargetName == "string_view" || TargetName == "optional" ||
        TargetName == "pair" || TargetName == "tuple" ||
        TargetName == "first" || TargetName == "second") {
      Std = getStdHoverInfo(TargetName);
    }

    std::string CleanRec;
    const pseudo::Token *RawTok = nullptr;
    if (Touched->OriginalIndex != pseudo::Token::Invalid &&
        Touched->OriginalIndex < Parsed->RawStream.tokens().size()) {
      RawTok = &Parsed->RawStream.tokens()[Touched->OriginalIndex];
    } else if (Touched >= Parsed->RawStream.tokens().data() &&
               Touched < Parsed->RawStream.tokens().data() + Parsed->RawStream.tokens().size()) {
      RawTok = Touched;
    }
    pseudo::Token::Index TouchedIdx =
        RawTok ? Parsed->RawStream.index(*RawTok) : pseudo::Token::Invalid;
    const pseudo::Token *OpTok = nullptr;
    size_t Step = 1;
    while (TouchedIdx != pseudo::Token::Invalid && TouchedIdx >= Step) {
      const auto &T = Parsed->RawStream.tokens()[TouchedIdx - Step];
      ++Step;
      if (T.Kind == tok::comment)
        continue;
      if (T.Kind == tok::period || T.Kind == tok::arrow) {
        OpTok = &T;
        break;
      }
      break;
    }
    std::string ResolvedRec;
    if (OpTok) {
      size_t OpOffset = tokenStartOffset(*OpTok, *Parsed);
      llvm::StringRef Pre = Code.take_front(OpOffset).rtrim();
      std::string ReceiverType = resolveExprType(
          *this, Pre, TouchedOffset, BestScope, Scopes, Code, File, FS.get(),
          Scopes[BestScope].EnclosingClass);
      CleanRec = unwrapType(ReceiverType);
      if (CleanRec == "entry" || ReceiverType.find("entry") != std::string::npos ||
          ReceiverType.find("Entry") != std::string::npos) {
        ResolvedRec = "SimpleRegistryEntry";
      }
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.Kind == DeclKind::TypeAlias && D.Name == CleanRec && !D.TypeName.empty()) {
            ResolvedRec = unwrapType(D.TypeName);
            break;
          }
        }
        if (!ResolvedRec.empty())
          break;
      }
      if (CleanRec == "pair" &&
          (TargetName == "first" || TargetName == "second")) {
        Std = getStdHoverInfo(TargetName);
      }
    }

    std::string ResolvedQualifier;
    if (Qualifier == "FeatureModuleRegistry" || llvm::StringRef(Qualifier).ends_with("Registry"))
      ResolvedQualifier = "Registry";
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.Kind == DeclKind::TypeAlias && D.Name == Qualifier && !D.TypeName.empty()) {
          ResolvedQualifier = unwrapType(D.TypeName);
          break;
        }
      }
      if (!ResolvedQualifier.empty())
        break;
    }

    HeaderDecl BestDecl;
    std::string BestHeaderPath;
    bool Found = false;

    if (FS) {
      traverseIncludedHeaders(
          *this, File, Code, *FS,
          [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
            for (const auto &D : Info.Decls) {
              if (D.Name == TargetName) {
                if (!CleanRec.empty() || !ResolvedRec.empty()) {
                  if (D.EnclosingClass == CleanRec ||
                      D.EnclosingScope == CleanRec ||
                      (!ResolvedRec.empty() &&
                       (D.EnclosingClass == ResolvedRec ||
                        D.EnclosingScope == ResolvedRec)) ||
                      (CleanRec == "entry" &&
                       D.EnclosingClass == "SimpleRegistryEntry") ||
                      (!CleanRec.empty() &&
                       llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec))) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                } else if (!Qualifier.empty() || !ResolvedQualifier.empty()) {
                  if (D.EnclosingClass == Qualifier ||
                      D.EnclosingScope == Qualifier ||
                      (!ResolvedQualifier.empty() &&
                       (D.EnclosingClass == ResolvedQualifier ||
                        D.EnclosingScope == ResolvedQualifier)) ||
                      (llvm::StringRef(Qualifier).ends_with("Registry") &&
                       D.EnclosingClass == "Registry") ||
                      (!Qualifier.empty() &&
                       llvm::StringRef(Qualifier).ends_with_insensitive(D.EnclosingClass))) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                } else if (ExpectsType) {
                  if (PseudoModule::isTypeDecl(D.Kind)) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                } else {
                  if (!PseudoModule::isTypeDecl(D.Kind)) {
                    BestDecl = D;
                    BestHeaderPath = HeaderPath.str();
                    Found = true;
                    return true;
                  }
                }
              }
            }
            return false;
          },
          /*MaxHeaders=*/100, /*MaxDepth=*/4);

      if (!Found && !CleanRec.empty()) {
        std::vector<std::string> CandHeaders;
        if (CleanRec.find("Small") != std::string::npos ||
            CleanRec.find("Dense") != std::string::npos ||
            CleanRec.find("String") != std::string::npos ||
            CleanRec.find("Array") != std::string::npos) {
          CandHeaders.push_back("llvm/ADT/" + CleanRec + ".h");
          CandHeaders.push_back("llvm/Support/" + CleanRec + ".h");
          if (CleanRec == "SmallString")
            CandHeaders.push_back("llvm/ADT/SmallVector.h");
        }
        CandHeaders.push_back("clang/Basic/" + CleanRec + ".h");
        CandHeaders.push_back("clang/AST/" + CleanRec + ".h");
        CandHeaders.push_back(CleanRec + ".h");

        for (const auto &Cand : CandHeaders) {
          IncludeDirective Inc;
          Inc.Written = Cand;
          Inc.IsAngled = true;
          std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
          if (!Resolved.empty()) {
            if (auto Info = getHeaderInfo(Resolved, *FS)) {
              for (const auto &D : Info->Decls) {
                if (D.Name == TargetName) {
                  if (D.EnclosingClass == CleanRec ||
                      D.EnclosingScope == CleanRec ||
                      (!ResolvedRec.empty() && D.EnclosingClass == ResolvedRec) ||
                      (CleanRec == "SmallString" && D.EnclosingClass == "SmallVector") ||
                      llvm::StringRef(D.EnclosingClass).ends_with_insensitive(CleanRec)) {
                    BestDecl = D;
                    BestHeaderPath = Resolved;
                    Found = true;
                    break;
                  }
                }
              }
              if (Found)
                break;
            }
          }
        }
      }

      if (!Found && OpTok && CleanRec.empty()) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.Name == TargetName && !D.EnclosingClass.empty()) {
                  BestDecl = D;
                  BestHeaderPath = HeaderPath.str();
                  Found = true;
                  return true;
                }
              }
              return false;
            },
            /*MaxHeaders=*/100, /*MaxDepth=*/4);
      }
    }

    if (Std) {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      std::string Val =
          "```cpp\n" + std::string(Std->Snippet) + "\n```\n\n" + Std->Doc;
      if (Found && !BestHeaderPath.empty())
        Val += "\n\n// Header: " + BestHeaderPath;
      H.contents.value = std::move(Val);
      return std::make_optional(std::move(H));
    }

    if (Found) {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      std::string Snippet = BestDecl.Name;
      if (auto Buf = FS->getBufferForFile(BestHeaderPath)) {
        llvm::StringRef HCode = (*Buf)->getBuffer();
        auto StartOff = positionToOffset(HCode, BestDecl.ScopeRange.start);
        auto EndOff = positionToOffset(HCode, BestDecl.ScopeRange.end);
        if (StartOff && EndOff && *StartOff < *EndOff &&
            *EndOff <= HCode.size()) {
          llvm::StringRef DeclText = HCode.slice(*StartOff, *EndOff);
          size_t Brace = DeclText.find('{');
          if (Brace != llvm::StringRef::npos)
            DeclText = DeclText.take_front(Brace);
          size_t Semi = DeclText.find(';');
          if (Semi != llvm::StringRef::npos)
            DeclText = DeclText.take_front(Semi);
          Snippet = DeclText.trim().str();
        }
        if (Snippet.empty() || Snippet == BestDecl.Name) {
          if (auto SOff = positionToOffset(HCode, BestDecl.NameRange.start)) {
            size_t LineStart = HCode.rfind('\n', *SOff);
            LineStart =
                (LineStart == llvm::StringRef::npos) ? 0 : LineStart + 1;
            size_t LineEnd = HCode.find('\n', *SOff);
            if (LineEnd == llvm::StringRef::npos)
              LineEnd = HCode.size();
            llvm::StringRef LineText = HCode.slice(LineStart, LineEnd);
            size_t Brace = LineText.find('{');
            if (Brace != llvm::StringRef::npos)
              LineText = LineText.take_front(Brace);
            Snippet = LineText.trim().str();
          }
        }
      }
      H.contents.value =
          "```cpp\n" + Snippet + "\n```\n\n// Header: " + BestHeaderPath;
      return std::make_optional(std::move(H));
    }

    if (Qualifier == "std") {
      Hover H;
      H.range = tokenRange(*Touched, *Parsed, Code);
      H.contents.kind = MarkupKind::Markdown;
      H.contents.value = "```cpp\nstd::" + TargetName + "\n```";
      return std::make_optional(std::move(H));
    }
  }

  return std::nullopt;
}

llvm::Expected<std::vector<HighlightingToken>>
PseudoModule::getSemanticHighlightings(llvm::StringRef Code) {
  auto Parsed = parseCode(Code);
  if (!Parsed || !Parsed->Root)
    return llvm::make_error<llvm::StringError>(
        "Pseudo-parser failed to build parse forest",
        llvm::inconvertibleErrorCode());

  std::vector<HighlightingToken> Tokens;

  // Build scopes and decls
  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = Code.size();
  Scopes.back().ScopeRange =
      Range{Position{0, 0}, offsetToPosition(Code, Code.size())};

  size_t CurScope = 0;
  pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
  buildScopes(Parsed->Root, NumTokens, *Parsed, Code, Scopes, CurScope);

  // Set to avoid duplicate tokens at the exact same location
  llvm::DenseMap<size_t, const LocalDecl *> DeclByOffset;

  for (const auto &S : Scopes) {
    for (const auto &D : S.Decls) {
      HighlightingToken Tok;
      Tok.R = D.NameRange;
      if (Tok.R.start.line != Tok.R.end.line ||
          Tok.R.start.character >= Tok.R.end.character)
        continue;

      DeclByOffset[D.DeclOffset] = &D;

      switch (D.Kind) {
      case DeclKind::Class:
        Tok.Kind = HighlightingKind::Class;
        break;
      case DeclKind::Enum:
        Tok.Kind = HighlightingKind::Enum;
        break;
      case DeclKind::EnumValue:
        Tok.Kind = HighlightingKind::EnumConstant;
        break;
      case DeclKind::TypeAlias:
        Tok.Kind = HighlightingKind::Typedef;
        break;
      case DeclKind::TemplateParam:
        Tok.Kind = HighlightingKind::TemplateParameter;
        break;
      case DeclKind::Concept:
        Tok.Kind = HighlightingKind::Concept;
        break;
      case DeclKind::Namespace:
        Tok.Kind = HighlightingKind::Namespace;
        break;
      case DeclKind::Function:
        Tok.Kind =
            D.IsMember ? HighlightingKind::Method : HighlightingKind::Function;
        break;
      case DeclKind::Constructor:
        Tok.Kind = HighlightingKind::Method;
        Tok.addModifier(HighlightingModifier::ConstructorOrDestructor);
        break;
      case DeclKind::Parameter:
        Tok.Kind = HighlightingKind::Parameter;
        Tok.addModifier(HighlightingModifier::FunctionScope);
        break;
      case DeclKind::Variable:
        if (D.IsMember) {
          Tok.Kind = HighlightingKind::Field;
          Tok.addModifier(HighlightingModifier::ClassScope);
        } else if (S.Kind == ScopeKind::Function || S.Kind == ScopeKind::Block) {
          Tok.Kind = HighlightingKind::LocalVariable;
          Tok.addModifier(HighlightingModifier::FunctionScope);
        } else {
          Tok.Kind = HighlightingKind::Variable;
          Tok.addModifier(HighlightingModifier::FileScope);
        }
        break;
      default:
        Tok.Kind = HighlightingKind::Unknown;
        break;
      }

      Tok.addModifier(HighlightingModifier::Declaration);
      if (D.IsDefinition)
        Tok.addModifier(HighlightingModifier::Definition);
      if (D.IsMember)
        Tok.addModifier(HighlightingModifier::ClassScope);

      Tokens.push_back(std::move(Tok));
    }
  }

  // Scan ParseableStream tokens for references, keywords, primitives, modifiers
  for (const auto &T : Parsed->ParseableStream.tokens()) {
    size_t Off = tokenStartOffset(T, *Parsed);
    Range R = tokenRange(T, *Parsed, Code);
    if (R.start.line != R.end.line || R.start.character >= R.end.character)
      continue;

    // If it is a declaration name we already added, skip it
    if (DeclByOffset.count(Off))
      continue;

    // Check if it's an identifier reference
    if (T.Kind == tok::raw_identifier || T.Kind == tok::identifier) {
      llvm::StringRef TokText = getOrigToken(T, *Parsed).text();
      if (TokText == "override" || TokText == "final") {
        HighlightingToken Tok;
        Tok.Kind = HighlightingKind::Modifier;
        Tok.R = R;
        Tokens.push_back(std::move(Tok));
        continue;
      }
      size_t BestScope = 0;
      size_t BestLen = std::numeric_limits<size_t>::max();
      for (const auto &S : Scopes) {
        if (Off >= S.StartOffset && Off <= S.EndOffset) {
          size_t Len = S.EndOffset - S.StartOffset;
          if (Len < BestLen) {
            BestLen = Len;
            BestScope = S.Id;
          }
        }
      }

      bool ExpectsType = isTypeContext(&T, *Parsed);
      const LocalDecl *TargetDecl =
          resolveTargetDecl(&T, BestScope, Scopes, *Parsed, Code, ExpectsType);
      if (TargetDecl) {
        HighlightingToken RefTok;
        RefTok.R = R;
        switch (TargetDecl->Kind) {
        case DeclKind::Class:
          RefTok.Kind = HighlightingKind::Class;
          break;
        case DeclKind::Enum:
          RefTok.Kind = HighlightingKind::Enum;
          break;
        case DeclKind::EnumValue:
          RefTok.Kind = HighlightingKind::EnumConstant;
          break;
        case DeclKind::TypeAlias:
          RefTok.Kind = HighlightingKind::Typedef;
          break;
        case DeclKind::TemplateParam:
          RefTok.Kind = HighlightingKind::TemplateParameter;
          break;
        case DeclKind::Concept:
          RefTok.Kind = HighlightingKind::Concept;
          break;
        case DeclKind::Namespace:
          RefTok.Kind = HighlightingKind::Namespace;
          break;
        case DeclKind::Function:
        case DeclKind::Constructor:
          RefTok.Kind = TargetDecl->IsMember ? HighlightingKind::Method
                                             : HighlightingKind::Function;
          break;
        case DeclKind::Parameter:
          RefTok.Kind = HighlightingKind::Parameter;
          break;
        case DeclKind::Variable:
          RefTok.Kind = TargetDecl->IsMember ? HighlightingKind::Field
                        : (TargetDecl->IsParameter ? HighlightingKind::Parameter
                                                   : HighlightingKind::LocalVariable);
          break;
        default:
          break;
        }
        if (RefTok.Kind != HighlightingKind::Unknown) {
          Tokens.push_back(std::move(RefTok));
          continue;
        }
      } else {
        if (TokText == "std" || TokText == "llvm" || TokText == "clang" ||
            TokText == "clangd") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Namespace;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
        if (TokText == "move" || TokText == "forward" ||
            TokText == "make_unique" || TokText == "make_shared" ||
            TokText == "swap") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Function;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
        if (TokText == "unique_ptr" || TokText == "shared_ptr" ||
            TokText == "weak_ptr" || TokText == "vector" ||
            TokText == "string" || TokText == "string_view" ||
            TokText == "optional" || TokText == "pair" || TokText == "tuple" ||
            TokText == "map" || TokText == "unordered_map" ||
            TokText == "set" || TokText == "unordered_set") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Class;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
        if (TokText == "first" || TokText == "second") {
          HighlightingToken RefTok;
          RefTok.R = R;
          RefTok.Kind = HighlightingKind::Field;
          Tokens.push_back(std::move(RefTok));
          continue;
        }
      }
    }

    // Primitives
    switch (T.Kind) {
    case tok::kw_int:
    case tok::kw_char:
    case tok::kw_bool:
    case tok::kw_float:
    case tok::kw_double:
    case tok::kw_void:
    case tok::kw_long:
    case tok::kw_short:
    case tok::kw_unsigned:
    case tok::kw_signed:
    case tok::kw_auto:
    case tok::kw_wchar_t:
    case tok::kw_char16_t:
    case tok::kw_char32_t: {
      HighlightingToken Tok;
      Tok.Kind = HighlightingKind::Primitive;
      Tok.R = R;
      Tokens.push_back(std::move(Tok));
      break;
    }
    // Modifiers
    case tok::kw_const:
    case tok::kw_constexpr:
    case tok::kw_static:
    case tok::kw_virtual:
    case tok::kw_inline:
    case tok::kw_explicit:
    case tok::kw_mutable:
    case tok::kw_volatile:
    case tok::kw_noexcept: {
      HighlightingToken Tok;
      Tok.Kind = HighlightingKind::Modifier;
      Tok.R = R;
      Tokens.push_back(std::move(Tok));
      break;
    }
    default:
      break;
    }
  }

  // Sort tokens by start position
  llvm::sort(Tokens, [](const HighlightingToken &A, const HighlightingToken &B) {
    if (A.R.start.line != B.R.start.line)
      return A.R.start.line < B.R.start.line;
    if (A.R.start.character != B.R.start.character)
      return A.R.start.character < B.R.start.character;
    return A.R.end.character > B.R.end.character;
  });

  // Filter out any overlapping tokens
  std::vector<HighlightingToken> NonOverlapping;
  for (const auto &Tok : Tokens) {
    if (Tok.R.start.line > Tok.R.end.line ||
        (Tok.R.start.line == Tok.R.end.line &&
         Tok.R.start.character >= Tok.R.end.character))
      continue;
    if (!NonOverlapping.empty()) {
      const auto &Last = NonOverlapping.back();
      if (Tok.R.start.line < Last.R.end.line)
        continue;
      if (Tok.R.start.line == Last.R.end.line &&
          Tok.R.start.character < Last.R.end.character)
        continue;
    }
    NonOverlapping.push_back(Tok);
  }

  return NonOverlapping;
}

llvm::Expected<SemanticTokens>
PseudoModule::getSemanticTokens(llvm::StringRef Code) {
  auto HT = getSemanticHighlightings(Code);
  if (!HT)
    return HT.takeError();
  SemanticTokens ST;
  ST.tokens = toSemanticTokens(*HT, Code);
  return ST;
}

llvm::Expected<CompletionList>
PseudoModule::getCompletions(PathRef File, llvm::StringRef Code, Position Pos) {
  auto LineStart = positionToOffset(Code, Position{Pos.line, 0});
  auto CursorOffset = positionToOffset(Code, Pos);
  if (!LineStart || !CursorOffset || *CursorOffset < *LineStart)
    return CompletionList{};

  llvm::StringRef LinePrefix = Code.slice(*LineStart, *CursorOffset);

  // 1. Preprocessor directives
  llvm::StringRef LTrim = LinePrefix.ltrim();
  if (LTrim.starts_with("#")) {
    llvm::StringRef Typed = LTrim.drop_front(1).ltrim();
    static const char *Directives[] = {
        "include", "define", "ifdef", "ifndef", "if",
        "elif",    "else",   "endif", "pragma", "undef"};
    CompletionList List;
    for (const char *Dir : Directives) {
      if (llvm::StringRef(Dir).starts_with_insensitive(Typed)) {
        CompletionItem Item;
        Item.label = Dir;
        Item.kind = CompletionItemKind::Keyword;
        Item.filterText = Dir;
        Item.insertText = Dir;
        Item.sortText = "0_" + std::string(Dir);
        List.items.push_back(std::move(Item));
      }
    }
    return List;
  }

  // 2. Analyze line prefix context
  size_t Idx = LinePrefix.size();
  while (Idx > 0 &&
         (llvm::isAlnum(LinePrefix[Idx - 1]) || LinePrefix[Idx - 1] == '_'))
    --Idx;
  llvm::StringRef PartialWord = LinePrefix.substr(Idx);
  llvm::StringRef Pre = LinePrefix.take_front(Idx).rtrim();

  bool IsMemberAccess = false;
  bool IsScopeResolution = false;
  size_t OpOffset = LinePrefix.size();
  if (Pre.ends_with(".")) {
    OpOffset = Pre.size() - 1;
    Pre = Pre.drop_back(1).rtrim();
    IsMemberAccess = true;
  } else if (Pre.ends_with("->")) {
    OpOffset = Pre.size() - 2;
    Pre = Pre.drop_back(2).rtrim();
    IsMemberAccess = true;
  } else if (Pre.ends_with("::")) {
    OpOffset = Pre.size() - 2;
    Pre = Pre.drop_back(2).rtrim();
    IsScopeResolution = true;
  }

  // Sanitize trailing incomplete operator so GLR parser parses the enclosing block
  std::string CodeToParse = Code.str();
  if (IsMemberAccess || IsScopeResolution) {
    size_t AbsOpOffset = *LineStart + OpOffset;
    if (AbsOpOffset < CodeToParse.size()) {
      size_t ParenCount = 0;
      for (size_t I = 0; I < AbsOpOffset; ++I) {
        if (CodeToParse[I] == '(' || CodeToParse[I] == '[')
          ++ParenCount;
        else if (CodeToParse[I] == ')' || CodeToParse[I] == ']') {
          if (ParenCount > 0)
            --ParenCount;
        }
      }
      if (ParenCount > 0) {
        for (size_t I = AbsOpOffset;
             I < *CursorOffset && I < CodeToParse.size(); ++I)
          CodeToParse[I] = ' ';
      } else {
        CodeToParse[AbsOpOffset] = ';';
        for (size_t I = AbsOpOffset + 1;
             I < *CursorOffset && I < CodeToParse.size(); ++I)
          CodeToParse[I] = ' ';
      }
    }
  }

  // 3. Parse code and build scopes
  auto Parsed = parseCode(CodeToParse);
  if (!Parsed || !Parsed->Root) {
    Parsed = parseCode(Code);
    if (!Parsed || !Parsed->Root) {
      std::string SemicolonCode = Code.str();
      if (*CursorOffset <= SemicolonCode.size()) {
        SemicolonCode.insert(*CursorOffset, ";");
        Parsed = parseCode(SemicolonCode);
      }
    }
  }

  std::vector<LexicalScope> Scopes;
  Scopes.emplace_back();
  Scopes.back().Id = 0;
  Scopes.back().StartOffset = 0;
  Scopes.back().EndOffset = CodeToParse.size();
  Scopes.back().ScopeRange =
      Range{Position{0, 0}, offsetToPosition(CodeToParse, CodeToParse.size())};

  if (Parsed && Parsed->Root) {
    size_t CurScope = 0;
    pseudo::Token::Index NumTokens = Parsed->ParseableStream.tokens().size();
    buildScopes(Parsed->Root, NumTokens, *Parsed, CodeToParse, Scopes, CurScope);
  }

  size_t BestScope = 0;
  size_t BestLen = std::numeric_limits<size_t>::max();
  for (const auto &S : Scopes) {
    if (*CursorOffset >= S.StartOffset && *CursorOffset <= S.EndOffset) {
      size_t Len = S.EndOffset - S.StartOffset;
      if (Len < BestLen) {
        BestLen = Len;
        BestScope = S.Id;
      }
    }
  }

  std::string EffectiveEnclosingClass;
  size_t SCheck = BestScope;
  while (true) {
    if (!Scopes[SCheck].EnclosingClass.empty()) {
      EffectiveEnclosingClass = Scopes[SCheck].EnclosingClass;
      break;
    }
    if (SCheck == 0)
      break;
    SCheck = Scopes[SCheck].ParentId;
  }

  CompletionList List;
  llvm::StringSet<> SeenLabels;

  if (IsMemberAccess) {
    std::string TypeName = resolveExprType(
        *this, Pre, *CursorOffset, BestScope, Scopes, Code, File, getFS().get(),
        EffectiveEnclosingClass);

    std::string CleanType = unwrapType(TypeName);

    // 1. Members from current file scopes
    if (!CleanType.empty()) {
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.EnclosingClass == CleanType || S.Name == CleanType) {
            if (PartialWord.empty() ||
                llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
              if (SeenLabels.insert(D.Name).second) {
                CompletionItem Item;
                Item.label = D.Name;
                Item.kind = (D.Kind == DeclKind::Function ||
                             D.Kind == DeclKind::Constructor)
                                ? CompletionItemKind::Method
                                : CompletionItemKind::Field;
                Item.detail = D.TypeName;
                Item.sortText = "0_" + D.Name;
                Item.filterText = D.Name;
                Item.insertText = D.Name;
                List.items.push_back(std::move(Item));
              }
            }
          }
        }
      }

      // 2. Members from included headers (direct and transitive)
      auto FS = getFS();
      if (FS) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.EnclosingClass == CleanType ||
                    D.EnclosingScope == CleanType) {
                  if (PartialWord.empty() ||
                      llvm::StringRef(D.Name).starts_with_insensitive(
                          PartialWord)) {
                    if (SeenLabels.insert(D.Name).second) {
                      CompletionItem Item;
                      Item.label = D.Name;
                      Item.kind = (D.Kind == DeclKind::Function ||
                                   D.Kind == DeclKind::Constructor)
                                      ? CompletionItemKind::Method
                                      : CompletionItemKind::Field;
                      Item.detail = D.TypeName;
                      Item.sortText = "0_" + D.Name;
                      Item.filterText = D.Name;
                      Item.insertText = D.Name;
                      List.items.push_back(std::move(Item));
                    }
                  }
                }
              }
              return false;
            });
      }
    }

    // 3. Known methods for common standard/llvm types
    if (CleanType == "entry" || TypeName.find("Registry") != std::string::npos) {
      static const char *EntryMethods[] = {"getName", "getDesc", "instantiate"};
      for (const char *M : EntryMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (CleanType == "StringRef" || CleanType == "string" ||
        CleanType == "string_view" || CleanType == "basic_string" ||
        CleanType == "basic_string_view" ||
        TypeName.find("StringRef") != std::string::npos ||
        TypeName.find("string") != std::string::npos) {
      static const char *StrMethods[] = {
          "data",         "size",         "length",       "empty",
          "str",          "bytes",        "begin",        "end",
          "front",        "back",         "substr",       "slice",
          "starts_with",  "ends_with",    "contains",     "find",
          "rfind",        "count",        "split",        "trim",
          "ltrim",        "rtrim",        "take_front",   "take_back",
          "drop_front",   "drop_back",    "consume_front","consume_back",
          "equals",       "compare",      "c_str",        "clear"};
      for (const char *M : StrMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (CleanType == "pair" || TypeName.find("pair") != std::string::npos) {
      static const char *PairMembers[] = {"first", "second"};
      for (const char *M : PairMembers) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Field;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("vector") != std::string::npos ||
        CleanType == "vector") {
      static const char *VecMethods[] = {
          "push_back", "emplace_back", "pop_back", "size",  "empty",
          "clear",     "begin",        "end",      "front", "back",
          "data",      "resize",       "reserve"};
      for (const char *M : VecMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("DenseMap") != std::string::npos ||
        TypeName.find("map") != std::string::npos || CleanType == "DenseMap") {
      static const char *MapMethods[] = {
          "try_emplace", "insert", "find",  "lookup", "count",
          "empty",       "size",   "clear", "begin",  "end", "erase"};
      for (const char *M : MapMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("optional") != std::string::npos) {
      static const char *OptMethods[] = {"emplace", "has_value", "value", "reset"};
      for (const char *M : OptMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
    if (TypeName.find("unique_ptr") != std::string::npos ||
        TypeName.find("shared_ptr") != std::string::npos) {
      static const char *PtrMethods[] = {"get", "reset", "release"};
      for (const char *M : PtrMethods) {
        if (PartialWord.empty() ||
            llvm::StringRef(M).starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(M).second) {
            CompletionItem Item;
            Item.label = M;
            Item.kind = CompletionItemKind::Method;
            Item.sortText = "0_" + std::string(M);
            Item.filterText = M;
            Item.insertText = M;
            List.items.push_back(std::move(Item));
          }
        }
      }
    }

    // 4. Fallback if still empty (e.g. unknown auto type on M->)
    if (List.items.empty()) {
      auto FS = getFS();
      if (FS) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (!D.EnclosingClass.empty() &&
                    (D.Kind == DeclKind::Function ||
                     D.Kind == DeclKind::Constructor ||
                     D.Kind == DeclKind::Variable)) {
                  if (PartialWord.empty() ||
                      llvm::StringRef(D.Name).starts_with_insensitive(
                          PartialWord)) {
                    if (SeenLabels.insert(D.Name).second) {
                      CompletionItem Item;
                      Item.label = D.Name;
                      Item.kind = (D.Kind == DeclKind::Function ||
                                   D.Kind == DeclKind::Constructor)
                                      ? CompletionItemKind::Method
                                      : CompletionItemKind::Field;
                      Item.detail = D.TypeName;
                      Item.sortText = "0_" + D.Name;
                      Item.filterText = D.Name;
                      Item.insertText = D.Name;
                      List.items.push_back(std::move(Item));
                    }
                  }
                }
              }
              return false;
            });
      }
    }
    return List;
  }

  if (IsScopeResolution) {
    size_t QEnd = Pre.size();
    size_t QStart = QEnd;
    while (QStart > 0 &&
           (llvm::isAlnum(Pre[QStart - 1]) || Pre[QStart - 1] == '_'))
      --QStart;
    llvm::StringRef Qualifier = Pre.slice(QStart, QEnd);
    if (!Qualifier.empty()) {
      // 1. Current file scopes
      for (const auto &S : Scopes) {
        for (const auto &D : S.Decls) {
          if (D.EnclosingClass == Qualifier ||
              (S.Kind == ScopeKind::Namespace && S.Name == Qualifier)) {
            if (PartialWord.empty() ||
                llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
              if (SeenLabels.insert(D.Name).second) {
                CompletionItem Item;
                Item.label = D.Name;
                if (D.Kind == DeclKind::Function ||
                    D.Kind == DeclKind::Constructor)
                  Item.kind = CompletionItemKind::Method;
                else if (D.Kind == DeclKind::Class)
                  Item.kind = CompletionItemKind::Class;
                else if (D.Kind == DeclKind::EnumValue)
                  Item.kind = CompletionItemKind::EnumMember;
                else if (D.Kind == DeclKind::Enum)
                  Item.kind = CompletionItemKind::Enum;
                else
                  Item.kind = CompletionItemKind::Field;
                Item.detail = D.TypeName;
                Item.sortText = "0_" + D.Name;
                Item.filterText = D.Name;
                Item.insertText = D.Name;
                List.items.push_back(std::move(Item));
              }
            }
          }
        }
      }

      if (Qualifier == "std") {
        struct StdCompItem {
          const char *Name;
          CompletionItemKind Kind;
          const char *Detail;
        };
        static const StdCompItem StdItems[] = {
            {"move", CompletionItemKind::Function,
             "template <typename T> constexpr remove_reference_t<T>&& move(T&& t) noexcept"},
            {"forward", CompletionItemKind::Function,
             "template <typename T> constexpr T&& forward(remove_reference_t<T>& t) noexcept"},
            {"make_unique", CompletionItemKind::Function,
             "template <typename T, typename... Args> unique_ptr<T> make_unique(Args&&... args)"},
            {"make_shared", CompletionItemKind::Function,
             "template <typename T, typename... Args> shared_ptr<T> make_shared(Args&&... args)"},
            {"make_pair", CompletionItemKind::Function,
             "template <typename T1, typename T2> constexpr pair<T1, T2> make_pair(T1&& t1, T2&& t2)"},
            {"make_tuple", CompletionItemKind::Function,
             "template <typename... Args> constexpr tuple<Args...> make_tuple(Args&&... args)"},
            {"swap", CompletionItemKind::Function,
             "template <typename T> void swap(T& a, T& b) noexcept"},
            {"to_string", CompletionItemKind::Function,
             "string to_string(int val)"},
            {"min", CompletionItemKind::Function,
             "template <typename T> constexpr const T& min(const T& a, const T& b)"},
            {"max", CompletionItemKind::Function,
             "template <typename T> constexpr const T& max(const T& a, const T& b)"},
            {"clamp", CompletionItemKind::Function,
             "template <typename T> constexpr const T& clamp(const T& v, const T& lo, const T& hi)"},
            {"find", CompletionItemKind::Function,
             "template <typename InputIt, typename T> InputIt find(InputIt first, InputIt last, const T& value)"},
            {"sort", CompletionItemKind::Function,
             "template <typename RandomIt> void sort(RandomIt first, RandomIt last)"},
            {"unique_ptr", CompletionItemKind::Class,
             "template <typename T, typename Deleter = default_delete<T>> class unique_ptr"},
            {"shared_ptr", CompletionItemKind::Class,
             "template <typename T> class shared_ptr"},
            {"weak_ptr", CompletionItemKind::Class,
             "template <typename T> class weak_ptr"},
            {"vector", CompletionItemKind::Class,
             "template <typename T, typename Allocator = allocator<T>> class vector"},
            {"string", CompletionItemKind::Class,
             "using string = basic_string<char>"},
            {"string_view", CompletionItemKind::Class,
             "using string_view = basic_string_view<char>"},
            {"pair", CompletionItemKind::Class,
             "template <typename T1, typename T2> struct pair"},
            {"tuple", CompletionItemKind::Class,
             "template <typename... Types> class tuple"},
            {"optional", CompletionItemKind::Class,
             "template <typename T> class optional"},
            {"variant", CompletionItemKind::Class,
             "template <typename... Types> class variant"},
            {"any", CompletionItemKind::Class,
             "class any"},
            {"function", CompletionItemKind::Class,
             "template <typename> class function"},
            {"map", CompletionItemKind::Class,
             "template <typename Key, typename T, typename Compare = less<Key>> class map"},
            {"unordered_map", CompletionItemKind::Class,
             "template <typename Key, typename T, typename Hash = hash<Key>> class unordered_map"},
            {"set", CompletionItemKind::Class,
             "template <typename Key, typename Compare = less<Key>> class set"},
            {"unordered_set", CompletionItemKind::Class,
             "template <typename Key, typename Hash = hash<Key>> class unordered_set"},
            {"array", CompletionItemKind::Class,
             "template <typename T, size_t N> struct array"},
            {"deque", CompletionItemKind::Class,
             "template <typename T> class deque"},
            {"list", CompletionItemKind::Class,
             "template <typename T> class list"},
            {"queue", CompletionItemKind::Class,
             "template <typename T> class queue"},
            {"stack", CompletionItemKind::Class,
             "template <typename T> class stack"},
            {"span", CompletionItemKind::Class,
             "template <typename T, size_t Extent = dynamic_extent> class span"},
            {"bitset", CompletionItemKind::Class,
             "template <size_t N> class bitset"},
            {"size_t", CompletionItemKind::Interface,
             "using size_t = unsigned long"},
            {"ptrdiff_t", CompletionItemKind::Interface,
             "using ptrdiff_t = long"},
            {"nullptr_t", CompletionItemKind::Interface,
             "using nullptr_t = decltype(nullptr)"},
            {"nullopt", CompletionItemKind::Constant,
             "constexpr nullopt_t nullopt"},
            {"nullopt_t", CompletionItemKind::Class,
             "struct nullopt_t"},
            {"initializer_list", CompletionItemKind::Class,
             "template <typename T> class initializer_list"},
            {"atomic", CompletionItemKind::Class,
             "template <typename T> struct atomic"},
            {"mutex", CompletionItemKind::Class,
             "class mutex"},
            {"lock_guard", CompletionItemKind::Class,
             "template <typename Mutex> class lock_guard"},
            {"unique_lock", CompletionItemKind::Class,
             "template <typename Mutex> class unique_lock"},
            {"thread", CompletionItemKind::Class,
             "class thread"},
        };
        for (const auto &SI : StdItems) {
          if (PartialWord.empty() ||
              llvm::StringRef(SI.Name).starts_with_insensitive(PartialWord)) {
            if (SeenLabels.insert(SI.Name).second) {
              CompletionItem Item;
              Item.label = SI.Name;
              Item.kind = SI.Kind;
              Item.detail = SI.Detail;
              Item.sortText = "0_" + std::string(SI.Name);
              Item.filterText = SI.Name;
              Item.insertText = SI.Name;
              List.items.push_back(std::move(Item));
            }
          }
        }
      }

      // 2. Included headers
      auto FS = getFS();
      if (FS) {
        traverseIncludedHeaders(
            *this, File, Code, *FS,
            [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
              for (const auto &D : Info.Decls) {
                if (D.EnclosingClass == Qualifier ||
                    D.EnclosingScope == Qualifier ||
                    (Qualifier == "std" && (D.EnclosingScope == "__1" ||
                                            D.EnclosingScope == "std"))) {
                  if (PartialWord.empty() ||
                      llvm::StringRef(D.Name).starts_with_insensitive(
                          PartialWord)) {
                    if (SeenLabels.insert(D.Name).second) {
                      CompletionItem Item;
                      Item.label = D.Name;
                      if (D.Kind == DeclKind::Function ||
                          D.Kind == DeclKind::Constructor)
                        Item.kind = CompletionItemKind::Method;
                      else if (D.Kind == DeclKind::Class)
                        Item.kind = CompletionItemKind::Class;
                      else if (D.Kind == DeclKind::EnumValue)
                        Item.kind = CompletionItemKind::EnumMember;
                      else if (D.Kind == DeclKind::Enum)
                        Item.kind = CompletionItemKind::Enum;
                      else
                        Item.kind = CompletionItemKind::Field;
                      Item.detail = D.TypeName;
                      Item.sortText = "0_" + D.Name;
                      Item.filterText = D.Name;
                      Item.insertText = D.Name;
                      List.items.push_back(std::move(Item));
                    }
                  }
                }
              }
              return false;
            },
            /*MaxHeaders=*/60, /*MaxDepth=*/3);
      }

      // 3. Known registry methods if qualifier contains Registry
      if (Qualifier.contains("Registry")) {
        static const char *RegItems[] = {"entry", "entries", "add"};
        for (const char *R : RegItems) {
          if (PartialWord.empty() ||
              llvm::StringRef(R).starts_with_insensitive(PartialWord)) {
            if (SeenLabels.insert(R).second) {
              CompletionItem Item;
              Item.label = R;
              Item.kind = (llvm::StringRef(R) == "entry")
                              ? CompletionItemKind::Class
                              : CompletionItemKind::Method;
              Item.sortText = "0_" + std::string(R);
              Item.filterText = R;
              Item.insertText = R;
              List.items.push_back(std::move(Item));
            }
          }
        }
      }

      return List;
    }
  }

  // General scope completion
  // 1. Current file scopes
  size_t Cur = BestScope;
  while (true) {
    const auto &S = Scopes[Cur];
    for (const auto &D : S.Decls) {
      if (PartialWord.empty() ||
          llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
        if (SeenLabels.insert(D.Name).second) {
          CompletionItem Item;
          Item.label = D.Name;
          std::string PriorityPrefix;
          if (D.Kind == DeclKind::Parameter || D.Kind == DeclKind::Variable) {
            Item.kind = CompletionItemKind::Variable;
            PriorityPrefix = (Cur == BestScope) ? "0_" : "1_";
          } else if (D.Kind == DeclKind::Function ||
                     D.Kind == DeclKind::Constructor) {
            Item.kind = D.IsMember ? CompletionItemKind::Method
                                   : CompletionItemKind::Function;
            PriorityPrefix = "2_";
          } else if (D.Kind == DeclKind::Class) {
            Item.kind = CompletionItemKind::Class;
            PriorityPrefix = "2_";
          } else if (D.Kind == DeclKind::Enum ||
                     D.Kind == DeclKind::EnumValue) {
            Item.kind = (D.Kind == DeclKind::Enum)
                            ? CompletionItemKind::Enum
                            : CompletionItemKind::EnumMember;
            PriorityPrefix = "2_";
          } else {
            Item.kind = CompletionItemKind::Text;
            PriorityPrefix = "2_";
          }
          Item.detail = D.TypeName;
          Item.sortText = PriorityPrefix + D.Name;
          Item.filterText = D.Name;
          Item.insertText = D.Name;
          List.items.push_back(std::move(Item));
        }
      }
    }
    if (Cur == 0)
      break;
    Cur = S.ParentId;
  }

  // 2. Members of enclosing class (if inside a member function of EnclosingClass)
  if (!EffectiveEnclosingClass.empty()) {
    llvm::StringRef EnclosingClass = EffectiveEnclosingClass;
    for (const auto &S : Scopes) {
      for (const auto &D : S.Decls) {
        if (D.EnclosingClass == EnclosingClass) {
          if (PartialWord.empty() ||
              llvm::StringRef(D.Name).starts_with_insensitive(PartialWord)) {
            if (SeenLabels.insert(D.Name).second) {
              CompletionItem Item;
              Item.label = D.Name;
              Item.kind = (D.Kind == DeclKind::Function ||
                           D.Kind == DeclKind::Constructor)
                              ? CompletionItemKind::Method
                              : CompletionItemKind::Field;
              Item.detail = D.TypeName;
              Item.sortText = "1_" + D.Name;
              Item.filterText = D.Name;
              Item.insertText = D.Name;
              List.items.push_back(std::move(Item));
            }
          }
        }
      }
    }
  }

  // 3. Known namespaces and declarations from included headers
  for (llvm::StringRef NS : {"std", "llvm", "clang", "clangd"}) {
    if (PartialWord.empty() || NS.starts_with_insensitive(PartialWord)) {
      if (SeenLabels.insert(NS).second) {
        CompletionItem Item;
        Item.label = NS.str();
        Item.kind = CompletionItemKind::Module;
        Item.detail = "namespace " + NS.str();
        Item.sortText = "0_" + NS.str();
        Item.filterText = NS.str();
        Item.insertText = NS.str();
        List.items.push_back(std::move(Item));
      }
    }
  }

  auto FS = getFS();
  if (FS) {
    traverseIncludedHeaders(
        *this, File, Code, *FS,
        [&](const HeaderInfo &Info, llvm::StringRef HeaderPath) {
          for (const auto &D : Info.Decls) {
            bool IsEnclosingMember =
                !EffectiveEnclosingClass.empty() &&
                (D.EnclosingClass == EffectiveEnclosingClass ||
                 D.EnclosingScope == EffectiveEnclosingClass);
            bool IsTopLevelOrType =
                D.EnclosingClass.empty() || D.Kind == DeclKind::Class ||
                D.Kind == DeclKind::Enum || D.Kind == DeclKind::TypeAlias;
            if (IsEnclosingMember || IsTopLevelOrType) {
              if (PartialWord.empty() ||
                  llvm::StringRef(D.Name).starts_with_insensitive(
                      PartialWord)) {
                if (SeenLabels.insert(D.Name).second) {
                  CompletionItem Item;
                  Item.label = D.Name;
                  if (D.Kind == DeclKind::Class)
                    Item.kind = CompletionItemKind::Class;
                  else if (D.Kind == DeclKind::Enum)
                    Item.kind = CompletionItemKind::Enum;
                  else if (D.Kind == DeclKind::EnumValue)
                    Item.kind = CompletionItemKind::EnumMember;
                  else if (D.Kind == DeclKind::TypeAlias)
                    Item.kind = CompletionItemKind::Interface;
                  else if (D.Kind == DeclKind::Function ||
                           D.Kind == DeclKind::Constructor)
                    Item.kind = IsEnclosingMember
                                    ? CompletionItemKind::Method
                                    : CompletionItemKind::Function;
                  else
                    Item.kind = IsEnclosingMember
                                    ? CompletionItemKind::Field
                                    : CompletionItemKind::Variable;
                  Item.detail = D.TypeName;
                  Item.sortText = IsEnclosingMember ? ("1_" + D.Name)
                                                   : ("2_" + D.Name);
                  Item.filterText = D.Name;
                  Item.insertText = D.Name;
                  List.items.push_back(std::move(Item));
                }
              }
            }
          }
          return false;
        },
        /*MaxHeaders=*/60, /*MaxDepth=*/3);
  }

  // 4. Identifiers from the document token stream (document word fallback)
  for (const auto &Tok : Parsed->ParseableStream.tokens()) {
    if (Tok.Kind == tok::raw_identifier || Tok.Kind == tok::identifier) {
      llvm::StringRef Word = getOrigToken(Tok, *Parsed).text();
      if (Word.size() >= 2) {
        if (PartialWord.empty() || Word.starts_with_insensitive(PartialWord)) {
          if (SeenLabels.insert(Word).second) {
            CompletionItem Item;
            Item.label = Word.str();
            Item.kind = CompletionItemKind::Text;
            Item.sortText = "3_" + Word.str();
            Item.filterText = Word.str();
            Item.insertText = Word.str();
            List.items.push_back(std::move(Item));
          }
        }
      }
    }
  }

  // 5. C++ Keywords
  static const char *Keywords[] = {
      "alignas", "alignof", "auto", "bool", "break", "case", "catch",
      "char", "class", "concept", "const", "constexpr", "const_cast",
      "continue", "decltype", "default", "delete", "do", "double",
      "dynamic_cast", "else", "enum", "explicit", "export", "extern",
      "false", "final", "float", "for", "friend", "goto", "if",
      "inline", "int", "long", "mutable", "namespace", "new",
      "noexcept", "nullptr", "operator", "override", "private",
      "protected", "public", "reinterpret_cast", "requires", "return",
      "short", "signed", "sizeof", "static", "static_cast", "struct",
      "switch", "template", "this", "throw", "true", "try",
      "typedef", "typeid", "typename", "union", "unsigned", "using",
      "virtual", "void", "volatile", "while"};

  for (const char *Kw : Keywords) {
    if (PartialWord.empty() ||
        llvm::StringRef(Kw).starts_with_insensitive(PartialWord)) {
      if (SeenLabels.insert(Kw).second) {
        CompletionItem Item;
        Item.label = Kw;
        Item.kind = CompletionItemKind::Keyword;
        Item.sortText = "4_" + std::string(Kw);
        Item.filterText = Kw;
        Item.insertText = Kw;
        List.items.push_back(std::move(Item));
      }
    }
  }

  llvm::sort(List.items, [](const CompletionItem &A, const CompletionItem &B) {
    return A.sortText < B.sortText;
  });
  return List;
}

void PseudoModule::onHover(
    const TextDocumentPositionParams &Params,
    Callback<std::optional<Hover>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoHover", File,
        [this, File = File.str(), Params, Reply = std::move(Reply)]() mutable {
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::nullopt);
          auto H = getHover(File, Code, Params.position);
          if (!H)
            return Reply(H.takeError());
          if (*H && HoverContentFormat == MarkupKind::PlainText) {
            (*H)->contents.kind = MarkupKind::PlainText;
            llvm::StringRef Val = (*H)->contents.value;
            if (Val.starts_with("```cpp\n") && Val.ends_with("\n```"))
              (*H)->contents.value = Val.drop_front(7).drop_back(4).str();
          }
          return Reply(std::move(*H));
        });
    return;
  }

  server().findHover(
      File, Params.position,
      [this, File = File.str(), Params, Reply = std::move(Reply)](
          llvm::Expected<std::optional<HoverInfo>> H) mutable {
        if (H && *H) {
          Hover R;
          R.contents.kind = HoverContentFormat;
          R.range = (*H)->SymRange;
          switch (HoverContentFormat) {
          case MarkupKind::Markdown:
          case MarkupKind::PlainText:
            R.contents.value = (*H)->present(HoverContentFormat);
            return Reply(std::move(R));
          }
          llvm_unreachable("unhandled MarkupKind");
        }
        if (isEnabled()) {
          std::string Code = getDocument(File);
          if (!Code.empty()) {
            auto HoverResult = getHover(File, Code, Params.position);
            if (HoverResult && *HoverResult) {
              vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for hover",
                   File, H ? llvm::Error::success() : H.takeError());
              if (HoverContentFormat == MarkupKind::PlainText) {
                (*HoverResult)->contents.kind = MarkupKind::PlainText;
                llvm::StringRef Val = (*HoverResult)->contents.value;
                if (Val.starts_with("```cpp\n") && Val.ends_with("\n```"))
                  (*HoverResult)->contents.value =
                      Val.drop_front(7).drop_back(4).str();
              }
              return Reply(std::move(*HoverResult));
            }
            if (HoverResult)
              consumeError(HoverResult.takeError());
          }
        }
        if (!H)
          return Reply(H.takeError());
        return Reply(std::nullopt);
      });
}

void PseudoModule::onSemanticTokens(
    const SemanticTokensParams &Params,
    Callback<SemanticTokens> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoSemanticTokens", File,
        [this, File = File.str(), Reply = std::move(Reply)]() mutable {
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(SemanticTokens{});
          auto Toks = getSemanticTokens(Code);
          if (!Toks)
            return Reply(Toks.takeError());
          {
            std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
            auto &Last = LastSemanticTokens[File];
            Last.tokens = Toks->tokens;
            increment(Last.resultId);
            Toks->resultId = Last.resultId;
          }
          return Reply(std::move(*Toks));
        });
    return;
  }

  server().semanticHighlights(
      File,
      [this, File = File.str(), Reply = std::move(Reply),
       Code = getDocument(File)](
          llvm::Expected<std::vector<HighlightingToken>> HT) mutable {
        if (!HT) {
          if (isEnabled() && !Code.empty()) {
            auto Toks = getSemanticTokens(Code);
            if (Toks) {
              vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for semantic tokens",
                   File, HT.takeError());
              {
                std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
                auto &Last = LastSemanticTokens[File];
                Last.tokens = Toks->tokens;
                increment(Last.resultId);
                Toks->resultId = Last.resultId;
              }
              return Reply(std::move(*Toks));
            }
            consumeError(Toks.takeError());
          }
          return Reply(HT.takeError());
        }
        SemanticTokens Result;
        Result.tokens = toSemanticTokens(*HT, Code);
        {
          std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
          auto &Last = LastSemanticTokens[File];
          Last.tokens = Result.tokens;
          increment(Last.resultId);
          Result.resultId = Last.resultId;
        }
        Reply(std::move(Result));
      });
}

void PseudoModule::onSemanticTokensDelta(
    const SemanticTokensDeltaParams &Params,
    Callback<SemanticTokensOrDelta> Reply) {
  PathRef File = Params.textDocument.uri.file();
  auto PrevResultID = Params.previousResultId;
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoSemanticTokensDelta", File,
        [this, File = File.str(), PrevResultID,
         Reply = std::move(Reply)]() mutable {
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(SemanticTokensOrDelta{});
          auto Toks = getSemanticTokens(Code);
          if (!Toks)
            return Reply(Toks.takeError());
          SemanticTokensOrDelta Result;
          {
            std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
            auto &Last = LastSemanticTokens[File];
            if (PrevResultID == Last.resultId) {
              Result.edits = diffTokens(Last.tokens, Toks->tokens);
            } else {
              Result.tokens = Toks->tokens;
            }
            Last.tokens = std::move(Toks->tokens);
            increment(Last.resultId);
            Result.resultId = Last.resultId;
          }
          return Reply(std::move(Result));
        });
    return;
  }

  server().semanticHighlights(
      File,
      [this, File = File.str(), PrevResultID, Reply = std::move(Reply),
       Code = getDocument(File)](
          llvm::Expected<std::vector<HighlightingToken>> HT) mutable {
        if (!HT) {
          if (isEnabled() && !Code.empty()) {
            auto Toks = getSemanticTokens(Code);
            if (Toks) {
              vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for semantic tokens delta",
                   File, HT.takeError());
              SemanticTokensOrDelta Result;
              {
                std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
                auto &Last = LastSemanticTokens[File];
                if (PrevResultID == Last.resultId) {
                  Result.edits = diffTokens(Last.tokens, Toks->tokens);
                } else {
                  Result.tokens = Toks->tokens;
                }
                Last.tokens = std::move(Toks->tokens);
                increment(Last.resultId);
                Result.resultId = Last.resultId;
              }
              return Reply(std::move(Result));
            }
            consumeError(Toks.takeError());
          }
          return Reply(HT.takeError());
        }
        std::vector<SemanticToken> Toks = toSemanticTokens(*HT, Code);
        SemanticTokensOrDelta Result;
        {
          std::lock_guard<std::mutex> Lock(SemanticTokensMutex);
          auto &Last = LastSemanticTokens[File];
          if (PrevResultID == Last.resultId) {
            Result.edits = diffTokens(Last.tokens, Toks);
          } else {
            Result.tokens = Toks;
          }
          Last.tokens = std::move(Toks);
          increment(Last.resultId);
          Result.resultId = Last.resultId;
        }
        Reply(std::move(Result));
      });
}

bool PseudoModule::shouldRunCompletion(const CompletionParams &Params) const {
  if (Params.context.triggerKind != CompletionTriggerKind::TriggerCharacter)
    return true;
  std::string Code = const_cast<PseudoModule *>(this)->getDocument(
      Params.textDocument.uri.file());
  if (Code.empty())
    return true;
  auto Offset = positionToOffset(Code, Params.position,
                                 /*AllowColumnsBeyondLineLength=*/false);
  if (!Offset) {
    elog("could not convert position '{0}' to offset for file '{1}': {2}",
         Params.position, Params.textDocument.uri.file(), Offset.takeError());
    return false;
  }
  return allowImplicitCompletion(Code, *Offset);
}

void PseudoModule::onCompletion(
    const CompletionParams &Params,
    Callback<CompletionList> Reply) {
  if (!shouldRunCompletion(Params)) {
    vlog("ignored auto-triggered completion, preceding char did not match");
    return Reply(CompletionList());
  }

  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoCompletion", File,
        [this, File = File.str(), Params, Reply = std::move(Reply)]() mutable {
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(CompletionList{});
          auto CompList = getCompletions(File, Code, Params.position);
          if (!CompList)
            return Reply(CompList.takeError());
          for (auto &C : CompList->items) {
            C.kind = adjustKindToCapability(C.kind, SupportedCompletionItemKinds);
            if (!SupportsCompletionLabelDetails)
              removeCompletionLabelDetails(C);
          }
          return Reply(std::move(*CompList));
        });
    return;
  }

  auto Opts = BaseCodeCompleteOpts;
  if (Params.limit && *Params.limit >= 0)
    Opts.Limit = *Params.limit;
  server().codeComplete(
      File, Params.position, Opts,
      [this, File = File.str(), Params, Opts, Reply = std::move(Reply)](
          llvm::Expected<CodeCompleteResult> List) mutable {
        if (!List || !List->RanParser) {
          if (isEnabled()) {
            std::string Code = getDocument(File);
            if (!Code.empty()) {
              auto CompList = getCompletions(File, Code, Params.position);
              if (CompList) {
                if (!List)
                  vlog("AST unavailable for {0} ({1}), falling back to pseudo-parser for completions",
                       File, List.takeError());
                else
                  vlog("AST completion fallback mode for {0}, falling back to pseudo-parser for completions", File);
                for (auto &C : CompList->items) {
                  C.kind = adjustKindToCapability(C.kind, SupportedCompletionItemKinds);
                  if (!SupportsCompletionLabelDetails)
                    removeCompletionLabelDetails(C);
                }
                return Reply(std::move(*CompList));
              }
              consumeError(CompList.takeError());
            }
          }
          if (!List)
            return Reply(List.takeError());
        }
        CompletionList LSPList;
        LSPList.isIncomplete = List->HasMore;
        for (const auto &R : List->Completions) {
          CompletionItem C = R.render(Opts);
          C.kind = adjustKindToCapability(C.kind, SupportedCompletionItemKinds);
          if (!SupportsCompletionLabelDetails)
            removeCompletionLabelDetails(C);
          LSPList.items.push_back(std::move(C));
        }
        return Reply(std::move(LSPList));
      });
}

void PseudoModule::onCustomPseudoSymbols(
    const DocumentSymbolParams &Params,
    Callback<std::vector<DocumentSymbol>> Reply) {
  std::string Path = Params.textDocument.uri.file().str();
  auto FS = fs().view(std::nullopt);
  auto Buf = FS->getBufferForFile(Path);
  if (!Buf)
    return Reply(error(llvm::errc::no_such_file_or_directory,
                       "Failed to read file for pseudo parsing: " + Path));
  Reply(getDocumentSymbols((*Buf)->getBuffer()));
}

void PseudoModule::onAST(const ASTParams &Params,
                         Callback<std::optional<ASTNode>> Reply) {
  PathRef File = Params.textDocument.uri.file();
  if (isPseudoOnly()) {
    scheduler().run(
        "PseudoAST", File,
        [this, File = File.str(), Params, Reply = std::move(Reply)]() mutable {
          std::string Code = getDocument(File);
          if (Code.empty())
            return Reply(std::nullopt);
          auto Node = getAST(File, Code, Params.range);
          if (!Node)
            return Reply(Node.takeError());
          return Reply(std::move(*Node));
        });
    return;
  }

  server().getAST(
      File, Params.range,
      [this, File = File.str(), Params, Reply = std::move(Reply)](
          llvm::Expected<std::optional<ASTNode>> Res) mutable {
        if (Res && *Res)
          return Reply(std::move(*Res));
        if (isEnabled()) {
          std::string Code = getDocument(File);
          if (!Code.empty()) {
            auto PseudoRes = getAST(File, Code, Params.range);
            if (PseudoRes && *PseudoRes) {
              vlog("AST unavailable for {0}, falling back to pseudo-parser for AST",
                   File);
              return Reply(std::move(*PseudoRes));
            }
            if (!PseudoRes)
              consumeError(PseudoRes.takeError());
          }
        }
        if (!Res)
          return Reply(Res.takeError());
        return Reply(std::nullopt);
      });
}

volatile int PseudoModuleAnchorSource = 0;

FeatureModuleRegistry::Add<PseudoModule>
    X("PseudoParser", "C++ pseudo-parser fallback module for clangd");

} // namespace clangd
} // namespace clang
