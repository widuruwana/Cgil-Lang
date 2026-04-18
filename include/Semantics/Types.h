#pragma once
#include <unordered_map>
#include <string>
#include <vector>
#include <memory>

// =============================================================================
// CGIL TYPE SYSTEM
// =============================================================================
//
// WHAT THIS FILE IS:
//   This file defines how Cgil thinks about types internally. When the semantic
//   analyzer sees the token "mark16" in a parameter list, it needs to represent
//   that type as a C++ data structure it can store, compare, and pass around.
//   TypeInfo is that data structure.
//
// TWO LEVELS:
//   The TYPE SYSTEM has two levels that are easy to confuse:
//
//   1. The SOURCE LEVEL — what the programmer writes: mark16, Disk, scroll, etc.
//      These are token strings from the lexer.
//
//   2. THE INTERNAL LEVEL — what the semantic analyzer and CodeGen use.
//      This is TypeKind + TypeInfo. The internal representation is richer:
//      it can express "an Omen whose success type is scroll and ruin type is
//      DiskError" in a way that token strings cannot.
//
// HOW TYPE RESOLUTION WORKS:
//   1. registerPrimitives() pre-loads the built-in types.
//   2. RankDecl and SigilDecl visitors add user types to the typeRegistry.
//   3. When a parameter or variable is used, its source token is looked up in
//      the typeRegistry to get a TypeInfo.
//   4. Type comparisons use TypeInfo::operator== to check compatibility.
//
// =============================================================================

// Every distinct category of Cgil type.
// Each category has different semantic rules and CodeGen behavior.
enum class TypeKind {
    // Built-in primitive types: mark16, mark32, soul16, soul32, addr, rune,
    // oath, flow, scroll, abyss, deck, tuple.
    // CodeGen: maps directly to a C stdint type (see Context.md table).
    PRIMITIVE,

    // User-defined struct with optional stances.
    // sigil Disk { stance Idle; soul16 sector_count; }
    // CodeGen: emits a C struct with uint16_t __stance as the first field.
    SIGIL,

    // Structure of Arrays (SoA)
    LEGION,

    // User-defined enum with sequential uint16_t discriminants.
    // rank DiskError { Timeout, HardwareFault, InvalidSector }
    // CodeGen: emits typedef uint16_t + #define constants.
    RANK,

    // A sum type: either a success value T or a typed error ruin<R>.
    // scroll | ruin<DiskError>
    // CodeGen: emits a tagged struct with __is_ruin discriminant and a union.
    // The successType and ruinType fields carry the sub-types.
    OMEN,

    // A multi-value boundary type used only in yield and divine.
    // (sigil* Disk, scroll | ruin<DiskError>)
    // CodeGen: emits a unique C struct typedef for each distinct tuple signature.
    // The tupleElements vector carries the element types in order.
    TUPLE,

    // A hardware-bound variable — leyline (MMIO) or portline (PIO).
    // Semantically: reading the identifier evaluates to the value at the address.
    //               &identifier evaluates to the address itself as addr (compile-time constant).
    // CodeGen: leyline -> volatile pointer; portline -> inline assembly.
    // This separate TypeKind allows AddressOfExpr to distinguish &hardware_var
    // from &regular_var and emit the right C.
    HARDWARE
};

// The internal representation of a Cgil type.
// Every variable, parameter, and expression has a TypeInfo.
struct TypeInfo {
    TypeKind    kind;   // Which category of type is this?
    std::string name;   // Human-readable name: "mark16", "Disk", "DiskError", etc.

    // --- For OMEN types: scroll | ruin<DiskError> ---
    // successType: the type of the success payload (e.g., scroll)
    // ruinType:    the rank type of the error (e.g., DiskError)
    // Both are null for non-Omen types.
    std::shared_ptr<TypeInfo> successType;
    std::shared_ptr<TypeInfo> ruinType;

    // --- For TUPLE types: (sigil* Disk, scroll | ruin<DiskError>) ---
    // Stores the element types in declaration order.
    // Empty for non-Tuple types.
    std::vector<std::shared_ptr<TypeInfo>> tupleElements;

    // --- For HARDWARE types ---
    // isPortline: false = leyline (MMIO volatile pointer)
    //             true  = portline (PIO inline assembly)
    bool isPortline = false;

    // --- For SIGIL types: field name -> field TypeInfo ---
    // Populated during Pass 1 in SemanticAnalyzer::visit(SigilDecl*).
    // Used by visit(BinaryExpr*) ARROW/DOT to return the correct field type
    // instead of the containing sigil type (Fatal Fix 3).
    //
    // Example: sigil Disk { soul16 sector_count; rune flags; }
    //   fields["sector_count"] -> TypeInfo{PRIMITIVE, "soul16"}
    //   fields["flags"]        -> TypeInfo{PRIMITIVE, "rune"}
    //
    // The __stance pseudo-field is implicitly soul16 — handled separately in
    // visit(BinaryExpr*) by checking if the field name is "stance".
    //
    // Empty for non-SIGIL types.
    std::unordered_map<std::string, std::shared_ptr<TypeInfo>> fields;

    // --- For ARRAY types (deck[N] T) ---
    // Populated when a deck variable is declared in visit(VarDeclStmt*).
    // Returned by visit(IndexExpr*) so that buf[i].field has the correct type.
    std::shared_ptr<TypeInfo> elementType = nullptr;

    // ADDED CONSTRUCTOR: Silences GCC's -Wmissing-field-initializers warnings
    TypeInfo(TypeKind k, std::string n, 
             std::shared_ptr<TypeInfo> s = nullptr, 
             std::shared_ptr<TypeInfo> r = nullptr)
        : kind(k), name(n), successType(s), ruinType(r) {}

    // Type equality. Two types are equal if they have the same kind and name.

    // Type equality. Two types are equal if they have the same kind and name.
    // For OMEN types, equality also requires matching success and ruin sub-types.
    //
    // NOTE: This does NOT deeply compare tupleElements (tuple equality is
    // structural — checked by the caller when needed). For V1 this is sufficient
    // since tuple types appear only in divine/yield boundaries.
    bool operator==(const TypeInfo& other) const {
        if (kind != other.kind || name != other.name) return false;
        // For Omen types, sub-types must also match.
        if (kind == TypeKind::OMEN) {
            bool successMatch = (!successType && !other.successType) ||
                                (successType && other.successType && *successType == *other.successType);
            bool ruinMatch    = (!ruinType && !other.ruinType) ||
                                (ruinType && other.ruinType && *ruinType == *other.ruinType);
            return successMatch && ruinMatch;
        }
        return true;
    }

    bool operator!=(const TypeInfo& other) const { return !(*this == other); }
};
