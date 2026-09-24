if(NOT DEFINED LLVM_MAIN_SRC_DIR)
  cmake_minimum_required(VERSION 3.20)
  project(SampleFeatureModule)

  find_package(LLVM REQUIRED CONFIG)
  find_package(Clang REQUIRED CONFIG)

  list(APPEND CMAKE_MODULE_PATH "${LLVM_CMAKE_DIR}")
  include(AddLLVM)

  include_directories(${LLVM_INCLUDE_DIRS} ${CLANG_INCLUDE_DIRS})
  link_directories(${LLVM_LIBDIR_SUFFIX})

  add_llvm_library(SampleFeatureModule MODULE
    SampleFeatureModule.cpp
    PLUGIN_TOOL clangd
  )
else()
  add_llvm_library(SampleFeatureModule MODULE
    SampleFeatureModule.cpp
    PLUGIN_TOOL clangd
  )

  target_include_directories(SampleFeatureModule PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/../../"
    "${CMAKE_CURRENT_BINARY_DIR}/../../"
  )

  if(WIN32 OR CYGWIN)
    set(LLVM_LINK_COMPONENTS
      Support
    )
  endif()
endif()
