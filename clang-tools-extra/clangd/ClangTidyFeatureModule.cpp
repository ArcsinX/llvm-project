//===--- ClangTidyFeatureModule.cpp - clang-tidy integration -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "ClangTidyFeatureModule.h"
#include "../clang-tidy/ClangTidyCheck.h"
#include "../clang-tidy/ClangTidyDiagnosticConsumer.h"
#include "../clang-tidy/ClangTidyModule.h"
#include "../clang-tidy/ClangTidyOptions.h"
#include "Config.h"
#include "Diagnostics.h"
#include "Feature.h"
#include "SourceCode.h"
#include "support/Logger.h"
#include "support/Trace.h"
#include "clang/AST/ASTConsumer.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Core/Diagnostic.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include <cassert>
#include <memory>
#include <optional>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

// Force the linker to link in clang-tidy modules. clangd doesn't support the
// static analyzer.
#if CLANGD_TIDY_CHECKS
#define CLANG_TIDY_DISABLE_STATIC_ANALYZER_CHECKS
#include "../clang-tidy/ClangTidyForceLinker.h"
#endif

#if CLANG_TIDY_ENABLE_QUERY_BASED_CUSTOM_CHECKS
#include "../clang-tidy/ClangTidy.h"
#include <mutex>
#endif

namespace clang {
namespace clangd {
namespace {

// Filters clang diagnostic groups enabled by ClangTidyOptions::Checks.
class TidyDiagnosticGroups {
  bool Default = false;
  llvm::DenseSet<unsigned> Exceptions;

public:
  TidyDiagnosticGroups(llvm::StringRef Checks) {
    constexpr llvm::StringLiteral Prefix = "clang-diagnostic-";
    llvm::StringRef Check;
    while (!Checks.empty()) {
      std::tie(Check, Checks) = Checks.split(',');
      Check = Check.trim();
      if (Check.empty())
        continue;

      bool Enable = !Check.consume_front("-");
      bool Glob = Check.consume_back("*");
      if (Glob) {
        if (Prefix.starts_with(Check)) {
          Default = Enable;
          Exceptions.clear();
        }
        continue;
      }
      if (Default == Enable || !Check.consume_front(Prefix))
        continue;
      if (auto Group = DiagnosticIDs::getGroupForWarningOption(Check))
        Exceptions.insert(static_cast<unsigned>(*Group));
    }
  }

  bool operator()(diag::Group Group) const {
    return Exceptions.contains(static_cast<unsigned>(Group)) ? !Default
                                                             : Default;
  }
};

void applyWarningOptions(llvm::ArrayRef<std::string> ExtraArgs,
                         llvm::function_ref<bool(diag::Group)> EnabledGroups,
                         DiagnosticsEngine &Diags) {
  for (llvm::StringRef Group : ExtraArgs) {
    llvm::SmallVector<diag::kind> Members;
    if (!Group.consume_front("-W") || Group.empty())
      continue;
    bool Enable = !Group.consume_front("no-");
    if (Diags.getDiagnosticIDs()->getDiagnosticsInGroup(
            diag::Flavor::WarningOrError, Group, Members))
      continue;

    bool NeedsWerrorExclusion = false;
    for (diag::kind ID : Members) {
      if (Enable) {
        if (Diags.getDiagnosticLevel(ID, SourceLocation()) >=
            DiagnosticsEngine::Warning)
          continue;
        auto DiagGroup = Diags.getDiagnosticIDs()->getGroupForDiag(ID);
        if (!DiagGroup || !EnabledGroups(*DiagGroup))
          continue;
        Diags.setSeverity(ID, diag::Severity::Warning, SourceLocation());
        NeedsWerrorExclusion |= Diags.getWarningsAsErrors();
      } else {
        Diags.setSeverity(ID, diag::Severity::Ignored, SourceLocation());
      }
    }
    if (NeedsWerrorExclusion)
      Diags.setDiagnosticGroupWarningAsError(Group, false);
  }
}

tidy::ClangTidyCheckFactories
filterFastChecks(const tidy::ClangTidyCheckFactories &All,
                 Config::FastCheckPolicy Policy) {
  if (Policy == Config::FastCheckPolicy::None)
    return All;
  bool AllowUnknown = Policy == Config::FastCheckPolicy::Loose;
  tidy::ClangTidyCheckFactories Fast;
  for (const auto &Factory : All)
    if (isFastTidyCheck(Factory.getKey()).value_or(AllowUnknown))
      Fast.registerCheckFactory(Factory.first(), Factory.second);
  return Fast;
}

/// MatchFinder normally runs from HandleTranslationUnit(), before clangd has
/// consumed its token stream and restricted AST traversal to main-file decls.
/// Delay that one callback until ASTListener::afterExecute().
class DeferredASTConsumer final : public ASTConsumer {
public:
  explicit DeferredASTConsumer(std::unique_ptr<ASTConsumer> Delegate)
      : Delegate(std::move(Delegate)) {}

  void Initialize(ASTContext &Ctx) override { Delegate->Initialize(Ctx); }

  void HandleTranslationUnit(ASTContext &Ctx) override { Pending = &Ctx; }

  void run() {
    if (!Pending)
      return;
    Delegate->HandleTranslationUnit(*Pending);
    Pending = nullptr;
  }

private:
  std::unique_ptr<ASTConsumer> Delegate;
  ASTContext *Pending = nullptr;
};

// Installed after BeginSourceFile(), when the existing consumer has already
// been initialized. setASTConsumer() initializes the replacement immediately.
class TidyMultiplexConsumer final : public MultiplexConsumer {
public:
  explicit TidyMultiplexConsumer(
      std::vector<std::unique_ptr<ASTConsumer>> Consumers)
      : MultiplexConsumer(std::move(Consumers)) {
    assert(this->Consumers.size() == 2);
  }

  void Initialize(ASTContext &Ctx) override {
    // Only the newly added tidy consumer needs initialization.
    Consumers.back()->Initialize(Ctx);
  }
};

class TidyASTListener final : public FeatureModule::ASTListener {
public:
  explicit TidyASTListener(std::shared_ptr<const TidyProvider> OptionsProvider)
      : Provider(std::move(OptionsProvider)) {}

  void beforeBeginSourceFile(CompilerInstance &CI) override {
    // Warning options must precede frontend initialization: module loading in
    // BeginSourceFile() can consult their severity to decide whether to fail.
    llvm::StringRef Filename = CI.getFrontendOpts().Inputs.front().getFile();
    trace::Span Tracer("ClangTidyOpts");
    Options = getTidyOptionsForFile(*Provider, Filename);
    dlog("ClangTidy configuration for file {0}: {1}", Filename,
         tidy::configurationAsText(*Options));

    TidyDiagnosticGroups EnabledGroups(Options->Checks ? *Options->Checks
                                                       : llvm::StringRef());
    if (Options->ExtraArgsBefore)
      applyWarningOptions(*Options->ExtraArgsBefore, EnabledGroups,
                          CI.getDiagnostics());
    if (Options->ExtraArgs)
      applyWarningOptions(*Options->ExtraArgs, EnabledGroups,
                          CI.getDiagnostics());
  }

  void beforePPCallbacks(CompilerInstance &CI) override {
    // Preamble builds don't call beforeBeginSourceFile(). Running tidy there
    // would duplicate diagnostics and interfere with PCH generation.
    if (!Options)
      return;

    assert(CI.hasASTConsumer());
    llvm::StringRef Filename = CI.getFrontendOpts().Inputs.front().getFile();

    trace::Span Tracer("ClangTidyInit");
    static const auto *AllFactories = [] {
      auto *Factories = new tidy::ClangTidyCheckFactories;
      for (const auto &Entry : tidy::ClangTidyModuleRegistry::entries())
        Entry.instantiate()->addCheckFactories(*Factories);
      return Factories;
    }();
    Context.emplace(
        std::make_unique<tidy::DefaultOptionsProvider>(
            tidy::ClangTidyGlobalOptions(), std::move(*Options)),
        /*AllowEnablingAnalyzerAlphaCheckers=*/false,
        /*EnableModuleHeadersParsing=*/false,
        Config::current().Diagnostics.ClangTidy.ExperimentalCustomChecks);
    Options.reset();
    Context->setDiagnosticsEngine(nullptr, &CI.getDiagnostics());
    Context->setASTContext(&CI.getASTContext());
    Context->setCurrentFile(Filename);
    Context->setSelfContainedDiags(true);
    tidy::ClangTidyCheckFactories Factories = *AllFactories;
#if CLANG_TIDY_ENABLE_QUERY_BASED_CUSTOM_CHECKS
    if (Context->canExperimentalCustomChecks() &&
        tidy::custom::RegisterCustomChecks) {
      // RegisterCustomChecks tracks names in process-wide mutable state.
      // Serializing its use keeps concurrent AST builds independent.
      static std::mutex CustomChecksMu;
      std::lock_guard<std::mutex> Lock(CustomChecksMu);
      tidy::custom::RegisterCustomChecks(Context->getOptions(), Factories);
    }
#endif
    auto FastFactories = filterFastChecks(
        Factories, Config::current().Diagnostics.ClangTidy.FastCheckFilter);
    Checks = FastFactories.createChecksForLanguage(&*Context);

    Preprocessor *PP = &CI.getPreprocessor();
    for (const auto &Check : Checks) {
      Check->registerPPCallbacks(CI.getSourceManager(), PP, PP);
      Check->registerMatchers(&Finder);
    }
  }

  void beforeExecute(CompilerInstance &CI) override {
    if (Checks.empty())
      return;

    assert(CI.hasASTConsumer());
    std::vector<std::unique_ptr<ASTConsumer>> Consumers;
    Consumers.push_back(CI.takeASTConsumer());
    auto TidyConsumer =
        std::make_unique<DeferredASTConsumer>(Finder.newASTConsumer());
    DeferredConsumer = TidyConsumer.get();
    Consumers.push_back(std::move(TidyConsumer));
    CI.setASTConsumer(
        std::make_unique<TidyMultiplexConsumer>(std::move(Consumers)));
  }

  void afterExecute(CompilerInstance &) override {
    if (!DeferredConsumer)
      return;
    trace::Span Tracer("ClangTidyMatch");
    DeferredConsumer->run();
  }

  void sawDiagnostic(const clang::Diagnostic &Info, clangd::Diag &D) override {
    if (!Context)
      return;
    std::string CheckName = Context->getCheckName(Info.getID());
    if (CheckName.empty())
      return;

    if (Config::current().Diagnostics.Suppress.contains(CheckName)) {
      D.Severity = DiagnosticsEngine::Ignored;
      return;
    }

    bool InsideMainFile =
        Info.hasSourceManager() &&
        isInsideMainFile(Info.getLocation(), Info.getSourceManager());
    llvm::SmallVector<tooling::Diagnostic, 1> SuppressionErrors;
    if (InsideMainFile &&
        Context->shouldSuppressDiagnostic(D.Severity, Info, SuppressionErrors,
                                          /*AllowIO=*/false,
                                          /*EnableNolintBlocks=*/true)) {
      D.Severity = DiagnosticsEngine::Ignored;
      return;
    }
    if (!Context->getOptions().SystemHeaders.value_or(false) &&
        Info.hasSourceManager() &&
        Info.getSourceManager().isInSystemMacro(Info.getLocation())) {
      D.Severity = DiagnosticsEngine::Ignored;
      return;
    }
    if (D.Severity == DiagnosticsEngine::Warning &&
        Context->treatAsError(CheckName))
      D.Severity = DiagnosticsEngine::Error;

    // Compiler warnings keep their clang source and -W name, even when their
    // severity or suppression is controlled by clang-tidy options.
    if (!Context->isCompilerDiagnostic(Info.getID())) {
      D.Name = std::move(CheckName);
      D.Source = Diag::ClangTidy;
    }
  }

  void finalizeDiagnostic(clangd::Diag &D) override {
    if (D.Source != Diag::ClangTidy)
      return;
    auto CleanMessage = [&](std::string &Message) {
      llvm::StringRef Rest(Message);
      if (Rest.consume_back("]") && Rest.consume_back(D.Name) &&
          Rest.consume_back(" ["))
        Message.resize(Rest.size());
    };
    CleanMessage(D.Message);
    for (auto &Note : D.Notes)
      CleanMessage(Note.Message);
    for (auto &Fix : D.Fixes)
      CleanMessage(Fix.Message);
    if (llvm::StringRef(D.Name).starts_with("misc-unused-") &&
        !llvm::is_contained(D.Tags, DiagnosticTag::Unnecessary))
      D.Tags.push_back(DiagnosticTag::Unnecessary);
    if (llvm::StringRef(D.Name).starts_with("modernize-") &&
        !llvm::is_contained(D.Tags, DiagnosticTag::Deprecated))
      D.Tags.push_back(DiagnosticTag::Deprecated);
  }

private:
  std::shared_ptr<const TidyProvider> Provider;
  std::optional<tidy::ClangTidyOptions> Options;
  // Destruction order matters: Finder and checks refer to Context.
  std::optional<tidy::ClangTidyContext> Context;
  std::vector<std::unique_ptr<tidy::ClangTidyCheck>> Checks;
  ast_matchers::MatchFinder Finder;
  DeferredASTConsumer *DeferredConsumer = nullptr;
};

} // namespace

std::unique_ptr<FeatureModule::ASTListener>
ClangTidyFeatureModule::astListeners() {
  std::lock_guard<std::mutex> Lock(ProviderMu);
  if (!Provider)
    return nullptr;
  return std::make_unique<TidyASTListener>(Provider);
}

void ClangTidyFeatureModule::setProvider(TidyProvider OptionsProvider) {
  std::shared_ptr<const TidyProvider> Replacement;
  if (OptionsProvider)
    Replacement =
        std::make_shared<const TidyProvider>(std::move(OptionsProvider));
  std::lock_guard<std::mutex> Lock(ProviderMu);
  Provider.swap(Replacement);
}

} // namespace clangd
} // namespace clang
