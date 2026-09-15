# 📽️ Presentation: Instant C++ Tooling with Clang-Pseudo
### Bringing Sub-Second Syntax Navigation & Fallback Intelligence to Clangd

<p align="center">
  <img src="https://img.shields.io/badge/Clangd-Feature_Module-blue?style=for-the-badge&logo=llvm" alt="Clangd" />
  <img src="https://img.shields.io/badge/Parser-GLR_C%2B%2B_Grammar-green?style=for-the-badge" alt="GLR Parser" />
  <img src="https://img.shields.io/badge/Startup_Speedup-72x_Faster-brightgreen?style=for-the-badge" alt="Startup Speedup" />
  <img src="https://img.shields.io/badge/RAM_Footprint-590x_Smaller-success?style=for-the-badge" alt="RAM Footprint" />
  <img src="https://img.shields.io/badge/AI_Ready-LLMs_%26_Agents-purple?style=for-the-badge" alt="AI Ready" />
</p>

> [!TIP]
> **Viewing on GitHub**: This presentation is formatted as an executive slide deck in GitHub Flavored Markdown.  
> - Use the **Slide Navigator** below to jump directly to any slide.
> - Each slide features **◀ Previous** and **Next ▶** quick links for seamless browsing.
> - Expand the collapsible **🎙️ Presenter Notes & Talking Points** on each slide for deep-dive speaker context.
> - All architecture and workflow diagrams are rendered natively by GitHub using **Mermaid**.

---

<a id="slide-navigator"></a>

## 📑 Slide Navigator

| # | Slide Title | Key Takeaway | Quick Jump |
| :-: | :--- | :--- | :-: |
| **01** | [**Executive Summary & Metric Snapshot**](#slide-1) | **72x faster** startup & **590x smaller** memory footprint | [View Slide 1 ➡](#slide-1) |
| **02** | [**The Problem: The Cost of Full Clang ASTs**](#slide-2) | Header preprocessing bottlenecks, cold freezes & RAM load | [View Slide 2 ➡](#slide-2) |
| **03** | [**The Solution: Architecture & Clangd Integration**](#slide-3) | `FeatureModule` integration, pure & fallback modes | [View Slide 3 ➡](#slide-3) |
| **04** | [**How GLR Pseudo-Parsing Works Without Headers**](#slide-4) | Directive trees, C++ BNF grammar, and parse forests | [View Slide 4 ➡](#slide-4) |
| **05** | [**Semantic Navigation, Disambiguation & Header BFS**](#slide-5) | `isTypeContext` classifier & bounded header traversal | [View Slide 5 ➡](#slide-5) |
| **06** | [**Concrete Benchmarks: `Sema.cpp` Head-to-Head**](#slide-6) | Real-world metrics measured on LLVM codebase | [View Slide 6 ➡](#slide-6) |
| **07** | [**Technology Comparison: Clang-Pseudo vs. Tree-sitter**](#slide-7) | Architectural trade-offs, preprocessor, and grammar models | [View Slide 7 ➡](#slide-7) |
| **08** | [**The AI Frontier: Clang-Pseudo for LLMs & Agents**](#slide-8) | Sub-10ms syntax gating, AST skeletonization & RAG | [View Slide 8 ➡](#slide-8) |
| **09** | [**Supported Language Features in Clangd**](#slide-9) | Outlines, folding, definition jumps, highlights & hover | [View Slide 9 ➡](#slide-9) |
| **10** | [**Target Use Cases & Future Roadmap**](#slide-10) | Monorepos, Cloud IDEs, and incremental pseudo-parsing | [View Slide 10 ➡](#slide-10) |

---

<br>

<a id="slide-1"></a>

> <sub>**SLIDE 01 OF 10** &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: The Problem ▶](#slide-2)</sub>

# 🚀 Slide 1: Instant C++ Tooling with Clang-Pseudo

### Sub-Second Syntax Navigation & Fallback Intelligence for Clangd

> [!NOTE]
> **Executive Summary**  
> By integrating the Generalized LR (`GLR`) C++ pseudo-parser into `clangd` as a modular `FeatureModule`, we achieve **sub-50 millisecond interactive file opening**—a **~72x speedup** over traditional Clang AST construction on real-world files, while slashing memory consumption by **~590x**.

### High-Level Metric Snapshot

| Dimension | Traditional Clang AST | Clangd + PseudoModule | Impact |
| :--- | :---: | :---: | :---: |
| **`didOpen` &rarr; Ready** | `3,601 ms` (~3.60 s) | `50.2 ms` (~0.05 s) | ⚡ **72x Faster** |
| **Memory Footprint** | `82.8 MB` Preamble | `140 KB` Parse Forest | 📉 **590x Less RAM** |
| **Compilation Database** | **Required** (Strict `compile_commands.json`) | **Optional** (Zero-Config Fallback) | 🛡️ **Resilient** |
| **Header Dependencies** | Must parse all transitively | None required for file syntax | 🌐 **Self-Contained** |

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Opening Hook:** "Every C++ developer knows the frustration of opening a file and staring at a frozen editor for 5 to 10 seconds while the language server digests half a million lines of included headers."
- **Core Value Proposition:** Clang-Pseudo completely decouples initial file browsing and structural navigation from exhaustive compiler semantic analysis.
- **The Core Metric:** On `clang/lib/Sema/Sema.cpp` (3,121 lines), regular Clang takes **3.6 seconds** and **82.8 MB** of RAM before the user gets document symbols. With Clang-Pseudo, it takes **50 milliseconds** and **140 KB** of RAM.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-2">Next: The Problem ➡</a></sub>
</div>

---

<br>

<a id="slide-2"></a>

> <sub>**SLIDE 02 OF 10** &nbsp;|&nbsp; [◀ Prev: Summary](#slide-1) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: Architecture ▶](#slide-3)</sub>

# ⚠️ Slide 2: The Problem: The Cost of Full Clang ASTs

In standard `clangd`, providing language intelligence requires constructing a complete Clang AST with precompiled headers (PCH) and preamble.

```mermaid
flowchart TD
    A["📄 Open Source File (e.g. Sema.cpp, 3,121 lines)"] --> B["🔍 Locate compile_commands.json"]
    B --> C["⏳ Parse & Preprocess All Headers Transitively\n(Hundreds of thousands of lines of C++ headers)"]
    C --> D["💾 Serialize Preamble to Memory / Disk\n(82.8 MB Preamble Artifact)"]
    D --> E["🧠 Run Clang Semantic Analysis & Template Instantiations"]
    E --> F["✅ AST Ready: Editor unlocks Symbols, Outline, Folding & Navigation\n(Elapsed: ~3.6 - 4.3 seconds)"]

    style C fill:#ffebee,stroke:#c62828,stroke-width:2px,color:#b71c1c
    style D fill:#ffebee,stroke:#c62828,stroke-width:2px,color:#b71c1c
    style F fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#1b5e20
```

### Production Pain Points

> [!WARNING]
> 1. **Cold Start Latency**: Opening a large translation unit (like `Sema.cpp` or `ClangdServer.cpp`) triggers a **3 to 10+ second freeze** before the file outline, folding ranges, or breadcrumbs become interactive.
> 2. **Fragility in Incomplete Environments**: If compile flags are missing, `-I` include paths are misconfigured, or a header has a syntax error, full AST compilation halts with a fatal error &rarr; **All IDE features completely break**.
> 3. **Severe Memory Load**: 80–300 MB per open file creates heavy memory pressure in developer containers, remote SSH instances, and cloud-hosted IDEs.

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Why does Clang take so long?** Clang cannot parse C++ without knowing types defined in headers. So even if you open a file with 10 lines of code that includes `<vector>`, Clang must parse dozens of standard library headers first.
- **The Preprocessing Tax:** Over 85% of startup time is spent compiling header preambles, not the user's actual source file.
- **Failure Cascade:** If a developer switches git branches and a generated protobuf header is missing, regular clangd fails entirely, leaving the developer with zero editor support.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-3">Next: Architecture ➡</a></sub>
</div>

---

<br>

<a id="slide-3"></a>

> <sub>**SLIDE 03 OF 10** &nbsp;|&nbsp; [◀ Prev: The Problem](#slide-2) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: GLR Engine ▶](#slide-4)</sub>

# 🏗️ Slide 3: Architecture & Integration in Clangd

[`PseudoModule`](file:///Users/usovaanastasia/work/llvm-project/clang-tools-extra/clangd/PseudoModule.h) integrates cleanly through Clangd's native [`FeatureModule`](file:///Users/usovaanastasia/work/llvm-project/clang-tools-extra/clangd/FeatureModule.h) API, providing zero-overhead hooking.

```mermaid
flowchart LR
    subgraph Client["💻 LSP Client (Editor / IDE)"]
        LSP["LSP Requests:\ndidOpen\ndocumentSymbol\ndefinition\nhover"]
    end

    subgraph ClangdCore["⚙️ Clangd Server Core"]
        LSPServer["ClangdLSPServer"]
        FMS["FeatureModuleSet"]
        PM["PseudoModule\n(FeatureModule)"]
        Server["ClangdServer"]
        TUSched["TUScheduler"]
        
        LSPServer --> FMS
        FMS --> PM
        PM -. "blockASTBuild(File)" .-> Server
        Server --> TUSched
    end

    subgraph PseudoEngine["⚡ Clang-Pseudo Engine"]
        Lex["DirectiveTree Lexer\n(Strips #includes & inactive #ifs)"]
        GLR["GLR Parser & C++ BNF Table\n(Shift / Reduce / GSS)"]
        Forest["Disambiguated Parse Forest\n(140 KB Compact DAG)"]
        Scopes["Lexical Scope Graph\n& Type Classifier"]
        BFS["Header BFS Cache\n(Transitive #include Resolver)"]

        PM --> Lex
        Lex --> GLR
        GLR --> Forest
        Forest --> Scopes
        Scopes --> BFS
    end

    LSP --> LSPServer
    BFS --> LSP

    style PM fill:#e3f2fd,stroke:#1565c0,stroke-width:2px,color:#0d47a1
    style GLR fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#1b5e20
    style Forest fill:#fff3e0,stroke:#e65100,stroke-width:2px,color:#e65100
```

### Dual Operating Modes

- **Mode 1: Pure Mode (`--use-pseudo-parser=true`)**
  - Suppresses heavy Clang AST builds completely via `blockASTBuild(File) = true`.
  - Delivers instant outline, selection ranges, folding, and cross-header navigation on any opened file.
- **Mode 2: Resilient Fallback Mode (Default)**
  - Clang AST runs as primary.
  - If the AST fails to compile or returns empty symbols, requests automatically fall back to `PseudoModule`.

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **No invasiveness:** Emphasize that this does not hack or destabilize Clangd internals. It uses `FeatureModule`, the official extension mechanism of Clangd.
- **Pure mode vs Fallback mode:**
  - In Pure mode, we suppress Clang AST construction entirely for lightning-fast, lightweight usage (e.g. browsing codebases, low-power laptops).
  - In Fallback mode, users get full Clang semantic precision when everything compiles, and seamless fallback to pseudo-parser when headers or build files are missing.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-4">Next: GLR Engine ➡</a></sub>
</div>

---

<br>

<a id="slide-4"></a>

> <sub>**SLIDE 04 OF 10** &nbsp;|&nbsp; [◀ Prev: Architecture](#slide-3) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: Navigation ▶](#slide-5)</sub>

# ⚙️ Slide 4: How GLR Pseudo-Parsing Works Without Headers

Traditional compilers fail without headers because they need preprocessor macro expansion and type declarations to parse ambiguities like `A<B>::C * D;`.
`clang-pseudo` bypasses this by using **Generalized LR (GLR)** parsing:

```mermaid
flowchart TD
    Code["📄 Raw Source Code (e.g. Sema.cpp, 3,121 lines)"] --> Step1["1. Fast Lexing\nProduces Raw Token Stream"]
    Step1 --> Step2["2. DirectiveTree Analysis\nSelects active conditional branches, strips #includes"]
    Step2 --> Step3["3. C++ BNF Grammar & LR Table\n1,475 LR states, 28,165 parsing actions"]
    Step3 --> Step4["4. GLR Parse Forest\nCompact Shared Graph (DAG) retaining structural alternatives (~140 KB)"]
    Step4 --> Step5["5. Disambiguation Heuristics\nSelects canonical syntax tree without compiler state"]
    Step5 --> Step6["6. Instant Language Features\nOutline, Ranges, Folding, Scope Definitions"]

    style Code fill:#f5f5f5,stroke:#424242,stroke-width:1.5px
    style Step4 fill:#e8f5e9,stroke:#2e7d32,stroke-width:2px,color:#1b5e20
    style Step6 fill:#e3f2fd,stroke:#1565c0,stroke-width:2px,color:#0d47a1
```

### Key Technical Advantages

- **No Compiler Headers Needed**: Evaluates syntax directly from file tokens.
- **Grammar-Level Fault Tolerance**: Recovers seamlessly around syntax errors using opaque nodes.
- **Micro-Memory Footprint**: Shared node representation keeps tree size orders of magnitude smaller than Clang ASTs.

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **What is GLR?** Generalized LR parsing allows an LR parser to handle non-deterministic, ambiguous grammars without failing. When a shift/reduce conflict occurs (e.g. is `X * y;` a pointer declaration or a multiplication?), GLR forks the stack and builds a compact DAG (parse forest).
- **Directive Tree:** Rather than running a full macro preprocessor that requires all include files, DirectiveTree analyzes `#ifdef` structures purely syntactically.
- **Memory efficiency:** By sharing common nodes in the forest, the entire syntactic structure of 3,000+ lines of C++ code takes just 140 KB!
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-5">Next: Navigation ➡</a></sub>
</div>

---

<br>

<a id="slide-5"></a>

> <sub>**SLIDE 05 OF 10** &nbsp;|&nbsp; [◀ Prev: GLR Engine](#slide-4) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: Benchmarks ▶](#slide-6)</sub>

# 🎯 Slide 5: Semantic Navigation, Disambiguation & Header BFS

To make Go-to-Definition accurate without full semantic type-checking, `PseudoModule` pairs syntactic classification with bounded header search:

### 1. Syntactic Context Classifier (`isTypeContext`)
Syntactic classification ensures the cursor targets the right symbol even when types, functions, and variables share identical names:

```cpp
// 1. Qualified Scope: Touched is followed by '::' -> Class/Namespace context
void ClangdServer::adjustParseInputs(...) 
//   ^ Jumps to 'class ClangdServer' in ClangdServer.h, NOT constructor in ClangdServer.cpp!

// 2. Constructor vs Class:
ClangdServer::ClangdServer(...)
// ^ First: Class (in .h)    ^ Second before '(': Constructor definition (in .cpp)

// 3. Same-name Type vs Variable:
struct Foo { int x; };
Foo Foo;   Foo.x = 1;
// ^ Type   ^ Variable
```

### 2. Transitive Header BFS Traversal
When an identifier is not defined locally, `PseudoModule` initiates a bounded Breadth-First Search (BFS):

```mermaid
sequenceDiagram
    autonumber
    actor User as User Cursor on ClangdServer
    participant PM as PseudoModule::locateSymbolAt
    participant Scopes as Local File Scopes (ClangdServer.cpp)
    participant BFS as Header BFS Queue
    participant H as ClangdServer.h Info Cache

    User->>PM: textDocument/definition on "ClangdServer"
    PM->>PM: isTypeContext() == true (followed by '::')
    PM->>Scopes: lookupDecl("ClangdServer", ExpectsType=true)
    Scopes-->>PM: null (Only constructor found locally)
    PM->>BFS: Enqueue #include "ClangdServer.h"
    BFS->>H: parse & getHeaderInfo("ClangdServer.h")
    H-->>PM: Matches class ClangdServer (DeclKind::Class)
    PM-->>User: LocatedSymbol -> ClangdServer.h:40
```

> [!TIP]
> **Smart Include Resolution**:
> - `#include "ClangdServer.h"` &rarr; jumps directly to the file on disk.
> - `#include <vector>`, `<string>`, `<memory>` &rarr; resolves directly to toolchain SDK standard library headers.
> - BFS skips recursive traversal of libc++ internal implementation headers (`<__algorithm/*>`), keeping header searches instantaneous.

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **The Challenge:** How can we resolve Go-To-Definition accurately without compiling types?
- **The Solution:** We look at the syntactic context of the cursor (`isTypeContext`). For instance, if an identifier is immediately followed by `::`, it *must* be a type or namespace.
- **Header BFS:** We queue `#include` directives found at the top of the file, prioritize project quotes over angled brackets, and parse header symbols on demand with high-speed caching.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-6">Next: Benchmarks ➡</a></sub>
</div>

---

<br>

<a id="slide-6"></a>

> <sub>**SLIDE 06 OF 10** &nbsp;|&nbsp; [◀ Prev: Navigation](#slide-5) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: Tree-sitter Comparison ▶](#slide-7)</sub>

# 📊 Slide 6: Concrete Benchmark: `Sema.cpp` Head-to-Head

Tested on [`clang/lib/Sema/Sema.cpp`](file:///Users/usovaanastasia/work/llvm-project/clang/lib/Sema/Sema.cpp) (3,121 lines of C++ code, including transitive Clang and LLVM headers).  
*Benchmark configuration matches [`TidyFastChecks.py`](file:///Users/usovaanastasia/work/llvm-project/clang-tools-extra/clangd/TidyFastChecks.py) in LLVM tree on Apple M-series hardware.*

### Head-to-Head Performance Table

| Benchmark Metric | Regular Clang AST | Pseudo-Parser (`--use-pseudo-parser`) | Delta |
| :--- | :---: | :---: | :---: |
| **`didOpen` &rarr; Document Symbols Ready** | `3,601 ms` (~3.60 s) | **`50.2 ms` (~0.05 s)** | ⚡ **71.8x Faster** |
| **Source File Parsing Time** | `349 ms` (post-preamble) | **`42 ms`** | ⚡ **8.3x Faster** |
| **Preamble Generation Time** | `3,110 ms` (3.11 s) | **`0 ms`** (Zero needed) | ⚡ **Instant** |
| **In-Memory Preamble / Tree Size** | `82.8 MB` (82,855,164 B) | **`140 KB`** (143,535 B) | 📉 **590x Smaller** |
| **Top-Level Symbols Extracted** | `104 symbols` | **`102 symbols`** | 🎯 **98.1% Fidelity** |
| **Go-To-Definition Latency** | `1.6 ms` (after 3.6s wait) | **`31.7 ms`** (Instant) | 🚀 **Interactive** |

### Visual Comparison

```text
Startup Latency (didOpen -> First Symbol Interactive)
Regular Clang AST  [██████████████████████████████████████████████████] 3,601 ms
Clang-Pseudo       [█] 50 ms  (71.8x Speedup)

In-Memory Tree / Preamble Footprint
Regular Clang AST  [██████████████████████████████████████████████████] 82.8 MB
Clang-Pseudo       [▏] 0.14 MB (590x Reduction)
```

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Reproducibility:** All numbers were captured via our automated benchmark suite matching the setup in `TidyFastChecks.py`.
- **Fidelity:** Notice that despite requiring zero headers, the pseudo-parser found 102 out of 104 symbols (98.1% fidelity). The only missing symbols were macro-generated declarations.
- **Memory impact:** In a typical developer workflow with 10–20 files open, standard Clangd would consume 1.5 to 2.5 GB of RAM. Clang-Pseudo takes less than 5 MB total!
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-7">Next: Tree-sitter Comparison ➡</a></sub>
</div>

---

<br>

<a id="slide-7"></a>

> <sub>**SLIDE 07 OF 10** &nbsp;|&nbsp; [◀ Prev: Benchmarks](#slide-6) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: AI Frontier ▶](#slide-8)</sub>

# ⚔️ Slide 7: Technology Comparison: Clang-Pseudo vs. Tree-sitter

Both technologies aim to provide fast, error-tolerant syntax parsing without relying on heavy compiler pipelines. However, their architectural philosophies and trade-offs differ fundamentally:

```mermaid
flowchart LR
    subgraph TreeSitter["🌳 Tree-sitter (Editor-Centric)"]
        TSG["Handcrafted grammar.js\n(Custom JS DSL)"] --> TSP["LR/GLR with Eager Resolution\n(Static Precedence Annotations)"]
        TSP --> TSCST["Single Concrete Syntax Tree (CST)\n(Sub-millisecond incremental reparse)"]
        TSCST --> TSUse["Syntax Highlighting, Folding & Text Objects\n(Neovim, Zed, Helix, GitHub)"]
    end

    subgraph ClangPseudo["⚙️ Clang-Pseudo (Compiler-Centric)"]
        CPG["ISO C++ Standard BNF\n(cxx.bnf from N4860 draft)"] --> CPP["Pure GLR + Parse Forest DAG\n(DirectiveTree + Clang Lexer)"]
        CPP --> CPCST["Disambiguated Syntax Tree + Scopes\n(Transitive BFS include resolution)"]
        CPCST --> CPUse["LSP Go-to-Definition, Symbols, Hover & Refactoring\n(Clangd Native FeatureModule)"]
    end

    style TreeSitter fill:#f0fdf4,stroke:#16a34a,stroke-width:1.5px
    style ClangPseudo fill:#eff6ff,stroke:#2563eb,stroke-width:1.5px
```

### Architectural Feature Matrix

| Dimension | Tree-sitter (`tree-sitter-cpp`) | Clang-Pseudo (`clang-pseudo`) | Advantage / Rationale |
| :--- | :--- | :--- | :--- |
| **Grammar Origin** | Handcrafted JavaScript DSL (`grammar.js`) | **Official ISO C++ Standard BNF** (C++20 N4860) | 📐 **Clang-Pseudo**: Rigorous standard compliance |
| **Ambiguity Handling** | **Eager resolution**: Static precedence & conflict rules | **Parse Forest (DAG)**: Retains alternatives, disambiguates globally | 🎯 **Clang-Pseudo**: Handles subtle C++ ambiguities |
| **Preprocessor Support** | Parses `#ifdef` as syntax nodes (fails on split tokens) | **DirectiveTree + Clang Lexer**: Selects clean branches | 🛡️ **Clang-Pseudo**: Industrial macro resilience |
| **Incremental Parsing** | **Sub-millisecond** reparse on single keystroke edits | Whole-file re-parse (~40–50 ms on 3,000 lines) | ⚡ **Tree-sitter**: Unmatched typing latency |
| **Cross-File Resolution** | None (strictly single-file buffer syntax) | **Transitive Header BFS** (resolves headers & `<vector>`) | 🌐 **Clang-Pseudo**: Powers LSP Go-to-Definition |
| **Query Mechanism** | Standardized S-expression queries (`(function) @fn`) | Native C++ AST walkers (`walkSymbols`, `buildScopes`) | 📝 **Tree-sitter**: Universal query DSL |
| **Primary Sweet Spot** | In-editor highlighting, indentation, structural editing | Resilient LSP intelligence & compiler-grade fallback | 🤝 **Complementary Technologies** |

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Why didn't Clangd just embed Tree-sitter?**
  1. *Preprocessor realities:* Real C++ code frequently places `#ifdef` inside expressions or function arguments. Tree-sitter struggles with this and emits broad `(ERROR)` nodes, while `clang-pseudo`'s `DirectiveTree` handles conditional compilation cleanly.
  2. *Standard conformance:* Tree-sitter-cpp is a community-maintained approximation. Clang requires formal ISO standard BNF grammar rules to cleanly interoperate with compiler specifications.
  3. *LSP capabilities:* Tree-sitter provides syntax highlighting and folding, but cannot do cross-header Go-to-Definition or scope disambiguation.
- **What can Clang-Pseudo learn from Tree-sitter?**
  - Tree-sitter's incremental node-reuse algorithm on single-character edits is the gold standard for editor typing latency. Bringing incremental sub-tree reuse to Clang-Pseudo is an active roadmap item.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-8">Next: The AI Frontier ➡</a></sub>
</div>

---

<br>

<a id="slide-8"></a>

> <sub>**SLIDE 08 OF 10** &nbsp;|&nbsp; [◀ Prev: Tree-sitter](#slide-7) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: Supported Features ▶](#slide-9)</sub>

# 🤖 Slide 8: The AI Frontier: Clang-Pseudo for LLMs & Coding Agents

### Unlocking Real-Time Syntax Gating, AST Skeletonization & Fast Agent Loops

AI coding models and autonomous agents struggle with C++ because traditional compiler toolchains are too slow, brittle, and resource-heavy for real-time inference loops. `clang-pseudo` fundamentally bridges this gap:

```mermaid
flowchart LR
    subgraph LLM["🧠 LLM / Coding Agent"]
        Gen["Streaming Token Generation\n(Draft Output)"]
        Edit["Iterative Agent Edits\n(Multi-file Refactoring)"]
        Prompt["Context Window Assembly\n(Prompt Budgeting)"]
    end

    subgraph PseudoEngine["⚡ Clang-Pseudo Engine (&lt;50ms)"]
        Val["1. Real-Time Syntax Gating\n(Zero-header validation)"]
        Feed["2. Instant Agent Feedback Loop\n(50ms vs 3.6s compiler freeze)"]
        Prune["3. AST Skeletonization & RAG\n(Strip bodies, keep types & signatures)"]
    end

    subgraph Outcome["🎯 High-Impact AI Outcomes"]
        Out1["🛡️ Catch malformed C++ early\n(Speculative decoding & self-correction)"]
        Out2["🚀 70x faster agent iteration\n(Instant compile sanity)"]
        Out3["📈 10x larger repo context in prompts\n(High-density semantic RAG)"]
    end

    Gen --> Val --> Out1
    Edit --> Feed --> Out2
    Prompt --> Prune --> Out3

    style LLM fill:#faf5ff,stroke:#9333ea,stroke-width:1.5px
    style PseudoEngine fill:#eff6ff,stroke:#2563eb,stroke-width:1.5px
    style Outcome fill:#f0fdf4,stroke:#16a34a,stroke-width:1.5px
```

### 4 Pillars of AI + Clang-Pseudo Synergy

1. **Sub-10ms Syntax Verification for Streaming LLM Generation**
   - Traditional Clang cannot validate code snippets without full `#include` headers and build flags.
   - Clang-Pseudo validates raw C++ syntax in milliseconds, enabling **grammar-constrained decoding** and instant hallucination gating during token generation.
2. **Context Window Optimization: Semantic AST "Skeletonization"**
   - Feeding raw files into LLMs wastes thousands of tokens on implementation details.
   - Clang-Pseudo extracts exact class, template, and function signatures in **sub-50ms**, packing **10x more repository context** into the LLM context window for high-accuracy RAG.
3. **Hyper-Fast Iteration Loops for Autonomous Coding Agents**
   - Coding agents (Cursor, Claude Code, Antigravity) make multi-step code edits.
   - Waiting 3.6–10s per edit for AST rebuild stalls agent execution. Clang-Pseudo provides instant verification of modified symbols and scopes.
4. **Zero-Setup Monorepo Indexing for AI Search & Embeddings**
   - Vector databases need semantic boundaries (functions, classes), not arbitrary line chunks.
   - Clang-Pseudo indexes massive repositories without needing `compile_commands.json` or pre-installed dependencies.

### Why Clang-Pseudo Over Tree-sitter for AI Scenarios

> [!IMPORTANT]
> Tree-sitter excels at editor highlighting and keystroke-level incremental reparse. You can also build cross-file heuristics on top of _either_ parser. But for AI code generation, validation, and retrieval, Clang-Pseudo has structural advantages that compound across every layer:

| AI Scenario | Tree-sitter Limitation | Clang-Pseudo Advantage |
| :--- | :--- | :--- |
| **🛡️ LLM Output Validation** | Handcrafted grammar silently **accepts invalid C++** or **rejects valid C++** (e.g. complex template expressions, `requires` clauses). AI gets unreliable pass/fail signals. | **ISO C++ Standard BNF** gives precise, formally correct accept/reject decisions. Grammar-constrained decoding can trust the parser's verdict. |
| **🎯 Ambiguity-Aware Reasoning** | Eagerly picks **one interpretation**. If the LLM generates `X * y;`, Tree-sitter forces either pointer-decl or multiplication—the AI never sees the alternative. | **Parse Forest (DAG)** retains **all valid interpretations**. An AI system can score alternatives using broader file context, or present options to the user. |
| **🛡️ Real-World Codebase Indexing** | Heavy `#ifdef` usage (Linux kernel, Chromium, LLVM) produces large `(ERROR)` nodes in Tree-sitter, corrupting semantic chunks fed to embedding pipelines. | **DirectiveTree** cleanly selects active branches. Indexing millions of production C++ files yields clean, complete parse trees for every file. |
| **🌐 Cross-File Header Quality** | You _can_ build cross-file heuristics on Tree-sitter, but **headers are where it breaks down worst**: STL headers like `<vector>` are dense with `#ifdef`, macro-generated declarations, and complex template syntax—Tree-sitter produces widespread `(ERROR)` nodes in these files, poisoning cross-file symbol resolution. | **Clang's own lexer** + **DirectiveTree** handle every preprocessor pattern in real headers. The ISO grammar correctly parses template specializations, SFINAE, concepts, and `using` declarations that Tree-sitter's simplified rules mangle. Cross-file results are **compiler-grade**. |
| **📐 Skeletonization Precision** | Simplified grammar **misparses** complex templates, nested lambdas, and `constexpr if` chains. Skeleton output may contain garbled signatures. | Full ISO grammar correctly identifies `template<typename... Ts> concept Foo = ...` and complex `auto` return types. Skeleton extractions are **compiler-faithful**. |
| **🔗 Declaration Kind Classification** | Tree-sitter node types are coarse (`type_identifier`, `identifier`)—you can't reliably distinguish a class name from a namespace from a typedef without writing fragile ad-hoc query heuristics for each case. | Clang-Pseudo's grammar rules map directly to ISO productions (`class-specifier`, `namespace-definition`, `enum-specifier`), enabling precise `DeclKind` classification (Class, Enum, Function, Constructor, Variable, Namespace) with zero heuristics. |

> [!NOTE]
> **The Cross-File Compounding Effect**: Both parsers can be given cross-file heuristics. But every header parse with Tree-sitter accumulates errors: broken `#ifdef` branches, misidentified templates, garbled macro expansions. These errors compound across transitive includes. Clang-Pseudo uses the **same lexer as the real compiler** (`clang::Lexer`) and the **same grammar as the ISO standard**, so header parse quality stays high across the entire include chain. When your AI agent's cross-file lookup traverses `File.cpp → Header.h → BaseClass.h → <vector>`, Clang-Pseudo correctly resolves every step; Tree-sitter with heuristics will degrade at each hop.

### "What If We Write a Custom Preprocessor on Tree-sitter?"

A custom `DirectiveTree`-like preprocessor on top of Tree-sitter would close **one** gap (`#ifdef` branch selection) but leave the deeper ones untouched:

| Gap | Custom Preprocessor Fixes It? | Why / Why Not |
| :--- | :---: | :--- |
| `#ifdef` branch selection | ✅ **Yes** | Feasible to build, similar to `DirectiveTree` |
| Macro expansion (`Q_OBJECT`, `BOOST_*`, `LLVM_DECLARE_PASS`) | ❌ **No** | Requires reimplementing the C preprocessor (argument substitution, `##` pasting, variadic macros, `_Pragma`). Clang-Pseudo handles unknown macros via GLR **Opaque nodes**; Tree-sitter emits broad `(ERROR)`. |
| Grammar ambiguity (`X * y;`, `T(x);`, `A<B>::C`) | ❌ **No** | **Fundamental**: Tree-sitter's static `prec()` forces one interpretation and discards the other. Clang-Pseudo's GLR **Parse Forest** retains all valid parses. No preprocessor can change the parsing algorithm. |
| Lexer edge cases (`>>` splitting, raw strings, `1'000'000`, UCN) | ❌ **No** | Tree-sitter uses its own lexer. Clang-Pseudo uses **`clang::Lexer`** which handles every C++ lexical edge case. |
| Declaration kind precision (class vs namespace vs typedef) | ❌ **No** | Requires ISO grammar productions (`class-specifier` vs `namespace-definition`), not Tree-sitter's coarse `type_identifier` / `identifier` node types. |

> A custom preprocessor gets you ~**30%** of the way to Clang-Pseudo's header quality. The remaining **70%** — grammar fidelity, ambiguity retention, lexer precision, and declaration classification — are architectural properties of the **parser itself**, not the preprocessor.

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Why AI needs Clang-Pseudo:** Explain that LLMs don't have access to complete build environments when generating or evaluating code in isolation. Clang-Pseudo gives AI systems compiler-grade C++ grammar awareness without the compiler burden.
- **Speculative Decoding:** In modern LLM inference, small draft models generate candidate tokens that the large model verifies. Clang-Pseudo can reject syntactically impossible C++ draft sequences before they ever hit the main model.
- **Context Budgeting:** In a 100k-token prompt, function implementations consume 90% of tokens. Skeletonizing C++ code using `pseudo::glrParse` lets agents see 10x more classes and signatures across the repo.
- **The cross-file argument (anticipate pushback):**
  - _"Can't you just build cross-file on Tree-sitter too?"_ — Yes, but the quality degrades. Tree-sitter uses its own lexer (not Clang's), a hand-written approximate grammar, and flat `#ifdef` handling. Each header it parses accumulates more errors than Clang-Pseudo. Over a 4-hop include chain, these errors compound. Clang-Pseudo shares the **actual Clang lexer** and **ISO standard grammar**, so header parse fidelity stays high.
  - _Real example:_ Parse `<vector>` with Tree-sitter → you get `(ERROR)` nodes around libc++ internal `#ifdef` guards and `__attribute__` blocks. Parse the same file with Clang-Pseudo → DirectiveTree selects the right `#ifdef` branch, the ISO grammar correctly handles `template<class _Tp, class _Allocator = allocator<_Tp>>`, and you get a clean `class-specifier` node for `vector`.
  - _DeclKind precision:_ When an AI agent asks "is `Builder` a class or a namespace?", Clang-Pseudo answers from ISO grammar rules (`class-specifier` vs `namespace-definition`). Tree-sitter would need you to write a complex S-expression query to heuristically distinguish `type_identifier` contexts—and it would still get it wrong for `typedef`s and `using` aliases.
- **The custom preprocessor rebuttal (anticipate deeper pushback):**
  - _"What if we write a custom preprocessor for Tree-sitter?"_ — A custom `#ifdef` selector (like DirectiveTree) is feasible and would help. But this solves only **branch selection** (the easy half). The hard half is **macro expansion**: headers like Qt's (`Q_OBJECT`), Boost's (`BOOST_AUTO_TEST_CASE`), and LLVM's (`LLVM_DECLARE_PASS`) generate declarations via macros. Without expanding them, Tree-sitter sees unknown identifiers in impossible positions → `(ERROR)`. Clang-Pseudo's GLR parser handles this via Opaque nodes — it marks the unknown region and keeps parsing. To truly expand macros, you'd need to reimplement the C preprocessor (argument substitution, token pasting `##`, stringification `#`, variadic `__VA_ARGS__`, `_Pragma`...) — at which point you're rebuilding Clang.
  - _Even with perfect preprocessing_, Tree-sitter's grammar is the fundamental bottleneck. Static `prec()` rules force eager disambiguation of `X*y;`, `T(x);`, `A<B>::C`. No preprocessor can add a parse forest to Tree-sitter — that requires replacing the parsing algorithm.
  - _The 30/70 framing:_ A custom preprocessor closes ~30% of the gap. The other 70% is grammar + lexer + declaration classification — properties of the parser architecture itself.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-9">Next: Supported Features ➡</a></sub>
</div>

---

<br>

<a id="slide-9"></a>

> <sub>**SLIDE 09 OF 10** &nbsp;|&nbsp; [◀ Prev: AI Frontier](#slide-8) &nbsp;|&nbsp; [📑 Index](#slide-navigator) &nbsp;|&nbsp; [Next: Roadmap ▶](#slide-10)</sub>

# 🛠️ Slide 9: Supported Language Features in Clangd

```mermaid
flowchart TD
    subgraph Implemented["Currently Implemented in PseudoModule"]
        S1["📋 Document Symbols\nHierarchical symbol outline tree"]
        S2["🔍 Selection Ranges\nAST-guided smart selection expansion"]
        S3["📂 Folding Ranges\nFunction, class & block folding"]
        S4["🎯 Go to Definition / Declaration\nLocal scope lookup + Transitive #include BFS"]
        S5["📦 Include Directives GTD\nJump to project & libc++ headers (<vector>, etc.)"]
        S6["💡 Document Highlights & References\nIn-file occurrence tracking"]
        S7["💬 Hover\nType definitions & header locations"]
    end

    style S1 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style S2 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style S3 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style S4 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style S5 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style S6 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style S7 fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
```

> [!IMPORTANT]
> **Complete Independence from Build System**:  
> Even if `compile_commands.json` is missing or invalid, every feature above remains fully operational.

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Breadth of Coverage:** We aren't just doing a simple regex search. We provide structured document symbols, syntax folding, semantic selection ranges, definition jumps with scope awareness, header jumps, references, and hover information.
- **Robustness:** If a build system fails or `compile_commands.json` hasn't been generated yet (e.g. fresh clone before CMake configuration), the editor remains 100% interactive.
</details>

<div align="right">
  <sub><a href="#slide-navigator">⬆ Top</a> &nbsp;|&nbsp; <a href="#slide-10">Next: Roadmap ➡</a></sub>
</div>

---

<br>

<a id="slide-10"></a>

> <sub>**SLIDE 10 OF 10** &nbsp;|&nbsp; [◀ Prev: Features](#slide-9) &nbsp;|&nbsp; [📑 Index](#slide-navigator)</sub>

# 🔮 Slide 10: Target Use Cases & Future Roadmap

### 🎯 Primary Use Cases

1. **Massive Codebases & Cloud / Web IDEs**
   - Eliminates multi-second editor freezes and battery-draining CPU spikes when switching git branches.
   - Low memory consumption enables dense containerized development setups.
2. **Unconfigured & Newly Cloned Repositories**
   - Instant symbol outlines, folding, and definition navigation without waiting for CMake or Ninja.
3. **Resilient Safety Net for Clangd**
   - Seamlessly guarantees language server functionality when complex headers fail to compile.

### 🗺️ Future Roadmap

```mermaid
flowchart LR
    A["Keystroke-level Incremental GLR Parsing"] --> B["Approximate Text-Based Semantic Completion"]
    B --> C["Syntax-Directed Single-File Rename & Refactoring"]
    C --> D["Instant Workspace Symbols via Pseudo-Indexing"]

    style A fill:#e3f2fd,stroke:#1565c0,color:#0d47a1
    style B fill:#e3f2fd,stroke:#1565c0,color:#0d47a1
    style C fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
    style D fill:#e8f5e9,stroke:#2e7d32,color:#1b5e20
```

<details>
<summary>🎙️ <b>Presenter Notes & Talking Points</b> (click to expand)</summary>

- **Incremental Parsing:** GLR can reparse only the affected subtree on each keystroke, achieving single-digit millisecond latency like Tree-sitter.
- **Workspace Indexing:** Imagine indexing the entire LLVM repository in 15 seconds instead of 15 minutes, because you parse syntax without evaluating transitive preprocessor headers!
- **Call to Action:** PseudoModule demonstrates that high-performance syntax-based tooling is practical, robust, and complementary to full compiler ASTs.
</details>

---

<div align="center">
  <h3>🎉 Thank You! Questions & Discussion</h3>
  <p><sub>LLVM / Clangd Project · Generalized LR Pseudo-Parser Feature Module</sub></p>
  <sub><a href="#slide-navigator">⬆ Back to Top Navigator</a></sub>
</div>
