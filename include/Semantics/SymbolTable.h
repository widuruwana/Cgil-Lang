#pragma once
#include "Types.h"
#include <string>
#include <unordered_map>
#include <vector>
#include <stdexcept>

// =============================================================================
// SYMBOL TABLE
// =============================================================================
//
// WHAT THIS IS:
//   The symbol table is a scoped map from variable names to their compile-time
//   state. At every point during semantic analysis, the symbol table represents
//   "what variables exist right now, and what do we know about each one?"
//
// SCOPING:
//   The table is a STACK OF SCOPES. Each scope is an unordered_map from name
//   to Symbol. The stack looks like:
//
//     scopes[0]  = Global scope  (hardware decls, nothing else)
//     scopes[1]  = Spell scope   (parameters declared here)
//     scopes[2]  = Block scope   (variables inside { } blocks)
//     scopes[3]  = Branch scope  (divine branch variables)
//
//   When lookup() searches for a name, it starts at the innermost (last) scope
//   and works outward. This is standard lexical scoping — inner declarations
//   shadow outer ones.
//
//   enterScope() pushes a new empty map onto the stack.
//   exitScope()  pops the innermost map, destroying all variables in it.
//
// CGIL-SPECIFIC STATE:
//   Each Symbol carries more than just its type. It also tracks:
//   - isOwned:      Was this declared with the 'own' keyword?
//   - isMoved:      Has ownership been transferred away (e.g., via `own &x`)?
//   - currentStance: What stance is this sigil currently in? ("Idle", "Reading", etc.)
//   - isHardware:   Is this a leyline or portline variable?
//
//   These fields are the core of Cgil's compile-time safety model. The semantic
//   analyzer reads and mutates these fields as it walks the AST.
//
// POINTER STABILITY WARNING:
//   lookup() returns a raw pointer into an unordered_map. This pointer is valid
//   as long as NO new symbols are declared in the SAME scope while the pointer
//   is live. If declare() causes the map to rehash (grow), the pointer becomes
//   a dangling pointer — undefined behavior.
//
//   SAFE PATTERN (always do this):
//     Symbol* sym = symbols.lookup("x");
//     sym->isMoved = true;              // OK — no declares in between
//
//   UNSAFE PATTERN (never do this):
//     Symbol* sym = symbols.lookup("x");
//     symbols.declare("y", type);       // May rehash the map sym points into!
//     sym->isMoved = true;              // UNDEFINED BEHAVIOR
//
//   In practice this is safe because:
//     a) We always call enterScope() before declare(), so new declares go to a
//        DIFFERENT map than the one the pointer references.
//     b) The spell scope is entered before parameters are declared, so we never
//        lookup a param and then declare another param in the same scope.
//
//   A production implementation would use stable storage (std::list, index-based
//   lookup) to remove this constraint entirely.
//
// =============================================================================

// The compile-time state of a single named variable.
struct Symbol {
    std::string               name;          // The variable's name (for error messages)
    std::shared_ptr<TypeInfo> type;          // What type it is

    // --- Cgil Ownership Model ---
    bool isOwned = false;     // Was this declared with 'own'? (own sigil* Disk ctrl)
    bool isMoved = false;     // Was ownership transferred away? (own &ctrl passed to spell)
                              // If isOwned && isMoved: using this variable is a compile error.

    // --- Cgil Typestate Model ---
    // The current stance of this variable as the compiler understands it.
    // Examples: "Idle", "Reading", "Fault", "Unknown"
    // Empty string means: no stance (not a sigil with stances, or sigil without stances).
    // "Unknown" means: stance diverged in a divine block — can only be used in
    //                  spells that accept unannotated sigil* (no stance requirement).
    std::string currentStance;

    // --- Hardware Variable ---
    // True if this was declared as a leyline or portline.
    // Used by AddressOfExpr: &hardware_var emits a compile-time addr constant.
    //                        &regular_var  emits C's standard address-of operator.
    bool isHardware = false;
};

class SymbolTable {
public:

    // Push a new empty scope onto the stack.
    // Called when entering: a spell body, a block, a for loop, a divine branch.
    void enterScope() {
        scopes.emplace_back();
    }

    // Pop the innermost scope, destroying all symbols declared in it.
    // Called when leaving: a spell body, a block, a for loop, a divine branch.
    // All variables that went out of scope are gone — any owned pointers that
    // were not yielded back are now permanently lost (compiler should warn).
    void exitScope() {
        if (!scopes.empty()) scopes.pop_back();
    }

    // Declare a new variable in the CURRENT (innermost) scope.
    //
    // Parameters:
    //   name:       The variable name (must be unique in the current scope)
    //   type:       The resolved TypeInfo for this variable
    //   isOwned:    Was 'own' present in the declaration?
    //   stance:     Initial stance (empty = no stance, "Idle" = starts Idle, etc.)
    //   isHardware: Is this a leyline or portline hardware variable?
    //
    // NOTE: Throws std::runtime_error on collision because the error() method
    // from SemanticAnalyzer is not accessible here. The caller (SemanticAnalyzer)
    // should check for collisions using lookup() first if it wants a proper
    // source-located error. For V1 this is acceptable.
    void declare(const std::string&          name,
                 std::shared_ptr<TypeInfo>   type,
                 bool                        isOwned    = false,
                 const std::string&          stance     = "",
                 bool                        isHardware = false)
    {
        if (scopes.empty()) {
            throw std::runtime_error("Internal: declare() called with no active scope.");
        }
        if (scopes.back().count(name)) {
            throw std::runtime_error("Variable '" + name + "' is already declared in this scope.");
        }
        scopes.back()[name] = Symbol{name, type, isOwned, false, stance, isHardware};
    }

    // Look up a variable by name, searching from innermost scope outward.
    // Returns nullptr if the name is not found in any scope.
    //
    // The returned pointer is stable as long as no new symbols are declared
    // in the SAME scope that the symbol was found in. See the Pointer Stability
    // Warning in the class comment above.
    Symbol* lookup(const std::string& name) {
        for (auto it = scopes.rbegin(); it != scopes.rend(); ++it) {
            auto found = it->find(name);
            if (found != it->end()) {
                return &found->second;
            }
        }
        return nullptr;
    }

    // Returns the number of currently active scopes.
    // Useful for assertions: spell bodies should always be at scope depth >= 2.
    int depth() const {
        return static_cast<int>(scopes.size());
    }

private:
    // The scope stack. Index 0 = global, last = innermost.
    // Each scope is an unordered_map from variable name to Symbol.
    std::vector<std::unordered_map<std::string, Symbol>> scopes;
};
