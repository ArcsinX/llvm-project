//===--- AscendCTests.cpp - Ascend C support tests ----------------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
#include "Annotations.h"
#include "CodeComplete.h"
#include "CompileCommands.h"
#include "Compiler.h"
#include "Hover.h"
#include "Preamble.h"
#include "TestFS.h"
#include "TestTU.h"
#include "XRefs.h"
#include "clang/AST/ExprCXX.h"
#include "clang/AST/RecursiveASTVisitor.h"
#include "clang/Format/Format.h"
#include "clang/Frontend/CompilerInvocation.h"
#include "llvm/ADT/STLExtras.h"
#include "gmock/gmock.h"
#include "gtest/gtest.h"

namespace clang::clangd {
namespace {
using testing::IsEmpty;

TestTU ascendTU(llvm::StringRef Code) {
  auto TU = TestTU::withCode(Code);
  TU.Filename = "kernel.asc";
  TU.ExtraArgs = {"-std=c++20"};
  return TU;
}

TEST(AscendC, LaunchesAndPreamble) {
  auto TU = ascendTU(R"cpp(
    template<class T> __global__ __aicore__ void kernel(__gm__ T *p) {
      __ubuf__ T local[16];
      local[0] = p[0];
    }
    void launch(__gm__ float *p, void *stream) {
      kernel<<<1, nullptr, stream>>>(p);
      kernel<<<1, 1024, stream>>>(p);
      kernel<<<1, 0, stream>>>(p);
      kernel<<<1>>>(p);
      kernel<<<1, 1024>>>(p);
      kernel<<<1, stream, stream>>>(p);
      kernel<float><<<dim3(2), dim3(32), 0, stream>>>(p);
    }
  )cpp");
  TU.HeaderCode =
      "using header_fp8 = __fp8e4m3;\n[aicore, host] void in_header();";
  auto AST = TU.build();
  EXPECT_THAT(AST.getDiagnostics(), IsEmpty());
  EXPECT_TRUE(AST.getLangOpts().CceExt);
  EXPECT_EQ(findDecl(AST, "header_fp8").getKind(), Decl::TypeAlias);
  struct Visitor : RecursiveASTVisitor<Visitor> {
    unsigned Launches = 0;
    bool VisitCUDAKernelCallExpr(CUDAKernelCallExpr *E) {
      ++Launches;
      EXPECT_EQ(E->getNumArgs(), 1u);
      EXPECT_GE(E->getConfig()->getNumArgs(), 3u);
      return true;
    }
  } V;
  V.TraverseDecl(AST.getASTContext().getTranslationUnitDecl());
  EXPECT_EQ(V.Launches, 7u);
}

TEST(AscendC, PreambleReuse) {
  auto TU = ascendTU("void f() { __fp8e4m3 value; }");
  TU.HeaderCode = "using serialized_fp8 = __fp8e4m3;";
  MockFS FS;
  auto Inputs = TU.inputs(FS);
  IgnoreDiagnostics Diags;
  auto CI = buildCompilerInvocation(Inputs, Diags);
  ASSERT_TRUE(CI);
  auto Preamble = buildPreamble(testPath(TU.Filename), *CI, Inputs,
                                /*StoreInMemory=*/true, nullptr);
  ASSERT_TRUE(Preamble);
  EXPECT_TRUE(
      isPreambleCompatible(*Preamble, Inputs, testPath(TU.Filename), *CI));
  Inputs.Contents = "void f() { __fp8e4m3 another; }";
  EXPECT_TRUE(
      isPreambleCompatible(*Preamble, Inputs, testPath(TU.Filename), *CI));
  FS.Files[testPath(TU.HeaderFilename)] = "using serialized_fp8 = __fp8e5m2;";
  FS.Timestamps[testPath(TU.HeaderFilename)] = 1;
  EXPECT_FALSE(
      isPreambleCompatible(*Preamble, Inputs, testPath(TU.Filename), *CI));
}

TEST(AscendC, SmallFloatTypes) {
  auto TU = ascendTU(R"cpp(
    static_assert(sizeof(__cce_half) == 2);
    static_assert(sizeof(__hif8) == 1);
    static_assert(sizeof(__hif4x2) == 1);
    static_assert(sizeof(__fp8e4m3) == 1);
    static_assert(sizeof(__fp8e5m2) == 1);
    static_assert(sizeof(__fp8e6m2) == 1);
    static_assert(sizeof(__fp8e8m0) == 1);
    static_assert(sizeof(__fp4e2m1x2) == 1);
    static_assert(sizeof(__fp4e1m2x2) == 1);
    static_assert(!__is_same(__fp8e6m2, __fp8e5m2));
    static_assert(__is_floating_point(__fp8e4m3));
    template<class T> struct Box {};
    Box<Box<Box<__fp8e6m2>>> nested;
    using vector_type = __fp8e4m3 __attribute__((ext_vector_type(2)));
    static_assert(sizeof(vector_type) == 2);
    constexpr int kind(__fp8e6m2) { return 6; }
    constexpr int kind(__fp8e5m2) { return 5; }
    static_assert(kind(__fp8e6m2{}) == 6);
    static_assert(kind(__fp8e5m2(0)) == 5);
    constexpr __fp8e4m3 e4max = 448.0f;
    static_assert(static_cast<float>(e4max) == 448.0f);
  )cpp");
  TU.HeaderCode = "using serialized_fp8 = __fp8e6m2;";
  EXPECT_THAT(TU.build().getDiagnostics(), IsEmpty());
}

TEST(AscendC, QualifiersAndAliases) {
  auto TU = ascendTU(R"cpp(
    [aicore] void annotated();
    [aicore, host] void dual();
    [aicpu] void cpu();
    __global__ __cube__ void cube();
    __global__ __vector__ void vector();
    __schedmode__(1) __global__ __mix__(1, 2) void mixed();
    __simd_vf__ void simd(__ubuf__ float *);
    __simt_vf__ __launch_bounds__(256) void simt(__gm__ float *);
    __simd_callee__ float simd_helper(float);
    __simt_callee__ float simt_helper(float);
    __attribute__((clang_builtin_alias(__builtin_cce_test))) int intrinsic(int);
    void print(__gm__ const char *);
    void spaces(__gm__ int *g, __ubuf__ int *u, __ca__ int *a,
                __cb__ int *b, __cc__ int *c, __cbuf__ int *l1,
                __fbuf__ int *f, __ssbuf__ int *s, __biasbuf__ int *bias) {
      __ubuf__ int buffer[16];
      __ubuf__ int *p = buffer;
      print("format");
      int aicore = 1;
      auto lambda = [aicore] { return aicore; };
      (void)intrinsic(lambda());
      (void)p;
    }
    static_assert(!__is_same(__gm__ int *, __ubuf__ int *));
  )cpp");
  EXPECT_THAT(TU.build().getDiagnostics(), IsEmpty());
}

TEST(AscendC, SmallFloatFormatDiagnostic) {
  auto TU = ascendTU(R"cpp(
    int printf(const char *, ...) __attribute__((format(printf, 1, 2)));
    void f() { printf("%d", __fp8e4m3{}); }
  )cpp");
  EXPECT_FALSE(TU.build().getDiagnostics().empty());
}

TEST(AscendC, InvalidCodeStillDiagnosed) {
  for (auto Code :
       {"// error-ok\nvoid k(); void f(){ k<<<1, nullptr, nullptr, nullptr, "
        "nullptr>>>(); }",
        "// error-ok\nvoid k(int); void f(){ k<<<1, nullptr, "
        "nullptr>>>(\"bad\"); }",
        "// error-ok\nvoid f(__gm__ int *g, __ubuf__ int *u){ g = u; }",
        "// error-ok\nvoid f(__ubuf__ int *u){ u = \"bad\"; }",
        "// error-ok\nvoid f(){ __gm__ __ubuf__ int *p; }"}) {
    SCOPED_TRACE(Code);
    EXPECT_FALSE(ascendTU(Code).build().getDiagnostics().empty());
  }
}

TEST(AscendC, OptInAndOrdinaryCpp) {
  auto TU = TestTU::withCode(R"cpp(
    int __hif8, __fp8e4m3, __cce_half;
    template<class T> struct A {};
    A<A<A<int>>> nested;
    int f(int aicore) { return [aicore] { return aicore; }(); }
    #ifdef __CLANGD_ASCENDC__
    #error Ascend compatibility leaked into C++
    #endif
  )cpp");
  auto AST = TU.build();
  EXPECT_FALSE(AST.getLangOpts().CceExt);
  EXPECT_THAT(AST.getDiagnostics(), IsEmpty());
  TU.Code = "__aicore__ void f(__gm__ float *);";
  TU.ExtraArgs = {"-D__CLANGD_ASCENDC__=1"};
  EXPECT_THAT(TU.build().getDiagnostics(), IsEmpty());
  // Explicit C++ header commands can also opt in.
  TU.Filename = "kernel.h";
  TU.ExtraArgs.push_back("-xc++");
  EXPECT_TRUE(TU.build().getLangOpts().CceExt);
}

TEST(AscendC, OrdinaryCppAddressSpacesStayStrict) {
  for (auto Code :
       {"// error-ok\nvoid f(){ int __attribute__((address_space(5))) a[4]; }",
        "// error-ok\nvoid p(const char __attribute__((address_space(1))) *); "
        "void f(){ p(\"text\"); }",
        "// error-ok\n__attribute__((clang_builtin_alias(__builtin_cce_test))) "
        "int f(int);"}) {
    SCOPED_TRACE(Code);
    auto TU = TestTU::withCode(Code);
    EXPECT_FALSE(TU.build().getDiagnostics().empty());
  }
}

TEST(AscendC, InferredHeaderCommand) {
  auto TU = TestTU::withCode("__aicore__ void f(__gm__ float *);");
  TU.Filename = "kernel.h";
  MockFS FS;
  auto Inputs = TU.inputs(FS);
  auto Mangler = CommandMangler::forTests();
  for (bool AlreadyTransferred : {false, true}) {
    auto &Cmd = Inputs.CompileCommand;
    Cmd.CommandLine = {"clang", AlreadyTransferred ? "kernel.h" : "kernel.asc"};
    Cmd.Heuristic = AlreadyTransferred ? "inferred from kernel.asc" : "";
    Mangler(Cmd, testPath(TU.Filename));
    IgnoreDiagnostics Diags;
    auto CI = buildCompilerInvocation(Inputs, Diags);
    ASSERT_TRUE(CI);
    EXPECT_TRUE(CI->getLangOpts().CPlusPlus);
    EXPECT_TRUE(CI->getLangOpts().CceExt);
  }
}

TEST(AscendC, NavigationAndHover) {
  Annotations Code(R"cpp(
    __global__ __aicore__ void [[kernel]](__gm__ float *p) {}
    void launch(__gm__ float *p) { $call^kernel<<<1, nullptr, nullptr>>>(p); }
    __fp8e4m3 $type^value;
  )cpp");
  auto TU = ascendTU(Code.code());
  auto AST = TU.build();
  auto Symbols = locateSymbolAt(AST, Code.point("call"));
  ASSERT_EQ(Symbols.size(), 1u);
  EXPECT_EQ(Symbols[0].Name, "kernel");
  EXPECT_EQ(Symbols[0].PreferredDeclaration.range, Code.range());
  auto H = getHover(AST, Code.point("type"), format::getLLVMStyle(), nullptr);
  ASSERT_TRUE(H);
  ASSERT_TRUE(H->Type);
  EXPECT_EQ(H->Type->Type, "__fp8e4m3");
}

TEST(AscendC, IndexSymbols) {
  auto TU = ascendTU("");
  TU.HeaderCode = R"cpp(
    __global__ __aicore__ void indexed_kernel(__gm__ float *p);
    void convert(__fp8e4m3);
    void convert(__fp8e5m2);
    __hif8 hif8(__hif8);
    __hif4x2 hif4(__hif4x2);
    __fp8e6m2 e6m2(__fp8e6m2);
    __fp8e8m0 e8m0(__fp8e8m0);
    __fp4e2m1x2 e2m1(__fp4e2m1x2);
    __fp4e1m2x2 e1m2(__fp4e1m2x2);
  )cpp";
  auto Symbols = TU.headerSymbols();
  EXPECT_EQ(findSymbol(Symbols, "indexed_kernel").Name, "indexed_kernel");
  std::vector<SymbolID> Overloads;
  for (const auto &S : Symbols)
    if (S.Name == "convert") {
      Overloads.push_back(S.ID);
      EXPECT_TRUE(S.Signature.contains("__fp8e"));
    }
  ASSERT_EQ(Overloads.size(), 2u);
  EXPECT_NE(Overloads[0], Overloads[1]);
}

TEST(AscendC, BuiltinTypeCompletion) {
  Annotations Code("void f() { __fp8^ }");
  auto TU = ascendTU(Code.code());
  MockFS FS;
  auto Inputs = TU.inputs(FS);
  IgnoreDiagnostics Diags;
  auto CI = buildCompilerInvocation(Inputs, Diags);
  ASSERT_TRUE(CI);
  auto Preamble = buildPreamble(testPath(TU.Filename), *CI, Inputs,
                                /*StoreInMemory=*/true, nullptr);
  ASSERT_TRUE(Preamble);
  auto Result = codeComplete(testPath(TU.Filename), Code.point(),
                             Preamble.get(), Inputs, {});
  for (auto Name : {"__fp8e4m3", "__fp8e5m2", "__fp8e6m2", "__fp8e8m0"})
    EXPECT_TRUE(llvm::any_of(Result.Completions, [&](const CodeCompletion &C) {
      return C.Name == Name;
    })) << Name;
}

TEST(AscendC, CompletionWithPreamble) {
  Annotations Code(R"cpp(
    struct Kernel { __aicore__ void Process(); };
    __aicore__ void f() { Kernel k; k.^ }
  )cpp");
  auto TU = ascendTU(Code.code());
  MockFS FS;
  auto Inputs = TU.inputs(FS);
  IgnoreDiagnostics Diags;
  auto CI = buildCompilerInvocation(Inputs, Diags);
  ASSERT_TRUE(CI);
  auto Preamble = buildPreamble(testPath(TU.Filename), *CI, Inputs,
                                /*StoreInMemory=*/true, nullptr);
  auto Result = codeComplete(testPath(TU.Filename), Code.point(),
                             Preamble.get(), Inputs, {});
  EXPECT_TRUE(llvm::any_of(Result.Completions, [](const CodeCompletion &C) {
    return C.Name == "Process";
  }));
}
} // namespace
} // namespace clang::clangd
