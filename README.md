<div align="center">

**The Arcane Systems Forge**

*A memory-safe, typestate-driven systems language targeting bare-metal Ring 0.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Stage: V1.0 Golden Master](https://img.shields.io/badge/Stage-V1.0%20Golden%20Master-brightgreen)]()
[![Target: Bare Metal x86](https://img.shields.io/badge/Target-Bare%20Metal%20x86-blue)]()
[![Backend: GNU C11](https://img.shields.io/badge/Backend-GNU%20C11-orange)]()

</div>

---

> *"Coding is modern-day magic. Every function is a spell. Every struct is a sigil carved into the bare earth of memory. Cgil makes that metaphor architecturally true."*

---

## What Is Cgil?

Cgil (pronounced *Sigil*) is a high-fantasy themed systems programming language that transpiles to GNU C11. It was designed from first principles to make bare-metal OS development **safe by construction** — not by convention. Every keyword maps to a real systems concept. The metaphors are not decoration. They are the mechanism.

**File extension:** `.gil`  
**Pipeline:** Cgil Source → Lexer → Parser → Semantic Analyzer → CodeGen → GCC → Binary  
**Target:** Bare-metal x86 (Ring 0), testable on QEMU  
**Backend:** Pure GNU C11, no LLVM, no runtime, no heap allocation

The compiler is a single self-contained C++17 binary (`cgilc`). It produces a C intermediate file which GCC compiles to your target. No dependencies beyond GCC.

---

## The Five Pillars That Make It Different

### 1. Native Typestate Enforcement

Hardware has states. A disk controller is either `Idle`, `Reading`, or `Faulted`. Every systems language knows this. Only Cgil *enforces* it at compile time.

```cgil
sigil Disk {
    stance Idle;
    stance Reading;
    stance Fault;
    soul16 sector_count;
}

// This spell REQUIRES the disk to be in Idle stance.
// Passing a Reading or Faulted disk is a compile error — not a runtime crash.
spell fetch_sector(own sigil* Disk:Idle d, soul16 sector) -> scroll | ruin<DiskError> {
    d = Disk:Reading;
    // ...
}
```

The compiler tracks `__stance` as a `uint16_t` discriminant inside every sigil struct. Transitions are validated statically. You cannot pass a faulted controller to a spell that expects an idle one. The error happens at the forge, not in production.

---

### 2. Ownership Transfer with `own` and `divine`

When a spell takes ownership of a hardware resource, the caller cannot use it until ownership returns. This is enforced by the semantic analyzer, not by convention.

```cgil
// my_disk is MOVED into ignite(). Using my_disk after this line is a compile error.
// The <~ operator rebinds ownership when ignite() returns.
my_disk <~ divine ignite(own &my_disk) {

    // Success branch — disk came back healthy
    (ctrl) => {
        ctrl = Disk:Idle;
    }

    // Specific error match — hardware fault
    (ctrl, ruin<DiskError::HardwareFault>) => {
        kernel_panic("Drive is dead.");
    }

    // Catch-all error
    (ctrl, ruin err) => {
        ctrl = Disk:Fault;
    }
}
// my_disk is accessible again here. ctrl is out of scope.
```

The `divine` pattern is exhaustive by construction. Every variant of the error rank must be covered, or the compiler refuses to proceed.

---

### 3. Deterministic RAII via `destined`

In C, forgetting to reset a hardware lock on an early return corrupts the system silently. In Cgil, `destined` blocks fire before every `yield` regardless of which exit path is taken. The cleanup is guaranteed.

```cgil
spell acquire_bus(own sigil* Controller:Free ctrl) -> (sigil* Controller, abyss | ruin<BusError>) {

    // This cleanup fires on EVERY yield in this spell — normal or error.
    destined {
        ctrl->flags = 0;          // Always release the bus flags
        ctrl = Controller:Free;   // Always restore the stance
    }

    ctrl = Controller:Busy;

    if (ctrl->error_code != 0) {
        yield (ctrl, ruin(BusError::Locked)); // destined fires before this returns
    }

    yield (ctrl); // destined fires before this returns too
}
```

The CodeGen expands every `yield` into `__ret = ...; goto __destined_N;` and emits the cleanup labels in LIFO order at the end of the function. This is mathematically guaranteed — not a convention.

---

### 4. Hardware Anchoring via `leyline` and `portline`

x86 hardware exists in two separate address spaces: memory-mapped registers (MMIO) and port I/O. C treats them identically and relies on the programmer to use `volatile` correctly and choose between pointer dereference and `inb`/`outb`. Cgil makes the distinction structural.

```cgil
// MMIO: locked to a physical RAM address. Emits as volatile pointer.
leyline vga_buffer: mark16 @ 0xB8000;

// Port I/O: lives in the x86 port address space. Emits inline asm on every access.
portline disk_status: rune @ 0x1F7;
portline disk_data:   soul16 @ 0x1F0;

spell read_status() -> rune {
    // Reading disk_status emits: inb 0x1F7
    // The compiler knows this is port I/O. You cannot accidentally use a pointer.
    rune status = disk_status;
    yield status;
}
```

The compiler enforces the correct mechanism at every access site. You cannot take a volatile pointer to a portline. You cannot use `inb` on a leyline. The architecture is impossible to confuse.

---

### 5. Typed Error Propagation with `Omen` and `?`

Cgil has no exceptions. Errors are values, carried in a typed union called an Omen. The `?` operator propagates errors up the call chain automatically — but only when the enclosing spell declares it can do so.

```cgil
rank DiskError {
    Timeout,
    HardwareFault,
    InvalidSector
}

spell check_sector(soul16 sector) -> scroll | ruin<DiskError> {
    if (sector > 2048) {
        yield ruin(DiskError::InvalidSector);
    }
    yield "SECTOR_DATA";
}

spell read_disk(own sigil* Disk:Idle d, soul16 sector) -> (sigil* Disk, scroll | ruin<DiskError>) {
    destined { }  // Required for ? to safely repackage the early return

    // If check_sector fails, ? immediately propagates the ruin
    // and packages d back into the return tuple — hardware is never leaked.
    scroll data = check_sector(sector)?;

    yield (d, data);
}
```

The `?` operator is not syntactic sugar over exceptions. It is a deterministic, zero-cost compile-time transformation that guarantees hardware ownership is preserved on every error path.

---

## Language Reference

### Primitives

| Cgil | C Type | Notes |
|------|--------|-------|
| `mark16` | `int16_t` | Signed 16-bit |
| `mark32` | `int32_t` | Signed 32-bit |
| `soul16` | `uint16_t` | Unsigned 16-bit |
| `soul32` | `uint32_t` | Unsigned 32-bit |
| `addr` | `uint16_t` | Hardware address |
| `rune` | `uint8_t` | Single byte |
| `oath` | `uint8_t` | Boolean (`kept` = 1, `forsaken` = 0) |
| `flow` | `float` | ⚠️ Emits FPU warning in kernel context |
| `scroll` | `Cgil_Scroll` | Fat pointer `{ rune* ptr; soul16 len; }` |
| `abyss` | `void` | No value |

### Arrays and Data Structures

```cgil
// Fixed-size stack array
deck[256] rune buffer;

// Sigil — struct with optional typestates
sigil Packet {
    stance Valid;
    stance Corrupt;
    soul16 length;
    soul16 checksum;
}

// Rank — typed enum with uint16_t discriminants
rank NetError { Timeout, Checksum, Overflow }

// Legion — Structure of Arrays (cache-optimal batch processing)
// The compiler transparently splits this into separate arrays per field
legion Particle {
    mark16 pos_x;
    mark16 pos_y;
    mark16 velocity;
}

deck[1000] Particle swarm; // Emits: mark16 swarm_pos_x[1000]; mark16 swarm_pos_y[1000]; ...
swarm[5].pos_x = 100;      // Emits: swarm_pos_x[5] = 100;
```

### Control Flow

```cgil
// Conditionals
if (x == 0) {
    // ...
} elif (x < 0) {
    // ...
} else {
    // ...
}

// Loops
fore (mark16 i = 0; i < 10; i++) { }  // for
whirl (condition) { }                   // while
shatter;                                // break
surge;                                  // continue

// Returns
yield value;   // return value
yield;         // return void
```

### Operators

```cgil
// Standard arithmetic and comparison
+  -  *  /  %  ==  !=  >  <  >=  <=

// Bitwise
&  |  ^  ~  <<  >>

// Logical
&&  ||

// Cgil-specific
~>   // Weave: passes left as first arg to right spell
<~   // Reverse weave: rebinds ownership from divine result
?    // Omen unpack: propagate ruin or unwrap success
::   // Rank variant: DiskError::HardwareFault
:    // Stance annotation: Disk:Idle

// Explicit casting
cast<mark32>(some_rune)    // value cast
cast<mark16*>(some_addr)   // pointer cast
```

### Spells (Functions)

```cgil
// Basic spell
spell add(mark16 a, mark16 b) -> mark16 {
    yield a + b;
}

// Hardware ISR — cannot propagate errors, cannot use FPU
warden spell disk_irq() -> abyss {
    // Handle interrupt
}

// External C function binding
conjure spell putchar(mark32 c) -> mark32;

// Noreturn external
conjure endless spell kernel_panic(scroll msg) -> abyss;
```

### Imports

```cgil
// Internal OS header (Ring 0 safe)
grimoire <hardware_defs.h>;

// Hosted C library (Ring 3 only — emits warning in kernel context)
pact <stdio.h>;
```

---

## Quick Start

### Building the Compiler

**Requirements:** GCC or Clang (C++17), GNU Make

```bash
git clone https://github.com/CgilLang/cgil.git
cd cgil
make
# Produces: cgilc.exe (Windows) or cgilc (Linux/macOS)
```

### Hello from the Forge

```cgil
// hello.gil
pact <stdio.h>;
conjure spell putchar(mark32 c) -> mark32;

spell print(scroll msg) -> abyss {
    fore (mark16 i = 0; i < msg.len; i++) {
        putchar(cast<mark32>(msg.ptr[i]));
    }
}

spell main() -> mark16 {
    print("The forge awakens.\n");
    yield 0;
}
```

```bash
./cgilc hello.gil --target=host -o hello
./hello
# The forge awakens.
```

### Compiling for Bare Metal

```bash
./cgilc kernel.gil --target=kernel -o kernel.o
# Applies: -mgeneral-regs-only -std=gnu99 -Wno-int-conversion
```

### Emitting Intermediate C

```bash
./cgilc driver.gil --emit-c
# Produces: driver.c — readable, inspectable GNU C11
```

---

## Compiler Architecture

```
.gil source
     │
     ▼
┌─────────┐
│  Lexer  │  49 token types. Hex literals, escape sequences,
│         │  multi-char operators (~>, <~, ::, ++, --, <=, >=, <<, >>).
└────┬────┘  Production-ready. Zero known bugs.
     │
     ▼
┌──────────────────────────────────────────────────────────────┐
│  Parser (Pratt / Precedence Climbing)                        │
│                                                              │
│  Top-down recursive descent for declarations.                │
│  Pratt parser for expressions with correct precedence.       │
│  3-token lookahead for sigil/legion bare declarations.       │
│  Handles: divine patterns, weave desugaring, cast<T*>.       │
└────┬─────────────────────────────────────────────────────────┘
     │  ProgramNode (AST root)
     ▼
┌──────────────────────────────────────────────────────────────┐
│  Semantic Analyzer (Two-Pass)                                │
│                                                              │
│  Pass 1: Register all types, spells, hardware variables.     │
│  Pass 2: Walk all spell bodies. Enforce:                     │
│    • Typestate transitions (stance tracking per variable)    │
│    • Ownership moves (isMoved flag, use-after-move errors)   │
│    • Omen exhaustiveness (all rank variants covered)         │
│    • ? operator constraints (destined required for tuples)   │
│    • Reserved __ namespace, array-to-array ban               │
│    • Strict assignment compatibility                         │
│    • FPU (flow) usage in warden context                      │
│    • shatter/surge inside destined blocks (banned)           │
└────┬─────────────────────────────────────────────────────────┘
     │  Verified AST
     ▼
┌──────────────────────────────────────────────────────────────┐
│  Code Generator (Three-Phase)                                │
│                                                              │
│  Phase 1 TYPES: rank typedefs, sigil structs, Omen unions,  │
│                 Tuple structs, legion SoA registration.      │
│  Phase 2 PROTOTYPES: forward declarations for all spells.   │
│  Phase 3 IMPLEMENTATIONS: spell bodies, leyline globals.    │
│                                                              │
│  Key emissions:                                              │
│    • destined → goto chain with LIFO label ordering          │
│    • divine → if/else chain with __elem0/__elem1 access      │
│    • ? → GNU statement expression with single evaluation     │
│    • portline reads → inb/inw inline asm                     │
│    • portline writes → outb/outw inline asm                  │
│    • legion arrays → transparent SoA field splitting         │
│    • warden → __attribute__((interrupt))                     │
└────┬─────────────────────────────────────────────────────────┘
     │  .c file
     ▼
   GCC → Binary
```

---

## Test Suite

All 11 tests pass. The final test (09) is a zero-copy lexer — written entirely in Cgil — that tokenizes a source string at runtime using every major language feature simultaneously.

| Test | What It Verifies |
|------|-----------------|
| 01 — Basic Syntax | Primitives, arithmetic, if/elif/else, FFI via conjure |
| 02 — Control Flow | fore, whirl, shatter (break), surge (continue) |
| 03 — Pointers & Decks | Address-of, dereference, deck arrays, pointer-to-spell params |
| 04 — Sigils & Stance | Sigil structs, stance transitions, typestate lock enforcement |
| 05 — Divine & Omens | divine pattern matching, ? propagation, payloadless omen |
| 06 — Loops & Destined | LIFO destined chains, conditional cleanup, multi-path yield |
| 07 — Arcane OS Sim | Full integration: leyline, portline, own, divine, destined, warden |
| 08 — Lexer Primitives | Bitwise ops (&, ^, <<, >>), modulo, integer-to-ASCII conversion |
| 09 — Bootstrap Lexer | Zero-copy lexer written in Cgil, tokenizes `"val = 42 + 5;"` |
| 10 — Type Safety | scroll field safety, array type propagation, cast<T> and cast<T*> |
| 11 — Legion SoA | Structure of Arrays transformation, transparent field access |

---

## What Each Bug C Cannot Catch — And Cgil Can

| Scenario | C | Cgil |
|----------|---|------|
| Call `read_sector` on a faulted disk | Silent corruption at runtime | Compile error: stance mismatch |
| Forget `volatile` on MMIO register | Optimizer silently removes the read | Impossible — `leyline` always volatile |
| Use `inb` on MMIO or pointer on port I/O | Wrong instruction, silent wrong result | Impossible — `leyline`/`portline` are distinct types |
| Use hardware after transferring ownership | Use-after-free at runtime | Compile error: use-after-move |
| Forget cleanup on early return | Resource leak, corruption | Compile error if destined absent with `?` |
| Silently ignore a disk error code | Program continues in broken state | Omen must be handled — `?` or `divine` |
| Access array element field after subscript | SA blindly accepts wrong field names | Hard error: field not found on element type |
| FPU in interrupt service routine | Silent kernel panic at next context switch | Compile error in `warden spell` context |

---

## Roadmap

**Now — Standard Library**
- `std/io.gil` — native print, format primitives
- `std/scroll.gil` — string manipulation
- `std/mem.gil` — memory utilities for OS work

**Next — Demonstration Programs**
- Brainfuck interpreter in Cgil (esolang demo)
- Tiny stack-based VM
- Bare-metal disk driver booting on QEMU

**Future**
- Full Ring 0 OS: bootloader → VGA → IDT → PIT → keyboard → ATA → FAT → shell
- Bootstrap: writing the Cgil compiler in Cgil itself

---

## Building from Source

```
cgil/
├── include/
│   ├── Lexer/Lexer.h
│   ├── Parser/AST.h
│   ├── Parser/Parser.h
│   ├── Semantics/SemanticAnalyzer.h
│   ├── Semantics/SymbolTable.h
│   ├── Semantics/Types.h
│   └── CodeGen/CodeGen.h
├── src/
│   ├── Lexer/Lexer.cpp
│   ├── Parser/Parser.cpp
│   ├── Semantics/SemanticAnalyzer.cpp
│   ├── CodeGen/CodeGen.cpp
│   └── main.cpp
├── test_cases/
│   ├── 01_basic_syntax.gil
│   ├── ...
└── makefile
```

```bash
make          # build cgilc
make clean    # remove build artifacts
```

**Compiler flags available:**

```
cgilc <file.gil> [options]
  -o <name>         Output binary name
  --emit-c          Stop after transpilation, keep the .c file
  --target=host     Standard desktop compilation (default)
  --target=kernel   Bare-metal: -mgeneral-regs-only, gnu99, no SSE
```

---

## Contributing

This project is in active development by a solo developer. If you find a bug, open an issue with the minimal `.gil` file that reproduces it. If the compiler crashes without a meaningful error message, that is always a bug — the compiler should never produce output that GCC rejects without a corresponding Cgil-level error pointing at the source line.

---

## Philosophy

The language was built on a single premise: the vocabulary of systems programming already sounds like magic. Volatile memory, interrupt vectors, ownership transfer, hardware state machines — these are the words of arcane engineering. Cgil takes that seriously. Every keyword was chosen because it is the *right word* for what it does, not because it sounds cool.

`destined` is RAII because cleanup is always destined to happen.  
`divine` is pattern matching because you are reading the truth in the result.  
`ruin` is an error because that is what a hardware fault actually is.  
`own` transfers responsibility because ownership is a moral concept as much as a technical one.  

The goal is that writing Cgil code feels like doing something that matters. Because bare-metal systems programming is one of the few things left in software where what you write is what the machine does, with no layers between you and the metal.

---

## License

MIT. Use it, extend it, learn from it.

---
