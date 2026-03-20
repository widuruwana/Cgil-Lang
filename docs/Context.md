# Cgil Compiler — Project Context
**Version:** 1.7 (Final Spec — Do Not Revise Further)  
**Status:** Spec complete. Lexer implementation phase.

---

## What This Project Is

Cgil (pronounced *Sigil*) is a high-fantasy RPG-themed systems programming language that transpiles to C. It is being built as a portfolio piece targeting Dr. Chamath at UCSC / WSO2. The deliverable is a working C++ transpiler that compiles a bare-metal x86 disk driver and boots it on QEMU.

**File extension:** `.gil`  
**Compiler pipeline:** Lexer → Parser → AST → C CodeGen (two-pass, forward-declaration first)  
**Backend:** Emits standard C using `<stdint.h>` types. No LLVM. No runtime. No heap.  
**Target:** 16-bit bare-metal x86 (QEMU emulated)

---

## Type System & C Mapping

| Cgil Primitive | Generated C Type | Notes |
|---|---|---|
| `mark16` | `int16_t` | Native word size |
| `mark32` | `int32_t` | |
| `soul16` | `uint16_t` | |
| `soul32` | `uint32_t` | |
| `addr` | `uint16_t` | Alias for soul16. Hardware addresses only. |
| `rune` | `uint8_t` | 8-bit char/byte |
| `oath` | `uint8_t` | `kept`=1, `forsaken`=0 |
| `flow` | `float` | Emits kernel-mode warning (no HW FPU on 16-bit) |
| `abyss` | `void` | |
| `deck[N] T` | `T[N]` | Stack-allocated fixed array. e.g. `deck[80] rune` → `uint8_t[80]` |

### `scroll` (Fat Pointer String)
```c
typedef struct {
    uint8_t* ptr;
    uint16_t len;   // excludes null terminator
} Cgil_Scroll;
```
- Passed by value to `conjure`d spells.
- Literals emit as: `(Cgil_Scroll){ .ptr = (uint8_t*)"hello", .len = 5 }`
- Allocates NO memory itself. Kernel-safe.

### `rank` (Enum)
Sequential `uint16_t` discriminants from 0 in declaration order.
```c
// rank DiskError { Timeout, HardwareFault, InvalidSector }
typedef uint16_t DiskError;
#define DiskError_Timeout       ((uint16_t)0)
#define DiskError_HardwareFault ((uint16_t)1)
#define DiskError_InvalidSector ((uint16_t)2)
```

### `tuple`
Ephemeral boundary type. Used only for multi-value `yield` and `divine` patterns. Never assignable to a named variable directly.
```c
// (sigil* Disk, scroll | ruin<DiskError>) emits:
typedef struct {
    Disk* __elem0;
    Omen_scroll_DiskError __elem1;
} Tuple_DiskPtr_Omen_scroll_DiskError;
```

### Integer Literal Inference
Untyped literals default to `mark16`. Implicitly widened to target type if in bounds. Out-of-bounds = compile error.

---

## Control Flow

| Cgil | C Equivalent |
|---|---|
| `if` / `elif` / `else` | `if` / `else if` / `else` |
| `fore (mark16 i = 0; i < 10; i++)` | `for (...)` |
| `whirl (condition)` | `while (condition)` |
| `yield` | `return` |
| `shatter` | `break` |
| `surge` | `continue` |

### Operator Precedence (Highest → Lowest)
1. `?` — Omen unpack
2. `->`, `.` — Member / pointer access
3. `*`, `/`
4. `+`, `-`
5. `==`, `!=`, `>`, `<`
6. `~>` — Weave (pipeline)
7. `<~` — Reverse weave (ownership extract)
8. `|` — Omen union

---

## Pointers & Ownership

- C-style pointers: `*`, `&`, `->`. **Auto-deref is forbidden.** Always use `->` for pointer field access.
- `own` keyword transfers state responsibility of a pointer to a spell.
- Caller **cannot use** the pointer again until `<~` rebinds it after a `divine` block.
- **CodeGen rule:** `own` is compile-time only. Stripped from emitted C. `own &my_disk` emits as `&my_disk`.
- Unrecoverable hardware exceptions → Panic/Abort. Ownership voided. OS halts.

---

## Imports & Externs

| Keyword | Meaning |
|---|---|
| `grimoire <file.h>` | Internal bare-metal OS header (Ring 0) |
| `pact <file.h>` | External hosted C library (Ring 3 / userspace only) |
| `conjure spell name(...) -> T` | Forward-declares an external C function |
| `conjure endless spell name(...) -> abyss` | Declares a noreturn function (e.g. kernel_panic). Suppresses missing-yield / unbound-own warnings in that branch. |

---

## OS-Level Mechanics (The Differentiators)

### `leyline` — MMIO Hardware Anchoring
Variables locked to physical RAM addresses. Emitted as `volatile` pointers.
```
leyline vga_buffer: deck[80] rune @ 0xB8000;
```
- Using the identifier evaluates to the **value at the address** (auto-deref).
- `&leyline_name` evaluates to the physical address as a compile-time `addr` constant (not a pointer).

### `portline` — Port I/O Hardware Anchoring
Variables locked to x86 Port I/O address space. Emits inline assembly.
```
portline disk_status_port: rune   @ 0x1F7;  // 8-bit status
portline disk_data_port:   soul16 @ 0x1F0;  // 16-bit data
```
**Instruction selection:**
- `rune` (8-bit) → `inb` / `outb`
- `soul16` (16-bit) → `inw` / `outw`

**Read template (rune):**
```c
uint8_t __tmp; asm volatile("inb %1, %0" : "=a"(__tmp) : "d"((uint16_t)0x1F7));
```
**Write template (rune):**
```c
asm volatile("outb %0, %1" :: "a"((uint8_t)VAL), "d"((uint16_t)0x1F7));
```
- `&portline_name` → compile-time `addr` constant of the port number.

---

### `sigil` — Struct with Optional Stances
```
sigil Disk {
    stance Idle;      // discriminant 0
    stance Reading;   // discriminant 1
    stance Fault;     // discriminant 2
    soul16 sector_count;
}
```
**Emitted C:**
```c
typedef struct {
    uint16_t __stance;      // implicit, always first field
    uint16_t sector_count;  // user fields in declaration order
} Disk;
```
- Stances get sequential `soul16` discriminants from 0.
- `.stance` and `->stance` are **reserved read-only accessors**. `.stance` emits `.__stance`. `->stance` emits `->__stance`.
- Direct writes to `__stance` are a compile error. Change stance via assignment only: `ctrl = Disk:Reading`.
- In expression context, `Disk:Fault` evaluates to its `soul16` discriminant (compile-time constant).

**Initialization:**
```
sigil Disk my_disk = Disk:Idle { sector_count: 0 };
```
- `{ }` block accepts only user-declared fields. Attempting to set `__stance` directly = compile error.

**Stance Syntax:**
- `:` (single colon) for stance transition/annotation: `Disk:Idle`, `ctrl = Disk:Reading`
- `::` (double colon) for rank variant resolution: `DiskError::HardwareFault`

**Unknown Stance:** After a `divine` block where branches diverge on stance, the rebound variable's stance is `Unknown`. It can only be passed to spells accepting unannotated `sigil* Disk`. Reset with a Stance Cast: `my_disk = Disk:Idle;` (outside divine block).

---

### `destined` — RAII Cleanup
```
destined (condition) { cleanup_body; }
```
Executes right before the spell yields, regardless of which `yield` fires. Condition is optional — evaluates current state to allow conditional cleanup (e.g. don't reset a faulted controller).

**CodeGen expansion (goto pattern):**
```c
Tuple_Y fetch_sector(...) {
    ctrl->__stance = 1;     // ctrl = Disk:Reading
    Tuple_Y __ret;

    // ... body ...
    __ret = (Tuple_Y){ ctrl, data };
    goto __destined_0;

__destined_0:
    if (ctrl->__stance != 2) {   // destined condition
        ctrl->__stance = 0;
    }
    return __ret;
}
```
- Multiple `destined` blocks chain in **LIFO order** (last declared fires first).
- Every `yield` rewrites to assign `__ret` and `goto __destined_N`.

---

### Omens — Typed Error Handling

**Syntax:**
- `T | ruin<R>` — return type union (success or error)
- `?` — unpack operator. Binds tighter than `~>`. On success: unwraps value. On ruin: immediately yields the ruin up the chain.
- `ruin(Rank::Variant)` — **construction** syntax (in `yield` statements)
- `ruin<Rank::Variant>` — **pattern** syntax (in `divine` branches only)

**CodeGen ABI:**
```c
typedef struct {
    uint8_t __is_ruin;   // 0 = success, 1 = ruin
    union {
        Cgil_Scroll __value;  // success payload T
        uint16_t    __ruin;   // error discriminant R
    };
} Omen_scroll_DiskError;
```
**Naming convention:** `Omen_<T_name>_<R_name>`. Pointer types append `Ptr`. Compiler generates each typedef exactly once.

---

### `divine` — Pattern Matching

Unpacks Omen tuples and rebinds ownership. Must be **exhaustive**: exactly one typed success branch + all ruin variants covered OR a catch-all `(ctrl, ruin err)`.

```
my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
    (ctrl, scroll data)                    => { process_data(data); }
    (ctrl, ruin<DiskError::HardwareFault>) => { kernel_panic("Drive dead"); }
    (ctrl, ruin err)                       => { /* catch-all */ }
}
```

- `<~` (reverse weave) extracts `__elem0` (ownership pointer) and rebinds to left-hand variable.
- `__elem1` (result) is consumed inside branches.
- Inside branches, the caller's original variable is **still owned-away** — do not access it inside the block.
- Stance cast / retry logic goes **after** the `divine` block once `<~` rebinding is complete.

**CodeGen expansion:**
```c
Tuple_DiskPtr_Omen_scroll_DiskError __result = fetch_sector(&my_disk, 0x0500);
my_disk = __result.__elem0;                       // <~ rebinding
if (!__result.__elem1.__is_ruin) {                // success
    Cgil_Scroll data = __result.__elem1.__value;
    process_data(data);
} else if (__result.__elem1.__ruin == 1) {        // HardwareFault discriminant
    kernel_panic((Cgil_Scroll){.ptr=(uint8_t*)"Drive dead", .len=10});
} else {                                          // catch-all
}
```

---

### `warden spell` — Interrupt Service Routines

```
warden spell disk_irq() -> abyss {
    acknowledge_interrupt();
}
```
- No arguments. Yields `abyss`. Cannot use `own`. Cannot propagate `ruin`.
- Can access `leyline`/`portline` variables. Can use `destined`. Can call non-blocking spells.
- **CodeGen rule:** Emits `__attribute__((interrupt))` on the generated C function.

---

### `legion` — Data-Oriented SoA (V1 Stub)
Syntax identical to `sigil`. **V1 CodeGen:** parsed as distinct keyword to demonstrate architectural intent, but stubbed as a standard `sigil` in code generation. Full SoA transformation deferred to V2.

---

### `~>` Weave Operator
Passes the output of the left expression as the **first argument** to the right expression.
```
scroll data = &disk_data_port ~> read_buffer()?;
// emits: read_buffer(&disk_data_port)  (then ? unpacks)
```

---

## Complete Token Set (Lexer Reference)

```cpp
enum class TokenType {
    // Keywords
    SPELL, SIGIL, RANK, LEGION, LEYLINE, PORTLINE,
    GRIMOIRE, PACT, CONJURE, ENDLESS, WARDEN,
    YIELD, SHATTER, SURGE, FORE, WHIRL,
    IF, ELIF, ELSE, OWN, DIVINE, DESTINED,
    STANCE, RUIN, MARK16, MARK32, SOUL16, SOUL32,
    ADDR, FLOW, RUNE, OATH, SCROLL, ABYSS,
    KEPT, FORSAKEN, DECK, TUPLE,

    // Operators
    ARROW,      // ->
    DOT,        // .
    STAR,       // *
    SLASH,      // /
    PLUS,       // +
    MINUS,      // -
    EQ,         // ==
    NEQ,        // !=
    GT,         // >
    LT,         // <
    WEAVE,      // ~>
    REV_WEAVE,  // <~
    PIPE,       // |
    QUESTION,   // ?
    AMP,        // &
    SCOPE,      // ::
    COLON,      // :

    // Literals & Identifiers
    IDENT, INT_LIT, STRING_LIT,

    // Delimiters
    LBRACE, RBRACE, LPAREN, RPAREN, LBRACKET, RBRACKET,
    SEMICOLON, COMMA, AT,

    END_OF_FILE
};
```

---

## V1 Implementation Order

Build in this sequence for maximum demo impact:

1. **Lexer** — tokenize all keywords, operators, literals
2. **Parser + AST** — build nodes for all constructs
3. **`leyline` / `portline` CodeGen** — proves hardware anchoring, emits `volatile` / inline asm
4. **`rank` + `ruin<Rank>` CodeGen** — proves typed error handling
5. **`stance` semantic analysis** — proves typestate enforcement (the crown feature)
6. **`destined` CodeGen** — RAII via goto pattern
7. **`divine` + `<~` CodeGen** — completes ownership cycle
8. **`warden spell` CodeGen** — ISR with `__attribute__((interrupt))`
9. **`legion` stub** — one-line CodeGen redirect to sigil

---

## The Demo Target

Boot the disk driver example on QEMU. The narrative for the room:

| Feature | C Bug It Eliminates |
|---|---|
| `stance` typestate | Reading from an uninitialized / faulted controller → compile error |
| `leyline` / `portline` | Volatile optimized away / wrong I/O mechanism → impossible |
| `destined` | Forgetting to release hardware lock on early return → impossible |
| `own` + `divine` | Caller using hardware after transferring ownership → compile error |
| `ruin<Rank>` | Silently ignoring error codes → impossible |

---

## What NOT To Do

- **Do not open a new spec draft.** The spec is done. Answer questions in code.
- **Do not implement `legion` SoA in V1.** Stub it.
- **Do not add `flow` to kernel code.** It emits a warning for a reason.
- **Do not use `pact` in kernel context.** Ring 3 only.
- **Do not auto-deref pointers.** Always `->`.
