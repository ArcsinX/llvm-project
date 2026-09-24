//===--- FeatureModule.cpp - Plugging features into clangd ----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "FeatureModule.h"
#include "support/Logger.h"
#include "llvm/Support/DynamicLibrary.h"
#include "llvm/Support/Error.h"

namespace clang {
namespace clangd {

void FeatureModule::initialize(const Facilities &F) {
  assert(!Fac && "Initialized twice");
  Fac.emplace(F);
}

FeatureModule::Facilities &FeatureModule::facilities() {
  assert(Fac && "Not initialized yet");
  return *Fac;
}

void FeatureModuleSet::add(std::unique_ptr<FeatureModule> M) {
  Modules.push_back(std::move(M));
}

bool FeatureModuleSet::addImpl(void *Key, std::unique_ptr<FeatureModule> M,
                               const char *Source) {
  if (!Map.try_emplace(Key, M.get()).second) {
    // Source should (usually) include the name of the concrete module type.
    elog("Tried to register duplicate feature modules via {0}", Source);
    return false;
  }
  Modules.push_back(std::move(M));
  return true;
}

FeatureModuleSet FeatureModuleSet::fromRegistry() {
  FeatureModuleSet ModuleSet;
  for (FeatureModuleRegistry::entry E : FeatureModuleRegistry::entries()) {
    vlog("Adding feature module '{0}' ({1})", E.getName(), E.getDesc());
    ModuleSet.add(E.instantiate());
  }
  return ModuleSet;
}

llvm::Error loadFeatureModule(llvm::StringRef SharedLibraryPath) {
  std::string Err;
  if (llvm::sys::DynamicLibrary::LoadLibraryPermanently(
          SharedLibraryPath.str().c_str(), &Err))
    return llvm::createStringError(llvm::inconvertibleErrorCode(),
                                   "Failed to load feature module '%s': %s",
                                   SharedLibraryPath.str().c_str(),
                                   Err.c_str());
  return llvm::Error::success();
}

} // namespace clangd
} // namespace clang

LLVM_INSTANTIATE_REGISTRY(clang::clangd::FeatureModuleRegistry)
