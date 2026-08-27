//===--- ClangTidyFeatureModule.cpp - Clang-Tidy feature module for clangd -------===//
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
#include "AST.h"
#include "Config.h"
#include "Diagnostics.h"
#include "Feature.h"
#include "support/FileCache.h"
#include "support/Logger.h"
#include "support/Path.h"
#include "support/ThreadsafeFS.h"
#include "support/Trace.h"
#include "clang/AST/ASTContext.h"
#include "clang/ASTMatchers/ASTMatchFinder.h"
#include "clang/Basic/AllDiagnostics.h"
#include "clang/Basic/Diagnostic.h"
#include "clang/Basic/DiagnosticIDs.h"
#include "clang/Basic/DiagnosticSema.h"
#include "clang/Basic/SourceLocation.h"
#include "clang/Basic/SourceManager.h"
#include "clang/Frontend/CompilerInstance.h"
#include "clang/Frontend/MultiplexConsumer.h"
#include "clang/Lex/Preprocessor.h"
#include "clang/Tooling/Core/Diagnostic.h"
#include "llvm/ADT/DenseSet.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallString.h"
#include "llvm/ADT/StringExtras.h"
#include "llvm/ADT/StringSet.h"
#include "llvm/Support/Allocator.h"
#include "llvm/Support/Process.h"
#include "llvm/Support/SourceMgr.h"
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// Force the linker to link in Clang-tidy modules.
// clangd doesn't support the static analyzer.
#if CLANGD_TIDY_CHECKS
#define CLANG_TIDY_DISABLE_STATIC_ANALYZER_CHECKS
#include "../clang-tidy/ClangTidyForceLinker.h"
#endif

namespace clang {
namespace clangd {
namespace {

// ============================================================================
// TidyProvider implementation (moved from TidyProvider.cpp)
// ============================================================================

// Access to config from a .clang-tidy file, caching IO and parsing.
class DotClangTidyCache : private FileCache {
  mutable std::shared_ptr<const tidy::ClangTidyOptions> Value;

public:
  DotClangTidyCache(PathRef Path) : FileCache(Path) {}

  std::shared_ptr<const tidy::ClangTidyOptions>
  get(const ThreadsafeFS &TFS,
      std::chrono::steady_clock::time_point FreshTime) const {
    std::shared_ptr<const tidy::ClangTidyOptions> Result;
    read(
        TFS, FreshTime,
        [this](std::optional<llvm::StringRef> Data) {
          Value.reset();
          if (Data && !Data->empty()) {
            auto Diagnostics = [](const llvm::SMDiagnostic &D) {
              switch (D.getKind()) {
              case llvm::SourceMgr::DK_Error:
                elog("tidy-config error at {0}:{1}:{2}: {3}", D.getFilename(),
                     D.getLineNo(), D.getColumnNo(), D.getMessage());
                break;
              case llvm::SourceMgr::DK_Warning:
                log("tidy-config warning at {0}:{1}:{2}: {3}", D.getFilename(),
                    D.getLineNo(), D.getColumnNo(), D.getMessage());
                break;
              case llvm::SourceMgr::DK_Note:
              case llvm::SourceMgr::DK_Remark:
                vlog("tidy-config note at {0}:{1}:{2}: {3}", D.getFilename(),
                     D.getLineNo(), D.getColumnNo(), D.getMessage());
                break;
              }
            };
            if (auto Parsed = tidy::parseConfigurationWithDiags(
                    llvm::MemoryBufferRef(*Data, path()), Diagnostics))
              Value = std::make_shared<const tidy::ClangTidyOptions>(
                  std::move(*Parsed));
            else
              elog("Error parsing clang-tidy configuration in {0}: {1}", path(),
                   Parsed.getError().message());
          }
        },
        [&]() { Result = Value; });
    return Result;
  }
};

// Access to combined config from .clang-tidy files governing a source file.
class DotClangTidyTree {
  const ThreadsafeFS &FS;
  std::string RelPath;
  std::chrono::steady_clock::duration MaxStaleness;

  mutable std::mutex Mu;
  mutable llvm::StringMap<DotClangTidyCache> Cache;

public:
  DotClangTidyTree(const ThreadsafeFS &FS)
      : FS(FS), RelPath(".clang-tidy"), MaxStaleness(std::chrono::seconds(5)) {}

  void apply(tidy::ClangTidyOptions &Result, PathRef AbsPath) {
    namespace path = llvm::sys::path;
    assert(path::is_absolute(AbsPath));

    llvm::SmallVector<DotClangTidyCache *> Caches;
    {
      std::lock_guard<std::mutex> Lock(Mu);
      for (auto Ancestor = absoluteParent(AbsPath); !Ancestor.empty();
           Ancestor = absoluteParent(Ancestor)) {
        auto It = Cache.find(Ancestor);
        if (It == Cache.end()) {
          llvm::SmallString<256> ConfigPath = Ancestor;
          path::append(ConfigPath, RelPath);
          It = Cache.try_emplace(Ancestor, ConfigPath.str()).first;
        }
        Caches.push_back(&It->second);
      }
    }
    std::chrono::steady_clock::time_point FreshTime =
        std::chrono::steady_clock::now() - MaxStaleness;
    llvm::SmallVector<std::shared_ptr<const tidy::ClangTidyOptions>>
        OptionStack;
    for (const DotClangTidyCache *Cache : Caches)
      if (auto Config = Cache->get(FS, FreshTime)) {
        OptionStack.push_back(std::move(Config));
        if (!OptionStack.back()->InheritParentConfig.value_or(false))
          break;
      }
    unsigned Order = 1u;
    for (auto &Option : llvm::reverse(OptionStack))
      Result.mergeWith(*Option, Order++);
  }
};

static void mergeCheckList(std::optional<std::string> &Checks,
                           llvm::StringRef List) {
  if (List.empty())
    return;
  if (!Checks || Checks->empty()) {
    Checks.emplace(List);
    return;
  }
  *Checks = llvm::join_items(",", *Checks, List);
}

// ============================================================================
// Diagnostic helpers (moved from ParsedAST.cpp)
// ============================================================================

// Filter for clang diagnostics groups enabled by CTOptions.Checks.
class TidyDiagnosticGroups {
  bool Default = false;
  llvm::DenseSet<unsigned> Exceptions;

public:
  TidyDiagnosticGroups(llvm::StringRef Checks) {
    constexpr llvm::StringLiteral CDPrefix = "clang-diagnostic-";

    llvm::StringRef Check;
    while (!Checks.empty()) {
      std::tie(Check, Checks) = Checks.split(',');
      Check = Check.trim();

      if (Check.empty())
        continue;

      bool Enable = !Check.consume_front("-");
      bool Glob = Check.consume_back("*");
      if (Glob) {
        if (CDPrefix.starts_with(Check)) {
          Default = Enable;
          Exceptions.clear();
        }
        continue;
      }

      if (Default == Enable)
        continue;
      if (!Check.consume_front(CDPrefix))
        continue;

      if (auto Group = DiagnosticIDs::getGroupForWarningOption(Check))
        Exceptions.insert(static_cast<unsigned>(*Group));
    }
  }

  bool operator()(diag::Group GroupID) const {
    return Exceptions.contains(static_cast<unsigned>(GroupID)) ? !Default
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
        if (Diags.getDiagnosticLevel(ID, SourceLocation()) <
            DiagnosticsEngine::Warning) {
          auto Group = Diags.getDiagnosticIDs()->getGroupForDiag(ID);
          if (!Group || !EnabledGroups(*Group))
            continue;
          Diags.setSeverity(ID, diag::Severity::Warning, SourceLocation());
          if (Diags.getWarningsAsErrors())
            NeedsWerrorExclusion = true;
        }
      } else {
        Diags.setSeverity(ID, diag::Severity::Ignored, SourceLocation());
      }
    }
    if (NeedsWerrorExclusion) {
      Diags.setDiagnosticGroupWarningAsError(Group, false);
    }
  }
}

tidy::ClangTidyCheckFactories
filterFastTidyChecks(const tidy::ClangTidyCheckFactories &All,
                     Config::FastCheckPolicy Policy) {
  if (Policy == Config::FastCheckPolicy::None)
    return All;
  bool AllowUnknown = Policy == Config::FastCheckPolicy::Loose;
  tidy::ClangTidyCheckFactories Fast;
  for (const auto &Factory : All) {
    if (isFastTidyCheck(Factory.getKey()).value_or(AllowUnknown))
      Fast.registerCheckFactory(Factory.first(), Factory.second);
  }
  return Fast;
}

} // namespace

// ============================================================================
// ASTListener implementation
// ============================================================================

class ClangTidyFeatureModule::TidyListener : public FeatureModule::ASTListener {
public:
  TidyListener(TidyProviderRef Provider) : Provider(Provider) {}

  ~TidyListener() override {
    // CTChecks must be destroyed before CTContext.
    CTChecks.clear();
  }

  void beforeIncludes(CompilerInstance &CI) override {
    const auto &Cfg = Config::current();
    auto Filename = getCurrentFilename(CI);

    trace::Span Tracer("ClangTidyInit");

    // Get tidy options for this file.
    ClangTidyOpts = getTidyOptionsForFile(Provider, Filename);
    dlog("ClangTidy configuration for file {0}: {1}", Filename,
         tidy::configurationAsText(ClangTidyOpts));

    // Apply warning options from tidy ExtraArgs to the DiagnosticsEngine.
    auto &Diags = CI.getDiagnostics();
    TidyDiagnosticGroups TidyGroups(ClangTidyOpts.Checks
                                        ? *ClangTidyOpts.Checks
                                        : llvm::StringRef());
    if (ClangTidyOpts.ExtraArgsBefore)
      applyWarningOptions(*ClangTidyOpts.ExtraArgsBefore, TidyGroups, Diags);
    if (ClangTidyOpts.ExtraArgs)
      applyWarningOptions(*ClangTidyOpts.ExtraArgs, TidyGroups, Diags);

    // Set up clang-tidy context and checks.
    static const auto *AllCTFactories = [] {
      auto *CTFactories = new tidy::ClangTidyCheckFactories;
      for (const auto &E : tidy::ClangTidyModuleRegistry::entries())
        E.instantiate()->addCheckFactories(*CTFactories);
      return CTFactories;
    }();
    tidy::ClangTidyCheckFactories FastFactories = filterFastTidyChecks(
        *AllCTFactories, Cfg.Diagnostics.ClangTidy.FastCheckFilter);
    if (ClangTidyOpts.Checks)
      dlog("ClangTidy enabled checks: {0}", *ClangTidyOpts.Checks);
    CTContext.emplace(std::make_unique<tidy::DefaultOptionsProvider>(
        tidy::ClangTidyGlobalOptions(), ClangTidyOpts));
    CTContext->setDiagnosticsEngine(nullptr, &CI.getDiagnostics());
    CTContext->setASTContext(&CI.getASTContext());
    CTContext->setCurrentFile(Filename);
    CTContext->setSelfContainedDiags(true);
    CTChecks = FastFactories.createChecksForLanguage(&*CTContext);

    // Register PPCallbacks and matchers on checks.
    Preprocessor *PP = &CI.getPreprocessor();
    for (const auto &Check : CTChecks) {
      Check->registerPPCallbacks(CI.getSourceManager(), PP, PP);
      Check->registerMatchers(&CTFinder);
    }
  }

  void beforeExecute(CompilerInstance &CI) override {}

  void sawDiagnostic(const clang::Diagnostic &Info,
                     clangd::Diag &D) override {
    if (!CTContext || CTChecks.empty()) {
      return;
    }

    std::string CheckName = CTContext->getCheckName(Info.getID());
    bool IsClangTidyDiag = !CheckName.empty();
    if (!IsClangTidyDiag)
      return;

    const auto &Cfg = Config::current();

    // Check if this check is suppressed via config.
    if (Cfg.Diagnostics.Suppress.contains(CheckName)) {
      D.Severity = DiagnosticsEngine::Ignored;
      return;
    }

    // Check for NOLINT suppression comment.
    bool IsInsideMainFile =
        Info.hasSourceManager() &&
        isInsideMainFile(Info.getLocation(), Info.getSourceManager());
    SmallVector<tooling::Diagnostic, 1> TidySuppressedErrors;
    if (IsInsideMainFile && CTContext->shouldSuppressDiagnostic(
                                D.Severity, Info, TidySuppressedErrors,
                                /*AllowIO=*/false,
                                /*EnableNolintBlocks=*/true)) {
      D.Severity = DiagnosticsEngine::Ignored;
      return;
    }

    // Filter diagnostics from system headers.
    if (!CTContext->getOptions().SystemHeaders.value_or(false) &&
        Info.hasSourceManager() &&
        Info.getSourceManager().isInSystemMacro(Info.getLocation())) {
      D.Severity = DiagnosticsEngine::Ignored;
      return;
    }

    // Check for warning-as-error.
    if (D.Severity == DiagnosticsEngine::Warning &&
        CTContext->treatAsError(CheckName)) {
      D.Severity = DiagnosticsEngine::Error;
    }

    // Set tidy diagnostic source and name.
    D.Name = CheckName;
    D.Source = Diag::ClangTidy;

    // Clean tidy message prefix: "[check-name] message [check-name]"
    auto CleanMessage = [&](std::string &Msg) {
      StringRef Rest(Msg);
      if (Rest.consume_back("]") && Rest.consume_back(D.Name) &&
          Rest.consume_back(" ["))
        Msg.resize(Rest.size());
    };
    CleanMessage(D.Message);

    // Add diagnostic tags for tidy checks.
    if (llvm::StringRef(D.Name).starts_with("misc-unused-"))
      D.Tags.push_back(DiagnosticTag::Unnecessary);
    if (llvm::StringRef(D.Name).starts_with("modernize-"))
      D.Tags.push_back(DiagnosticTag::Deprecated);
  }

  void afterExecute(CompilerInstance &CI) override {
    if (!CTChecks.empty()) {
      trace::Span Tracer("ClangTidyMatch");
      CTFinder.matchAST(CI.getASTContext());
    }
  }

  void finalizeDiag(clangd::Diag &D) override {
    if (D.Source != Diag::ClangTidy || D.Name.empty())
      return;
    // Clean tidy message suffix in notes and fixes.
    // sawDiagnostic() sets D.Name, but notes and fixes are added after the
    // callback runs, so they still have "[check-name]".
    std::string Suffix = " [" + D.Name + "]";
    for (auto &Note : D.Notes) {
      if (llvm::StringRef(Note.Message).ends_with(Suffix))
        Note.Message.resize(Note.Message.size() - Suffix.size());
    }
    for (auto &Fix : D.Fixes) {
      if (llvm::StringRef(Fix.Message).ends_with(Suffix))
        Fix.Message.resize(Fix.Message.size() - Suffix.size());
    }
  }

private:
  static llvm::StringRef getCurrentFilename(CompilerInstance &CI) {
    const auto &Inputs = CI.getFrontendOpts().Inputs;
    if (!Inputs.empty())
      return Inputs[0].getFile();
    return {};
  }

  TidyProviderRef Provider;
  tidy::ClangTidyOptions ClangTidyOpts;
  std::vector<std::unique_ptr<tidy::ClangTidyCheck>> CTChecks;
  ast_matchers::MatchFinder CTFinder;
  std::optional<tidy::ClangTidyContext> CTContext;
};

// ============================================================================
// TidyProvider free functions
// ============================================================================

TidyProvider provideEnvironment() {
  static const std::optional<std::string> User = [] {
    std::optional<std::string> Ret = llvm::sys::Process::GetEnv("USER");
#ifdef _WIN32
    if (!Ret)
      return llvm::sys::Process::GetEnv("USERNAME");
#endif
    return Ret;
  }();

  if (User)
    return
        [](tidy::ClangTidyOptions &Opts, llvm::StringRef) { Opts.User = User; };
  return [](tidy::ClangTidyOptions &, llvm::StringRef) {};
}

TidyProvider provideDefaultChecks() {
  static const std::string DefaultChecks = llvm::join_items(
      ",", "readability-misleading-indentation", "readability-deleted-default",
      "bugprone-integer-division", "bugprone-sizeof-expression",
      "bugprone-suspicious-missing-comma", "bugprone-unused-raii",
      "bugprone-unused-return-value", "misc-unused-using-decls",
      "misc-unused-alias-decls", "misc-definitions-in-headers");
  return [](tidy::ClangTidyOptions &Opts, llvm::StringRef) {
    if (!Opts.Checks || Opts.Checks->empty())
      Opts.Checks = DefaultChecks;
  };
}

TidyProvider addTidyChecks(llvm::StringRef Checks,
                           llvm::StringRef WarningsAsErrors) {
  return [Checks = std::string(Checks),
          WarningsAsErrors = std::string(WarningsAsErrors)](
             tidy::ClangTidyOptions &Opts, llvm::StringRef) {
    mergeCheckList(Opts.Checks, Checks);
    mergeCheckList(Opts.WarningsAsErrors, WarningsAsErrors);
  };
}

TidyProvider disableUnusableChecks(llvm::ArrayRef<std::string> ExtraBadChecks) {
  constexpr llvm::StringLiteral Separator(",");
  static const std::string BadChecks = llvm::join_items(
      Separator,
      "",
      "-misc-include-cleaner",
      "-llvm-header-guard", "-modernize-macro-to-enum",
      "-cppcoreguidelines-macro-to-enum",
      "-bugprone-use-after-move",
      "-bugprone-unchecked-optional-access",
      "-abseil-unchecked-statusor-access");

  size_t Size = BadChecks.size();
  for (const std::string &Str : ExtraBadChecks) {
    if (Str.empty())
      continue;
    Size += Separator.size();
    if (LLVM_LIKELY(Str.front() != '-'))
      ++Size;
    Size += Str.size();
  }
  std::string DisableGlob;
  DisableGlob.reserve(Size);
  DisableGlob += BadChecks;
  for (const std::string &Str : ExtraBadChecks) {
    if (Str.empty())
      continue;
    DisableGlob += Separator;
    if (LLVM_LIKELY(Str.front() != '-'))
      DisableGlob.push_back('-');
    DisableGlob += Str;
  }

  return [DisableList(std::move(DisableGlob))](tidy::ClangTidyOptions &Opts,
                                               llvm::StringRef) {
    if (Opts.Checks && !Opts.Checks->empty())
      Opts.Checks->append(DisableList);
  };
}

TidyProvider provideClangdConfig() {
  return [](tidy::ClangTidyOptions &Opts, llvm::StringRef) {
    const auto &CurTidyConfig = Config::current().Diagnostics.ClangTidy;
    if (!CurTidyConfig.Checks.empty())
      mergeCheckList(Opts.Checks, CurTidyConfig.Checks);

    for (const auto &CheckOption : CurTidyConfig.CheckOptions)
      Opts.CheckOptions.insert_or_assign(CheckOption.getKey(),
                                         tidy::ClangTidyOptions::ClangTidyValue(
                                             CheckOption.getValue(), 10000U));
  };
}

TidyProvider provideClangTidyFiles(const ThreadsafeFS &TFS) {
  return [Tree = std::make_unique<DotClangTidyTree>(TFS)](
             tidy::ClangTidyOptions &Opts, llvm::StringRef Filename) {
    Tree->apply(Opts, Filename);
  };
}

TidyProvider combine(std::vector<TidyProvider> Providers) {
  return [Providers(std::move(Providers))](tidy::ClangTidyOptions &Opts,
                                           llvm::StringRef Filename) {
    for (const auto &Provider : Providers)
      Provider(Opts, Filename);
  };
}

tidy::ClangTidyOptions getTidyOptionsForFile(TidyProviderRef Provider,
                                             llvm::StringRef Filename) {
  static const auto *DefaultOpts = [] {
    auto *Opts = new tidy::ClangTidyOptions;
    *Opts = tidy::ClangTidyOptions::getDefaults();
    Opts->Checks->clear();
    return Opts;
  }();
  auto Opts = *DefaultOpts;
  if (Provider)
    Provider(Opts, Filename);
  return Opts;
}

bool isRegisteredTidyCheck(llvm::StringRef Check) {
  assert(!Check.empty());
  assert(!Check.contains('*') && !Check.contains(',') &&
         "isRegisteredCheck doesn't support globs");
  assert(Check.ltrim().front() != '-');

  static const llvm::StringSet<llvm::BumpPtrAllocator> AllChecks = [] {
    llvm::StringSet<llvm::BumpPtrAllocator> Result;
    tidy::ClangTidyCheckFactories Factories;
    for (tidy::ClangTidyModuleRegistry::entry E :
         tidy::ClangTidyModuleRegistry::entries())
      E.instantiate()->addCheckFactories(Factories);
    for (const auto &Factory : Factories)
      Result.insert(Factory.getKey());
    return Result;
  }();

  return AllChecks.contains(Check);
}

std::optional<bool> isFastTidyCheck(llvm::StringRef Check) {
  static auto &Fast = *new llvm::StringMap<bool>{
#define FAST(CHECK, TIME) {#CHECK,true},
#define SLOW(CHECK, TIME) {#CHECK,false},
#include "TidyFastChecks.inc"
  };
  if (auto It = Fast.find(Check); It != Fast.end())
    return It->second;
  return std::nullopt;
}

// ============================================================================
// ClangTidyFeatureModule implementation
// ============================================================================

void ClangTidyFeatureModule::initialize(const Facilities &F) {
  FeatureModule::initialize(F);

  // Set up default providers if none were explicitly configured.
  if (Provider)
    return;

  std::vector<TidyProvider> Providers;
  Providers.push_back(provideEnvironment());
  Providers.push_back(provideClangTidyFiles(F.FS));
  Providers.push_back(provideClangdConfig());
  Providers.push_back(provideDefaultChecks());
  Providers.push_back(disableUnusableChecks());
  Provider = combine(std::move(Providers));
}

void ClangTidyFeatureModule::setProvider(TidyProvider P) {
  Provider = std::move(P);
}

TidyProviderRef ClangTidyFeatureModule::provider() const {
  return Provider;
}

std::unique_ptr<FeatureModule::ASTListener>
ClangTidyFeatureModule::astListeners() {
  if (!Provider)
    return nullptr;
  return std::make_unique<TidyListener>(Provider);
}

// ============================================================================
// Registry registration and force-link anchor
// ============================================================================

// Register the ClangTidyFeatureModule via the feature module registry.
static FeatureModuleRegistry::Add<ClangTidyFeatureModule>
    X("clang-tidy", "Integrates clang-tidy checks into clangd diagnostics.");

// This anchor is used to force the linker to link in this module's
// translation unit and thus register the ClangTidyFeatureModule.
// NOLINTNEXTLINE(misc-use-internal-linkage)
volatile int ClangTidyFeatureModuleAnchorSource = 0;

} // namespace clangd
} // namespace clang
