add_clang_library(clangdFeatureModules
  ClangTidyFeatureModule.cpp

  LINK_LIBS
  clangdSupport
  clangTidy
  clangTidyUtils
  )

target_include_directories(clangdFeatureModules
  PRIVATE
  ${CMAKE_CURRENT_SOURCE_DIR}/..
  ${CMAKE_CURRENT_SOURCE_DIR}/../../clang-tidy
  ${CMAKE_CURRENT_SOURCE_DIR}/../../include-cleaner/include
  )
