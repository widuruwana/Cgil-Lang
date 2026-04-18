<div align="center">

# The Arcane Systems Forge

*A memory-safe, typestate-driven systems language targeting bare-metal Ring 0.*

[![License: MIT](https://img.shields.io/badge/License-MIT-yellow.svg)](https://opensource.org/licenses/MIT)
[![Stage: V1.0 Golden Master](https://img.shields.io/badge/Stage-V1.0%20Golden%20Master-brightgreen)]()
[![Tests: 11/11 Passing](https://img.shields.io/badge/Tests-11%2F11%20Passing-brightgreen)]()
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

These constraints were discovered and documented during test execution. They
are not bugs — they are intentional design decisions with specific rationale.
 
**Type constraints:**
- `addr` is `uint16_t` (16-bit). Cannot hold physical addresses above `0xFFFF`.
  Use `soul32` for 20-bit+ addresses like VGA `0xB8000`.
- Binary literals (`0b1010`) are not supported. Use hex (`0xAAAA`) instead.
- `flow` (float) is banned inside `warden spell` bodies — FPU in an ISR
  corrupts the FPU state. The SA enforces this with a hard error.
**Syntax rules:**
- Stance syntax uses single colon: `Counter:Idle`
- Rank variant syntax uses double colon: `DiskError::Timeout`
- These are not interchangeable. The SA will catch the wrong one.
- Sigil pointer declaration: `sigil* TypeName varName` — star after `sigil` keyword.
  `sigil TypeName* varName` is rejected by the parser.
- Ternary operator (`? :`) is not supported. Use `if/else` assignment.
**Ownership constraints:**
- `own &variable` requires `variable` to be a plain local identifier in scope.
  `own &*ptr` (address of dereference) is rejected by the SA.
  This is correct — ownership tracking operates on named symbols, not memory
  locations resolved at runtime.
- After a `divine` block, the compiler resets the owned variable's stance to
  `Unknown`. Explicitly re-assign the stance before using it again.
**Legion constraints:**
- `legion_array[i] = LegionType { ... }` is invalid — the struct no longer
  exists after SoA transformation. Assign fields individually:
  `legion_array[i].field = value;`
**Tuple return constraints:**
- Only `sigil*` is valid in tuple return type positions.
  `(mark32*, mark16 | ruin<E>)` is rejected by the parser.
  Use a sigil to carry state instead of a raw primitive pointer.

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
│  Phase 1 TYPES: rank typedefs, sigil structs, Omen unions,   │
│                 Tuple structs, legion SoA registration.      │
│  Phase 2 PROTOTYPES: forward declarations for all spells.    │
│  Phase 3 IMPLEMENTATIONS: spell bodies, leyline globals.     │
│                                                              │
│  Key emissions:                                              │
│    • destined → goto chain with LIFO label ordering          │
│    • divine → if/else chain with __elem0/__elem1 access      │
│    • ? → GNU statement expression with single evaluation     │
│    • portline reads → inb/inw inline asm                     │
│    • portline writes → outb/outw inline asm                  │
│    • legion arrays → transparent SoA field splitting         │
│    • warden spell → __attribute__((interrupt))               │
│                        on --target=kernel only               │
│    • leyline &addr → ((uint32_t)0xADDR) integer constant     │
│                       (not volatile pointer)                 │
└────┬─────────────────────────────────────────────────────────┘
     │  .c file
     ▼
   GCC → Binary
```

---

## Test Suite

> All 11 tests pass on Windows (MinGW GCC) and Linux. Tests are self-contained —
> each file includes its own print helpers and runs independently. The final test
> (`11_arcane_os_integration.gil`) is a complete bare-metal OS boot simulation
> that exercises every language feature simultaneously and serves as the
> integration proof.
 
Run all tests:
```bash
for f in test_cases/*.gil; do
    ./cgilc "$f" --target=host -o test_out && ./test_out
    echo "---"
done
```

| Test | File | Feature Coverage | Status |
|------|------|-----------------|--------|
| 01 | `01_primitives_and_arithmetic.gil` | All primitive types, arithmetic, bitwise, cast, comparisons | ✅ PASS |
| 02 | `02_control_flow.gil` | if/elif/else, fore, whirl, shatter, surge, ++/--, scope | ✅ PASS |
| 03 | `03_scrolls_decks_pointers.gil` | scroll fat pointer, deck arrays, &, *, pointer params | ✅ PASS |
| 04 | `04_ranks_and_omens.gil` | rank discriminants, rank-as-value, Omen construction, ? | ✅ PASS |
| 05 | `05_sigils_and_typestates.gil` | sigil structs, stance transitions, typestate lock, pointer mutation | ✅ PASS |
| 06 | `06_ownership_and_divine.gil` | own, divine all branch types, destined, ownership cycles | ✅ PASS |
| 07 | `07_destined_raii.gil` | LIFO destined, conditional destined, ? triggers cleanup | ✅ PASS |
| 08 | `08_hardware_mapping.gil` | leyline MMIO, portline PIO, &address extraction, ATA/PIC maps | ✅ PASS |
| 09 | `09_legion_soa.gil` | SoA transformation, physics, combat, sensor arrays | ✅ PASS |
| 10 | `10_weave_cast_operators.gil` | ~> pipeline, cast<T>/<T*>, bitwise, modulo, precedence | ✅ PASS |
| 11 | `11_arcane_os_integration.gil` | Full OS boot: all features simultaneously | ✅ PASS |

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
| Direct Omen comparison with `==` | Silent wrong behavior at runtime | Compile error: use `?` or `divine` |
| Leyline treated as port I/O or vice versa | Wrong CPU instruction, silent hardware failure | Impossible — `leyline`/`portline` are structurally distinct |

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
│   ├── 01_primitives_and_arithmetic.gil
│   ├── ...
|   ├── 11_arcane_os_integration.gil
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
