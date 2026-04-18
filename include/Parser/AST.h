#pragma once

#include "../Lexer/Lexer.h"
#include <vector>
#include <string>
#include <memory>

// =============================================================================
// CGIL ABSTRACT SYNTAX TREE (AST)
// =============================================================================
//
// WHAT THIS FILE IS:
//   The AST is the central data structure of the entire compiler. After the
//   Lexer produces a flat sequence of tokens, the Parser organizes them into
//   this tree, which represents the full grammatical and semantic structure
//   of the .gil source file.
//
//   Every subsequent compiler phase reads this tree. Nothing touches the raw
//   token stream again after parsing is complete.
//
// TREE STRUCTURE:
//   ASTNode  (base of everything — carries a Token for error reporting)
//     |
//     +-- Decl  (top-level declarations: spell, sigil, rank, leyline, etc.)
//     +-- Stmt  (statements inside spell bodies: if, whirl, yield, destined...)
//     +-- Expr  (expressions that produce values: a + b, f(), 0x1F7, ident)
//
// VISITOR PATTERN:
//   Instead of putting CodeGen or semantic analysis logic inside each node
//   (which would scatter it everywhere), we use the Visitor pattern.
//
//   Each compiler pass (SemanticAnalyzer, CodeGenerator) implements ASTVisitor
//   and provides a visit() function for every concrete node type. Each node's
//   accept() method calls visitor.visit(this), which routes to the correct
//   visit() overload via C++ virtual dispatch.
//
//   This means ALL code generation lives in the CodeGen class. ALL semantic
//   checking lives in the SemanticAnalyzer class. The AST nodes are pure data.
//
// MEMORY OWNERSHIP:
//   Child nodes are owned via std::unique_ptr. When a parent node is destroyed,
//   all its children are automatically destroyed. There are no manual deletes.
//   The root ProgramNode owns the entire tree.
//
// =============================================================================

// =============================================================================
// FORWARD DECLARATIONS
// Every concrete node type must be forward-declared here so the ASTVisitor
// interface below can reference them before their full definitions.
// =============================================================================

struct TypeInfo;

// -- Declarations --
struct ProgramNode;
struct GrimoireDecl;
struct RankDecl;
struct SigilDecl;
struct LegionDecl;
struct SpellDecl;
struct HardwareDecl;

// -- Statements --
struct BlockStmt;
struct ExprStmt;
struct AssignStmt;
struct YieldStmt;
struct ShatterStmt;
struct SurgeStmt;
struct IfStmt;
struct ForeStmt;
struct WhirlStmt;
struct DestinedStmt;
struct DivineStmt;
struct VarDeclStmt;

// -- Expressions --
struct BinaryExpr;
struct UnaryExpr;
struct PostfixExpr;
struct LiteralExpr;
struct IdentifierExpr;
struct CallExpr;
struct AddressOfExpr;
struct IndexExpr;
struct StructInitExpr;
struct AssignExpr;
struct CastExpr;
struct UpdateExpr;

// =============================================================================
// THE VISITOR INTERFACE
// =============================================================================
// Every compiler pass that needs to walk the AST inherits from this class
// and implements a visit() function for every node type listed below.
//
// If you add a new node type to the AST, you MUST add a corresponding
// pure virtual visit() here, or the compiler will reject any class that
// tries to implement ASTVisitor without covering the new node.
// =============================================================================
class ASTVisitor {
public:
    virtual ~ASTVisitor() = default;

    // Declarations
    virtual void visit(GrimoireDecl* node) = 0;
    virtual void visit(RankDecl*     node) = 0;
    virtual void visit(SigilDecl*    node) = 0;
    virtual void visit(LegionDecl*   node) = 0;
    virtual void visit(SpellDecl*    node) = 0;
    virtual void visit(HardwareDecl* node) = 0;

    // Statements
    virtual void visit(BlockStmt*    node) = 0;
    virtual void visit(ExprStmt*     node) = 0;
    virtual void visit(AssignStmt*   node) = 0;
    virtual void visit(YieldStmt*    node) = 0;
    virtual void visit(ShatterStmt*  node) = 0;
    virtual void visit(SurgeStmt*    node) = 0;
    virtual void visit(IfStmt*       node) = 0;
    virtual void visit(ForeStmt*     node) = 0;
    virtual void visit(WhirlStmt*    node) = 0;
    virtual void visit(DestinedStmt* node) = 0;
    virtual void visit(DivineStmt*   node) = 0;
    virtual void visit(VarDeclStmt*  node) = 0;

    // Expressions
    virtual void visit(BinaryExpr*     node) = 0;
    virtual void visit(UnaryExpr*      node) = 0;
    virtual void visit(PostfixExpr*    node) = 0;
    virtual void visit(LiteralExpr*    node) = 0;
    virtual void visit(IdentifierExpr* node) = 0;
    virtual void visit(CallExpr*       node) = 0;
    virtual void visit(AddressOfExpr*  node) = 0;
    virtual void visit(IndexExpr*      node) = 0;
    virtual void visit(StructInitExpr* node) = 0;
    virtual void visit(AssignExpr*     node) = 0;
    virtual void visit(CastExpr*       node) = 0;
    virtual void visit(UpdateExpr*     node) = 0;
};

// =============================================================================
// BASE NODE
// =============================================================================
// Every AST node carries the Token that produced it. This is how the compiler
// knows which line and column to point to when reporting errors during semantic
// analysis or code generation — phases that run long after the Parser finishes.
// =============================================================================
struct ASTNode {
    Token token;                         // Source location for error reporting.
    virtual ~ASTNode() = default;
    virtual void accept(ASTVisitor& visitor) = 0;
};

// The three base categories. All concrete nodes inherit from one of these.
struct Expr : public ASTNode {};
struct Stmt : public ASTNode {};
struct Decl : public ASTNode {};

// =============================================================================
// HELPER STRUCTURES
// These are not AST nodes themselves but are embedded inside nodes that need
// structured sub-data (parameters, branches, etc.).
// =============================================================================

// Represents one parameter in a spell signature, or one field in a sigil/legion.
//
// Examples:
//   own sigil* Disk:Idle ctrl  ->  isOwned=true,  isPointer=true,  stanceName="Idle"
//   addr target                ->  isOwned=false, isPointer=false
//   soul16 sector_count        ->  isOwned=false, isPointer=false  (field use)
struct Param {
    bool  isOwned   = false;  // Was the 'own' keyword present?
    bool  isPointer = false;  // Is this a pointer type (sigil*)?
    Token type;               // The type token: mark16, soul16, IDENT (for sigil name), etc.
    Token stanceName;         // Optional stance constraint: "Idle" in Disk:Idle
    Token name;               // The parameter or field name token.
};

// Represents one elif-branch or else-branch of an if statement.
// For 'else', condition is nullptr (use hasCondition = false on the caller side).
struct ElifBranch {
    std::unique_ptr<Expr>      condition; // nullptr for a bare 'else'
    std::unique_ptr<BlockStmt> body;
};

// One field in a struct initializer: fieldName: value
// e.g., the "length: 64" part of "Packet { length: 64, checksum: 0 }"
struct StructFieldInit {
    Token                 name;   // The field name token (e.g., "length")
    std::unique_ptr<Expr> value;  // The initializer expression
};

// Represents one branch inside a divine { } block.
//
// Three possible forms:
//   SUCCESS:       (ctrl, scroll data)                    — isRuin=false
//   SPECIFIC RUIN: (ctrl, ruin<DiskError::HardwareFault>) — isRuin=true, isSpecificRuin=true
//   CATCH-ALL:     (ctrl, ruin err)                       — isRuin=true, isSpecificRuin=false
//
// Represents a single branch inside a divine { } block.
//
// Four possible forms, all tracked by the flags below:
//
//   SUCCESS with payload:    (ctrl, scroll data)                    isRuin=false, isPayloadless=false
//   SUCCESS without payload: (ctrl)                                 isRuin=false, isPayloadless=true
//   SPECIFIC RUIN:           (ctrl, ruin<DiskError::HardwareFault>) isRuin=true,  isSpecificRuin=true
//   CATCH-ALL RUIN:          (ctrl, ruin err)                       isRuin=true,  isSpecificRuin=false
//
// WHY isPayloadless EXISTS:
//   For spells returning 'abyss | ruin<T>', the success has no payload value.
//   Writing '(ctrl, abyss my_var)' would register my_var in the symbol table
//   but CodeGen would skip declaring it (void variable), making any reference
//   to my_var an undeclared identifier error in generated C.
//
//   The correct syntax for an abyss success branch is just '(ctrl) => { }'.
//   The Parser sets isPayloadless=true when ')' follows the ownerVar directly.
//   The Semantic Analyzer skips payload declaration. CodeGen skips __value access.
struct DivineBranch {
    Token ownerVar;              // Always present: the rebinding variable (e.g., ctrl)

    bool  isRuin         = false; // Is this a ruin branch?
    bool  isSpecificRuin = false; // Specific: ruin<Rank::Variant> vs catch-all: ruin err
    bool  isPayloadless  = false; // Success branch with no payload variable (abyss omen)

    // --- Specific ruin: ruin<DiskError::HardwareFault> ---
    Token rankName;              // e.g., "DiskError"
    Token variantName;           // e.g., "HardwareFault"

    // --- Success branch with payload: scroll data ---
    Token successType;           // e.g., scroll, mark16
    Token successVar;            // e.g., data

    // --- Catch-all ruin: ruin err ---
    Token catchAllVar;           // e.g., err

    std::unique_ptr<BlockStmt> body;
};

// =============================================================================
// RETURN TYPE INFO 
// =============================================================================
//
// WHY THIS EXISTS:
//   The Parser reads a return type like `sigil* Disk` and previously stored
//   only the base type token ("Disk") in returnTypes, silently discarding `*`.
//   CodeGen then had to GUESS that tuples start with a pointer, breaking any
//   tuple whose first element is not a sigil pointer (e.g., (mark16, soul16)).
//
//   ReturnTypeInfo carries both the type token AND isPointer.
//   The Parser populates it. CodeGen reads it exactly. No guessing ever.
//
// Examples:
//   spell foo() -> mark16               -> [{mark16, false}]
//   spell foo() -> (sigil* Disk, scroll) -> [{Disk, true}, {scroll, false}]
//   spell foo() -> abyss | ruin<E>      -> [{abyss, false}] + hasOmen=true
//
struct ReturnTypeInfo {
    Token typeToken;         // The base type token: mark16, Disk, scroll, abyss...
    bool  isPointer = false; // Was '*' present after the type? (sigil* -> true)
};

// =============================================================================
// DECLARATIONS
// Top-level constructs. A valid .gil file is a sequence of these.
// =============================================================================

// Represents the entire parsed .gil source file.
// The top-level entry point of the tree. Owns all declarations.
struct ProgramNode : public ASTNode {
    std::vector<std::unique_ptr<Decl>> declarations;
    // ProgramNode is visited directly by compiler passes, not via accept().
    void accept(ASTVisitor& /*visitor*/) override { /* visited directly */ }
};

// grimoire <hardware_defs.h>;   — internal bare-metal OS header (Ring 0)
// pact <stdlib.h>;              — external hosted C library   (Ring 3)
//
// CodeGen emits: #include <path>
// The 'isPact' flag allows the semantic analyzer to warn when pact is used
// in kernel context (Ring 3 libraries assume an OS beneath them).
struct GrimoireDecl : public Decl {
    Token path;           // The header name token (e.g., "hardware_defs" or "my_driver.h")
    bool  isPact = false; // false = grimoire (Ring 0), true = pact (Ring 3)
    bool  isSystem = true; // NEW: true = <file.h>, false = "file.h"
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// rank DiskError { Timeout, HardwareFault, InvalidSector }
//
// CodeGen emits:
//   typedef uint16_t DiskError;
//   #define DiskError_Timeout       ((uint16_t)0)
//   #define DiskError_HardwareFault ((uint16_t)1)
//   #define DiskError_InvalidSector ((uint16_t)2)
//
// Variants are assigned sequential uint16_t discriminants in declaration order.
struct RankDecl : public Decl {
    Token               name;     // e.g., "DiskError"
    std::vector<Token>  variants; // e.g., ["Timeout", "HardwareFault", "InvalidSector"]
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// sigil Disk { stance Idle; stance Reading; soul16 sector_count; }
//
// CodeGen emits:
//   typedef struct {
//       uint16_t __stance;      // implicit, always first — 0=Idle, 1=Reading, 2=Fault
//       uint16_t sector_count;
//   } Disk;
//
// stances: Named compile-time typestates. Also stored at runtime as __stance.
// fields:  User-declared data fields. Emitted after __stance in declaration order.
// boundSpells: V1.5 — methods attached to this sigil (encapsulation without OOP).
struct SigilDecl : public Decl {
    Token                                  name;
    std::vector<Token>                     stances;     // In declaration order
    std::vector<Param>                     fields;      // In declaration order
    std::vector<std::unique_ptr<SpellDecl>> boundSpells; // V1.5: attached spells
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// legion SectorCache { mark16 sector_id; flow read_time; oath is_corrupted; }
//
// Written like a sigil but signals intent for Structure of Arrays layout.
// V1 CodeGen: Stubbed as a standard sigil (struct). No SoA transformation yet.
// V2 CodeGen: Full SoA — separate arrays for each field for cache efficiency.
//
// The keyword is parsed distinctly so V2 can identify legion nodes easily
// without changing the AST structure.
struct LegionDecl : public Decl {
    Token              name;
    std::vector<Param> fields;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// All spell variants share this node. The boolean flags distinguish them:
//
//   spell fetch_sector(...)          -> isWarden=false, isConjure=false
//   warden spell disk_irq()          -> isWarden=true
//   conjure spell read_buffer(...)   -> isConjure=true, body is empty
//   conjure endless spell k_panic()  -> isConjure=true, isEndless=true
//
// CodeGen rules:
//   warden   -> emits __attribute__((interrupt)) on the C function
//   conjure  -> emits only the C function declaration (no body)
//   endless  -> suppresses "missing yield" and "unbound own" warnings on
//               branches that call this spell, because they never return
//   own      -> stripped from emitted C (compile-time only annotation)
//   hasOmen  -> return type has a | ruin<R> part; omenErrorType names R
struct SpellDecl : public Decl {
    Token name;
    bool  isWarden  = false;
    bool  isEndless = false;
    bool  isConjure = false;

    std::vector<Param> params;

    // Return types. Each entry carries the type token AND whether it is a pointer.
    // Simple spell:  one entry.    e.g., scroll -> [{scroll, false}]
    // Tuple spell:   multiple entries. e.g., (sigil* Disk, scroll) -> [{Disk,true},{scroll,false}]
    // The isPointer flag is critical — it is set by the Parser when '*' is present
    // and read by CodeGen to emit the correct C declaration without guessing.
    std::vector<ReturnTypeInfo> returnTypes;

    // If the return type has an omen (| ruin<T>), hasOmen is true and
    // omenErrorType.lexeme is the rank name (e.g., "DiskError").
    bool  hasOmen = false;
    Token omenErrorType;

    std::vector<std::unique_ptr<Stmt>> body; // Empty for conjure spells.
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// leyline disk_status_port: rune @ 0x1F7;
// portline disk_data_port: soul16 @ 0x1F0;
//
// leyline (MMIO): hardware register is in RAM address space.
//   CodeGen emits: volatile uint8_t* const disk_status_port = (volatile uint8_t*)0x1F7;
//
// portline (PIO): hardware register is in x86 Port I/O address space.
//   CodeGen emits: inline assembly (inb/outb for rune, inw/outw for soul16)
//   on every read or write to the variable.
//
// Using &name evaluates to the address itself as a compile-time addr constant.
// Using name in an expression evaluates to the value stored at that address.
struct HardwareDecl : public Decl {
    bool  isPortline = false; // false = leyline (MMIO), true = portline (PIO)
    Token name;
    Token type;               // e.g., RUNE (8-bit), SOUL16 (16-bit)
    Token address;            // e.g., 0x1F7
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// =============================================================================
// STATEMENTS
// Things that happen inside spell bodies. They execute sequentially and do not
// produce values (values come from expressions inside them).
// =============================================================================

// A sequence of statements surrounded by { }.
// Used as the body of spells, if-branches, loops, destined blocks, etc.
struct BlockStmt : public Stmt {
    std::vector<std::unique_ptr<Stmt>> statements;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// A statement that is just an expression followed by ';'.
// e.g., process_data(data);    acknowledge_interrupt();
// The expression is evaluated for its side effects; its return value is discarded.
struct ExprStmt : public Stmt {
    std::unique_ptr<Expr> expression;
    explicit ExprStmt(std::unique_ptr<Expr> expr) : expression(std::move(expr)) {}
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// An assignment statement: target = value;
// Covers:
//   ctrl = Disk:Reading;    (stance transition — emits: ctrl->__stance = 1)
//   my_disk = Disk:Idle;    (stance cast after divine block)
//   sector_count = sector_count + 1;
//
// Note: 'target' can be an IdentifierExpr, a member access BinaryExpr, or
// anything that is a valid lvalue. Semantic analysis enforces lvalue validity.
struct AssignStmt : public Stmt {
    std::unique_ptr<Expr> target; // Left-hand side (must be an lvalue)
    std::unique_ptr<Expr> value;  // Right-hand side
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// yield (ctrl, data);    — tuple return
// yield 0;              — single value return
// yield;                — void return (abyss spells)
//
// CodeGen rule: Every yield in a spell that contains a destined block is
// rewritten to:
//   __ret = <values>;
//   goto __destined_N;
// This ensures the destined cleanup fires on every exit path.
struct YieldStmt : public Stmt {
    // Empty for void yields. One element for single returns. Multiple for tuples.
    std::vector<std::unique_ptr<Expr>> values;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// shatter; — breaks out of the innermost fore or whirl loop.
// Maps to C: break;
struct ShatterStmt : public Stmt {
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// surge; — skips the rest of the current loop body and starts the next iteration.
// Maps to C: continue;
struct SurgeStmt : public Stmt {
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// if (condition) { } elif (condition) { } else { }
// Maps to C: if/else if/else chain.
// elifBranches may be empty. elseBranch is nullptr if no 'else' was written.
struct IfStmt : public Stmt {
    std::unique_ptr<Expr>      condition;
    std::unique_ptr<BlockStmt> thenBranch;
    std::vector<ElifBranch>    elifBranches; // Zero or more
    std::unique_ptr<BlockStmt> elseBranch;   // nullptr if absent
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// fore (mark16 i = 0; i < 10; i++) { }
// Maps to C: for (int16_t i = 0; i < 10; i++) { }
//
// initType and initVar define the loop variable.
// initValue is the starting expression (e.g., 0).
// condition is evaluated before each iteration.
// increment is evaluated after each iteration (e.g., i++, stored as an expression).
struct ForeStmt : public Stmt {
    Token                      initType;    // e.g., MARK16
    Token                      initVar;     // e.g., i
    std::unique_ptr<Expr>      initValue;   // e.g., 0
    std::unique_ptr<Expr>      condition;   // e.g., i < 10
    std::unique_ptr<Expr>      increment;   // Left side (e.g., i)
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// whirl (condition) { }
// Maps to C: while (condition) { }
struct WhirlStmt : public Stmt {
    std::unique_ptr<Expr>      condition;
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// destined (condition) { cleanup_body; }
// destined { cleanup_body; }             — condition is OPTIONAL
//
// Guaranteed cleanup that runs right before any yield in the enclosing spell,
// regardless of which yield path fires.
//
// hasCondition: true if a condition was written; false for unconditional cleanup.
// condition:    nullptr when hasCondition is false.
//
// CodeGen: Every yield in the spell becomes:
//   __ret = <value>; goto __destined_N;
// The label __destined_N: is placed before the return, with the condition
// guard and cleanup body, ending in: return __ret;
// Multiple destined blocks chain in LIFO order (last declared fires first).
struct DestinedStmt : public Stmt {
    bool                       hasCondition = false;
    std::unique_ptr<Expr>      condition;   // nullptr when hasCondition is false
    std::unique_ptr<BlockStmt> body;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
//     (ctrl, scroll data)                    => { ... }
//     (ctrl, ruin<DiskError::HardwareFault>) => { ... }
//     (ctrl, ruin err)                       => { ... }
// }
//
// targetVar: The variable that receives ownership after the block (my_disk).
// spellCall: The call expression (fetch_sector(...)).
// branches:  The pattern match branches (see DivineBranch above).
//
// CodeGen expands this to:
//   Tuple_T __result = fetch_sector(&my_disk, ...);
//   my_disk = __result.__elem0;      // <~ rebinding
//   if (!__result.__elem1.__is_ruin) { ... }   // success
//   else if (__result.__elem1.__ruin == N) { } // specific ruin
//   else { }                                   // catch-all
struct DivineStmt : public Stmt {
    Token                      targetVar;  // my_disk
    std::unique_ptr<Expr>      spellCall;  // fetch_sector(own &my_disk, 0x0500)
    std::vector<DivineBranch>  branches;
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// mark16 x = 5;
// sigil Device dev = Device:Idle { ... };
struct VarDeclStmt : public Stmt {
    Token typeToken;                      // e.g., MARK16 or the IDENT "Device"
    Token name;                           // e.g., "x" or "dev"
    std::unique_ptr<Expr> initializer;    // The expression after '=', can be null
    // Is this an array declaration? e.g., deck[80] rune name;
    bool  isArray         = false;
    Token arraySizeToken;   // The size token: "80" in deck[80]
    // Is this a pointer declaration? e.g., mark16* px = ...; sigil* Device ptr = ...;
    // When isPointer = true, CodeGen emits: cType* name;
    bool  isPointer       = false;

    VarDeclStmt() = default;
    VarDeclStmt(Token typeToken, Token name, std::unique_ptr<Expr> initializer)
        : typeToken(std::move(typeToken)), name(std::move(name)), initializer(std::move(initializer)) {}

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// =============================================================================
// EXPRESSIONS
// Things that produce values. Expressions can be nested arbitrarily deep.
// =============================================================================

// Binary infix operation: a + b, a == b, a ~> b, ctrl->stance, etc.
// 'op' is the operator token — used by CodeGen to emit the right C operator
// and by error reporting to point at the right source location.
struct BinaryExpr : public Expr {
    std::unique_ptr<Expr> left;
    Token                 op;
    std::unique_ptr<Expr> right;

    BinaryExpr(std::unique_ptr<Expr> l, Token op, std::unique_ptr<Expr> r)
        : left(std::move(l)), op(op), right(std::move(r)) {
        token = op; // Source location of the expression = the operator
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Prefix unary operation: -x (negation), *ptr (dereference).
// 'op' is the operator token. 'operand' is the expression it applies to.
struct UnaryExpr : public Expr {
    Token                 op;
    std::unique_ptr<Expr> operand;

    UnaryExpr(Token op, std::unique_ptr<Expr> operand)
        : op(op), operand(std::move(operand)) {
        token = op;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Postfix '?' — the Omen unpack operator.
// e.g., read_buffer()?
//
// CodeGen: Checks __is_ruin on the returned Omen. If ruin, immediately yields
// the ruin up the call chain. If success, unwraps and produces the value.
// This is the ? operator that binds tighter than ~> per the spec.
struct PostfixExpr : public Expr {
    std::unique_ptr<Expr> operand;
    Token                 op;
    std::shared_ptr<TypeInfo> resolvedOmenType = nullptr;

    // PATCH 3: The name of the ownership parameter (sigil* param) to use
    // in the early-return tuple when this ? operator propagates a ruin.
    // Populated by SemanticAnalyzer::visit(PostfixExpr*) during Pass 2.
    // CodeGen reads this instead of the type-name heuristic.
    // Empty string = no ownership slot (spell does not return a tuple).
    std::string ownershipParamName;

    PostfixExpr(std::unique_ptr<Expr> operand, Token op)
        : operand(std::move(operand)), op(op) {
        token = op;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// An integer literal, string literal, or boolean literal.
// e.g., 0x1F7, 42, "Drive dead", kept, forsaken
//
// The token.type tells CodeGen which kind it is:
//   INT_LIT    -> emit as integer constant (with target type cast)
//   STRING_LIT -> emit as (Cgil_Scroll){ .ptr = ..., .len = ... }
//   KEPT       -> emit as 1
//   FORSAKEN   -> emit as 0
struct LiteralExpr : public Expr {
    explicit LiteralExpr(Token tok) { token = tok; }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// A reference to a named entity: variable, type, stance, or rank variant.
//
// Plain identifier:       token.lexeme = "my_disk",  stanceName empty, variantName empty
// Stance reference:       token.lexeme = "Disk",     stanceName.lexeme = "Fault"
// Rank variant reference: token.lexeme = "DiskError",variantName.lexeme = "HardwareFault"
//
// Semantic analysis uses stanceName and variantName to distinguish these cases
// and validate them against declared stances and rank variants.
struct IdentifierExpr : public Expr {
    Token stanceName;   // Populated for Disk:Fault style
    Token variantName;  // Populated for DiskError::HardwareFault style

    explicit IdentifierExpr(Token tok) { token = tok; }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// A function call: fetch_sector(own &my_disk, 0x0500)
//
// callee: The function being called (usually an IdentifierExpr).
// args: The argument expressions in order.
// argIsOwned: Parallel to args — true if 'own' keyword was on that argument.
//             'own' is compile-time only; CodeGen strips it and emits the bare arg.
//
// The parenToken (stored as node->token) points to the '(' for error reporting.
struct CallExpr : public Expr {
    std::unique_ptr<Expr>              callee;
    std::vector<std::unique_ptr<Expr>> args;
    std::vector<bool>                  argIsOwned; // parallel to args

    CallExpr(std::unique_ptr<Expr> callee, Token parenToken)
        : callee(std::move(callee)) {
        token = parenToken;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// The address-of operator: &my_disk, &disk_data_port
//
// For regular variables: standard C address-of, emits &my_disk
// For leyline/portline:  evaluates to the bound hardware address as a
//                        compile-time addr constant (not a pointer).
//                        e.g., &disk_data_port -> (uint16_t)0x1F0
//
// Semantic analysis distinguishes these cases using the symbol table.
struct AddressOfExpr : public Expr {
    std::unique_ptr<Expr> operand;

    AddressOfExpr(Token ampToken, std::unique_ptr<Expr> operand)
        : operand(std::move(operand)) {
        token = ampToken; // The '&' token
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Array subscript: target[index]
// e.g., vga_row[col], cache[i].sector_id
//
// Used for:
//   - deck array access: deck[80] rune buf -> buf[i]
//   - leyline array access: vga_buffer[offset]
//
// CodeGen emits: (target)[index]
// The parentheses around target prevent ambiguity when target is complex.
struct IndexExpr : public Expr {
    std::unique_ptr<Expr> target; // The array-like expression being subscripted
    std::unique_ptr<Expr> index;  // The subscript expression

    IndexExpr(std::unique_ptr<Expr> t, std::unique_ptr<Expr> i, Token bracket)
        : target(std::move(t)), index(std::move(i)) {
        token = bracket; // The '[' token — source location for errors
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Struct/sigil initializer expression: TypeName { field: value, ... }
// e.g., Packet { length: 64, checksum: 0xABCD, type_byte: 0x01, sequence: 100 }
// e.g., Device:Idle { device_id: 1, error_code: 0, flags: 0 }
//
// This appears as the RHS of VarDeclStmt:
//   sigil Packet pkt = Packet { length: 64, ... };
//
// STANCE-PREFIXED FORM:
//   sigil Device dev = Device:Idle { device_id: 0, ... }
//   The stance is parsed as part of the type prefix (IdentifierExpr with stanceName).
//   typeName.lexeme = "Device", stanceName.lexeme = "Idle"
//
// CodeGen emits GNU designated initializer compound literal:
//   (Packet){ .length = 64, .checksum = 0xABCD, .type_byte = 0x01, .sequence = 100 }
//
// For stance-prefixed structs, CodeGen also emits the __stance field:
//   (Device){ .__stance = Device_Idle, .device_id = 0, .error_code = 0, .flags = 0 }
//
// CONSTRAINT (enforced by Semantic Analyzer):
//   A StructInitExpr compound literal MUST NOT be passed to a conjure spell
//   by non-owned pointer, as the compound literal's lifetime ends at the
//   statement boundary and the external C function cannot be inspected for
//   pointer retention. Passing to conjure by value (copy) is always safe.
struct StructInitExpr : public Expr {
    Token typeName;    // The sigil/legion type name (e.g., "Packet", "Device")
    Token stanceName;  // Optional stance prefix (e.g., "Idle" for Device:Idle { ... })
                       // Empty lexeme means no stance prefix.

    std::vector<StructFieldInit> fields; // Field initializers in source order

    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Formal Assignment Expression (e.g., i = i + 1 used inside fore loops)
struct AssignExpr : public Expr {
    std::unique_ptr<Expr> target;
    Token                 op;      // The '=' token
    std::unique_ptr<Expr> value;

    AssignExpr(std::unique_ptr<Expr> t, Token o, std::unique_ptr<Expr> v)
        : target(std::move(t)), op(o), value(std::move(v)) {
        token = o;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// cast<mark32>(val) or cast<mark16*>(val) — explicit type conversion
struct CastExpr : public Expr {
    Token                 targetType; 
    bool                  isPointer;
    std::unique_ptr<Expr> operand;

    CastExpr(Token targetTok, bool isPtr, std::unique_ptr<Expr> op)
        : targetType(targetTok), isPointer(isPtr), operand(std::move(op)) {
        token = targetTok;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};

// Prefix or Postfix update: ++i, i++, --val, val--
struct UpdateExpr : public Expr {
    std::unique_ptr<Expr> operand;
    Token                 op;        // PLUS_PLUS or MINUS_MINUS
    bool                  isPrefix;  // true for ++i, false for i++

    UpdateExpr(std::unique_ptr<Expr> operand, Token op, bool isPrefix)
        : operand(std::move(operand)), op(op), isPrefix(isPrefix) {
        token = op;
    }
    void accept(ASTVisitor& visitor) override { visitor.visit(this); }
};