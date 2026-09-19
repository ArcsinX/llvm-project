//===--- Diagnostics.cpp - Pseudo-parser diagnostics generation ---*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "pseudo/Diagnostics.h"
#include "SourceCode.h"
#include "clang-pseudo/cxx/CXX.h"
#include "llvm/ADT/DenseSet.h"
#include <algorithm>
#include <set>
#include <tuple>

namespace clang {
namespace clangd {

namespace {

constexpr int DiagnosticSeverityError = 1;

void collectOpaqueNodes(const pseudo::ForestNode *N,
                        const ParseOutput &Parsed,
                        std::vector<const pseudo::ForestNode *> &Opaques,
                        llvm::DenseSet<const pseudo::ForestNode *> &Visited) {
  if (!N || !Visited.insert(N).second)
    return;
  if (N->kind() == pseudo::ForestNode::Opaque) {
    Opaques.push_back(N);
    return;
  }
  if (N->kind() == pseudo::ForestNode::Ambiguous) {
    auto Alts = N->alternatives();
    if (!Alts.empty()) {
      auto It = Parsed.Disambig.find(N);
      unsigned AltIdx =
          (It != Parsed.Disambig.end() && It->second < Alts.size()) ? It->second
                                                                   : 0;
      collectOpaqueNodes(Alts[AltIdx], Parsed, Opaques, Visited);
    }
    return;
  }
  for (const auto *Child : N->children())
    collectOpaqueNodes(Child, Parsed, Opaques, Visited);
}

} // namespace

std::vector<Diagnostic> getDiagnostics(const ParseOutput &Parsed,
                                       llvm::StringRef Code) {
  std::vector<Diagnostic> Diags;
  std::set<std::tuple<int, int, std::string>> Seen;

  auto AddDiag = [&](Diagnostic D) {
    auto Key = std::make_tuple(D.range.start.line, D.range.start.character,
                               D.code);
    if (Seen.insert(Key).second)
      Diags.push_back(std::move(D));
  };

  // 1. Bracket / brace mismatches using Token::Pair from pseudo::pairBrackets
  for (const auto &Tok : Parsed.ParseableStream.tokens()) {
    if (Tok.Kind == tok::l_brace && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "missing-brace";
      D.message = "unclosed '{', expected closing '}'";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::r_brace && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "unmatched-brace";
      D.message = "unmatched '}', extraneous closing brace";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::l_paren && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "missing-paren";
      D.message = "unclosed '(', expected closing ')'";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::r_paren && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "unmatched-paren";
      D.message = "unmatched ')', extraneous closing parenthesis";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::l_square && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "missing-bracket";
      D.message = "unclosed '[', expected closing ']'";
      AddDiag(std::move(D));
    } else if (Tok.Kind == tok::r_square && Tok.Pair == 0) {
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "unmatched-bracket";
      D.message = "unmatched ']', extraneous closing bracket";
      AddDiag(std::move(D));
    }
  }

  // 2. Grammar syntax errors: opaque recovery nodes in the forest
  if (!Parsed.Root || Parsed.Root->kind() == pseudo::ForestNode::Opaque) {
    if (Diags.empty()) {
      Diagnostic D;
      D.range = Range{Position{0, 0}, offsetToPosition(Code, Code.size())};
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "syntax-error";
      D.message = "syntax error: failed to parse translation unit";
      AddDiag(std::move(D));
    }
    return Diags;
  }

  std::vector<const pseudo::ForestNode *> Opaques;
  llvm::DenseSet<const pseudo::ForestNode *> Visited;
  collectOpaqueNodes(Parsed.Root, Parsed, Opaques, Visited);

  const auto &Lang = pseudo::cxx::getLanguage();
  for (const auto *Node : Opaques) {
    auto StartIdx = Node->startTokenIndex();
    if (StartIdx < Parsed.ParseableStream.tokens().size()) {
      const auto &Tok = Parsed.ParseableStream.tokens()[StartIdx];
      if (Tok.Kind == tok::eof)
        continue;
      std::string SymName = Lang.G.symbolName(Node->symbol()).str();
      Diagnostic D;
      D.range = tokenRange(Tok, Parsed, Code);
      D.severity = DiagnosticSeverityError;
      D.source = "pseudo-parser";
      D.code = "grammar-error";
      D.message =
          "syntax error: code does not fit grammar (expected " + SymName + ")";
      AddDiag(std::move(D));
    }
  }

  llvm::sort(Diags, [](const Diagnostic &A, const Diagnostic &B) {
    if (A.range.start.line != B.range.start.line)
      return A.range.start.line < B.range.start.line;
    return A.range.start.character < B.range.start.character;
  });

  return Diags;
}

} // namespace clangd
} // namespace clang
