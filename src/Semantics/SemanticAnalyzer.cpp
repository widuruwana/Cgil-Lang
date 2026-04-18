#include "../../include/Semantics/SemanticAnalyzer.h"
#include <functional> // For std::function
#include <set>        // For std::set
#include <typeinfo>   // For typeid

// Returns true if valueType can be assigned to targetType without a cast.
// Cgil assignment compatibility rules:
//   - Same type: always OK
//   - Numeric primitives: OK (mark16, mark32, soul16, soul32, addr, rune inter-assign)
//   - SIGIL to SIGIL: must be the exact same named sigil
//   - LEGION to LEGION: must be the exact same named legion
//   - Everything else: ERROR
static bool isAssignmentCompatible(
    const std::shared_ptr<TypeInfo>& target,
    const std::shared_ptr<TypeInfo>& value)
{
    if (!target || !value) return true; // Fallback if unresolved

    // 1. Exact match (handles identical sigils, legions, ranks, and primitives)
    if (target->kind == value->kind && target->name == value->name) {
        // Reject direct array-to-array assignments
        if (target->name.rfind("__array_", 0) == 0) return false; 
        return true;
    }

    // 2. Numeric Primitive Inter-assignment (C handles the widening/truncation)
    if (target->kind == TypeKind::PRIMITIVE && value->kind == TypeKind::PRIMITIVE) {
        // 'abyss' (void) is never assignable
        if (target->name == "abyss" || value->name == "abyss") return false;
        
        // 'scroll' (fat pointer) cannot be implicitly assigned to/from a number
        if (target->name == "scroll" || value->name == "scroll") return false;
        
        // 'oath' (boolean) is strict
        if (target->name == "oath" && value->name != "rune") return false;
        
        return true; // Other primitives (mark16, soul32, addr, rune) can mix
    }

    return false;
}

// =============================================================================
// PRIVATE HELPERS
// =============================================================================

// Evaluate an expression and return its type.
// This is the standard way to ask "what type does this expression produce?"
// Sets currentExprType as a side effect, then returns it.
std::shared_ptr<TypeInfo> SemanticAnalyzer::evaluate(Expr* node) {
    // Reset before every evaluation so stale values cannot escape.
    // If the visitor does not set currentExprType, the nullptr triggers the
    // assertion below instead of silently returning a wrong type.
    currentExprType = nullptr;

    node->accept(*this);

    // Hard assertion: every expression visitor MUST set currentExprType.
    // If this fires, a visitor is missing its currentExprType assignment.
    // This is a compiler development error, not a user error.
    if (!currentExprType) {
        // We cannot call error() here because we may not have a valid source
        // token. Throw a runtime_error that propagates to main.cpp's handler.
        throw std::runtime_error(
            "Internal compiler error: expression visitor did not set "
            "currentExprType. This is a bug in the Cgil compiler itself. "
            "The expression node type: " +
            std::string(typeid(*node).name()));
    }

    return currentExprType;
}

// Look up a type name in the registry. Error if not found.
// Use this instead of typeRegistry[] which silently inserts null on missing keys.
std::shared_ptr<TypeInfo> SemanticAnalyzer::resolveType(Token nameToken) {
    auto it = typeRegistry.find(nameToken.lexeme);
    if (it == typeRegistry.end()) {
        error(nameToken, "Unknown type '" + nameToken.lexeme + "'.");
    }
    return it->second;
}

std::shared_ptr<TypeInfo> SemanticAnalyzer::getBuiltinType(const std::string& name) const {
    auto it = typeRegistry.find(name);
    if (it != typeRegistry.end() && it->second) return it->second;
    return std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, name});
}

// =============================================================================
// PASS 1 — THE GLOBAL LEDGER
// =============================================================================
// In Pass 1, we walk all top-level declarations and register every type, spell,
// and hardware variable that exists in the program. We do NOT look inside spell
// bodies. We do NOT check types of expressions.
//
// After Pass 1 completes, the typeRegistry and spellRegistry are fully populated.
// Pass 2 can then look up any type or spell by name without forward-declaration
// ordering concerns — this is how Cgil eliminates header-ordering hell.
//
// PATTERN: Every visitor checks `if (!isPassOne) return;` at the top.
//          This means Pass 2 re-enters the same visitor but skips immediately.
//          Pass 2 entry points are at the BOTTOM of each declaration visitor
//          in a clearly marked section.
// =============================================================================

// grimoire <hardware_defs.h>;   pact <stdlib.h>;
// Pass 1: Nothing to register. Includes are resolved at the C level.
// Pass 2: In a full implementation, pact in kernel context would warn here.
void SemanticAnalyzer::visit(GrimoireDecl* node) {
    // Warn if pact is used in a bare-metal context
    if (isPassOne && node->isPact) {
        std::cerr << "[Semantic Warning Line " << node->token.line << "] "
                  << "pact imports assume a hosted OS environment and are unsafe for Ring 0 wardens. "
                  << "Ensure this file is not compiled into a bare-metal kernel.\n";
    }
}

// rank DiskError { Timeout, HardwareFault, InvalidSector }
//
// PASS 1: Register "DiskError" in the typeRegistry with kind=RANK.
//         The variants (Timeout, HardwareFault, etc.) are NOT put in the symbol
//         table. They are accessed via scope resolution (DiskError::Timeout) and
//         are looked up directly from the RankDecl AST node by Pass 2 code that
//         handles rank variant references.
//
// PASS 2: No additional action needed — variants are validated at use sites.
void SemanticAnalyzer::visit(RankDecl* node) {
    if (!isPassOne) return;

    if (typeRegistry.count(node->name.lexeme)) {
        error(node->name, "Type '" + node->name.lexeme + "' is already declared.");
    }

    typeRegistry[node->name.lexeme] = std::make_shared<TypeInfo>(
        TypeInfo{TypeKind::RANK, node->name.lexeme}
    );

    // Populate rankVariants so DivineStmt can enforce exhaustiveness.
    // Store the variant list in declaration order.
    // Pass 2's visit(DivineStmt*) reads this to verify specific ruin branches
    // cover all variants when no catch-all is present.
    std::vector<std::string> variants;
    variants.reserve(node->variants.size());
    for (const auto& v : node->variants) {
        variants.push_back(v.lexeme);
    }
    rankVariants[node->name.lexeme] = std::move(variants);
}

// sigil Disk { stance Idle; stance Reading; soul16 sector_count; }
//
// PASS 1: Register the sigil type. Register any bound spells under a mangled name.
//         Name mangling: bound spell "emit" on sigil "ASTNode" -> "ASTNode_emit".
//         This prevents name collisions between top-level spells and bound spells.
//
// PASS 2: Validate that all field types exist in the registry.
//         Analyze the bodies of all bound spells.
void SemanticAnalyzer::visit(SigilDecl* node) {
    if (isPassOne) {
        if (typeRegistry.count(node->name.lexeme)) {
            error(node->name, "Type '" + node->name.lexeme + "' is already declared.");
        }

        typeRegistry[node->name.lexeme] = std::make_shared<TypeInfo>(
            TypeInfo{TypeKind::SIGIL, node->name.lexeme}
        );

        // Register bound spells (V1.5 encapsulation).
        for (auto& boundSpell : node->boundSpells) {
            std::string mangledName = node->name.lexeme + "_" + boundSpell->name.lexeme;
            if (spellRegistry.count(mangledName)) {
                error(boundSpell->name, "Bound spell '" + mangledName + "' is already declared.");
            }
            spellRegistry[mangledName] = boundSpell.get();
        }
        return;
    }

    // Pass 2: Validate field types, check FPU usage, and populate TypeInfo::fields.
    //
    // Populate the field map on the sigil's TypeInfo so that
    // member access expressions (ctrl->sector_count) resolve to the correct
    // field type (soul16) instead of the container type (Disk).
    //
    // We write into the TypeInfo that was registered in Pass 1.
    // Since typeRegistry holds shared_ptr<TypeInfo>, we can mutate it here.
    auto& sigilTypeInfo = typeRegistry.at(node->name.lexeme);

    for (auto& field : node->fields) {
        warnIfFlow(field.type);
        auto fieldType = resolveType(field.type);
        // Store the field's resolved type in the sigil's TypeInfo field map.
        // This enables BinaryExpr ARROW/DOT to return the actual field type.
        sigilTypeInfo->fields[field.name.lexeme] = fieldType;
    }

    // Pass 2: Analyze bound spell bodies.
    for (auto& boundSpell : node->boundSpells) {
        boundSpell->accept(*this);
    }
}

// legion SectorCache { mark16 sector_id; flow read_time; oath is_corrupted; }
//
// PASS 1: Register as SIGIL (V1 stub — no SoA transformation yet).
//         The distinct LEGION keyword is preserved for V2 to identify these nodes.
//
// PASS 2: Validate field types exist.
void SemanticAnalyzer::visit(LegionDecl* node) {
    if (isPassOne) {
        if (typeRegistry.count(node->name.lexeme)) {
            error(node->name, "Type '" + node->name.lexeme + "' is already declared.");
        }
        typeRegistry[node->name.lexeme] = std::make_shared<TypeInfo>(
            TypeInfo{TypeKind::LEGION, node->name.lexeme}
        );
        return;
    }

    // Pass 2: Validate field types.
    auto& legionTypeInfo = typeRegistry.at(node->name.lexeme);
    for (auto& field : node->fields) {
        warnIfFlow(field.type);
        auto fieldType = resolveType(field.type);
        // Populate the field map so DOT access knows the type
        legionTypeInfo->fields[field.name.lexeme] = fieldType; 
    }
}

// spell fetch_sector(...)   warden spell disk_irq()   conjure spell read_buffer(...)
//
// PASS 1: Register the spell in the spellRegistry. Validation of parameter types
//         and return types happens in Pass 2 when we have the full type registry.
//
// PASS 2: This is where the real work happens.
//   1. Set currentSpell so all nested visitors know the enclosing context.
//   2. Validate parameter types exist.
//   3. Open a scope and declare all parameters as symbols.
//   4. Walk every statement in the body.
//   5. Close the scope and clear currentSpell.
//
void SemanticAnalyzer::visit(SpellDecl* node) {
    if (isPassOne) {
        if (spellRegistry.count(node->name.lexeme)) {
            error(node->name, "Spell '" + node->name.lexeme + "' is already declared.");
        }
        spellRegistry[node->name.lexeme] = node;
        return;
    }

    // conjure spells are extern declarations — they have no body to analyze.
    if (node->isConjure) return;

    // --- WARDEN SPELL CONSTRAINT CHECK ---
    // warden spells are Interrupt Service Routines. They have strict rules
    // per the spec: no own, no ruin propagation.
    // We enforce this during body analysis by checking currentSpell->isWarden.

    // Set context so nested visitors know what spell they are inside.
    SpellDecl* previousSpell = currentSpell;
    currentSpell = node;

    // RAII guard guarantees scope exit even if an exception is thrown
    struct ScopeGuard {
        SymbolTable& sym;
        ~ScopeGuard() { sym.exitScope(); }
    };

    // Enter the spell's scope. Parameters live here.
    symbols.enterScope();
    ScopeGuard guard{symbols};

    // Declare all parameters as symbols. Also check each for FPU type.
    for (auto& param : node->params) {
        warnIfFlow(param.type); // PLAN A: check each parameter type
        auto paramType = resolveType(param.type);
        symbols.declare(
            param.name.lexeme,
            paramType,
            param.isOwned,
            param.stanceName.lexeme,
            false
        );
    }

    // PLAN A: Check each return type for FPU usage.
    for (const auto& rt : node->returnTypes) {
        warnIfFlow(rt.typeToken);
    }

    // Walk the spell body. Every statement will be analyzed in this scope.
    for (auto& stmt : node->body) {
        stmt->accept(*this);
    }

    // Clean up.
    currentSpell = previousSpell;
}

// leyline disk_status_port: rune @ 0x1F7;
// portline disk_data_port: soul16 @ 0x1F0;
//
// PASS 1: Validate the type exists, then declare as a global hardware variable.
//         isHardware=true distinguishes these from regular variables for the
//         `&name` address-of semantic — &hardware_var emits a compile-time
//         address constant, not C's address-of operator.
//
// PASS 2: No action — hardware is globally registered in Pass 1.
//
void SemanticAnalyzer::visit(HardwareDecl* node) {
    if (!isPassOne) return;

    auto hwType = resolveType(node->type); // Error if "rune" etc. not registered

    // Build a HARDWARE-kind TypeInfo wrapping the underlying data type.
    auto hardwareTypeInfo = std::make_shared<TypeInfo>(TypeInfo{
        TypeKind::HARDWARE,
        node->name.lexeme,
        nullptr, // no successType
        nullptr  // no ruinType
    });
    hardwareTypeInfo->isPortline = node->isPortline;

    // Declare in global scope (scopes[0]).
    // isHardware=true is critical for AddressOfExpr to emit the right C.
    symbols.declare(
        node->name.lexeme,
        hardwareTypeInfo,
        false, // not owned
        "",    // no stance
        true   // IS hardware
    );
}

// =============================================================================
// PASS 2 — THE CRUCIBLE
// Statement and expression visitors. These only run during Pass 2.
// Every visitor that is statement/expression-only starts with `if (isPassOne) return;`
// =============================================================================

// --- STATEMENT VISITORS ---

// A block of statements: { stmt1; stmt2; ... }
// Creates a new lexical scope so variables declared inside do not leak out.
// Called from: IfStmt branches, WhirlStmt body, ForeStmt body (inner),
//              DestinedStmt body, DivineStmt branch bodies.
//
// NOTE: SpellDecl bodies are NOT visited via BlockStmt — they are iterated
// directly in visit(SpellDecl*) which manages the spell scope explicitly.
void SemanticAnalyzer::visit(BlockStmt* node) {
    if (isPassOne) return;

    symbols.enterScope();
    for (auto& stmt : node->statements) {
        stmt->accept(*this);
    }
    symbols.exitScope();
}

// An expression used as a statement: process_data(data);   acknowledge_interrupt();
// Evaluates the expression for side effects (ownership moves, stance transitions
// triggered by evaluate() -> visit(CallExpr*), etc.).
// The resulting type is discarded — if you wanted the value you would have used
// an assignment, not a bare expression statement.
void SemanticAnalyzer::visit(ExprStmt* node) {
    if (isPassOne) return;
    evaluate(node->expression.get());
}

// yield (ctrl, data);   yield 0;   yield;
//
// Checks that the yield values are consistent with the enclosing spell's
// declared return type. For V1, we check:
//   - Void spells (abyss): yield with no values
//   - Single-value spells: yield with exactly one expression
//   - Tuple-returning spells: yield with the right number of values
//
// Full sub-type checking (e.g., "does the scroll match the Omen success type?")
// is a V1.5 improvement — the types are complex enough that getting the count
// right is the primary V1 safety check.
//
// WARDEN CONSTRAINT: warden spells must yield abyss. They cannot return values.
void SemanticAnalyzer::visit(YieldStmt* node) {
    if (isPassOne) return;
    if (!currentSpell) {
        error(node->token, "'yield' used outside of a spell body.");
    }

    if (currentSpell->isWarden && !node->values.empty()) {
        error(node->token, "'warden spell' must yield 'abyss'. ISRs cannot return values.");
    }

    // Strict Yield Compatibility Checking
    for (size_t i = 0; i < node->values.size(); i++) {
        auto valType = evaluate(node->values[i].get());
        
        std::shared_ptr<TypeInfo> expectedType = nullptr;
        
        if (currentSpell->returnTypes.size() > 1) {
            if (i < currentSpell->returnTypes.size()) {
                expectedType = resolveType(currentSpell->returnTypes[i].typeToken);
            }
        } else if (currentSpell->returnTypes.size() == 1) {
            expectedType = resolveType(currentSpell->returnTypes[0].typeToken);
        }
        
        if (expectedType) {
            // Omen ruin(...) construction returns an OMEN type, which is valid if the spell expects an Omen
            if (currentSpell->hasOmen && valType->kind == TypeKind::OMEN && i == currentSpell->returnTypes.size() - 1) {
                continue; 
            }
            if (!isAssignmentCompatible(expectedType, valType)) {
                error(node->values[i]->token, 
                      "Type mismatch in yield: cannot implicitly assign '" +
                      (valType ? valType->name : "?") + "' to expected return type '" + expectedType->name + "'.");
            }
        }
    }
}

// shatter; — break out of the innermost loop.
// Error if used outside a fore or whirl loop.
void SemanticAnalyzer::visit(ShatterStmt* node) {
    if (isPassOne) return;
    if (loopDepth == 0) {
        error(node->token, "'shatter' used outside of a loop. Only valid inside 'fore' or 'whirl'.");
    }
}

// surge; — continue to the next loop iteration.
// Error if used outside a fore or whirl loop.
void SemanticAnalyzer::visit(SurgeStmt* node) {
    if (isPassOne) return;
    if (loopDepth == 0) {
        error(node->token, "'surge' used outside of a loop. Only valid inside 'fore' or 'whirl'.");
    }
}

// if (condition) { } elif (condition) { } else { }
// Checks that the condition produces a boolean-like type, then visits branches.
// Each branch (BlockStmt) manages its own scope.
void SemanticAnalyzer::visit(IfStmt* node) {
    if (isPassOne) return;

    // Evaluate and check the if condition.
    auto condType = evaluate(node->condition.get());
    if (!isAssignmentCompatible(getBuiltinType("oath"), condType)) {
        error(node->condition->token, "Condition must evaluate to an 'oath' (boolean) type.");
    }

    // Visit each branch. BlockStmt::accept() handles scope for each.
    node->thenBranch->accept(*this);

    for (auto& elifBranch : node->elifBranches) {
        auto elifCondType = evaluate(elifBranch.condition.get());
        if (!isAssignmentCompatible(getBuiltinType("oath"), elifCondType)) {
            error(elifBranch.condition->token, "Condition must evaluate to an 'oath' (boolean) type.");
        }
        elifBranch.body->accept(*this);
    }

    if (node->elseBranch) {
        node->elseBranch->accept(*this);
    }
}

// fore (mark16 i = 0; i < 10; i++) { }
// Opens a scope for the loop variable, declares it, visits condition, body,
// and increment. Increments loopDepth so shatter/surge are valid inside.
void SemanticAnalyzer::visit(ForeStmt* node) {
    if (isPassOne) return;

    // Outer scope for the loop variable — it lives for the whole loop.
    symbols.enterScope();

    // PLAN A: Check loop variable type for FPU usage.
    warnIfFlow(node->initType);
    auto initType = resolveType(node->initType);
    symbols.declare(node->initVar.lexeme, initType);

    // Check the initializer value type (e.g., 0 is mark16-compatible).
    auto initValType = evaluate(node->initValue.get());

    // ENFORCEMENT: Ensure the value assigned to the loop variable matches its type.
    if (!isAssignmentCompatible(initType, initValType)) {
        error(node->initValue->token, 
              "Type mismatch in 'fore' loop initializer: cannot assign '" + 
              (initValType ? initValType->name : "?") + "' to loop variable '" + 
              node->initVar.lexeme + "' of type '" + initType->name + "'.");
    }

    // Check the condition (e.g., i < 10).
    auto condType = evaluate(node->condition.get());
    (void)condType;

    // Track loop nesting for shatter/surge validation.
    loopDepth++;

    // Visit the loop body. BlockStmt creates an inner scope for body statements.
    node->body->accept(*this);

    loopDepth--;

    if (node->increment) {
        // Allow AssignExpr (i = i + 1) AND UpdateExpr (i++)
        if (!dynamic_cast<AssignExpr*>(node->increment.get()) &&
            !dynamic_cast<UpdateExpr*>(node->increment.get())) {
            std::cerr << "[Semantic Warning Line " << node->token.line << "] "
                      << "Loop increment is not an assignment or update expression. "
                      << "The loop variable '" << node->initVar.lexeme 
                      << "' may never be modified, causing an infinite loop.\n";
        }
        evaluate(node->increment.get());
    }

    symbols.exitScope(); // Loop variable goes out of scope
}

// whirl (condition) { }
// Validates condition, increments loopDepth, visits body.
void SemanticAnalyzer::visit(WhirlStmt* node) {
    if (isPassOne) return;

    auto condType = evaluate(node->condition.get());
    if (!isAssignmentCompatible(getBuiltinType("oath"), condType)) {
        error(node->condition->token, "Condition must evaluate to an 'oath' (boolean) type.");
    }

    loopDepth++;
    node->body->accept(*this);
    loopDepth--;
}

// destined (condition) { cleanup_body; }
// destined { cleanup_body; }              <- condition is OPTIONAL
//
// The destined block is RAII cleanup that fires before every yield in the
// enclosing spell. During semantic analysis, we:
//   1. If a condition is present, evaluate it and check it is boolean-like.
//   2. Visit the body to check for errors inside the cleanup block.
//
// The actual CodeGen rewriting (every yield -> __ret = ...; goto __destined_N;)
// happens in the CodeGen phase, not here. Semantic analysis just validates.
//
// NOTE: destined does NOT create a new scope — it reads from the enclosing
// spell scope. The cleanup code needs to see ctrl, data, etc. from the spell.
void SemanticAnalyzer::visit(DestinedStmt* node) {
    if (isPassOne) return;
    if (!currentSpell) {
        error(node->token, "'destined' used outside of a spell body.");
    }

    // WARDEN CONSTRAINT: warden spells CAN use destined (per spec).
    // No restriction needed here.

    // Evaluate the optional condition.
    if (node->hasCondition) {
        auto condType = evaluate(node->condition.get());
        if (!isAssignmentCompatible(getBuiltinType("oath"), condType)) {
            error(node->condition->token, "Destined condition must evaluate to an 'oath' (boolean) type.");
        }
    }

    // Visit the cleanup body. Note: NOT entering a new scope here.
    // The destined body shares the spell's scope — it needs to see all the
    // spell's variables (especially 'ctrl' for stance checks).
    //
    // Reject yield inside destined.
    //
    // WHY THIS IS FATAL:
    //   destined blocks are emitted as goto LABELS at the END of the C function.
    //   Every `yield` in a destined spell emits: __ret = val; goto __destined_N;
    //   If `yield` appears inside the cleanup body itself, CodeGen emits:
    //
    //     __destined_N:;
    //         __ret = val;
    //         goto __destined_N;   ← jumps to itself = infinite loop
    //
    //   This is guaranteed undefined behavior. The CPU hangs. No GCC warning.
    //
    // We scan ALL statements recursively — a yield inside an if inside destined
    // is equally catastrophic. We use a depth-first walk of the cleanup body.
    //
    // This helper lambda walks the block recursively and errors on any YieldStmt.
    std::function<void(BlockStmt*)> checkNoJumpStmt = [&](BlockStmt* block) {
        for (auto& stmt : block->statements) {
            // YieldStmt is always illegal inside destined — yield emits a goto
            // that jumps to the destined label itself, creating an infinite loop.
            if (dynamic_cast<YieldStmt*>(stmt.get())) {
                error(stmt->token,
                      "'yield' is illegal inside a 'destined' cleanup block. "
                      "The destined block executes AFTER yield — placing yield here "
                      "creates an infinite goto loop in the generated C.");
            }

            // ShatterStmt and SurgeStmt are only illegal at the TOP LEVEL of the
            // destined body, or inside if-branches within the destined body.
            // They are VALID inside nested fore/whirl loops within destined because
            // they break/continue the inner loop, not the destined scope.
            // The CodeGen emits 'break' and 'continue' which are only valid
            // inside loop constructs — at the destined label level, there is no loop.
            if (dynamic_cast<ShatterStmt*>(stmt.get()) ||
                dynamic_cast<SurgeStmt*>(stmt.get())) {
                error(stmt->token,
                      "'shatter' and 'surge' are illegal at the top level of a "
                      "'destined' cleanup block. The destined labels are emitted "
                      "after all loop constructs have closed — 'break' and 'continue' "
                      "would be invalid C at that point. Move cleanup logic before "
                      "the loop, or use a whirl/fore inside destined if iteration is needed.");
            }

            // Recurse ONLY into if-branches. These do not change the loop context,
            // so shatter/surge inside an if-branch inside a destined is still invalid.
            // Do NOT recurse into WhirlStmt or ForeStmt — shatter/surge inside those
            // loops is valid (they target the inner loop, not the destined scope).
            if (auto* ifStmt = dynamic_cast<IfStmt*>(stmt.get())) {
                checkNoJumpStmt(ifStmt->thenBranch.get());
                for (auto& elif : ifStmt->elifBranches) {
                    checkNoJumpStmt(elif.body.get());
                }
                if (ifStmt->elseBranch) {
                    checkNoJumpStmt(ifStmt->elseBranch.get());
                }
            }
            // Nested destined blocks also checked recursively.
            if (auto* nested = dynamic_cast<DestinedStmt*>(stmt.get())) {
                if (nested->body) checkNoJumpStmt(nested->body.get());
            }
        }
    };
    checkNoJumpStmt(node->body.get());

    for (auto& stmt : node->body->statements) {
        stmt->accept(*this);
    }
}

// my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
//     (ctrl, scroll data)                    => { ... }
//     (ctrl, ruin<DiskError::HardwareFault>) => { ... }
//     (ctrl, ruin err)                       => { ... }
// }
//
// This is the most complex semantic rule in the entire language. It handles:
//   1. The spell call must return an Omen type (T | ruin<R>).
//   2. The <~ rebinding must reference a real variable.
//   3. Each branch must be well-typed.
//   4. The block must be exhaustive.
//   5. OWNERSHIP RULE: my_disk is still owned-away INSIDE every branch.
//      The <~ rebinding happens AFTER the block completes, not inside branches.
//
void SemanticAnalyzer::visit(DivineStmt* node) {
    if (isPassOne) return;

    // 1. Evaluate the spell call. It must return a TUPLE.
    //    Then validate the tuple's second element is an Omen.
    //
    //    Trojan Horse Tuple:
    //    Checking TypeKind::TUPLE alone is insufficient. A math spell returning
    //    (sigil* Disk, mark16) also produces a TUPLE, but its second element
    //    is mark16, not an Omen. divine on such a spell would compile here but
    //    crash in CodeGen when it emits __result.__elem1.__is_ruin on an int16_t.
    //
    //    Check tupleElements[1].kind == TypeKind::OMEN explicitly.
    //    This requires tupleElements to be populated — which is why the CallExpr
    //    visitor now fills them in.
    auto returnType = evaluate(node->spellCall.get());

    if (returnType->kind != TypeKind::TUPLE) {
        error(node->token,
              "'divine' requires a spell that returns a TUPLE: (sigil* Owner, T | ruin<R>). "
              "A spell returning a plain Omen (T | ruin<R>) should be unpacked with '?' instead.");
    }

    // Validate the tuple has at least two elements.
    if (returnType->tupleElements.size() < 2) {
        error(node->token,
              "'divine' requires a tuple with at least two elements: "
              "(sigil* Owner, T | ruin<R>). The spell's return tuple has fewer than 2 elements.");
    }

    // Validate the second element (index 1) is an Omen type.
    // This is the Trojan Horse check — catches (sigil* Disk, mark16) being used
    // with divine, which would produce __elem1.__is_ruin on a raw integer.
    if (returnType->tupleElements[1]->kind != TypeKind::OMEN) {
        error(node->token,
              "'divine' requires the second tuple element to be an Omen type (T | ruin<R>). "
              "The spell returns a tuple whose second element is '" +
              returnType->tupleElements[1]->name +
              "', which is not an Omen. Only tuple-returning spells with an Omen "
              "second element can be used with 'divine'.");
    }

    // 2. Find the target variable (e.g., my_disk).
    //    It must exist. It should currently be marked as moved (ownership was just
    //    transferred via the 'own &my_disk' argument in the spell call above).
    Symbol* targetSym = symbols.lookup(node->targetVar.lexeme);
    if (!targetSym) {
        error(node->targetVar, "Undeclared variable '" + node->targetVar.lexeme + "' in divine target.");
    }

    // 3. Analyze each branch.
    bool hasCatchAll = false;
    bool hasSuccess  = false;

    for (auto& branch : node->branches) {
        // Each branch gets its own scope for its local variables.
        symbols.enterScope();

        // Declare the ownership variable (ctrl).
        // IMPORTANT: 'ctrl' is a NEW local alias for the owned pointer INSIDE this branch.
        // The original variable (my_disk) is still moved and inaccessible — only 'ctrl'
        // is accessible inside the branch body.
        // The type of ctrl is the sigil pointer type (same type as my_disk).
        symbols.declare(
            branch.ownerVar.lexeme,
            targetSym->type,
            true,                      // isOwned=true (ctrl owns the pointer in this branch)
            targetSym->currentStance,  // carries the known stance from before the call
            false
        );

        if (branch.isRuin) {
            if (!branch.isSpecificRuin) {
                // Catch-all ruin: (ctrl, ruin err)
                hasCatchAll = true;
                auto errType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, "soul16"});
                symbols.declare(branch.catchAllVar.lexeme, errType);
            } else {
                // Specific ruin: (ctrl, ruin<DiskError::HardwareFault>)
                if (!typeRegistry.count(branch.rankName.lexeme)) {
                    error(branch.rankName,
                          "Unknown rank type '" + branch.rankName.lexeme + "' in divine pattern.");
                }
            }
        } else if (branch.isPayloadless) {
            // Payloadless success branch: (ctrl) => { }
            // Used when the omen success type is 'abyss' — no payload to declare.
            // The body can reference ctrl and outer scope variables only.
            // DO NOT declare any payload variable here — that is the ghost variable trap.
            hasSuccess = true;
        } else {
            // Standard success branch: (ctrl, scroll data) => { ... }
            hasSuccess = true;
            auto successType = resolveType(branch.successType);
            symbols.declare(branch.successVar.lexeme, successType);
        }

        // Analyze the branch body.
        branch.body->accept(*this);

        symbols.exitScope();
    }

    // 4. Exhaustiveness check.
    //    Spec: "Must include exactly one success branch, plus exhaustive ruin coverage
    //    (enumerated variants OR a catch-all (ctrl, ruin err))."
    if (!hasSuccess) {
        error(node->token, "Divine block is missing a success branch.");
    }

    // Exhaustiveness enforcement.
    //
    // If a catch-all is present, coverage is unconditionally complete — done.
    //
    // If NO catch-all: every specific ruin branch names one variant of the rank.
    // We must verify that the set of named variants equals the full variant list.
    // Any missing variant is a hard error — at runtime, that ruin would fall
    // through all branches silently, corrupting program state.
    //
    // MATH:
    //   Let V = set of all variants in the matched rank (from rankVariants).
    //   Let B = set of variants named in specific ruin branches.
    //   Exhaustive iff B == V.
    //   Missing variants = V \ B.  Report each missing variant by name.
    if (!hasCatchAll) {
        // Collect the rank name from specific ruin branches.
        // All specific ruin branches in one divine must match the same rank
        // (the SA earlier validates this via typeRegistry rank lookup).
        std::string matchedRankName;
        std::set<std::string> coveredVariants;

        for (const auto& branch : node->branches) {
            if (branch.isRuin && branch.isSpecificRuin) {
                matchedRankName = branch.rankName.lexeme;
                coveredVariants.insert(branch.variantName.lexeme);
            }
        }

        if (!matchedRankName.empty() && rankVariants.count(matchedRankName)) {
            // We have a known rank — verify full coverage.
            const auto& allVariants = rankVariants.at(matchedRankName);
            std::vector<std::string> missing;

            for (const auto& v : allVariants) {
                if (!coveredVariants.count(v)) {
                    missing.push_back(v);
                }
            }

            if (!missing.empty()) {
                // Build the error message listing every uncovered variant.
                std::string missingList;
                for (size_t i = 0; i < missing.size(); ++i) {
                    missingList += "'" + matchedRankName + "::" + missing[i] + "'";
                    if (i < missing.size() - 1) missingList += ", ";
                }
                error(node->token,
                      "Non-exhaustive divine block. The rank '" + matchedRankName +
                      "' has variants not covered by any branch: " + missingList + ". "
                      "Either add specific branches for each missing variant, "
                      "or add a catch-all branch '(ctrl, ruin err) => { }' "
                      "to handle all remaining variants.");
            }
        } else if (matchedRankName.empty() && !hasCatchAll) {
            // There are ruin branches but none are specific, and no catch-all.
            // This means there are ruin branches with no coverage at all.
            // The hasSuccess check above will have caught the no-success case.
            // This path catches: divine with only a success branch and no ruin coverage.
            // Emit a warning — incomplete ruin handling is risky but not always fatal
            // if the spell cannot actually return a ruin at this call site.
            std::cerr << "[Semantic Warning Line " << node->token.line << "] "
                      << "Divine block has no ruin branches. "
                      << "If the called spell can return a ruin, it will be silently dropped. "
                      << "Add ruin branches or a catch-all '(ctrl, ruin err) => { }'.\n";
        }
    }

    // 5. POST-BLOCK OWNERSHIP REBINDING (the <~ semantics).
    if (targetSym) {
        targetSym->isMoved = false; // my_disk is accessible again
        // Stance becomes "Unknown" because different branches may have left
        // the controller in different stances (Idle from destined, Fault from error).
        // The programmer must perform a stance cast to use it with strict-stance spells.
        targetSym->currentStance = "Unknown";
    }
}

// Handles BOTH assignment statements AND stance transitions.
//
// NORMAL ASSIGNMENT: ctrl->sector_count = 5;
//   Left side is an expression (member access). Right side is evaluated.
//   V1.5 TODO: Full lvalue validation and type compatibility checking.
//
// STANCE TRANSITION: ctrl = Disk:Reading;
//   Left side is an identifier. Right side is a stance reference (IdentifierExpr
//   with stanceName set). The compiler updates the symbol's currentStance.
//   This is the moment the compiler "remembers" the hardware changed state.
//
// STANCE CAST: my_disk = Disk:Idle;
//   Same as above but after a divine block with Unknown stance.
//   Programmer is asserting the hardware is in a known state.
//   The compiler trusts this assertion and updates currentStance.
//
void SemanticAnalyzer::visit(AssignStmt* node) {
    if (isPassOne) return;

    // PLAN B: Lvalue validation — the impenetrable first shield.
    // This fires BEFORE any special-case logic (stance transitions, portline
    // writes, normal assignments) so invalid targets are rejected with a
    // Cgil source line number rather than a GCC C line number.
    //
    // Valid targets: plain identifier, member access (-> or .), array
    // subscript ([]), pointer dereference (*). Everything else is rejected.
    if (!isLvalue(node->target.get())) {
        error(node->target->token,
              "Invalid assignment target. The left side of '=' must be a "
              "variable, member access (->field or .field), array subscript ([i]), "
              "or pointer dereference (*ptr). "
              "Stance references (Disk:Fault), rank variants (DiskError::Timeout), "
              "literals, and expression results cannot be assigned to.");
    }

    // === STANCE TRANSITION INTERCEPTION ===
    // Before anything else, check if the RHS is a stance reference.
    // Stance transitions MUST target a plain named variable.
    // The typestate system tracks stances by variable name — it cannot track
    // what *ptr or **pptr is pointing to at compile time.
    {
        auto* valIdent = dynamic_cast<IdentifierExpr*>(node->value.get());
        if (valIdent && !valIdent->stanceName.lexeme.empty()) {
            // This is a stance transition attempt.
            auto* targetIdent = dynamic_cast<IdentifierExpr*>(node->target.get());
            if (!targetIdent) {
                // Target is not a plain identifier — could be *ptr, **pptr, arr[i], etc.
                error(node->target->token,
                      "Stance transitions require a directly-named variable as the target. "
                      "The Cgil typestate system tracks stances by variable name in the "
                      "symbol table and cannot resolve stance through pointer dereferences, "
                      "array subscripts, or member access expressions at compile time. "
                      "Assign the hardware pointer to a named variable first: "
                      "'sigil* Disk ctrl = *pptr; ctrl = Disk:Active;'");
            }
            // targetIdent is valid — fall through to the existing identifier path.
        }
    }
    
    // Check if the left side is a plain identifier.
    auto* targetIdent = dynamic_cast<IdentifierExpr*>(node->target.get());

    if (targetIdent) {
        Symbol* sym = symbols.lookup(targetIdent->token.lexeme);
        if (!sym) {
            error(targetIdent->token, "Assignment to undeclared variable '" + targetIdent->token.lexeme + "'.");
        }

        // Do NOT call evaluate(node->value.get()) here. 
        // We only inspect the AST shape for stance transitions.
        auto* valIdent = dynamic_cast<IdentifierExpr*>(node->value.get());
        if (valIdent && !valIdent->stanceName.lexeme.empty()) {
            if (sym->type->kind != TypeKind::SIGIL && sym->type->kind != TypeKind::HARDWARE) {
                error(node->token, "Cannot perform stance transition on '" + sym->name + "' — it is not a sigil.");
            }
            if (sym->type->name != valIdent->token.lexeme) {
                error(node->token, "Stance type mismatch: '" + sym->name + "' is '" + sym->type->name + "' but stance belongs to '" + valIdent->token.lexeme + "'.");
            }
            sym->currentStance = valIdent->stanceName.lexeme;
            return; 
        }

        // Complex target: ctrl->field = value, buf[i] = value, *ptr = value
        // The stance interception above guarantees the value is NOT a stance reference
        // if we reach here. Evaluate both sides cleanly without double-evaluation.
        {
            auto targetType = evaluate(node->target.get());
            auto valueType  = evaluate(node->value.get());

            if (targetType && valueType && !isAssignmentCompatible(targetType, valueType)) {
                // Build a helpful error message that names the actual types involved.
                std::string targetName = targetType->name;
                // Strip internal __array_ prefix from error messages.
                if (targetName.rfind("__array_", 0) == 0) {
                    error(node->target->token,
                        "Array-to-array assignment is not supported. "
                        "Copy elements individually with a fore loop.");
                }
                error(node->token,
                    "Type mismatch in assignment: cannot assign '" +
                    (valueType->name) + "' to '" + targetName + "'. "
                    "Use 'cast<" + targetName + ">(expr)' for explicit numeric conversion.");
            }
        }
        return;
    }

    // Left side is not a plain identifier (e.g., ctrl->field = value).
    auto targetType = evaluate(node->target.get()); // Validates lvalue
    auto valueType  = evaluate(node->value.get());  // Validates rvalue

    // --- Strict Assignment Check ---
    if (!isAssignmentCompatible(targetType, valueType)) {
        error(node->token,
              "Type mismatch: cannot implicitly assign '" +
              (valueType ? valueType->name : "?") + "' to '" +
              (targetType ? targetType->name : "?") + "'.");
    }
}

// =============================================================================
// PLAN A: warnIfFlow() — Centralized FPU Type Warning
// =============================================================================
//
// Every site that processes a type token calls this helper.
// It is a single enforcement point for the FPU safety rule:
//
//   "flow (32-bit float) maps to the x86 FPU register set. Using float in
//    bare-metal kernel code without explicitly saving and restoring FPU state
//    (FXSAVE/FXRSTOR or similar) causes the interrupted process's FPU state
//    to be silently corrupted. In an ISR (warden spell), this is guaranteed
//    to cause a kernel panic or data corruption."
//
// Context-aware severity:
//   warden spell  → HARD ERROR  (ISR + FPU = definite panic)
//   normal context → WARNING    (risky but not always fatal)
//
// NULL SAFETY:
//   SigilDecl fields are processed at global scope where currentSpell == nullptr.
//   We MUST check currentSpell != nullptr before accessing isWarden.
void SemanticAnalyzer::warnIfFlow(Token typeToken) {
    if (typeToken.lexeme != "flow") return;

    if (currentSpell != nullptr && currentSpell->isWarden) {
        // Hard error inside an ISR — this will panic the kernel.
        error(typeToken,
              "Cannot use 'flow' (float) in a 'warden spell'. "
              "ISRs run with the x86 FPU in an undefined state. "
              "Using float registers here will corrupt the interrupted context "
              "and cause a kernel panic. Use integer math only in ISRs.");
    } else {
        // Warning in normal kernel context.
        std::cerr << "[Semantic Warning Line " << typeToken.line << " Col " << typeToken.column << "] "
                  << "Type 'flow' (float) used in kernel context. "
                  << "FPU register usage without explicit FXSAVE/FXRSTOR "
                  << "will corrupt interrupted task state. "
                  << "Ensure FPU context switching is in place before using float.\n";
    }
}

// =============================================================================
// PLAN B: isLvalue() — Stateless Lvalue Validation
// =============================================================================
//
// Structurally determines if an expression is a valid assignment target.
// This function is STATELESS — it does not read typeRegistry, symbols, or
// currentExprType. It operates purely on the AST node shape.
//
// RATIFIED WHITELIST:
//
//   IdentifierExpr: valid ONLY if stanceName AND variantName are both empty.
//     → Disk:Fault is NOT an lvalue (it is a stance constant, not a variable).
//     → DiskError::Timeout is NOT an lvalue (it is a rank constant).
//     → my_disk IS an lvalue (plain variable reference).
//
//   BinaryExpr(ARROW or DOT): member access is an lvalue.
//     → ctrl->sector_count = 5   valid
//     → pkt.length = 64          valid
//
//   IndexExpr: array subscript is an lvalue.
//     → buf[i] = val             valid
//
//   UnaryExpr(STAR): pointer dereference is an lvalue.
//     → *ptr = 5                 valid
//
//   EVERYTHING ELSE: not an lvalue.
//     LiteralExpr, CallExpr, PostfixExpr(?), AddressOfExpr(&x),
//     BinaryExpr with arithmetic/comparison operators.
bool SemanticAnalyzer::isLvalue(Expr* expr) const {
    if (auto* ident = dynamic_cast<IdentifierExpr*>(expr)) {
        // Plain variable: valid lvalue.
        // Stance reference (Disk:Fault) or rank variant (DiskError::Timeout):
        // NOT a valid lvalue — they are compile-time constants.
        return ident->stanceName.lexeme.empty() && ident->variantName.lexeme.empty();
    }

    if (auto* binary = dynamic_cast<BinaryExpr*>(expr)) {
        // Member access via -> or . is a valid lvalue.
        return binary->op.type == TokenType::ARROW || binary->op.type == TokenType::DOT;
    }

    if (dynamic_cast<IndexExpr*>(expr)) {
        // Array subscript is a valid lvalue.
        return true;
    }

    if (auto* unary = dynamic_cast<UnaryExpr*>(expr)) {
        // Pointer dereference (*ptr) is a valid lvalue.
        // Negation (-x) and other unary ops are NOT.
        return unary->op.type == TokenType::STAR;
    }

    // Everything else: LiteralExpr, CallExpr, PostfixExpr, AddressOfExpr,
    // BinaryExpr with non-member operators — all invalid lvalues.
    return false;
}

void SemanticAnalyzer::visit(VarDeclStmt* node) {
    if (isPassOne) return;

    // Reserved Compiler Internals
    if (node->name.lexeme.rfind("__", 0) == 0) {
        error(node->name, "Variable names beginning with '__' are reserved for Cgil compiler internals.");
    }

    auto varType = resolveType(node->typeToken);

    // PLAN A: Centralized FPU warning — replaces the old inline check.
    // warnIfFlow() handles context (warden = error, normal = warning) safely.
    warnIfFlow(node->typeToken);

    std::string initialStance = "";

    if (node->initializer) {
        auto initType = evaluate(node->initializer.get());
        
        // If initialized with a stance-prefixed struct, remember the stance!
        if (auto* structInit = dynamic_cast<StructInitExpr*>(node->initializer.get())) {
            initialStance = structInit->stanceName.lexeme;
        }
        // If initialized with a pointer to a variable (&dev), inherit its stance!
        // Without this, local pointers to hardware drop the typestate lock.
        else if (auto* addrOf = dynamic_cast<AddressOfExpr*>(node->initializer.get())) {
            if (auto* targetIdent = dynamic_cast<IdentifierExpr*>(addrOf->operand.get())) {
                if (Symbol* sym = symbols.lookup(targetIdent->token.lexeme)) {
                    initialStance = sym->currentStance;
                }
            }
        }
    }

    // Array Element Type Propagation
    if (node->isArray) {
        auto arrayType = std::make_shared<TypeInfo>(
            TypeInfo{TypeKind::PRIMITIVE, "__array_" + varType->name}
        );
        arrayType->elementType = varType;
        symbols.declare(node->name.lexeme, arrayType, false, initialStance, false);
    } else {
        symbols.declare(node->name.lexeme, varType, false, initialStance, false);
    }
}

// =============================================================================
// EXPRESSION VISITORS
// =============================================================================

// Binary infix operations: a + b, a == b, ctrl->stance, a ~> b, etc.
//
// For member access (-> and .): left side must be a sigil type,
//                               right side is a field name identifier.
//   Result type = the field's type (looked up from the sigil's fields).
//   For ->stance access: returns soul16 (the __stance discriminant).
//   V1: We return the left side's type as a simplified approximation.
//
// For pipeline (~>): left output type feeds into right as first arg.
//   Result type = the right spell's return type.
//   V1: We evaluate both sides and return the right side's type.
//
// For arithmetic and comparison: V1 returns a primitive type.
void SemanticAnalyzer::visit(BinaryExpr* node) {
    if (isPassOne) return;

    auto leftType = evaluate(node->left.get());
    std::shared_ptr<TypeInfo> rightType = nullptr;

    // Do not evaluate the right side if this is member access!
    // For 'a->b' or 'a.b', 'b' is a field name, not a local variable.
    if (node->op.type != TokenType::ARROW && node->op.type != TokenType::DOT) {
        rightType = evaluate(node->right.get());
    }

    switch (node->op.type) {
        case TokenType::ARROW:
        case TokenType::DOT: {
            // Prevent -> access on legion SoA value elements
            if (node->op.type == TokenType::ARROW && leftType && leftType->kind == TypeKind::LEGION) {
                error(node->op, "Cannot use '->' on a legion element. Legion array elements are values, not pointers. Use '.' instead.");
            }
            // Member access: ctrl->sector_count, pkt.length
            //
            // SPECIAL CASE: ->stance / .stance
            //   The ->stance accessor is a reserved read of the __stance field.
            //   Its type is always soul16 (the discriminant type).
            //
            // FALLBACK: If the field is not found in the TypeInfo field map
            //   (e.g., the sigil had no Pass 2 processing, or this is a hardware
            //   type with no fields), fall back to leftType. This maintains
            //   V1 behavior for unknown fields rather than crashing.
            auto* rightIdent = dynamic_cast<IdentifierExpr*>(node->right.get());

            if (rightIdent && rightIdent->token.lexeme == "stance") {
                // ->stance / .stance: always soul16 (the __stance discriminant)
                currentExprType = typeRegistry.count("soul16")
                    ? getBuiltinType("soul16")
                    : leftType;
            } else if (rightIdent && leftType && leftType->fields.count(rightIdent->token.lexeme)) {
                // Actually extract and return the specific field's type!
                currentExprType = leftType->fields[rightIdent->token.lexeme];
            } else if (leftType && leftType->kind == TypeKind::PRIMITIVE && !leftType->fields.empty()) {
                if (rightIdent) {
                    error(node->token,
                          "Unknown field '" + rightIdent->token.lexeme +
                          "' on type '" + leftType->name + "'. "
                          "Valid fields: " + [&]() {
                              std::string list;
                              for (const auto& kv : leftType->fields)
                                  list += "'" + kv.first + "' ";
                              return list;
                          }());
                } else {
                    error(node->token,
                          "Member access on primitive type '" + leftType->name +
                          "' requires a field name identifier.");
                }
            } else {
                // Fallback (undeclared fields default to container type to avoid crash)
                currentExprType = leftType;
            }
            
            break;
        }

        case TokenType::WEAVE:
            // ~> pipeline: left output feeds into right as first arg.
            // The result type is whatever the right-hand call returns.
            // Since evaluate(right) already set currentExprType, it's correct.
            currentExprType = rightType;
            break;

        case TokenType::EQ:
        case TokenType::NEQ:
        case TokenType::GT:
        case TokenType::LT:
        case TokenType::GEQ:
        case TokenType::LEQ:
        case TokenType::AMPAMP:
        case TokenType::PIPEPIPE:
            // Comparisons and logicals produce oath (boolean).
            currentExprType = getBuiltinType("oath");
            break;

        case TokenType::PLUS:
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT: {
            // ENFORCEMENT: Arithmetic is only valid for numeric primitives (mark, soul, addr, flow, rune).
            // It is strictly illegal for sigils, legions, ranks, and scrolls.
            bool leftValid = (leftType && leftType->kind == TypeKind::PRIMITIVE && leftType->name != "scroll" && leftType->name != "abyss");
            bool rightValid = (rightType && rightType->kind == TypeKind::PRIMITIVE && rightType->name != "scroll" && rightType->name != "abyss");

            if (!leftValid || !rightValid) {
                error(node->op, 
                      "Arithmetic operator '" + node->op.lexeme + "' is only valid for numeric types. " +
                      "Cannot perform math on '" + (leftType ? leftType->name : "?") + 
                      "' and '" + (rightType ? rightType->name : "?") + "'.");
            }
            
            // Result type follows the left operand's type.
            currentExprType = leftType;
            break;
        }

        case TokenType::PIPE:
            // | in expression context = Omen union (T | ruin<R>).
            // V1: This appears mainly in type annotations, not as a runtime
            // expression. We return an OMEN type.
            currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
            break;

        default:
            // Unknown binary operator — pass through left type.
            currentExprType = leftType;
            break;
    }
}

// Prefix unary operations: -x (negation), *ptr (pointer dereference).
void SemanticAnalyzer::visit(UnaryExpr* node) {
    if (isPassOne) return;

    auto operandType = evaluate(node->operand.get());

    if (node->op.type == TokenType::STAR) {
        if (operandType && operandType->elementType) {
            currentExprType = operandType->elementType;
        } else {
            // Fallback if the AST node wasn't explicitly wrapped (e.g. from a raw parameter)
            currentExprType = operandType; 
        }
    } else {
        currentExprType = operandType;
    }
}

// Postfix '?' — the Omen unpack operator.
// e.g., read_buffer()?
//
// SEMANTICS: The operand must be an Omen type (T | ruin<R>).
//   On success: unwraps and produces the success value T.
//   On ruin: immediately yields the ruin up the call chain.
//
// For the semantic analyzer, we:
//   1. Evaluate the operand and confirm it's an Omen.
//   2. Set currentExprType to the Omen's successType (the unwrapped value).
void SemanticAnalyzer::visit(PostfixExpr* node) {
    if (isPassOne) return;

    auto operandType = evaluate(node->operand.get());

    if (operandType->kind != TypeKind::OMEN) {
        error(node->op,
              "'?' can only be applied to an Omen type (T | ruin<R>). "
              "The expression before '?' does not return an Omen.");
    }

    // WARDEN CONSTRAINT: warden spells cannot propagate ruin.
    // If ? is used inside a warden spell, it would propagate a ruin to hardware
    // context, which is forbidden.
    if (currentSpell && currentSpell->isWarden) {
        error(node->op,
              "'warden spell' cannot use the '?' unpack operator. "
              "ISRs cannot propagate errors up to the hardware; they must handle ruins internally.");
    }

    // OMEN PROPAGATION CONSTRAINT:
    // The '?' operator returns a ruin to the caller. Therefore, the enclosing
    // spell MUST have an Omen in its return type. You cannot propagate an error
    // out of an 'abyss' spell or a pure primitive-returning spell.
    if (currentSpell && !currentSpell->hasOmen) {
        error(node->op,
              "The '?' operator propagates ruins, but the enclosing spell '" +
              currentSpell->name.lexeme + "' does not return an Omen. "
              "You can only use '?' inside a spell that returns 'T | ruin<E>'.");
    }

    // PATCH 3: Bind the ownership parameter by finding the FIRST sigil pointer
    // parameter declared with 'own' in the enclosing spell's parameter list.
    // This is the semantically correct variable to carry in the early-return tuple —
    // it is the parameter that holds hardware ownership and must be returned
    // to the caller even on the error path.
    //
    // We search parameters in declaration order and take the first isPointer param.
    // The spec requires exactly one owned sigil* in tuple-returning spells.
    if (currentSpell && currentSpell->returnTypes.size() > 1) {
        for (const auto& param : currentSpell->params) {
            if (param.isPointer) {
                node->ownershipParamName = param.name.lexeme;
                break;
            }
        }
        if (node->ownershipParamName.empty()) {
            // Defensive: should not happen if SA validated the tuple spell correctly.
            error(node->op,
                  "Internal: tuple-returning spell has no sigil* parameter to "
                  "carry in the early-return slot. This is a compiler bug.");
        }
    }

    // RESOLVEDOMENTYPE PATCH:
    // Store the full Omen TypeInfo on the AST node so CodeGen can:
    //   a) Emit the correct concrete typedef name for _tmp (not __auto_type fallback)
    //   b) Detect abyss Omens where __value does not exist in the union
    //   c) Emit the correct early-return path in the destined goto chain
    node->resolvedOmenType = operandType;

    // Unwrap: the ? operator produces the SUCCESS type of the Omen.
    if (operandType->successType) {
        currentExprType = operandType->successType;
    } else {
        // Omen with no successType populated — produce a generic primitive.
        // This happens when the spell's return type was not fully resolved.
        // V1.5 TODO: Ensure all Omen types are fully populated with successType.
        currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, "soul16"});
    }
}

// An integer literal, string literal, or boolean literal.
// The type is determined by the token type.
//
//   INT_LIT    -> mark16 (default integer per spec; widened at use site if needed)
//   STRING_LIT -> scroll (fat pointer; len calculated by CodeGen)
//   KEPT       -> oath (true)
//   FORSAKEN   -> oath (false)
void SemanticAnalyzer::visit(LiteralExpr* node) {
    if (isPassOne) return;

    switch (node->token.type) {
        case TokenType::INT_LIT:
            // Integer literals default to mark16 per spec.
            // The CodeGen and type-checking will widen or cast as needed.
            currentExprType = getBuiltinType("mark16");
            break;

        case TokenType::FLOAT_LIT:
            // Floating point literals map to the 'flow' FPU type
            currentExprType = getBuiltinType("flow");
            break;

        case TokenType::STRING_LIT:
            // String literals are Cgil_Scroll values.
            currentExprType = getBuiltinType("scroll");
            break;

        case TokenType::KEPT:
        case TokenType::FORSAKEN:
            // Boolean literals.
            currentExprType = getBuiltinType("oath");
            break;

        default:
            // Fallback for any other literal-like token.
            currentExprType = getBuiltinType("mark16");
            break;
    }
}

// A reference to a named variable, stance, or rank variant.
//
// Three cases based on what stanceName and variantName contain:
//
//   PLAIN IDENTIFIER (stanceName empty, variantName empty):
//     Normal variable lookup. Check it exists. Check it's not moved.
//     Return its type.
//
//   STANCE REFERENCE (stanceName set): e.g., Disk:Fault
//     The sigil type name must exist. Returns a special __STANCE_REF__ pseudo-type
//     so AssignStmt knows to treat this as a stance transition.
//
//   RANK VARIANT REFERENCE (variantName set): e.g., DiskError::Timeout
//     The rank type must exist. Returns a soul16 (the discriminant value).
void SemanticAnalyzer::visit(IdentifierExpr* node) {
    if (isPassOne) return;

    // CASE 1: Stance reference (Disk:Fault)
    if (!node->stanceName.lexeme.empty()) {
        auto it = typeRegistry.find(node->token.lexeme);
        if (it == typeRegistry.end() || it->second->kind != TypeKind::SIGIL) {
            error(node->token,
                  "'" + node->token.lexeme + "' is not a known sigil type. "
                  "Stance references must use a declared sigil name.");
        }
        // Return a sentinel type so AssignStmt can recognize a stance transition.
        // The sentinel name encodes which sigil and which stance.
        auto stanceRefType = std::make_shared<TypeInfo>(TypeInfo{
            TypeKind::PRIMITIVE,
            "__STANCE_REF__:" + node->token.lexeme + ":" + node->stanceName.lexeme
        });
        currentExprType = stanceRefType;
        return;
    }

    // CASE 2: Rank variant reference (DiskError::Timeout)
    if (!node->variantName.lexeme.empty()) {
        auto it = typeRegistry.find(node->token.lexeme);
        if (it == typeRegistry.end() || it->second->kind != TypeKind::RANK) {
            error(node->token,
                  "'" + node->token.lexeme + "' is not a known rank type. "
                  "Variant access (::) requires a declared rank name.");
        }
        
        // Validate node->variantName.lexeme is actually in the rank's variants
        const auto& variants = rankVariants[node->token.lexeme];
        bool variantExists = false;
        for (const auto& v : variants) {
            if (v == node->variantName.lexeme) {
                variantExists = true;
                break;
            }
        }
        
        if (!variantExists) {
            error(node->variantName, 
                  "Rank '" + node->token.lexeme + "' has no variant named '" + node->variantName.lexeme + "'.");
        }

        currentExprType = getBuiltinType("soul16");
        return;
    }

    // CASE 3: Plain variable lookup
    Symbol* sym = symbols.lookup(node->token.lexeme);
    if (!sym) {
        error(node->token, "Undeclared identifier '" + node->token.lexeme + "'.");
    }

    // --- THE OWNERSHIP SAFETY LOCK ---
    // If this variable had ownership transferred away (passed with 'own'),
    // using it again is a compile error. The programmer must wait for
    // <~ to rebind it after a divine block.
    // Enforce the move lock regardless of how the variable was declared!
    if (sym->isMoved) {
        error(node->token,
              "Use-after-move: '" + sym->name + "' has been transferred with 'own' "
              "and cannot be used until ownership is rebound via '<~'.");
    }

    currentExprType = sym->type;
}

// A function call: fetch_sector(own &my_disk, 0x0500)
//                 ruin(DiskError::HardwareFault)
//
// Checks:
//   1. The callee is a known spell.
//   2. The argument count matches the parameter count.
//   3. For sigil* parameters with stance constraints: the argument's current
//      stance matches the required stance (THE TYPESTATE LOCK).
//   4. For 'own' parameters: the 'own' keyword was present at the call site.
//      The argument variable is then marked as moved (isMoved = true).
//   5. For warden spells: own and ruin propagation are forbidden.
//
void SemanticAnalyzer::visit(CallExpr* node) {
    if (isPassOne) return;

    auto* calleeIdent = dynamic_cast<IdentifierExpr*>(node->callee.get());
    if (!calleeIdent) {
        error(node->token, "Only named spells can be called in V1.");
    }

    const std::string& calleeName = calleeIdent->token.lexeme;

    // SPECIAL CASE: ruin(DiskError::HardwareFault)
    // This is not a spell call — it's an Omen error construction expression.
    // It takes one argument (a rank variant) and returns an Omen type.
    // It appears in: yield (ctrl, ruin(DiskError::HardwareFault));
    if (calleeName == "ruin") {
        if (node->args.size() != 1) {
            error(node->token, "'ruin(...)' takes exactly one rank variant argument.");
        }
        // Evaluate the argument (the rank variant expression).
        evaluate(node->args[0].get());
        // Produces an Omen type.
        currentExprType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
        return;
    }

    // Normal spell lookup.
    auto spellIt = spellRegistry.find(calleeName);
    if (spellIt == spellRegistry.end()) {
        error(node->token, "Call to unknown spell '" + calleeName + "'.");
    }
    SpellDecl* spell = spellIt->second;

    // WARDEN CONSTRAINT: warden spells cannot use 'own' (per spec).
    if (currentSpell && currentSpell->isWarden) {
        for (size_t i = 0; i < node->argIsOwned.size(); i++) {
            if (node->argIsOwned[i]) {
                error(node->args[i]->token,
                      "'warden spell' cannot use 'own' — ISRs cannot transfer hardware ownership.");
            }
        }
    }

    // Arity check.
    if (node->args.size() != spell->params.size()) {
        error(node->token,
              "Argument count mismatch. Spell '" + calleeName + "' expects " +
              std::to_string(spell->params.size()) + " arguments, got " +
              std::to_string(node->args.size()) + ".");
    }

    // Per-argument checks.
    for (size_t i = 0; i < node->args.size(); i++) {
        auto argType  = evaluate(node->args[i].get());
        Param& param  = spell->params[i];

        if (param.isPointer) {
            // Both Stance constraints AND Ownership constraints require us to 
            // look at the actual variable being passed, so we must unwrap it.
            if (!param.stanceName.lexeme.empty() || param.isOwned) {
                
                auto* argIdent = dynamic_cast<IdentifierExpr*>(node->args[i].get());
                auto* addrOf = dynamic_cast<AddressOfExpr*>(node->args[i].get());
                
                // If it's &my_buf, look inside to grab the 'my_buf' identifier
                if (addrOf) {
                    argIdent = dynamic_cast<IdentifierExpr*>(addrOf->operand.get());
                }

                if (!argIdent) {
                    error(node->args[i]->token,
                          "Parameter '" + param.name.lexeme + "' requires a plain variable "
                          "(not a complex expression) so its state and ownership can be tracked.");
                }

                Symbol* argSym = symbols.lookup(argIdent->token.lexeme);
                if (!argSym) {
                    error(node->args[i]->token, "Cannot resolve argument variable for typestate tracking.");
                }

                // --- THE TYPESTATE LOCK ---
                if (!param.stanceName.lexeme.empty()) {
                    if (argSym->currentStance != param.stanceName.lexeme) {
                        error(node->args[i]->token,
                              "Stance mismatch: spell '" + calleeName + "' requires '" +
                              param.stanceName.lexeme + "' but '" + argSym->name +
                              "' is currently '" + argSym->currentStance + "'.");
                    }
                }

                // --- THE OWNERSHIP LOCK ---
                if (param.isOwned) {
                    if (!node->argIsOwned[i]) {
                        error(node->args[i]->token,
                              "Parameter '" + param.name.lexeme + "' requires 'own'. "
                              "Write: " + calleeName + "(own &" + argSym->name + ") to acknowledge the transfer.");
                    }
                    // Transfer ownership. The variable is now inaccessible until <~ rebinds it.
                    argSym->isMoved = true;
                }
            }
        }
    }

    // Determine the return type:
    //
    //   TUPLE return (multiple elements):
    //     Produces TypeKind::TUPLE with tupleElements populated.
    //     tupleElements[0] = resolved type of first return element
    //     tupleElements[1] = resolved type of second return element (usually OMEN)
    //     Downstream passes (DivineStmt) NEED these elements to be populated.
    //     An empty tupleElements is type erasure — the validator is blinded.
    //
    //   OMEN return (single value with ruin suffix):
    //     Produces TypeKind::OMEN. Used by '?' postfix unpack.
    //
    //   Single primitive return:
    //     Produces the resolved primitive TypeInfo.
    //
    //   abyss return (no values):
    //     Produces the abyss TypeInfo.
    if (spell->returnTypes.size() > 1) {
        // Multi-element tuple return.
        auto tupleType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::TUPLE, "Tuple"});

        for (const auto& rt : spell->returnTypes) {
            auto elemIt = typeRegistry.find(rt.typeToken.lexeme);
            if (elemIt != typeRegistry.end()) {
                tupleType->tupleElements.push_back(elemIt->second);
            } else {
                tupleType->tupleElements.push_back(
                    std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, rt.typeToken.lexeme})
                );
            }
        }

        if (spell->hasOmen && !tupleType->tupleElements.empty()) {
            auto omenType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
            // Propagate the success type into the Omen!
            omenType->successType = tupleType->tupleElements.back();
            tupleType->tupleElements.back() = omenType;
        }

        currentExprType = tupleType;

    } else if (spell->hasOmen) {
        // Single-element Omen: scroll | ruin<E>
        auto omenType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::OMEN, "Omen"});
        
        // Extract actual success type to prevent soul16/auto fallback
        if (!spell->returnTypes.empty()) {
            auto retIt = typeRegistry.find(spell->returnTypes[0].typeToken.lexeme);
            if (retIt != typeRegistry.end()) {
                omenType->successType = retIt->second;
            } else {
                omenType->successType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, spell->returnTypes[0].typeToken.lexeme});
            }
        }
        currentExprType = omenType;

    } else if (!spell->returnTypes.empty()) {
        auto retIt = typeRegistry.find(spell->returnTypes[0].typeToken.lexeme);
        currentExprType = (retIt != typeRegistry.end()) ? retIt->second : getBuiltinType("abyss");
    } else {
        currentExprType = getBuiltinType("abyss");
    }
}

// The address-of operator: &my_disk, &disk_data_port
//
// TWO COMPLETELY DIFFERENT SEMANTICS depending on what the operand is:
//
//   &regular_variable -> C address-of. Returns a pointer to the variable.
//                        Type: a pointer to the variable's type.
//
//   &hardware_variable -> Compile-time address constant. Returns the physical
//                         address of the leyline/portline as an 'addr' value.
//                         e.g., &disk_data_port evaluates to 0x1F0 as addr.
//                         This is NOT a pointer dereference — it is a constant.
//                         CodeGen emits: (uint16_t)0x1F0
//
// The isHardware flag on the Symbol (set by HardwareDecl) is what enables
// this distinction.
void SemanticAnalyzer::visit(AddressOfExpr* node) {
    if (isPassOne) return;

    // Evaluate the operand to resolve what it refers to.
    auto operandType = evaluate(node->operand.get());

    // Check if the operand is an identifier referring to a hardware variable.
    auto* ident = dynamic_cast<IdentifierExpr*>(node->operand.get());
    if (ident) {
        Symbol* sym = symbols.lookup(ident->token.lexeme);
        if (sym && sym->isHardware) {
            currentExprType = getBuiltinType("addr");
            return;
        }
        // Ownership Escape Prevention
        if (sym && sym->isOwned) {
            error(node->token, "Cannot take the address of an 'own' pointer — this creates an alias that bypasses ownership typestate tracking.");
        }
    }

    // Regular address-of: &my_disk -> pointer to the variable.
    // Wrap the operand's type in a pointer TypeInfo
    auto ptrType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, operandType->name + "*"});
    ptrType->elementType = operandType; // Store the pointee type for unwrapping
    currentExprType = ptrType;
}

// Array subscript: target[index]
// Validates:
//   1. The target is an array-like type (deck variable, leyline of array type).
//   2. The index is an integer-compatible type.
//   3. Returns the element type of the array.
void SemanticAnalyzer::visit(IndexExpr* node) {
    if (isPassOne) return;

    auto targetType = evaluate(node->target.get());
    auto indexType  = evaluate(node->index.get());
    (void)indexType;

    // Return the actual element type so field access works
    if (targetType && targetType->elementType) {
        currentExprType = targetType->elementType;
        return;
    }

    // Fallback for unknown arrays
    currentExprType = typeRegistry.count("mark16")
        ? getBuiltinType("mark16")
        : std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, "mark16"});
}
// Struct/sigil initializer: Device:Idle { device_id: 0, error_code: 0, flags: 0 }
//
// Validates:
//   1. The type name refers to a declared sigil or legion.
//   2. If a stance prefix is present, the stance belongs to that sigil.
//   3. All field names exist on the sigil.
//   4. All field values evaluate without error.
//
// Produces: the sigil's TypeInfo as the expression type.
void SemanticAnalyzer::visit(StructInitExpr* node) {
    if (isPassOne) return;

    auto typeIt = typeRegistry.find(node->typeName.lexeme);
    if (typeIt == typeRegistry.end()) {
        error(node->typeName,
              "Unknown type '" + node->typeName.lexeme + "' in struct initializer. "
              "Only declared sigil and legion types can be initialized with { }.");
    }

    auto sigilType = typeIt->second;

    if (sigilType->kind != TypeKind::SIGIL && sigilType->kind != TypeKind::LEGION) {
        error(node->typeName,
              "'" + node->typeName.lexeme + "' is not a sigil or legion type. "
              "Struct initializer { } syntax is only valid for sigil and legion types.");
    }

    // Evaluate all field initializer expressions and validate them against the struct definition.
    for (auto& field : node->fields) {
        auto valueType = evaluate(field.value.get());
        
        // Validate the field actually exists in the sigil/legion
        if (sigilType->fields.find(field.name.lexeme) == sigilType->fields.end()) {
            error(field.name, "Struct '" + sigilType->name + "' has no field named '" + field.name.lexeme + "'.");
        } else {
            // Enforce strict assignment compatibility for the field value
            auto expectedType = sigilType->fields[field.name.lexeme];
            if (!isAssignmentCompatible(expectedType, valueType)) {
                error(field.value->token, 
                      "Type mismatch in struct initialization: cannot assign '" + 
                      (valueType ? valueType->name : "?") + "' to field '" + field.name.lexeme + 
                      "' of type '" + expectedType->name + "'.");
            }
        }
    }

    currentExprType = sigilType;
}

void SemanticAnalyzer::visit(AssignExpr* node) {
    if (isPassOne) return;

    // PLAN B: Lvalue validation — same shield as AssignStmt.
    // Fore loop increments like 'i = i + 1' always have a valid lvalue (IdentifierExpr).
    // Array increments like 'my_array[i] = val + 1' have IndexExpr — also valid.
    // Literal increments like '5 = 5 + 1' are caught and rejected here with
    // a Cgil source location, not a GCC error pointing at generated C.
    if (!isLvalue(node->target.get())) {
        error(node->target->token,
              "Invalid assignment target. The left side of '=' must be a "
              "variable, member access (->field or .field), array subscript ([i]), "
              "or pointer dereference (*ptr).");
    }

    auto targetType = evaluate(node->target.get());
    auto valueType  = evaluate(node->value.get());
    
    // --- Strict Assignment Check ---
    if (!isAssignmentCompatible(targetType, valueType)) {
        error(node->op,
              "Type mismatch in expression: cannot implicitly assign '" +
              (valueType ? valueType->name : "?") + "' to '" +
              (targetType ? targetType->name : "?") + "'.");
    }
    
    currentExprType = targetType;
}

void SemanticAnalyzer::visit(CastExpr* node) {
    if (isPassOne) return;

    auto operandType = evaluate(node->operand.get());
    auto targetType  = resolveType(node->targetType);

    if (targetType->name == "abyss") {
        error(node->targetType, "Cannot cast to 'abyss'. Use 'yield;' for void returns.");
    }
    if (operandType && operandType->kind == TypeKind::SIGIL) {
        error(node->token, "Cannot cast sigil type '" + operandType->name + "'. Access fields directly.");
    }
    // Allow casting to sigil* (pointers), but not to raw sigil values
    if (targetType->kind == TypeKind::SIGIL && !node->isPointer) {
        error(node->targetType, "Cannot cast to sigil type '" + targetType->name + "'.");
    }

    // Wrap the type if casting to a pointer
    if (node->isPointer) {
        auto ptrType = std::make_shared<TypeInfo>(TypeInfo{TypeKind::PRIMITIVE, targetType->name + "*"});
        ptrType->elementType = targetType;
        currentExprType = ptrType;
    } else {
        currentExprType = targetType;
    }
}

void SemanticAnalyzer::visit(UpdateExpr* node) {
    if (isPassOne) return;

    // You cannot do ++5 or (a + b)++. The operand must be a valid memory location.
    if (!isLvalue(node->operand.get())) {
        error(node->token, "Invalid operand for '" + node->op.lexeme + "'. Must be a variable, array element, or member field.");
    }

    // Return the type of the operand (e.g., mark16)
    currentExprType = evaluate(node->operand.get());
}