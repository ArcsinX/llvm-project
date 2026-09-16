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
    S.EnclosingClass =
        !FuncEnclosingClass.empty() ? FuncEnclosingClass : std::string(EnclosingClass);
    Scopes[CurrentScopeId].Children.push_back(NewScopeId);
    Scopes.push_back(std::move(S));

    size_t SavedScope = CurrentScopeId;
    CurrentScopeId = NewScopeId;
    auto Children = N->elements();
    for (size_t I = 0; I < Children.size(); ++I) {
      pseudo::Token::Index ChildEnd =
          (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
      buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                  S.EnclosingClass);
    }
    CurrentScopeId = SavedScope;
    return;
  }

  // 4. Parameter declaration
  if (Sym == pseudo::cxx::Symbol::parameter_declaration) {
    const pseudo::Token *NameTok = nullptr;
    const pseudo::Token *TypeTok = nullptr;
    bool HasExplicitType = false;
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::equal || NodeTokens[I].Kind == tok::comma ||
          NodeTokens[I].Kind == tok::r_paren)
        break;
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
        if (NameTok)
          TypeTok = NameTok;
        NameTok = &NodeTokens[I];
      }
    }
    bool HasTypeName = (TypeTok != nullptr) || HasExplicitType;
    if (NameTok && HasTypeName) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
      if (TypeTok)
        LD.TypeName = getOrigToken(*TypeTok, Out).text().str();
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
      LD.EnclosingClass = std::string(EnclosingClass);
      LD.IsMember = (Sym == pseudo::cxx::Symbol::member_declarator) ||
                    !EnclosingClass.empty();
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
      Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
    }
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
    if (NameTok) {
      LocalDecl LD;
      LD.Name = getOrigToken(*NameTok, Out).text().str();
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
        Member.EnclosingClass = std::string(EnclosingClass);
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
    for (size_t I = 0; I < NodeTokens.size(); ++I) {
      if (NodeTokens[I].Kind == tok::kw_using && I + 1 < NodeTokens.size()) {
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
          LD.Kind = PseudoModule::DeclKind::TypeAlias;
          Scopes[CurrentScopeId].Decls.push_back(std::move(LD));
          break;
        }
      }
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
  if (Sym == pseudo::cxx::Symbol::simple_declaration) {
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

  auto Children = N->elements();
  for (size_t I = 0; I < Children.size(); ++I) {
    pseudo::Token::Index ChildEnd =
        (I + 1 == Children.size()) ? End : Children[I + 1]->startTokenIndex();
    buildScopes(Children[I], ChildEnd, Out, Code, Scopes, CurrentScopeId,
                EnclosingClass, ChildDeclaredType);
  }
}

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

PseudoModule::PseudoModule() : SupportedSymbolKinds(defaultSymbolKinds()) {}
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
      HeaderDecl HD;
      HD.Name = D.Name;
      HD.NameRange = D.NameRange;
      HD.ScopeRange = D.DeclRange;
      HD.EnclosingScope = Scope.Name;
      HD.Kind = D.Kind;
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

  Bind.method("clangd/pseudoSymbols", this,
              &PseudoModule::onCustomPseudoSymbols);
}

std::unique_ptr<FeatureModule::ASTListener> PseudoModule::astListeners() {
  return std::make_unique<PseudoASTListener>();
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

static bool isTypeContext(const pseudo::Token *Touched, const ParseOutput &Out) {
  if (!Touched)
    return false;
  size_t Idx = Out.RawStream.index(*Touched);
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

  return false;
}

static const LocalDecl *resolveTargetDecl(
    const pseudo::Token *Touched, size_t BestScope,
    const std::vector<LexicalScope> &Scopes, const ParseOutput &Parsed,
    llvm::StringRef Code, bool ExpectsType = false) {
  std::string TargetName = getOrigToken(*Touched, Parsed).text().str();
  size_t TouchedOffset = tokenStartOffset(*Touched, Parsed);

  const LocalDecl *TargetDecl = nullptr;
  pseudo::Token::Index TouchedIdx = Parsed.RawStream.index(*Touched);
  const pseudo::Token *OpTok = nullptr;
  const pseudo::Token *LhsTok = nullptr;
  size_t Step = 1;
  while (TouchedIdx >= Step) {
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
  if (OpTok && LhsTok) {
    std::string LhsName = getOrigToken(*LhsTok, Parsed).text().str();
    if (LhsName == "this") {
      std::string TargetClass = Scopes[BestScope].EnclosingClass;
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
              TargetDecl = &D;
              break;
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
        for (const auto &CS : Scopes) {
          if (CS.Kind == ScopeKind::Class && CS.Name == LhsDecl->TypeName) {
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

  if (!TargetDecl)
    TargetDecl = lookupDecl(BestScope, TargetName, TouchedOffset, Scopes, Code,
                            ExpectsType);
  if (!TargetDecl)
    TargetDecl = findAnyDecl(TargetName, Scopes, ExpectsType);
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
      if (const auto *Decl = findMatchingDeclaration(TargetDecl, Scopes))
        LS.PreferredDeclaration = Location{FileURI, Decl->NameRange};
      else
        LS.PreferredDeclaration = *LS.Definition;
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

  std::queue<std::pair<std::string, int>> Queue;
  llvm::StringSet<> Visited;

  // Push quoted (project) headers first, then angled headers
  for (const auto &Inc : Includes) {
    if (!Inc.IsAngled) {
      std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
      if (!Resolved.empty() && Visited.insert(Resolved).second) {
        Queue.push({Resolved, 1});
      }
    }
  }
  for (const auto &Inc : Includes) {
    if (Inc.IsAngled) {
      std::string Resolved = resolveHeader(Inc, CurrentDir, IncludeDirs, *FS);
      if (!Resolved.empty() && Visited.insert(Resolved).second) {
        Queue.push({Resolved, 1});
      }
    }
  }

  const int MaxDepth = 4;
  const size_t MaxHeaders = 100;
  size_t HeadersVisited = 0;

  HeaderDecl BestDecl;
  std::string BestHeaderPath;
  bool Found = false;
  HeaderDecl FallbackDecl;
  std::string FallbackHeaderPath;
  bool FallbackFound = false;

  while (!Queue.empty() && HeadersVisited < MaxHeaders) {
    auto [HeaderPath, Depth] = Queue.front();
    Queue.pop();
    ++HeadersVisited;

    auto Info = getHeaderInfo(HeaderPath, *FS);
    if (!Info)
      continue;

    for (const auto &D : Info->Decls) {
      if (D.Name == TargetName) {
        if (ExpectsType) {
          if (PseudoModule::isTypeDecl(D.Kind)) {
            BestDecl = D;
            BestHeaderPath = HeaderPath;
            Found = true;
            break;
          }
        } else {
          if (!PseudoModule::isTypeDecl(D.Kind)) {
            BestDecl = D;
            BestHeaderPath = HeaderPath;
            Found = true;
            break;
          } else if (!FallbackFound) {
            FallbackDecl = D;
            FallbackHeaderPath = HeaderPath;
            FallbackFound = true;
          }
        }
      }
    }
    if (Found)
      break;

    bool IsSys = HeaderPath.rfind("/usr/", 0) == 0 ||
                 HeaderPath.rfind("/Library/Developer/", 0) == 0 ||
                 HeaderPath.rfind("/Applications/Xcode.app/", 0) == 0 ||
                 HeaderPath.rfind("/opt/homebrew/", 0) == 0;
    if (Depth < MaxDepth && !IsSys) {
      std::string HDir = llvm::sys::path::parent_path(HeaderPath).str();
      for (const auto &SubInc : Info->Includes) {
        if (!SubInc.IsAngled) {
          std::string SubResolved = resolveHeader(SubInc, HDir, IncludeDirs, *FS);
          if (!SubResolved.empty() && Visited.insert(SubResolved).second) {
            Queue.push({SubResolved, Depth + 1});
          }
        }
      }
      for (const auto &SubInc : Info->Includes) {
        if (SubInc.IsAngled) {
          std::string SubResolved = resolveHeader(SubInc, HDir, IncludeDirs, *FS);
          if (!SubResolved.empty() && Visited.insert(SubResolved).second) {
            Queue.push({SubResolved, Depth + 1});
          }
        }
      }
    }
  }

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
      File, Params.position, /*Limit=*/0, /*AddContainer=*/false,
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

void PseudoModule::onHover(
    const TextDocumentPositionParams &Params,
    Callback<std::optional<Hover>> Reply) {
  PathRef File = Params.textDocument.uri.file();

  auto MakeHover = [this, File = File.str()](
                       llvm::StringRef MainCode,
                       const LocatedSymbol &Sym) -> Hover {
    Hover R;
    R.contents.kind = MarkupKind::Markdown;
    auto DeclRange = Sym.PreferredDeclaration.range;
    std::string TargetFile = Sym.PreferredDeclaration.uri.file().str();
    std::string TargetCode;
    if (TargetFile == File) {
      TargetCode = MainCode.str();
    } else if (auto FS = getFS()) {
      if (auto Buf = FS->getBufferForFile(TargetFile))
        TargetCode = (*Buf)->getBuffer().str();
    }

    std::string Snippet;
    if (!TargetCode.empty()) {
      if (auto StartOffset = positionToOffset(TargetCode, DeclRange.start)) {
        if (auto EndOffset = positionToOffset(TargetCode, DeclRange.end)) {
          if (*StartOffset < *EndOffset && *EndOffset <= TargetCode.size())
            Snippet = TargetCode.substr(*StartOffset, *EndOffset - *StartOffset);
        }
      }
    }
    if (Snippet.empty() || Snippet == Sym.Name) {
      if (TargetFile != File)
        Snippet = "// Header: " + TargetFile;
      else
        Snippet = Sym.Name;
    }
    R.contents.value = "```cpp\n" + Snippet + "\n```";
    return R;
  };

  auto DoPseudoHover = [this, File = File.str(), Params, MakeHover,
                        Reply = std::move(Reply)]() mutable {
    std::string Code = getDocument(File);
    if (Code.empty())
      return Reply(std::nullopt);
    auto Syms = locateSymbolAt(File, Code, Params.position);
    if (!Syms || Syms->empty())
      return Reply(std::nullopt);
    return Reply(MakeHover(Code, (*Syms)[0]));
  };

  if (isPseudoOnly()) {
    scheduler().run("PseudoHover", File, std::move(DoPseudoHover));
    return;
  }

  server().findHover(
      File, Params.position,
      [this, File = File.str(), Params, MakeHover = std::move(MakeHover),
       Reply = std::move(Reply)](
          llvm::Expected<std::optional<HoverInfo>> H) mutable {
        if (H && *H) {
          Hover R;
          R.contents.kind = MarkupKind::Markdown;
          R.range = (*H)->SymRange;
          R.contents.value = (*H)->present(MarkupKind::Markdown);
          return Reply(std::move(R));
        }
        if (isEnabled()) {
          std::string Code = getDocument(File);
          if (!Code.empty()) {
            auto Syms = locateSymbolAt(File, Code, Params.position);
            if (Syms && !Syms->empty()) {
              return Reply(MakeHover(Code, (*Syms)[0]));
            }
          }
        }
        if (!H)
          return Reply(H.takeError());
        return Reply(std::nullopt);
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

volatile int PseudoModuleAnchorSource = 0;

FeatureModuleRegistry::Add<PseudoModule>
    X("PseudoParser", "C++ pseudo-parser fallback module for clangd");

} // namespace clangd
} // namespace clang
