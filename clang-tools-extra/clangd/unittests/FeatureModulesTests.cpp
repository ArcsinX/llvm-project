//===--- FeatureModulesTests.cpp  -------------------------------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "Annotations.h"
#include "ClangTidyFeatureModule.h"
#include "Compiler.h"
#include "Feature.h"
#include "FeatureModule.h"
#include "Selection.h"
#include "TestFS.h"
#include "TestTU.h"
#include "refactor/Tweak.h"
#include "support/Logger.h"
#include "clang/AST/Decl.h"
#include "clang/Basic/DiagnosticFrontend.h"
#include "clang/Basic/DiagnosticLex.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/FrontendOptions.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/PPCallbacks.h"
#include "clang/Lex/Preprocessor.h"
#include "llvm/Support/Error.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include <functional>
#include <memory>

namespace clang {
namespace clangd {
namespace {

struct TestModule final : FeatureModule {
  struct Listener final : ASTListener {
    Listener(TestModule &Module) : Module(Module) {}

    void beforeBeginSourceFile(CompilerInstance &CI) override {
      if (Module.BeforeBeginSourceFile)
        Module.BeforeBeginSourceFile(CI);
    }
    void beforePPCallbacks(CompilerInstance &CI) override {
      if (Module.BeforePPCallbacks)
        Module.BeforePPCallbacks(CI);
    }
    void beforeExecute(CompilerInstance &CI) override {
      if (Module.BeforeExecute)
        Module.BeforeExecute(CI);
    }
    void afterExecute(CompilerInstance &CI) override {
      if (Module.AfterExecute)
        Module.AfterExecute(CI);
    }
    void sawDiagnostic(const clang::Diagnostic &Info,
                       clangd::Diag &Diag) override {
      if (Module.SawDiagnostic)
        Module.SawDiagnostic(Info, Diag);
    }
    void finalizeDiagnostic(clangd::Diag &Diag) override {
      if (Module.FinalizeDiagnostic)
        Module.FinalizeDiagnostic(Diag);
    }

  private:
    TestModule &Module;
  };

  std::unique_ptr<ASTListener> astListeners() override {
    return std::make_unique<Listener>(*this);
  }

  std::function<void(CompilerInstance &)> BeforeBeginSourceFile;
  std::function<void(CompilerInstance &)> BeforePPCallbacks;
  std::function<void(CompilerInstance &)> BeforeExecute;
  std::function<void(CompilerInstance &)> AfterExecute;
  std::function<void(const clang::Diagnostic &, clangd::Diag &)> SawDiagnostic;
  std::function<void(clangd::Diag &)> FinalizeDiagnostic;
};

TEST(FeatureModulesTest, ContributesTweak) {
  static constexpr const char *TweakID = "ModuleTweak";
  struct TweakContributingModule final : public FeatureModule {
    struct ModuleTweak final : public Tweak {
      const char *id() const override { return TweakID; }
      bool prepare(const Selection &Sel) override { return true; }
      Expected<Effect> apply(const Selection &Sel) override {
        return error("not implemented");
      }
      std::string title() const override { return id(); }
      llvm::StringLiteral kind() const override {
        return llvm::StringLiteral("");
      };
    };

    void contributeTweaks(std::vector<std::unique_ptr<Tweak>> &Out) override {
      Out.emplace_back(new ModuleTweak);
    }
  };

  FeatureModuleSet Set;
  Set.add(std::make_unique<TweakContributingModule>());

  auto AST = TestTU::withCode("").build();
  auto Tree =
      SelectionTree::createRight(AST.getASTContext(), AST.getTokens(), 0, 0);
  auto Actual = prepareTweak(
      TweakID, Tweak::Selection(nullptr, AST, 0, 0, std::move(Tree), nullptr),
      &Set);
  ASSERT_TRUE(bool(Actual));
  EXPECT_EQ(Actual->get()->id(), TweakID);
}

TEST(FeatureModulesTest, SuppressDiags) {
  struct DiagModifierModule final : public FeatureModule {
    struct Listener : public FeatureModule::ASTListener {
      void sawDiagnostic(const clang::Diagnostic &Info,
                         clangd::Diag &Diag) override {
        Diag.Severity = DiagnosticsEngine::Ignored;
      }
    };
    std::unique_ptr<ASTListener> astListeners() override {
      return std::make_unique<Listener>();
    };
  };
  FeatureModuleSet FMS;
  FMS.add(std::make_unique<DiagModifierModule>());

  Annotations Code("[[test]]; /* error-ok */");
  TestTU TU;
  TU.Code = Code.code().str();

  {
    auto AST = TU.build();
    EXPECT_THAT(AST.getDiagnostics(), testing::Not(testing::IsEmpty()));
  }

  TU.FeatureModules = &FMS;
  {
    auto AST = TU.build();
    EXPECT_THAT(AST.getDiagnostics(), testing::IsEmpty());
  }
}

TEST(FeatureModulesTest, BeforeBeginSourceFile) {
  std::vector<frontend::ActionKind> Builds;
  auto Module = std::make_unique<TestModule>();
  Module->BeforeBeginSourceFile = [&](CompilerInstance &CI) {
    Builds.push_back(CI.getFrontendOpts().ProgramAction);
  };
  FeatureModuleSet Modules;
  Modules.add(std::move(Module));
  auto TU = TestTU::withCode(R"cpp(
    #include "header.h"
    HeaderType value;
  )cpp");
  TU.AdditionalFiles["header.h"] = "struct HeaderType {};";
  TU.FeatureModules = &Modules;
  EXPECT_THAT(TU.build().getDiagnostics(), testing::IsEmpty());
  // The preamble is built from header.h, but only the main-file build calls
  // this hook.
  EXPECT_THAT(Builds, testing::ElementsAre(frontend::ParseSyntaxOnly));
}

TEST(FeatureModulesTest, BeforeBeginSourceFileDiagnostics) {
  unsigned SeenDiagnostics = 0;
  auto Module = std::make_unique<TestModule>();
  Module->BeforeBeginSourceFile = [](CompilerInstance &CI) {
    // The newline warning is emitted while BeginSourceFile initializes macros,
    // so beforePPCallbacks and beforeExecute would be too late to promote it.
    CI.getDiagnostics().setSeverity(
        diag::warn_fe_macro_contains_embedded_newline, diag::Severity::Error,
        SourceLocation());
  };
  Module->SawDiagnostic = [&](const clang::Diagnostic &Info, clangd::Diag &) {
    if (Info.getID() == diag::warn_fe_macro_contains_embedded_newline)
      ++SeenDiagnostics;
  };
  FeatureModuleSet Modules;
  Modules.add(std::move(Module));

  auto TU = TestTU::withCode("int value;");
  TU.ExtraArgs = {"-DMACRO=first\nsecond"};
  TU.FeatureModules = &Modules;
  auto AST = TU.build();
  // clangd filters out this location-less diagnostic even when promoted, so
  // check Clang's error count to verify that it was emitted as an error.
  EXPECT_EQ(AST.getPreprocessor().getDiagnostics().getNumErrors(), 1u);
  // StoreDiags filters it out before sawDiagnostic: it has no source location
  // and is a warning by default, despite being promoted to an error here.
  EXPECT_EQ(SeenDiagnostics, 0u);
}

TEST(FeatureModulesTest, BeforePPCallbacks) {
  struct IncludeRecorder : public PPCallbacks {
    IncludeRecorder(std::vector<std::string> &Includes) : Includes(Includes) {}

    void InclusionDirective(SourceLocation, const Token &, StringRef FileName,
                            bool, CharSourceRange, OptionalFileEntryRef,
                            StringRef, StringRef, const clang::Module *, bool,
                            SrcMgr::CharacteristicKind) override {
      Includes.push_back(FileName.str());
    }

  private:
    std::vector<std::string> &Includes;
  };
  std::vector<std::string> Includes;
  auto Module = std::make_unique<TestModule>();
  Module->BeforePPCallbacks = [&Includes](CompilerInstance &CI) {
    // The preamble build processes the main file's initial directives,
    // including #include "header.h", and the included header's contents. The
    // main-file build reuses that preamble and skips those directives.
    // ReplayPreamble synthesizes InclusionDirective callbacks for the saved
    // direct includes. Register only during the main-file build to observe this
    // replay, rather than the original include during preamble construction.
    if (CI.getFrontendOpts().ProgramAction == frontend::ParseSyntaxOnly)
      CI.getPreprocessor().addPPCallbacks(
          std::make_unique<IncludeRecorder>(Includes));
  };
  FeatureModuleSet FMS;
  FMS.add(std::move(Module));

  TestTU TU = TestTU::withCode(R"cpp(
    #include "header.h"
    void mainFileFunc(); // Ends the preamble; parsed during the main-file build.
  )cpp");
  TU.AdditionalFiles["header.h"] = "";
  TU.FeatureModules = &FMS;
  TU.build();
  EXPECT_THAT(Includes, testing::ElementsAre("header.h"));
}

TEST(FeatureModulesTest, BeforeExecute) {
  auto Module = std::make_unique<TestModule>();
  Module->BeforeExecute = [](CompilerInstance &CI) {
    CI.getPreprocessor().SetSuppressIncludeNotFoundError(true);
  };
  FeatureModuleSet FMS;
  FMS.add(std::move(Module));

  TestTU TU = TestTU::withCode(R"cpp(
    /*error-ok*/
    #include "not_found.h"

    void foo() {
      #include "not_found_not_preamble.h"
    }
  )cpp");

  {
    auto AST = TU.build();
    EXPECT_THAT(AST.getDiagnostics(), testing::Not(testing::IsEmpty()));
  }

  TU.FeatureModules = &FMS;
  {
    auto AST = TU.build();
    EXPECT_THAT(AST.getDiagnostics(), testing::IsEmpty());
  }
}

TEST(FeatureModulesTest, AfterExecute) {
  std::vector<std::string> DeclNames;
  auto Module = std::make_unique<TestModule>();
  Module->AfterExecute = [&DeclNames](CompilerInstance &CI) {
    for (Decl *D : CI.getASTContext().getTraversalScope())
      if (const auto *ND = llvm::dyn_cast<NamedDecl>(D))
        DeclNames.push_back(ND->getNameAsString());
  };
  FeatureModuleSet FMS;
  FMS.add(std::move(Module));

  TestTU TU = TestTU::withCode(R"cpp(
    #include "header.h"
    void mainFileFunc();
  )cpp");
  TU.AdditionalFiles["header.h"] = "void headerFunc();";
  TU.FeatureModules = &FMS;
  TU.build();

  // afterExecute runs once clangd has restricted the traversal scope, so the
  // declaration from the header is intentionally not visible here.
  EXPECT_THAT(DeclNames, testing::ElementsAre("mainFileFunc"));
}

TEST(FeatureModulesTest, FinalizeDiagnostic) {
  unsigned Notes = 0;
  unsigned Fixes = 0;
  auto Module = std::make_unique<TestModule>();
  Module->FinalizeDiagnostic = [&](clangd::Diag &Diag) {
    if (Diag.Message.find("undeclared identifier 'fooo'") == std::string::npos)
      return;
    Notes = Diag.Notes.size();
    Fixes = Diag.Fixes.size();
  };
  FeatureModuleSet FMS;
  FMS.add(std::move(Module));

  TestTU TU = TestTU::withCode(R"cpp(
    void foo();
    void bar() { fooo(); } // error-ok
  )cpp");
  TU.FeatureModules = &FMS;
  EXPECT_THAT(TU.build().getDiagnostics(), testing::SizeIs(1));
  EXPECT_EQ(Notes, 1u);
  EXPECT_EQ(Fixes, 1u);
}

TEST(FeatureModulesTest, ClangTidyWarningOptionsBeforeBeginSourceFile) {
  auto Build = [](bool Suppress, bool ExtraArgsBefore) {
    unsigned Warnings = 0;
    auto Observer = std::make_unique<TestModule>();
    Observer->SawDiagnostic = [&](const clang::Diagnostic &Info,
                                  clangd::Diag &) {
      if (Info.getID() == diag::warn_mmap_umbrella_dir_not_found)
        ++Warnings;
    };
    FeatureModuleSet Modules;
    Modules.add(std::move(Observer));
    Modules.add(std::make_unique<ClangTidyFeatureModule>(
        [=](tidy::ClangTidyOptions &Opts, llvm::StringRef) {
          Opts.Checks = "-*";
          if (Suppress) {
            auto &Args =
                ExtraArgsBefore ? Opts.ExtraArgsBefore : Opts.ExtraArgs;
            Args = {"-Wno-incomplete-umbrella"};
          }
        }));
    auto TU = TestTU::withCode("int value;");
    TU.FeatureModules = &Modules;
    TU.ExtraArgs = {"-fmodules",
                    "-fmodule-map-file=" + testPath("module.modulemap")};
    TU.AdditionalFiles["module.modulemap"] =
        R"(module M { umbrella "missing" })";

    // Observe module-map warnings from BeginSourceFile directly: StoreDiags
    // may drop them because they are outside the main file. Don't build a PCH.
    MockFS FS;
    auto Inputs = TU.inputs(FS);
    IgnoreDiagnostics Diags;
    auto CI = buildCompilerInvocation(Inputs, Diags);
    ASSERT_TRUE(CI);
    ASSERT_TRUE(ParsedAST::build(testPath(TU.Filename), Inputs, std::move(CI),
                                 {}, nullptr));
    EXPECT_EQ(Warnings, Suppress ? 0u : 1u);
  };
  Build(false, false);
  Build(true, false);
  Build(true, true);
}

TEST(FeatureModulesTest, ClangTidyRelativeInput) {
  if (!CLANGD_TIDY_CHECKS)
    GTEST_SKIP() << "Requires clang-tidy checks";
  MockFS FS;
  auto TU = TestTU::withCode("int *p = 0;");
  TU.Filename = "project/src/test.cpp";
  TU.AdditionalFiles["project/src/.clang-tidy"] =
      "Checks: '-*,modernize-use-nullptr'";
  auto Provider = provideClangTidyFiles(FS);
  FeatureModuleSet Modules;
  Modules.add(std::make_unique<ClangTidyFeatureModule>(
      [&](tidy::ClangTidyOptions &Opts, llvm::StringRef Filename) {
        EXPECT_EQ(Filename, testPath(TU.Filename));
        Provider(Opts, Filename);
      }));
  TU.FeatureModules = &Modules;
  auto Inputs = TU.inputs(FS);
  Inputs.CompileCommand.Directory = testPath("project");
  // Configuration lookup must use the requested file, not the frontend input.
  for (auto InputFile : {"src/./test.cpp", "../other/test.cpp"}) {
    SCOPED_TRACE(InputFile);
    IgnoreDiagnostics Diags;
    auto CI = buildCompilerInvocation(Inputs, Diags);
    ASSERT_TRUE(CI);
    auto Kind = CI->getFrontendOpts().Inputs.front().getKind();
    CI->getFrontendOpts().Inputs = {FrontendInputFile(InputFile, Kind)};
    auto AST = ParsedAST::build(testPath(TU.Filename), Inputs, std::move(CI),
                                {}, nullptr);
    ASSERT_TRUE(AST);
    ASSERT_THAT(AST->getDiagnostics(), testing::SizeIs(1));
    EXPECT_EQ(AST->getDiagnostics().front().Name, "modernize-use-nullptr");
  }
}

TEST(FeatureModulesTest, ClangTidyTemporaryProvider) {
  if (!CLANGD_TIDY_CHECKS)
    GTEST_SKIP() << "Requires clang-tidy checks";
  FeatureModuleSet Modules;
  Modules.add(std::make_unique<ClangTidyFeatureModule>(
      addTidyChecks("modernize-use-nullptr")));
  auto TU = TestTU::withCode("int *p = 0;");
  TU.FeatureModules = &Modules;
  auto *Tidy = Modules.get<ClangTidyFeatureModule>();
  auto Provider = std::make_shared<const TidyProvider>(addTidyChecks("-*"));
  // Check-mode timing temporarily overrides tidy's configuration, then
  // restores the original provider for subsequent builds.
  Tidy->swapProvider(Provider);
  EXPECT_THAT(TU.build().getDiagnostics(), testing::IsEmpty());
  Tidy->swapProvider(Provider);
  auto AST = TU.build();
  ASSERT_THAT(AST.getDiagnostics(), testing::SizeIs(1));
  EXPECT_EQ(AST.getDiagnostics().front().Name, "modernize-use-nullptr");
}

TEST(FeatureModulesTest, ClangTidyDoesNotReinitializeConsumer) {
  if (!CLANGD_TIDY_CHECKS)
    GTEST_SKIP() << "Requires clang-tidy checks";
  class InitializationGuard final : public MultiplexConsumer {
  public:
    using MultiplexConsumer::MultiplexConsumer;
    void Initialize(ASTContext &Ctx) override {
      EXPECT_FALSE(Initialized);
      Initialized = true;
      MultiplexConsumer::Initialize(Ctx);
    }

  private:
    bool Initialized = false;
  };
  auto Module = std::make_unique<TestModule>();
  Module->BeforePPCallbacks = [](CompilerInstance &CI) {
    if (CI.getFrontendOpts().ProgramAction == frontend::ParseSyntaxOnly)
      CI.setASTConsumer(
          std::make_unique<InitializationGuard>(CI.takeASTConsumer()));
  };
  FeatureModuleSet Modules;
  Modules.add(std::move(Module));
  auto TU = TestTU::withCode("int *p = 0;");
  TU.FeatureModules = &Modules;
  Modules.add(std::make_unique<ClangTidyFeatureModule>(
      addTidyChecks("modernize-use-nullptr")));
  // Installing tidy's multiplexer must not initialize the existing consumer
  // again after setASTConsumer() has already initialized it above.
  EXPECT_THAT(TU.build().getDiagnostics(), testing::SizeIs(1));
}

} // namespace
} // namespace clangd
} // namespace clang
