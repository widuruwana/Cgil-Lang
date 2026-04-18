#pragma once
#include "../Parser/AST.h"
#include "../Semantics/Types.h"
#include <iostream>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

// =============================================================================
// CGIL CODE GENERATOR
// =============================================================================
//
// WHAT THIS PHASE DOES:
//   Takes the semantically verified AST and emits a single valid C source file.
//   After this phase, the output is fed to GCC which produces the final binary.
//
// THREE-PHASE GENERATION STRATEGY:
//   C requires things to be declared before they are used. A function cannot
//   call another function that appears later in the file unless a prototype
//   exists. Struct types must be defined before pointer-to-struct declarations.
//
//   To solve this without burdening the programmer, we walk the entire AST
//   THREE times, each time emitting only the declarations appropriate for that
//   phase. This eliminates header-ordering problems in generated C:
//
//   Phase 1 — TYPES:
//     Emit: rank typedefs (#define constants)
//            sigil structs (typedef struct { ... })
//            legion structs (V1 stub as sigil)
//            Omen tagged-union typedefs
//            Tuple struct typedefs
//     Why first? Spell prototypes and implementations both depend on types.
//
//   Phase 2 — PROTOTYPES:
//     Emit: forward declarations (function signatures ending in ';')
//     Why second? Implementations can call any spell without ordering concerns.
//
//   Phase 3 — IMPLEMENTATIONS:
//     Emit: leyline volatile pointer globals
//            spell bodies
//     Why last? Bodies reference types and other spells already declared above.
//
//   Grimoire includes are emitted in a special pre-pass BEFORE all three phases.
//
// VISITOR PATTERN:
//   CodeGenVisitor inherits ASTVisitor and implements a visit() for every node.
//   Each visit() checks currentPhase and returns early if this node type is not
//   relevant in the current phase. This keeps each pass clean.
//
// OUTPUT:
//   Everything goes to the std::ostream passed to the constructor.
//   Use std::ofstream to write to a file, std::cout for stdout.
//
// PORTLINE HANDLING:
//   portline variables do NOT produce a C variable declaration. Instead:
//   - Reads: intercepted in visit(IdentifierExpr*), emit GCC statement expression
//            with inline asm (inb for rune, inw for soul16).
//   - Writes: intercepted in visit(AssignStmt*), emit outb/outw inline asm.
//   - &portline_name: intercepted in visit(AddressOfExpr*), emit address constant.
//   The portlineMap and portlineTypeMap store the information needed at usage sites.
//
// DESTINED BLOCK EXPANSION (RAII goto pattern):
//   The spec requires every 'yield' in a spell with a 'destined' block to be
//   rewritten to:
//     __ret = <value>;
//     goto __destined_N;
//   And the destined label + cleanup body is emitted at the END of the function,
//   before the final 'return __ret;'.
//   Multiple destined blocks chain in LIFO order (last declared fires first).
//   The 'inDestinedSpell' flag and 'destinedBlocks' vector in visit(SpellDecl*)
//   implement this two-pass approach within the spell.
//
// OMEN AND TUPLE TYPEDEF GENERATION:
//   Each distinct 'T | ruin<R>' type used anywhere in the program needs exactly
//   one typedef. Each distinct tuple return type needs exactly one struct typedef.
//   The emittedOmenTypes and emittedTupleTypes sets track what has been emitted
//   to prevent duplicates.
//
// =============================================================================

class CodeGenVisitor : public ASTVisitor {
public:
    explicit CodeGenVisitor(std::ostream& output) : out(output) {
        setupTypeMap();
    }

    void setKernelMode(bool km) { kernelMode = km; }

    // Main entry point. Call after semantic analysis succeeds.
    // Writes the entire generated C file to the output stream.
    void generate(ProgramNode* program);
    bool kernelMode = false;

    // =========================================================================
    // ASTVISITOR OVERRIDES
    // All 24 must be implemented — ASTVisitor has pure virtual methods for all.
    // =========================================================================

    void visit(GrimoireDecl* node) override;
    void visit(RankDecl*     node) override;
    void visit(SigilDecl*    node) override;
    void visit(LegionDecl*   node) override;
    void visit(SpellDecl*    node) override;
    void visit(HardwareDecl* node) override;

    void visit(BlockStmt*    node) override;
    void visit(ExprStmt*     node) override;
    void visit(AssignStmt*   node) override;
    void visit(YieldStmt*    node) override;
    void visit(ShatterStmt*  node) override;
    void visit(SurgeStmt*    node) override;
    void visit(IfStmt*       node) override;
    void visit(ForeStmt*     node) override;
    void visit(WhirlStmt*    node) override;
    void visit(DestinedStmt* node) override;
    void visit(DivineStmt*   node) override;
    void visit(VarDeclStmt*  node) override;

    void visit(BinaryExpr*     node) override;
    void visit(UnaryExpr*      node) override;
    void visit(PostfixExpr*    node) override;
    void visit(LiteralExpr*    node) override;
    void visit(IdentifierExpr* node) override;
    void visit(CallExpr*       node) override;
    void visit(AddressOfExpr*  node) override;
    void visit(IndexExpr*      node) override;
    void visit(StructInitExpr* node) override;
    void visit(AssignExpr*     node) override;
    void visit(CastExpr*       node) override;
    void visit(UpdateExpr*     node) override;

private:
    // =========================================================================
    // OUTPUT STATE
    // =========================================================================

    std::ostream& out;       // Destination stream for generated C
    int indentLevel = 0;     // Current indentation depth (each level = 4 spaces)

    // Which of the three generation phases is currently active?
    // Initialized to TYPES.
    enum class Phase { TYPES, PROTOTYPES, IMPLEMENTATIONS };
    Phase currentPhase = Phase::TYPES;

    // =========================================================================
    // TYPE MAPPING
    // =========================================================================

    // Maps Cgil primitive type names to C99 type strings.
    // User-defined types (sigil/rank/legion names) fall through to the raw name.
    std::unordered_map<std::string, std::string> cTypeMap;

    // Tracks legion declarations so we can look up their fields for SoA expansion
    std::unordered_map<std::string, LegionDecl*> legionRegistry;

    // Tracks variables that are deck arrays of a legion type (e.g., "entities")
    std::unordered_set<std::string> legionArrayVars;

    void setupTypeMap() {
        cTypeMap["mark16"] = "int16_t";
        cTypeMap["mark32"] = "int32_t";
        cTypeMap["soul16"] = "uint16_t";
        cTypeMap["soul32"] = "uint32_t";
        cTypeMap["addr"]   = "uint16_t";    // 16-bit hardware address
        cTypeMap["rune"]   = "uint8_t";     // 8-bit byte
        cTypeMap["oath"]   = "uint8_t";     // boolean (kept=1, forsaken=0)
        cTypeMap["flow"]   = "float";
        cTypeMap["abyss"]  = "void";
        cTypeMap["scroll"] = "Cgil_Scroll"; // fat pointer struct
        cTypeMap["deck"]   = "uint8_t";     // deck[N] T emitted as T[N] — 'deck' itself unused
        cTypeMap["tuple"]  = "void";        // tuple is emitted as Tuple_<...> structs
    }

    // Translate a Cgil type token to its C equivalent string.
    // Returns the raw lexeme for user-defined types (sigil/rank names).
    std::string getCType(const Token& token) const {
        auto it = cTypeMap.find(token.lexeme);
        if (it != cTypeMap.end()) return it->second;
        return token.lexeme; // sigil/legion/rank name — used directly in C
    }

    // =========================================================================
    // PORTLINE TRACKING
    // portline variables don't produce C variables. Their reads and writes are
    // intercepted at usage sites and replaced with inline assembly.
    // These maps are populated in visit(HardwareDecl*) during Phase::TYPES.
    // =========================================================================

    // portline name -> port address string ("0x1F7")
    std::unordered_map<std::string, std::string> portlineAddressMap;

    std::unordered_map<std::string, std::string> leylineAddressMap;

    // portline name -> type string ("rune" = 8-bit, "soul16" = 16-bit)
    std::unordered_map<std::string, std::string> portlineTypeMap;

    // Returns true if this identifier is a declared portline variable.
    bool isPortline(const std::string& name) const {
        return portlineAddressMap.count(name) > 0;
    }

    // Emits a portline READ as a GCC statement expression with inline assembly.
    // rune   portline -> uint8_t  __tmp; asm volatile("inb  %1, %0" ...); __tmp
    // soul16 portline -> uint16_t __tmp; asm volatile("inw  %1, %0" ...); __tmp
    void emitPortlineRead(const std::string& name);

    // Emits a portline WRITE as inline assembly.
    // rune   portline -> asm volatile("outb %0, %1" ...);
    // soul16 portline -> asm volatile("outw %0, %1" ...);
    // valExpr is a lambda that emits the value expression inline.
    void emitPortlineWrite(const std::string& name, const std::string& valExpr);

    // =========================================================================
    // STANCE TRACKING
    // Sigils with stances emit #define constants for each discriminant.
    // AssignStmt uses these when emitting stance transitions.
    // Populated in visit(SigilDecl*) during Phase::TYPES.
    // =========================================================================

    // sigilName -> ordered list of stance names (index = discriminant value)
    // e.g., "Disk" -> ["Idle", "Reading", "Fault"]
    std::unordered_map<std::string, std::vector<std::string>> stanceMap;

    // stancePointerVars: names of parameters that are sigil POINTERS.
    // Used in AssignStmt to decide -> vs . for __stance access.
    // Populated in visit(SpellDecl*) when processing parameters.
    std::unordered_set<std::string> stancePointerVars;

    // =========================================================================
    // OMEN AND TUPLE TYPEDEF TRACKING
    // The spec requires each distinct Omen/Tuple type to emit exactly one typedef.
    // These sets prevent duplicate emission.
    // =========================================================================

    // Tracks which Omen typedef names have been emitted.
    // e.g., "Omen_scroll_DiskError" after emitting that Omen's struct.
    std::unordered_set<std::string> emittedOmenTypes;

    // Tracks which Tuple typedef names have been emitted.
    std::unordered_set<std::string> emittedTupleTypes;

    // Compute the Omen typedef name from success type and ruin rank type.
    // scroll | ruin<DiskError> -> "Omen_scroll_DiskError"
    std::string getOmenTypeName(const std::string& successType,
                                const std::string& ruinType) const {
        return "Omen_" + successType + "_" + ruinType;
    }

    // Emit the Omen tagged-union struct typedef if it has not been emitted yet.
    // Checks emittedOmenTypes first. Called in Phase::TYPES when processing SpellDecl.
    //
    // Emits:
    //   typedef struct {
    //       uint8_t __is_ruin;
    //       union {
    //           Cgil_Scroll __value;   (or appropriate success type)
    //           uint16_t    __ruin;
    //       };
    //   } Omen_scroll_DiskError;
    void emitOmenTypedefIfNeeded(const std::string& successCType,
                                 const std::string& successTypeName,
                                 const std::string& ruinTypeName);

    // Compute the Tuple typedef name from its element types.
    // (sigil* Disk, scroll | ruin<DiskError>) -> "Tuple_Disk_Omen_scroll_DiskError"
    std::string getTupleTypeName(const SpellDecl* node) const;

    // Emit the Tuple struct typedef if not yet emitted.
    // Emits:
    //   typedef struct {
    //       Disk* __elem0;
    //       Omen_scroll_DiskError __elem1;
    //   } Tuple_Disk_Omen_scroll_DiskError;
    void emitTupleTypedefIfNeeded(SpellDecl* node);

    // =========================================================================
    // DESTINED BLOCK STATE
    // Used during SpellDecl body emission to implement the RAII goto pattern.
    // =========================================================================

    // True when we are inside a spell that has one or more destined blocks.
    // When true, visit(YieldStmt*) emits:
    //     __ret = <value>;  goto __destined_N;
    // instead of:
    //     return <value>;
    bool inDestinedSpell = false;

    // Tracks the current spell so YieldStmt knows what the return type is
    SpellDecl* currentSpell = nullptr;

    // The destined blocks found in the current spell, in declaration order.
    // LIFO order means we emit labels in reverse: last declared fires first.
    std::vector<DestinedStmt*> currentDestinedBlocks;

    // Global counter for generating unique __destined_N label names.
    // Acts as a sequential label allocator across the entire program.
    // Reset to 0 in generate() at the start of each compiler run to support
    // multi-file compilation within a single process lifetime.
    //
    // IMPORTANT: This counter is incremented ONCE per spell, atomically, at the
    // START of spell body emission (not at label emission time). This guarantees
    // that PostfixExpr's goto target calculation is stable throughout body emission.
    int globalDestinedCounter = 0;

    // The base label index allocated for the current spell's destined blocks.
    // Captured atomically at the start of body emission:
    //   currentSpellDestinedBase = globalDestinedCounter;
    //   globalDestinedCounter += currentDestinedBlocks.size();
    //
    // All goto target calculations within a spell use this base, not the
    // mutable globalDestinedCounter. This eliminates the race condition where
    // PostfixExpr reads globalDestinedCounter before labels have been allocated.
    //
    // Value is 0 when not inside a destined spell. Reset at spell cleanup.
    int currentSpellDestinedBase = 0;

    // The C return type string for the current spell being emitted.
    // Used in YieldStmt when emitting 'return __ret;' in destined mode.
    std::string currentReturnTypeName;

    // Scan a list of statements for top-level DestinedStmt nodes.
    // Does NOT recurse into nested blocks (destined blocks are always top-level
    // inside a spell body per spec).
    std::vector<DestinedStmt*> findDestinedBlocks(
        const std::vector<std::unique_ptr<Stmt>>& body) const;

    // =========================================================================
    // OUTPUT HELPERS
    // =========================================================================

    // Emit the current indentation (4 spaces per level).
    void indent() {
        for (int i = 0; i < indentLevel; ++i) out << "    ";
    }

    // Emit a raw string with no newline. Caller controls line structure.
    void emit(const std::string& str) { out << str; }

    // Emit indentation + string + newline. Used for most complete lines.
    void emitLine(const std::string& str) { indent(); out << str << "\n"; }
};
