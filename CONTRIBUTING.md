# Contributing to Cgil

The forge welcomes contributors. Before you open a PR, read this.

---

## The Core Rule

Every change must pass the full test suite. No exceptions. If your change breaks a test, fix the test first or justify why the test was wrong. The 11 tests represent verified behavior — they are not negotiable.

```bash
# Run all tests
for i in test_cases/*.gil; do
    ./cgilc "$i" --target=host -o test_out && ./test_out
done
```

---

## What Is Welcome

**Bug reports** — with a minimal `.gil` reproduction case. If the compiler crashes without a Cgil-level error pointing at your source, that is always a bug. The compiler must never produce GCC-rejected output without first producing a Cgil error.

**Failing tests** — open an issue with the `.gil` file that fails and what you expected. If you can also point at which phase (Lexer/Parser/SA/CodeGen) is responsible, even better.

**Standard library contributions** — see the `std/` directory once it exists. New grimoires must be written in Cgil itself, not as raw conjure wrappers. The C machinery lives inside the grimoire files, hidden from users.

**Test cases** — new `.gil` files that exercise behavior not covered by the existing 11. Focus on edge cases in ownership, stance transitions, and error propagation.

---

## What Is Not Welcome Right Now

- New language features. The spec is frozen at v1.7. We are building the OS, not extending the language.
- Changes to the Cgil keyword vocabulary. Every keyword was chosen deliberately.
- Dependency additions to the compiler. `cgilc` must build with only a C++17 compiler and GNU Make.

---

## Code Style

The codebase is heavily commented. New code should be equally documented. Each visitor function should explain: what it handles, what it validates, what it produces, and why.

When you add a fix, document the bug it fixes in the comment above the fix — the same way the existing bugs are documented. Future readers should be able to understand the history from the code alone.

---

## Reporting a Compiler Bug

Open an issue with:

1. The minimal `.gil` file that reproduces the issue
2. What you ran: `cgilc yourfile.gil [flags]`
3. What you got (error message, wrong output, crash)
4. What you expected

If the bug is in generated C output, include the `.c` file from `--emit-c`.

---

## Architecture Reference

If you are working on a specific phase, the relevant files are:

| Phase | Files |
|-------|-------|
| Lexer | `include/Lexer/Lexer.h`, `src/Lexer/Lexer.cpp` |
| Parser | `include/Parser/Parser.h`, `include/Parser/AST.h`, `src/Parser/Parser.cpp` |
| Semantic Analyzer | `include/Semantics/SemanticAnalyzer.h`, `include/Semantics/Types.h`, `include/Semantics/SymbolTable.h`, `src/Semantics/SemanticAnalyzer.cpp` |
| Code Generator | `include/CodeGen/CodeGen.h`, `src/CodeGen/CodeGen.cpp` |
| Entry Point | `src/main.cpp` |

The AST uses the Visitor pattern. Every new expression or statement node requires:
1. A struct in `AST.h` inheriting from `Expr` or `Stmt`
2. A forward declaration at the top of `AST.h`
3. A pure virtual `visit(NewNode*)` in `ASTVisitor`
4. Implementations in both `SemanticAnalyzer.cpp` and `CodeGen.cpp`

Missing any of these produces a compile error, which is the intended safety net.
