//===-- PseudoModuleTests.cpp ----------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Annotations.h"
#include "ClangdLSPServer.h"
#include "ClangdServer.h"
#include "FeatureModule.h"
#include "GlobalCompilationDatabase.h"
#include "LSPClient.h"
#include "Protocol.h"
#include "PseudoModule.h"
#include "SyncAPI.h"
#include "TestFS.h"
#include "support/Logger.h"
#include "support/Threading.h"
#include "llvm/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <memory>
#include <vector>

namespace clang {
namespace clangd {
namespace {

static std::string dumpAST(ClangdServer &Server, PathRef File) {
  std::string Result;
  Notification Done;
  Server.customAction(File, "DumpAST", [&](llvm::Expected<InputsAndAST> AST) {
    if (AST) {
      llvm::raw_string_ostream ResultOS(Result);
      AST->AST.getASTContext().getTranslationUnitDecl()->dump(ResultOS, true);
    } else {
      llvm::consumeError(AST.takeError());
      Result = "<no-ast>";
    }
    Done.notify();
  });
  Done.wait();
  return Result;
}

using ::testing::AllOf;
using ::testing::Contains;
using ::testing::ElementsAre;
using ::testing::Field;
using ::testing::IsEmpty;
using ::testing::Not;
using ::testing::UnorderedElementsAre;

MATCHER_P(withName, N, "") { return arg.name == N; }
MATCHER_P(withKind, Kind, "") { return arg.kind == Kind; }

template <class... ChildMatchers>
::testing::Matcher<DocumentSymbol> children(ChildMatchers... ChildrenM) {
  return Field(&DocumentSymbol::children, UnorderedElementsAre(ChildrenM...));
}

TEST(PseudoModuleTest, RegistryRegistration) {
  FeatureModuleSet Set = FeatureModuleSet::fromRegistry();
  auto *Mod = Set.get<PseudoModule>();
  EXPECT_NE(Mod, nullptr);
}

TEST(PseudoModuleTest, BasicDocumentSymbols) {
  PseudoModule Mod;
  llvm::StringRef Code = R"cpp(
    namespace my_ns {
      class MyClass {
        void myMethod() {}
        int myField;
      };
      struct MyStruct {
        int x;
      };
      void globalFunc() {}
      enum class Color {
        Red,
        Green,
        Blue
      };
      using IntAlias = int;
    }
  )cpp";

  auto Symbols = Mod.getDocumentSymbols(Code);
  ASSERT_TRUE(bool(Symbols)) << llvm::toString(Symbols.takeError());

  EXPECT_THAT(
      *Symbols,
      ElementsAre(AllOf(
          withName("my_ns"), withKind(SymbolKind::Namespace),
          children(
              AllOf(withName("MyClass"), withKind(SymbolKind::Class),
                    children(AllOf(withName("myMethod"),
                                   withKind(SymbolKind::Method)),
                             AllOf(withName("myField"),
                                   withKind(SymbolKind::Field)))),
              AllOf(withName("MyStruct"), withKind(SymbolKind::Struct),
                    children(
                        AllOf(withName("x"), withKind(SymbolKind::Field)))),
              AllOf(withName("globalFunc"), withKind(SymbolKind::Function)),
              AllOf(withName("Color"), withKind(SymbolKind::Enum),
                    children(
                        AllOf(withName("Red"), withKind(SymbolKind::EnumMember)),
                        AllOf(withName("Green"),
                              withKind(SymbolKind::EnumMember)),
                        AllOf(withName("Blue"),
                              withKind(SymbolKind::EnumMember)))),
              AllOf(withName("IntAlias"), withKind(SymbolKind::Class))))));
}

TEST(PseudoModuleTest, BrokenCodeRecovery) {
  PseudoModule Mod;
  // Code with broken function body or missing tokens that GLR recovers from
  llvm::StringRef Code = R"cpp(
    namespace broken_ns {
      void goodFunc() {}
      void brokenFunc() {
        invalid syntax ;;; %%%
      }
      class Survives {
        void method() {}
      };
    }
  )cpp";

  auto Symbols = Mod.getDocumentSymbols(Code);
  ASSERT_TRUE(bool(Symbols)) << llvm::toString(Symbols.takeError());

  EXPECT_THAT(
      *Symbols,
      ElementsAre(AllOf(
          withName("broken_ns"), withKind(SymbolKind::Namespace),
          children(
              AllOf(withName("goodFunc"), withKind(SymbolKind::Function)),
              AllOf(withName("brokenFunc"), withKind(SymbolKind::Function)),
              AllOf(withName("Survives"), withKind(SymbolKind::Class),
                    children(AllOf(withName("method"),
                                   withKind(SymbolKind::Method))))))));
}

TEST(PseudoModuleTest, SemanticRanges) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    namespace test {
      void func() {
        int $point^x = 42;
      }
    }
  )cpp");

  auto Ranges = Mod.getSemanticRanges(Code.code(), {Code.point("point")});
  ASSERT_TRUE(bool(Ranges)) << llvm::toString(Ranges.takeError());
  ASSERT_EQ(Ranges->size(), 1u);

  // Innermost selection range should cover the variable or expression
  const auto &Innermost = (*Ranges)[0];
  EXPECT_TRUE(Innermost.range.start.line <= Code.point("point").line);
  EXPECT_TRUE(Innermost.range.end.line >= Code.point("point").line);
  EXPECT_NE(Innermost.parent, nullptr);
}

TEST(PseudoModuleTest, FoldingRanges) {
  PseudoModule Mod;
  llvm::StringRef Code = R"cpp(
    namespace fold_ns {
      void myFunc() {
        int a = 1;
        int b = 2;
      }
    }
  )cpp";

  auto Foldings = Mod.getFoldingRanges(Code, /*LineFoldingOnly=*/false);
  ASSERT_TRUE(bool(Foldings)) << llvm::toString(Foldings.takeError());
  EXPECT_FALSE(Foldings->empty());
  for (const auto &FR : *Foldings) {
    EXPECT_LT(FR.startLine, FR.endLine);
  }
}

TEST(PseudoModuleTest, LocateSymbolAtLocal) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void testFunc() {
      int $decl[[myVar]] = 10;
      $usage^myVar = 20;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "myVar");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.uri.file(), File);
}

TEST(PseudoModuleTest, LocateSymbolAtParameter) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void testFunc(int $decl[[param]]) {
      int x = $usage^param + 1;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "param");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, LocateSymbolAtMember) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    class MyClass {
      int $decl[[memberVal]];
      void foo() {
        $usage^memberVal = 42;
      }
    };
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "memberVal");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, LocateSymbolAtOutOfLineMethod) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    class MyClass {
      int $decl[[memberVal]];
      void method();
    };
    void MyClass::method() {
      $usage^memberVal = 100;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point("usage"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "memberVal");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, FindReferencesLocal) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void funcOne() {
      int $ref1[[var]] = 1;
      $ref2[[var]] = 2;
    }
    void funcTwo() {
      int var = 3;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Refs = Mod.findReferences(File, Code.code(), Code.range("ref2").start);
  ASSERT_TRUE(bool(Refs)) << llvm::toString(Refs.takeError());
  EXPECT_EQ(Refs->References.size(), 2u);
  std::vector<Range> Ranges;
  for (const auto &R : Refs->References)
    Ranges.push_back(R.Loc.range);
  EXPECT_THAT(Ranges, UnorderedElementsAre(Code.range("ref1"), Code.range("ref2")));
  EXPECT_TRUE(Refs->References[0].Attributes & ReferencesResult::Declaration);
}

TEST(PseudoModuleTest, FindReferencesFunctionAndLimit) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    void $call1[[foo]]() {}
    void caller() {
      $call2[[foo]]();
      $call3[[foo]]();
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Refs = Mod.findReferences(File, Code.code(), Code.range("call1").start);
  ASSERT_TRUE(bool(Refs)) << llvm::toString(Refs.takeError());
  EXPECT_EQ(Refs->References.size(), 3u);

  // Test limit
  auto Limited = Mod.findReferences(File, Code.code(), Code.range("call1").start, /*Limit=*/2);
  ASSERT_TRUE(bool(Limited)) << llvm::toString(Limited.takeError());
  EXPECT_EQ(Limited->References.size(), 2u);
  EXPECT_TRUE(Limited->HasMore);
}

TEST(PseudoModuleTest, ClangdServerFallbackOnInvalidCommand) {
  // End-to-end test verifying ClangdServer falls back to PseudoModule
  // when compile command is invalid (AST cannot be built).
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  // Use -### so clang driver prints command without running cc1, causing AST build to fail.
  CDB->ExtraClangFlags = {"-###"};

  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  Modules.add(std::move(Pseudo));

  ClangdServer::Options Opts = ClangdServer::optsForTest();
  Opts.FeatureModules = &Modules;

  ClangdServer Server(*CDB, FS, Opts);

  Annotations Source(R"cpp(
    namespace fallback_ns {
      class FallbackClass {
        void $decl[[work]]() {}
      };
      void topLevel() {
        $call^work();
      }
    }
  )cpp");

  std::string FilePath = testPath("broken_command.cpp");
  FS.Files[FilePath] = Source.code().str();
  Server.addDocument(FilePath, Source.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // Verify AST indeed failed to build
  EXPECT_EQ(dumpAST(Server, FilePath), "<no-ast>");

  // Test documentSymbols fallback
  bool Done = false;
  std::optional<llvm::Expected<llvm::json::Value>> SymbolsResult;
  DocumentSymbolParams SymParams;
  SymParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PseudoPtr->onDocumentSymbol(
      SymParams,
      [&](llvm::Expected<llvm::json::Value> Symbols) {
        SymbolsResult = std::move(Symbols);
        Done = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(Done);
  ASSERT_TRUE(SymbolsResult.has_value());
  ASSERT_TRUE(bool(*SymbolsResult))
      << "Fallback should succeed: "
      << llvm::toString(SymbolsResult->takeError());

  // Test semantic ranges fallback
  bool RangesDone = false;
  std::optional<llvm::Expected<std::vector<SelectionRange>>> RangesResult;
  SelectionRangeParams SelParams;
  SelParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  SelParams.positions = {Position{2, 12}};
  PseudoPtr->onSelectionRange(
      SelParams,
      [&](llvm::Expected<std::vector<SelectionRange>> Ranges) {
        RangesResult = std::move(Ranges);
        RangesDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(RangesDone);
  ASSERT_TRUE(RangesResult.has_value());
  ASSERT_TRUE(bool(*RangesResult))
      << "Semantic ranges fallback should succeed: "
      << llvm::toString(RangesResult->takeError());
  EXPECT_FALSE((*RangesResult)->empty());

  // Test locateSymbolAt fallback
  TextDocumentPositionParams PosParams;
  PosParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PosParams.position = Source.point("call");
  bool LocDone = false;
  std::optional<llvm::Expected<std::vector<Location>>> LocResult;
  PseudoPtr->onGoToDefinition(
      PosParams,
      [&](llvm::Expected<std::vector<Location>> Locs) {
        LocResult = std::move(Locs);
        LocDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(LocDone);
  ASSERT_TRUE(LocResult.has_value());
  ASSERT_TRUE(bool(*LocResult)) << llvm::toString(LocResult->takeError());
  ASSERT_EQ((*LocResult)->size(), 1u);
  EXPECT_EQ((*LocResult)->front().range, Source.range("decl"));

  // Test findDocumentHighlights fallback
  bool HighDone = false;
  std::optional<llvm::Expected<std::vector<DocumentHighlight>>> HighResult;
  PseudoPtr->onDocumentHighlight(
      PosParams,
      [&](llvm::Expected<std::vector<DocumentHighlight>> Highs) {
        HighResult = std::move(Highs);
        HighDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(HighDone);
  ASSERT_TRUE(HighResult.has_value());
  ASSERT_TRUE(bool(*HighResult)) << llvm::toString(HighResult->takeError());
  EXPECT_EQ((*HighResult)->size(), 2u);

  // Test findReferences fallback
  bool RefsDone = false;
  std::optional<llvm::Expected<std::vector<ReferenceLocation>>> RefsResult;
  ReferenceParams RefParams;
  RefParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  RefParams.position = Source.point("call");
  RefParams.context.includeDeclaration = true;
  PseudoPtr->onReference(
      RefParams,
      [&](llvm::Expected<std::vector<ReferenceLocation>> Refs) {
        RefsResult = std::move(Refs);
        RefsDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(RefsDone);
  ASSERT_TRUE(RefsResult.has_value());
  ASSERT_TRUE(bool(*RefsResult)) << llvm::toString(RefsResult->takeError());
  EXPECT_EQ((*RefsResult)->size(), 2u);

  // When pseudo-parser is disabled, it should use the original implementation and NOT fall back
  PseudoPtr->setEnabled(false);
  std::optional<llvm::Expected<std::vector<Location>>> DisabledLoc;
  PseudoPtr->onGoToDefinition(
      PosParams,
      [&](llvm::Expected<std::vector<Location>> Locs) {
        DisabledLoc = std::move(Locs);
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  ASSERT_TRUE(DisabledLoc.has_value());
  EXPECT_FALSE(bool(*DisabledLoc));
  llvm::consumeError(DisabledLoc->takeError());
}

TEST(PseudoModuleTest, LocateSymbolAtTemplateTypeParameter) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    template <typename [[T]]>
    void foo() {
      ^T var;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "T");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range());
}

TEST(PseudoModuleTest, LocateSymbolAtTypedMemberAccess) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    struct ClassA {
      int $declA[[val]];
    };
    struct ClassB {
      int val;
    };
    void test() {
      ClassA a;
      a.^val = 1;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "val");
  // Must specifically point to ClassA's val, not ClassB's
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("declA"));
}

TEST(PseudoModuleTest, LocateSymbolAtUsingDeclaration) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    namespace my_ns {
      void $decl[[targetFunc]]();
    }
    using my_ns::^targetFunc;
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "targetFunc");
}

TEST(PseudoModuleTest, LocateSymbolAtNamespaceAlias) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    namespace LongNamespaceName {
      int x;
    }
    namespace $decl[[ShortName]] = LongNamespaceName;
    void test() {
      ^ShortName::x = 42;
    }
  )cpp");
  std::string File = testPath("test.cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u);
  EXPECT_EQ((*Loc)[0].Name, "ShortName");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("decl"));
}

TEST(PseudoModuleTest, UsePseudoParserOnlyOption) {
  // End-to-end test verifying setPseudoOnly disables Clang parser completely
  // and routes open-file navigation requests directly to PseudoModule.
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();

  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  Modules.add(std::move(Pseudo));

  ClangdServer::Options Opts = ClangdServer::optsForTest();
  Opts.FeatureModules = &Modules;

  ClangdServer Server(*CDB, FS, Opts);

  Annotations Source(R"cpp(
    namespace test_only {
      class Worker {
        void $decl[[doJob]]() {}
      };
      void run() {
        Worker w;
        w.^doJob();
      }
    }
  )cpp");

  std::string FilePath = testPath("pseudo_only.cpp");
  FS.Files[FilePath] = Source.code().str();
  Server.addDocument(FilePath, Source.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // In UsePseudoParserOnly mode, no Clang AST is built at all!
  EXPECT_EQ(dumpAST(Server, FilePath), "<no-ast>");

  // locateSymbolAt should work via PseudoModule
  TextDocumentPositionParams PosParams;
  PosParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PosParams.position = Source.point();
  bool LocDone = false;
  std::optional<llvm::Expected<std::vector<Location>>> LocResult;
  PseudoPtr->onGoToDefinition(
      PosParams,
      [&](llvm::Expected<std::vector<Location>> Locs) {
        LocResult = std::move(Locs);
        LocDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(LocDone);
  ASSERT_TRUE(LocResult.has_value());
  ASSERT_TRUE(bool(*LocResult)) << llvm::toString(LocResult->takeError());
  ASSERT_EQ((*LocResult)->size(), 1u);
  EXPECT_EQ((*LocResult)->front().range, Source.range("decl"));

  // findDocumentHighlights should work via PseudoModule
  bool HighDone = false;
  std::optional<llvm::Expected<std::vector<DocumentHighlight>>> HighResult;
  PseudoPtr->onDocumentHighlight(
      PosParams,
      [&](llvm::Expected<std::vector<DocumentHighlight>> Highs) {
        HighResult = std::move(Highs);
        HighDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(HighDone);
  ASSERT_TRUE(HighResult.has_value());
  ASSERT_TRUE(bool(*HighResult)) << llvm::toString(HighResult->takeError());
  EXPECT_EQ((*HighResult)->size(), 2u);

  // findReferences should work via PseudoModule
  bool RefsDone = false;
  std::optional<llvm::Expected<std::vector<ReferenceLocation>>> RefsResult;
  ReferenceParams RefParams;
  RefParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  RefParams.position = Source.point();
  RefParams.context.includeDeclaration = true;
  PseudoPtr->onReference(
      RefParams,
      [&](llvm::Expected<std::vector<ReferenceLocation>> Refs) {
        RefsResult = std::move(Refs);
        RefsDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(RefsDone);
  ASSERT_TRUE(RefsResult.has_value());
  ASSERT_TRUE(bool(*RefsResult)) << llvm::toString(RefsResult->takeError());
  EXPECT_EQ((*RefsResult)->size(), 2u);

  // documentSymbols should work via PseudoModule
  bool Done = false;
  std::optional<llvm::Expected<llvm::json::Value>> SymbolsResult;
  DocumentSymbolParams SymParams;
  SymParams.textDocument.uri = URIForFile::canonicalize(FilePath, FilePath);
  PseudoPtr->onDocumentSymbol(
      SymParams,
      [&](llvm::Expected<llvm::json::Value> Symbols) {
        SymbolsResult = std::move(Symbols);
        Done = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(Done);
  ASSERT_TRUE(SymbolsResult.has_value());
  ASSERT_TRUE(bool(*SymbolsResult));

  // hover should work via PseudoModule
  bool HoverDone = false;
  std::optional<llvm::Expected<std::optional<Hover>>> HoverResult;
  PseudoPtr->onHover(
      PosParams,
      [&](llvm::Expected<std::optional<Hover>> H) {
        HoverResult = std::move(H);
        HoverDone = true;
      });
  ASSERT_TRUE(Server.blockUntilIdleForTest());
  EXPECT_TRUE(HoverDone);
  ASSERT_TRUE(HoverResult.has_value());
  ASSERT_TRUE(bool(*HoverResult)) << llvm::toString(HoverResult->takeError());
  ASSERT_TRUE((*HoverResult)->has_value());
  EXPECT_NE((**HoverResult)->contents.value.find("doJob"), std::string::npos);
}

TEST(PseudoModuleTest, EndToEndLSPOverriding) {
  MockFS FS;
  ClangdLSPServer::Options Opts;
  ClangdServer::Options &Base = Opts;
  Base = ClangdServer::optsForTest();
  FeatureModuleSet FeatureModules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  FeatureModules.add(std::move(Pseudo));
  Base.FeatureModules = &FeatureModules;

  LSPClient Client;
  std::optional<ClangdLSPServer> Server;
  std::optional<std::thread> ServerThread;

  Server.emplace(Client.transport(), FS, Opts);
  ServerThread.emplace([&] { EXPECT_TRUE(Server->run()); });
  Client.call("initialize", llvm::json::Object{});

  Annotations Code(R"cpp(
    int $decl[[my_var]] = 10;
    int foo() {
      return ^my_var + 1;
    }
  )cpp");

  Client.didOpen("test.cpp", Code.code());

  auto &Def = Client.call("textDocument/definition",
                          llvm::json::Object{
                              {"textDocument", Client.documentID("test.cpp")},
                              {"position", Code.point()},
                          });
  auto DefValue = Def.takeValue();
  ASSERT_TRUE(DefValue.getAsArray() != nullptr);
  EXPECT_EQ(DefValue.getAsArray()->size(), 1u);
  auto Loc = (*DefValue.getAsArray())[0].getAsObject();
  auto RangeVal = Loc->get("range");
  ASSERT_NE(RangeVal, nullptr);
  Range R;
  llvm::json::Path::Root Root;
  ASSERT_TRUE(fromJSON(*RangeVal, R, Root));
  EXPECT_EQ(R, Code.range("decl"));

  Client.call("shutdown", nullptr);
  Client.notify("exit", nullptr);
  Client.stop();
  ServerThread->join();
  Server.reset();
  ServerThread.reset();
}

TEST(PseudoModuleTest, ClangdServerAdjustParseInputsNavigation) {
  PseudoModule Mod;
  std::string Path = testPath("ClangdServer.cpp");
  Annotations Code(R"cpp(
    void adjustParseInputs(bool SkipPreambleBuild, const char *File) {
      auto $def[[HasRequiredModules]] = [File]() {
        return hasRequiredModules($file_use^File);
      };
      if (SkipPreambleBuild || $call^HasRequiredModules())
        return;
    }
  )cpp");

  auto Loc = Mod.locateSymbolAt(Path, Code.code(), Code.point("call"));
  ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
  EXPECT_EQ(Loc->size(), 1u) << "Loc size is " << Loc->size();
  if (!Loc->empty()) {
    EXPECT_EQ((*Loc)[0].Name, "HasRequiredModules");
    EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("def"));
  }

  auto LocFile = Mod.locateSymbolAt(Path, Code.code(), Code.point("file_use"));
  ASSERT_TRUE(bool(LocFile)) << llvm::toString(LocFile.takeError());
  EXPECT_EQ(LocFile->size(), 1u) << "LocFile size is " << LocFile->size();
  if (!LocFile->empty()) {
    EXPECT_EQ((*LocFile)[0].Name, "File");
  }
}

TEST(PseudoModuleTest, ExtractIncludes) {
  llvm::StringRef Code = R"cpp(
#include "my_header.h"
#  include <vector>
#include_next "next_header.h"
// #include "commented.h"
int main() {}
#include "footer.h"
)cpp";

  auto Incs = PseudoModule::extractIncludes(Code);
  ASSERT_EQ(Incs.size(), 4u);
  EXPECT_EQ(Incs[0].Written, "my_header.h");
  EXPECT_FALSE(Incs[0].IsAngled);
  EXPECT_EQ(Incs[0].HashLine, 1);

  EXPECT_EQ(Incs[1].Written, "vector");
  EXPECT_TRUE(Incs[1].IsAngled);
  EXPECT_EQ(Incs[1].HashLine, 2);

  EXPECT_EQ(Incs[2].Written, "next_header.h");
  EXPECT_FALSE(Incs[2].IsAngled);
  EXPECT_EQ(Incs[2].HashLine, 3);

  EXPECT_EQ(Incs[3].Written, "footer.h");
  EXPECT_FALSE(Incs[3].IsAngled);
  EXPECT_EQ(Incs[3].HashLine, 6);
}

TEST(PseudoModuleTest, GoToDefinitionOnIncludeDirective) {
  MockFS FS;
  FS.Files[testPath("foo.h")] = "struct Foo {};\n";
  FS.Files[testPath("sys/bar.h")] = "struct Bar {};\n";

  MockCompilationDatabase CDB(testRoot());
  CDB.ExtraClangFlags = {"-I" + testPath("sys")};

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(&CDB);

  Annotations Main(R"cpp(
#include ^"foo.h"
#include <^bar.h>
int main() {}
)cpp");

  auto LocFoo =
      Mod.locateSymbolAt(testPath("main.cpp"), Main.code(), Main.points()[0]);
  ASSERT_TRUE(bool(LocFoo)) << llvm::toString(LocFoo.takeError());
  ASSERT_EQ(LocFoo->size(), 1u);
  EXPECT_EQ((*LocFoo)[0].Name, "foo.h");
  EXPECT_EQ((*LocFoo)[0].PreferredDeclaration.uri.file(), testPath("foo.h"));

  auto LocBar =
      Mod.locateSymbolAt(testPath("main.cpp"), Main.code(), Main.points()[1]);
  ASSERT_TRUE(bool(LocBar)) << llvm::toString(LocBar.takeError());
  ASSERT_EQ(LocBar->size(), 1u);
  EXPECT_EQ((*LocBar)[0].Name, "bar.h");
  EXPECT_EQ((*LocBar)[0].PreferredDeclaration.uri.file(), testPath("sys/bar.h"));
}

TEST(PseudoModuleTest, GoToDefinitionAcrossHeadersDirectAndTransitive) {
  MockFS FS;
  FS.Files[testPath("inc2/header_b.h")] = R"cpp(
    struct TransitiveWidget {
      int value;
    };
  )cpp";

  FS.Files[testPath("inc1/header_a.h")] = R"cpp(
    #include "header_b.h"

    struct DirectStruct {
      int x;
    };

    class DirectClass {
    public:
      void doSomething();
    };

    enum class DirectEnum {
      First,
      Second
    };

    using DirectAlias = DirectStruct;
  )cpp";

  MockCompilationDatabase CDB(testRoot());
  CDB.ExtraClangFlags = {"-I" + testPath("inc1"), "-I" + testPath("inc2")};

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(&CDB);

  Annotations Main(R"cpp(
    #include "header_a.h"

    void test() {
      ^DirectStruct S;
      ^DirectClass C;
      ^DirectEnum E;
      ^DirectAlias A;
      ^TransitiveWidget W;
    }
  )cpp");

  auto CheckSym = [&](Position Pos, llvm::StringRef ExpectedName,
                      PathRef ExpectedFile) {
    auto Loc = Mod.locateSymbolAt(testPath("main.cpp"), Main.code(), Pos);
    ASSERT_TRUE(bool(Loc)) << llvm::toString(Loc.takeError());
    ASSERT_EQ(Loc->size(), 1u) << "Expected symbol " << ExpectedName.str();
    EXPECT_EQ((*Loc)[0].Name, ExpectedName);
    EXPECT_EQ((*Loc)[0].PreferredDeclaration.uri.file(), ExpectedFile);
  };

  auto P = Main.points();
  CheckSym(P[0], "DirectStruct", testPath("inc1/header_a.h"));
  CheckSym(P[1], "DirectClass", testPath("inc1/header_a.h"));
  CheckSym(P[2], "DirectEnum", testPath("inc1/header_a.h"));
  CheckSym(P[3], "DirectAlias", testPath("inc1/header_a.h"));
  CheckSym(P[4], "TransitiveWidget", testPath("inc2/header_b.h"));
}

TEST(PseudoModuleTest, ClangdServerParseInputsAndPathRefFromHeaders) {
  MockFS FS;
  FS.Files[testPath("include/Compiler.h")] = R"cpp(
    struct ParseInputs {
      int Version;
    };
  )cpp";
  FS.Files[testPath("include/support/Path.h")] = R"cpp(
    using PathRef = const char *;
  )cpp";
  FS.Files[testPath("include/TUScheduler.h")] = R"cpp(
    #include "Compiler.h"
  )cpp";
  FS.Files[testPath("include/ClangdServer.h")] = R"cpp(
    #include "support/Path.h"
    #include "TUScheduler.h"

    class ClangdServer {
    public:
      ClangdServer();
      void adjustParseInputs(ParseInputs &Inputs, PathRef File) const;
    };
  )cpp";
  FS.Files[testPath("sys_include/vector")] = R"cpp(
    namespace std { template <typename T> class vector {}; }
  )cpp";

  MockCompilationDatabase CDB(testRoot());
  CDB.ExtraClangFlags = {"-I" + testPath("include"), "-isystem" + testPath("sys_include")};

  PseudoModule Mod;
  Mod.setFSForTesting(&FS);
  Mod.setCompilationDatabaseForTesting(&CDB);

  std::string Path = testPath("ClangdServer.cpp");
  Annotations Code(R"cpp(
    #include "ClangdServer.h"
    #include <vector>

    ClangdServer::ClangdServer() {}

    void ClangdServer::adjustParseInputs(ParseInputs &Inputs, PathRef File) const {}
  )cpp");

  // 1. Jump on #include "ClangdServer.h"
  size_t IncPos = Code.code().find("#include \"ClangdServer.h\"");
  ASSERT_NE(IncPos, llvm::StringRef::npos);
  Position IncPosPoint = offsetToPosition(Code.code(), IncPos + 12);
  auto LocInc = Mod.locateSymbolAt(Path, Code.code(), IncPosPoint);
  ASSERT_TRUE(bool(LocInc)) << llvm::toString(LocInc.takeError());
  ASSERT_EQ(LocInc->size(), 1u);
  EXPECT_EQ((*LocInc)[0].Name, "ClangdServer.h");
  EXPECT_TRUE(llvm::StringRef((*LocInc)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.h"));

  // 2. Jump to ParseInputs (declared in Compiler.h, included via TUScheduler.h)
  size_t ParseInputsPos =
      Code.code().find("void ClangdServer::adjustParseInputs(ParseInputs &Inputs, PathRef File) const");
  ASSERT_NE(ParseInputsPos, llvm::StringRef::npos);
  size_t ParseInputsOffset =
      ParseInputsPos + std::string("void ClangdServer::adjustParseInputs(").size() + 2;
  Position ParseInputsPosition = offsetToPosition(Code.code(), ParseInputsOffset);

  auto LocParseInputs = Mod.locateSymbolAt(Path, Code.code(), ParseInputsPosition);
  ASSERT_TRUE(bool(LocParseInputs)) << llvm::toString(LocParseInputs.takeError());
  ASSERT_EQ(LocParseInputs->size(), 1u);
  EXPECT_EQ((*LocParseInputs)[0].Name, "ParseInputs");
  EXPECT_TRUE(llvm::StringRef((*LocParseInputs)[0].PreferredDeclaration.uri.file())
                  .ends_with("Compiler.h"))
      << "Actual file: "
      << (*LocParseInputs)[0].PreferredDeclaration.uri.file().str();

  // 3. Jump to PathRef (defined in support/Path.h, included via ClangdServer.h)
  size_t PathRefOffset =
      ParseInputsPos +
      std::string("void ClangdServer::adjustParseInputs(ParseInputs &Inputs, ").size() + 2;
  Position PathRefPosition = offsetToPosition(Code.code(), PathRefOffset);

  auto LocPathRef = Mod.locateSymbolAt(Path, Code.code(), PathRefPosition);
  ASSERT_TRUE(bool(LocPathRef)) << llvm::toString(LocPathRef.takeError());
  ASSERT_EQ(LocPathRef->size(), 1u);
  EXPECT_EQ((*LocPathRef)[0].Name, "PathRef");
  EXPECT_TRUE(llvm::StringRef((*LocPathRef)[0].PreferredDeclaration.uri.file())
                  .ends_with("Path.h"))
      << "Actual file: "
      << (*LocPathRef)[0].PreferredDeclaration.uri.file().str();

  // 4. Jump to ClangdServer class qualifier from adjustParseInputs
  // Should jump to class ClangdServer in ClangdServer.h, NOT to constructor in ClangdServer.cpp!
  size_t ClassQualifierPos = ParseInputsPos + std::string("void ").size() + 2;
  Position ClassQualifierPoint = offsetToPosition(Code.code(), ClassQualifierPos);
  auto LocClass = Mod.locateSymbolAt(Path, Code.code(), ClassQualifierPoint);
  ASSERT_TRUE(bool(LocClass)) << llvm::toString(LocClass.takeError());
  ASSERT_EQ(LocClass->size(), 1u);
  EXPECT_EQ((*LocClass)[0].Name, "ClangdServer");
  EXPECT_TRUE(llvm::StringRef((*LocClass)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.h"))
      << "Actual file: "
      << (*LocClass)[0].PreferredDeclaration.uri.file().str();

  // 5. In ClangdServer::ClangdServer:
  // First ClangdServer is class (jumps to ClangdServer.h)
  // Second ClangdServer is constructor definition (jumps to ClangdServer.cpp)
  size_t CtorPos = Code.code().find("ClangdServer::ClangdServer(");
  ASSERT_NE(CtorPos, llvm::StringRef::npos);
  Position CtorClassPoint = offsetToPosition(Code.code(), CtorPos + 2);
  auto LocCtorClass = Mod.locateSymbolAt(Path, Code.code(), CtorClassPoint);
  ASSERT_TRUE(bool(LocCtorClass)) << llvm::toString(LocCtorClass.takeError());
  ASSERT_EQ(LocCtorClass->size(), 1u);
  EXPECT_EQ((*LocCtorClass)[0].Name, "ClangdServer");
  EXPECT_TRUE(llvm::StringRef((*LocCtorClass)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.h"))
      << "Actual file: "
      << (*LocCtorClass)[0].PreferredDeclaration.uri.file().str();

  Position CtorFuncPoint =
      offsetToPosition(Code.code(), CtorPos + std::string("ClangdServer::").size() + 2);
  auto LocCtorFunc = Mod.locateSymbolAt(Path, Code.code(), CtorFuncPoint);
  ASSERT_TRUE(bool(LocCtorFunc)) << llvm::toString(LocCtorFunc.takeError());
  ASSERT_EQ(LocCtorFunc->size(), 1u);
  EXPECT_EQ((*LocCtorFunc)[0].Name, "ClangdServer");
  EXPECT_TRUE(llvm::StringRef((*LocCtorFunc)[0].PreferredDeclaration.uri.file())
                  .ends_with("ClangdServer.cpp"))
      << "Actual file: "
      << (*LocCtorFunc)[0].PreferredDeclaration.uri.file().str();

  // 6. Jump on #include <vector>
  size_t VectorPos = Code.code().find("#include <vector>");
  ASSERT_NE(VectorPos, llvm::StringRef::npos);
  Position VectorPoint = offsetToPosition(Code.code(), VectorPos + 12);
  auto LocVector = Mod.locateSymbolAt(Path, Code.code(), VectorPoint);
  ASSERT_TRUE(bool(LocVector)) << llvm::toString(LocVector.takeError());
  ASSERT_EQ(LocVector->size(), 1u);
  EXPECT_EQ((*LocVector)[0].Name, "vector");
  EXPECT_TRUE(llvm::StringRef((*LocVector)[0].PreferredDeclaration.uri.file())
                  .ends_with("vector"));
}

TEST(PseudoModuleTest, DisambiguateTypeAndValueWithSameName) {
  PseudoModule Mod;
  Annotations Code(R"cpp(
    struct $structDecl[[Foo]] {
      int x;
    };

    void test() {
      ^Foo $varDecl[[Foo]];
      ^Foo.x = 1;
    }
  )cpp");

  std::string File = testPath("test.cpp");
  auto Points = Code.points();
  ASSERT_EQ(Points.size(), 2u);

  // 1. First Foo is in type position -> should locate struct Foo
  auto LocType = Mod.locateSymbolAt(File, Code.code(), Points[0]);
  ASSERT_TRUE(bool(LocType)) << llvm::toString(LocType.takeError());
  ASSERT_EQ(LocType->size(), 1u);
  EXPECT_EQ((*LocType)[0].Name, "Foo");
  EXPECT_EQ((*LocType)[0].PreferredDeclaration.range, Code.range("structDecl"));

  // 2. Second Foo is in value/member-access position -> should locate variable Foo
  auto LocVal = Mod.locateSymbolAt(File, Code.code(), Points[1]);
  ASSERT_TRUE(bool(LocVal)) << llvm::toString(LocVal.takeError());
  ASSERT_EQ(LocVal->size(), 1u);
  EXPECT_EQ((*LocVal)[0].Name, "Foo");
  EXPECT_EQ((*LocVal)[0].PreferredDeclaration.range, Code.range("varDecl"));
}

TEST(PseudoModuleTest, SlideExample2) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    #define USE_V2 1
    #if USE_V2
    class $serviceDecl[[DataService]] {
    #else
    class LegacyService {
    #endif
    public:
      void $syncDecl[[sync]]();
    };
    void run() {
      ^DataService svc;
      svc.^sync();
    }
  )cpp");
  auto Points = Code.points();
  auto LocSvc = Mod.locateSymbolAt(File, Code.code(), Points[0]);
  ASSERT_TRUE(bool(LocSvc)) << "Ex2 svc error: " << llvm::toString(LocSvc.takeError());
  ASSERT_EQ(LocSvc->size(), 1u) << "Ex2 svc size";
  EXPECT_EQ((*LocSvc)[0].Name, "DataService");
  EXPECT_EQ((*LocSvc)[0].PreferredDeclaration.range, Code.range("serviceDecl"));

  auto LocSync = Mod.locateSymbolAt(File, Code.code(), Points[1]);
  ASSERT_TRUE(bool(LocSync)) << "Ex2 sync error: " << llvm::toString(LocSync.takeError());
  ASSERT_EQ(LocSync->size(), 1u) << "Ex2 sync size";
  EXPECT_EQ((*LocSync)[0].Name, "sync");
  EXPECT_EQ((*LocSync)[0].PreferredDeclaration.range, Code.range("syncDecl"));
}

TEST(PseudoModuleTest, SlideExample3) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    class $serverDecl[[Server]] {
    public:
      Server(int port);
      void start();
    };
    void ^Server::start() {}
    Server::Server(int port) {}
  )cpp");
  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << "Ex3 error: " << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u) << "Ex3 size";
  EXPECT_EQ((*Loc)[0].Name, "Server");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("serverDecl"));
}

TEST(PseudoModuleTest, SlideExample4) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    #define DECLARE_SERVICE(Name) \
    public: static const char* id() { return #Name; } private:

    class AuthManager {
      DECLARE_SERVICE(AuthManager)
    public:
      void $authDecl[[authenticate]]();
    };

    void handleLogin(AuthManager* mgr) {
      mgr->^authenticate();
    }
  )cpp");
  auto Syms = Mod.getDocumentSymbols(Code.code());
  ASSERT_TRUE(bool(Syms)) << llvm::toString(Syms.takeError());
  ASSERT_EQ(Syms->size(), 2u);
  EXPECT_EQ((*Syms)[0].name, "AuthManager");
  ASSERT_EQ((*Syms)[0].children.size(), 1u);
  EXPECT_EQ((*Syms)[0].children[0].name, "authenticate");

  auto Loc = Mod.locateSymbolAt(File, Code.code(), Code.point());
  ASSERT_TRUE(bool(Loc)) << "Ex4 error: " << llvm::toString(Loc.takeError());
  ASSERT_EQ(Loc->size(), 1u) << "Ex4 size";
  EXPECT_EQ((*Loc)[0].Name, "authenticate");
  EXPECT_EQ((*Loc)[0].PreferredDeclaration.range, Code.range("authDecl"));
}

TEST(PseudoModuleTest, DataServiceOutOfClassDefinition) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    #define USE_V2 1
    #if USE_V2
    class $serviceDecl[[DataService]] {
    #else
    class LegacyService {
    #endif
    public:
      void $syncDecl[[sync]]();
    };

    void DataService::$syncDef[[sync]]() {
      int work = 42;
    }

    void run() {
      $svcDecl[[DataService]] svc;
      svc.$callSync[[sync]]();
    }
  )cpp");

  // 1. locateSymbolAt on svc.sync() call
  Position CallSyncPos = Code.range("callSync").start;
  auto LocCallSync = Mod.locateSymbolAt(File, Code.code(), CallSyncPos);
  ASSERT_TRUE(bool(LocCallSync)) << llvm::toString(LocCallSync.takeError());
  ASSERT_EQ(LocCallSync->size(), 1u);
  EXPECT_EQ((*LocCallSync)[0].Name, "sync");
  EXPECT_EQ((*LocCallSync)[0].PreferredDeclaration.range, Code.range("syncDecl"));
  ASSERT_TRUE((*LocCallSync)[0].Definition.has_value());
  EXPECT_EQ((*LocCallSync)[0].Definition->range, Code.range("syncDef"));

  // 2. locateSymbolAt on in-class declaration void sync();
  Position DeclPos = Code.range("syncDecl").start;
  auto LocDecl = Mod.locateSymbolAt(File, Code.code(), DeclPos);
  ASSERT_TRUE(bool(LocDecl)) << llvm::toString(LocDecl.takeError());
  ASSERT_EQ(LocDecl->size(), 1u);
  EXPECT_EQ((*LocDecl)[0].Name, "sync");
  EXPECT_EQ((*LocDecl)[0].PreferredDeclaration.range, Code.range("syncDecl"));
  ASSERT_TRUE((*LocDecl)[0].Definition.has_value());
  EXPECT_EQ((*LocDecl)[0].Definition->range, Code.range("syncDef"));

  // 3. locateSymbolAt on out-of-class definition void DataService::sync()
  Position DefPos = Code.range("syncDef").start;
  auto LocDef = Mod.locateSymbolAt(File, Code.code(), DefPos);
  ASSERT_TRUE(bool(LocDef)) << llvm::toString(LocDef.takeError());
  ASSERT_EQ(LocDef->size(), 1u);
  EXPECT_EQ((*LocDef)[0].Name, "sync");
  EXPECT_EQ((*LocDef)[0].PreferredDeclaration.range, Code.range("syncDecl"));
  ASSERT_TRUE((*LocDef)[0].Definition.has_value());
  EXPECT_EQ((*LocDef)[0].Definition->range, Code.range("syncDef"));

  // 4. Test Go-To-Definition and Go-To-Declaration via ClangdServer / LSP
  MockFS FS;
  auto CDB = std::make_unique<MockCompilationDatabase>();
  FeatureModuleSet Modules;
  auto Pseudo = std::make_unique<PseudoModule>();
  auto *PseudoPtr = Pseudo.get();
  PseudoPtr->setPseudoOnly(true);
  Modules.add(std::move(Pseudo));

  ClangdServer::Options SvrOpts = ClangdServer::optsForTest();
  SvrOpts.FeatureModules = &Modules;
  ClangdServer Server(*CDB, FS, SvrOpts);

  FS.Files[File] = Code.code().str();
  Server.addDocument(File, Code.code());
  ASSERT_TRUE(Server.blockUntilIdleForTest());

  // (a) Go-to-Definition on svc.sync() -> should jump to out-of-class definition (syncDef)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = CallSyncPos;
    std::optional<llvm::Expected<std::vector<Location>>> DefResult;
    PseudoPtr->onGoToDefinition(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          DefResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(DefResult.has_value());
    ASSERT_TRUE(bool(*DefResult)) << llvm::toString(DefResult->takeError());
    ASSERT_EQ((*DefResult)->size(), 1u);
    EXPECT_EQ((*DefResult)->front().range, Code.range("syncDef"));
  }

  // (b) Go-to-Declaration on svc.sync() -> should jump to in-class declaration (syncDecl)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = CallSyncPos;
    std::optional<llvm::Expected<std::vector<Location>>> DeclResult;
    PseudoPtr->onGoToDeclaration(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          DeclResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(DeclResult.has_value());
    ASSERT_TRUE(bool(*DeclResult)) << llvm::toString(DeclResult->takeError());
    ASSERT_EQ((*DeclResult)->size(), 1u);
    EXPECT_EQ((*DeclResult)->front().range, Code.range("syncDecl"));
  }

  // (c) Go-to-Definition while on out-of-class definition (syncDef) -> should toggle to declaration (syncDecl)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = DefPos;
    std::optional<llvm::Expected<std::vector<Location>>> ToggleResult;
    PseudoPtr->onGoToDefinition(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          ToggleResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(ToggleResult.has_value());
    ASSERT_TRUE(bool(*ToggleResult)) << llvm::toString(ToggleResult->takeError());
    ASSERT_EQ((*ToggleResult)->size(), 1u);
    EXPECT_EQ((*ToggleResult)->front().range, Code.range("syncDecl"));
  }

  // (d) Go-to-Definition while on in-class declaration (syncDecl) -> should toggle to definition (syncDef)
  {
    TextDocumentPositionParams PosParams;
    PosParams.textDocument.uri = URIForFile::canonicalize(File, File);
    PosParams.position = DeclPos;
    std::optional<llvm::Expected<std::vector<Location>>> ToggleResult;
    PseudoPtr->onGoToDefinition(
        PosParams, [&](llvm::Expected<std::vector<Location>> Locs) {
          ToggleResult = std::move(Locs);
        });
    ASSERT_TRUE(Server.blockUntilIdleForTest());
    ASSERT_TRUE(ToggleResult.has_value());
    ASSERT_TRUE(bool(*ToggleResult)) << llvm::toString(ToggleResult->takeError());
    ASSERT_EQ((*ToggleResult)->size(), 1u);
    EXPECT_EQ((*ToggleResult)->front().range, Code.range("syncDef"));
  }
}

TEST(PseudoModuleTest, TemplateWithRequiresClause) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    template <typename T>
    requires (sizeof(T) > 4)
    class $decl[[DataBuffer]] {
    public:
      void $writeDecl[[write]](T data);
    };

    void run() {
      $typeUsage[[DataBuffer]]<double> buf;
      buf.$callWrite[[write]](3.14);
    }
  )cpp");

  auto Syms = Mod.getDocumentSymbols(Code.code());
  ASSERT_TRUE(bool(Syms)) << llvm::toString(Syms.takeError());
  ASSERT_GE(Syms->size(), 2u);
  EXPECT_EQ((*Syms)[0].name, "DataBuffer");
  ASSERT_GE((*Syms)[0].children.size(), 1u);
  EXPECT_EQ((*Syms)[0].children[0].name, "write");

  // (a) Go-to-Definition on DataBuffer
  auto Loc1 = Mod.locateSymbolAt(File, Code.code(), Code.range("typeUsage").start);
  ASSERT_TRUE(bool(Loc1)) << llvm::toString(Loc1.takeError());
  ASSERT_EQ(Loc1->size(), 1u);
  EXPECT_EQ((*Loc1)[0].Name, "DataBuffer");
  EXPECT_EQ((*Loc1)[0].PreferredDeclaration.range, Code.range("decl"));

  // (b) Go-to-Definition on write()
  auto Loc2 = Mod.locateSymbolAt(File, Code.code(), Code.range("callWrite").start);
  ASSERT_TRUE(bool(Loc2)) << llvm::toString(Loc2.takeError());
  ASSERT_EQ(Loc2->size(), 1u);
  EXPECT_EQ((*Loc2)[0].Name, "write");
  EXPECT_EQ((*Loc2)[0].PreferredDeclaration.range, Code.range("writeDecl"));
}

TEST(PseudoModuleTest, ConceptAndConstrainedTemplate) {
  PseudoModule Mod;
  std::string File = testPath("test.cpp");
  Annotations Code(R"cpp(
    template <typename T>
    concept $conceptDecl[[Serializable]] = requires(T x) {
      x.serialize();
    };

    template <$conceptUsage[[Serializable]] T>
    class $pipelineDecl[[DataPipeline]] {
    public:
      void $processDecl[[process]](T data);
    };

    void run() {
      $pipelineUsage[[DataPipeline]]<int> pipeline;
      pipeline.$callProcess[[process]](42);
    }
  )cpp");

  auto Syms = Mod.getDocumentSymbols(Code.code());
  ASSERT_TRUE(bool(Syms)) << llvm::toString(Syms.takeError());
  bool FoundPipeline = false;
  for (const auto &S : *Syms) {
    if (S.name == "DataPipeline")
      FoundPipeline = true;
  }
  EXPECT_TRUE(FoundPipeline);

  // (a) Go-to-Definition on DataPipeline
  auto Loc1 = Mod.locateSymbolAt(File, Code.code(), Code.range("pipelineUsage").start);
  ASSERT_TRUE(bool(Loc1)) << llvm::toString(Loc1.takeError());
  ASSERT_EQ(Loc1->size(), 1u);
  EXPECT_EQ((*Loc1)[0].Name, "DataPipeline");
  EXPECT_EQ((*Loc1)[0].PreferredDeclaration.range, Code.range("pipelineDecl"));

  // (b) Go-to-Definition on concept Serializable
  auto Loc2 = Mod.locateSymbolAt(File, Code.code(), Code.range("conceptUsage").start);
  ASSERT_TRUE(bool(Loc2)) << llvm::toString(Loc2.takeError());
  ASSERT_EQ(Loc2->size(), 1u);
  EXPECT_EQ((*Loc2)[0].Name, "Serializable");
  EXPECT_EQ((*Loc2)[0].PreferredDeclaration.range, Code.range("conceptDecl"));
}

} // namespace
} // namespace clangd
} // namespace clang
