#include "../../include/Parser/Parser.h"
#include <iostream>

// =============================================================================
// CONSTRUCTOR AND MAIN ENTRY POINT
// =============================================================================

Parser::Parser(const std::vector<Token>& tokens) : tokens(tokens) {}

// parse() drives the top-level loop. It calls parseDeclaration() for each
// top-level construct in the file. If parseDeclaration() throws (a parse error),
// the error is printed and synchronize() skips to the next safe recovery point
// so parsing can continue. This means one run can report multiple errors.
std::vector<std::unique_ptr<Decl>> Parser::parse() {
    std::vector<std::unique_ptr<Decl>> declarations;

    while (!isAtEnd()) {
        try {
            declarations.push_back(parseDeclaration());
        } catch (const std::runtime_error& e) {
            std::cerr << e.what() << "\n";
            synchronize();
        }
    }

    return declarations;
}

// =============================================================================
// DECLARATION PARSERS
// =============================================================================

// Routes to the right declaration parser based on the keyword we see.
std::unique_ptr<Decl> Parser::parseDeclaration() {
    if (match({TokenType::GRIMOIRE, TokenType::PACT}))
        return parseGrimoireDecl();
    if (match({TokenType::RANK}))
        return parseRankDecl();
    if (match({TokenType::SIGIL}))
        return parseSigilDecl();
    if (match({TokenType::LEGION}))
        return parseLegionDecl();
    if (match({TokenType::LEYLINE, TokenType::PORTLINE}))
        return parseHardwareDecl();
    if (match({TokenType::SPELL, TokenType::WARDEN, TokenType::CONJURE}))
        return parseSpellDecl(true);

    error(peek(), "Expected a top-level declaration. Valid keywords: "
                  "spell, warden, conjure, sigil, rank, legion, "
                  "leyline, portline, grimoire, pact.");
}

// grimoire <hardware_defs.h>;
// pact <stdlib.h>;
//
// The previous() token on entry is GRIMOIRE or PACT (consumed by match()).
// isPact distinguishes them for the semantic analyzer (pact = Ring 3 warning).
std::unique_ptr<GrimoireDecl> Parser::parseGrimoireDecl() {
    auto node = std::make_unique<GrimoireDecl>();
    node->token  = previous(); // grimoire or pact token
    node->isPact = (previous().type == TokenType::PACT);

    if (check(TokenType::STRING_LIT)) {
        // LOCAL INCLUDE: grimoire "local_file.h";
        node->isSystem = false;
        node->path = advance(); // Consume the string literal token
    } else {
        // SYSTEM INCLUDE: grimoire <system_file.h>;
        node->isSystem = true;
        consume(TokenType::LT, "Expected '<' or string literal after grimoire/pact.");

        std::string pathStr = "";
        while (!check(TokenType::GT) && !isAtEnd()) {
            pathStr += advance().lexeme;
        }

        if (pathStr.empty()) {
            throw std::runtime_error("[Parse Error] Expected header file name between < >");
        }

        Token pathToken = previous(); 
        pathToken.lexeme = pathStr;   
        node->path = pathToken;

        consume(TokenType::GT, "Expected '>' after header name.");
    }

    consume(TokenType::SEMICOLON, "Expected ';' after grimoire/pact declaration.");
    return node;
}

// rank DiskError { Timeout, HardwareFault, InvalidSector }
//
// Variants are stored in declaration order. The semantic analyzer assigns
// sequential uint16_t discriminants: Timeout=0, HardwareFault=1, etc.
// The CodeGen emits typedef + #define constants for each variant.
std::unique_ptr<RankDecl> Parser::parseRankDecl() {
    auto node = std::make_unique<RankDecl>();
    node->token = previous(); // 'rank' token

    node->name = consume(TokenType::IDENT, "Expected rank name after 'rank'.");
    consume(TokenType::LBRACE, "Expected '{' before rank body.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->variants.push_back(consume(TokenType::IDENT, "Expected variant name."));
        if (!match({TokenType::COMMA})) break; // Trailing comma is optional
    }

    consume(TokenType::RBRACE, "Expected '}' after rank variants.");
    return node;
}

// sigil Disk { stance Idle; stance Reading; soul16 sector_count; }
//
// Inside the sigil body we can encounter:
//   - 'stance' keyword  -> a typestate declaration
//   - 'spell' keyword   -> a bound method
//   - anything else     -> a data field (type + name)
//
// Stances and fields are stored in declaration order because the CodeGen must
// emit them in that order: __stance first (implicit), then fields in order.
std::unique_ptr<SigilDecl> Parser::parseSigilDecl() {
    auto node = std::make_unique<SigilDecl>();
    node->token = previous(); // 'sigil' token

    node->name = consume(TokenType::IDENT, "Expected sigil name after 'sigil'.");
    consume(TokenType::LBRACE, "Expected '{' before sigil body.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        if (match({TokenType::STANCE})) {
            // stance Idle;
            node->stances.push_back(consume(TokenType::IDENT, "Expected stance name after 'stance'."));
            consume(TokenType::SEMICOLON, "Expected ';' after stance declaration.");

        } else if (match({TokenType::SPELL})) {
            // Bound spell — a method attached to this sigil (V1.5 feature).
            // isTopLevel=false signals to parseSpellDecl that this spell belongs
            // to a sigil and should not be treated as a standalone function.
            node->boundSpells.push_back(parseSpellDecl(false));

        } else {
            // Data field: soul16 sector_count;
            Param field;
            field.isOwned   = false;
            field.isPointer = false;
            field.type      = consumeType("Expected a valid type for sigil field.");
            field.name      = consume(TokenType::IDENT, "Expected field name.");
            consume(TokenType::SEMICOLON, "Expected ';' after field declaration.");
            node->fields.push_back(field);
        }
    }

    consume(TokenType::RBRACE, "Expected '}' after sigil body.");
    return node;
}

// legion SectorCache { mark16 sector_id; flow read_time; oath is_corrupted; }
//
// Written like a sigil with no stances. V1: Parsed distinctly but stubbed as
// a sigil in CodeGen. V2 will implement the full SoA memory layout.
std::unique_ptr<LegionDecl> Parser::parseLegionDecl() {
    auto node = std::make_unique<LegionDecl>();
    node->token = previous(); // 'legion' token

    node->name = consume(TokenType::IDENT, "Expected legion name after 'legion'.");
    consume(TokenType::LBRACE, "Expected '{' before legion body.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        Param field;
        field.type = consumeType("Expected a valid type for legion field.");
        field.name = consume(TokenType::IDENT, "Expected field name.");
        consume(TokenType::SEMICOLON, "Expected ';' after field declaration.");
        node->fields.push_back(field);
    }

    consume(TokenType::RBRACE, "Expected '}' after legion body.");
    return node;
}

// Parses all four spell variants. The 'previous()' token on entry is
// SPELL, WARDEN, or CONJURE (consumed by match() in parseDeclaration or
// by the explicit match in parseSigilDecl for bound spells).
//
// isTopLevel: true  = standalone spell at file scope
//             false = bound spell inside a sigil body
//
// Default argument (bool isTopLevel = true) is in the DECLARATION
// (Parser.h), not in this definition. In C++, default arguments must be
// in the declaration, not the definition. Having it here previously caused
// a signature mismatch that prevented compilation.
std::unique_ptr<SpellDecl> Parser::parseSpellDecl(bool /*isTopLevel*/) {
    auto node = std::make_unique<SpellDecl>();
    node->token = previous();

    node->isWarden  = (node->token.type == TokenType::WARDEN);
    node->isConjure = (node->token.type == TokenType::CONJURE);

    // After 'conjure', check for optional 'endless' modifier.
    if (node->isConjure && match({TokenType::ENDLESS})) {
        node->isEndless = true;
    }

    // 'warden' and 'conjure' are modifiers — the 'spell' keyword still follows.
    if (node->isWarden || node->isConjure) {
        consume(TokenType::SPELL, "Expected 'spell' keyword after modifier.");
    }

    node->name = consume(TokenType::IDENT, "Expected spell name.");
    consume(TokenType::LPAREN, "Expected '(' after spell name.");

    // --- Parameters ---
    if (!check(TokenType::RPAREN)) {
        do {
            Param p;
            p.isOwned = match({TokenType::OWN}); // 'own' is optional

            if (match({TokenType::SIGIL})) {
                // Sigil pointer parameter: [own] sigil* TypeName[:StanceName] paramName
                consume(TokenType::STAR, "Expected '*' after 'sigil' in parameter.");
                p.type      = consume(TokenType::IDENT, "Expected sigil type name.");
                p.isPointer = true;

                // Optional stance constraint: Disk:Idle
                if (match({TokenType::COLON})) {
                    p.stanceName = consume(TokenType::IDENT, "Expected stance name after ':'.");
                }
            } else {
                // Primitive type: addr target, scroll msg, mark16 x, etc.
                p.type = consumeType("Expected a valid parameter type.");
                
                // Allow primitive pointers (e.g., mark16* target_ptr)
                if (match({TokenType::STAR})) {
                    p.isPointer = true;
                } else {
                    p.isPointer = false;
                }
            }

            p.name = consume(TokenType::IDENT, "Expected parameter name.");
            node->params.push_back(p);

        } while (match({TokenType::COMMA}));
    }
    consume(TokenType::RPAREN, "Expected ')' after parameter list.");

    // --- Return Type ---
    consume(TokenType::ARROW, "Expected '->' before return type.");

    if (match({TokenType::LPAREN})) {
        // Tuple return: (sigil* Disk, scroll | ruin<DiskError>)
        //
        // After parsing each element type, check for `|`. If found,
        // consume the omen suffix, set hasOmen, and break — the omen
        // terminates the tuple element list.
        do {
            ReturnTypeInfo ri;
            if (match({TokenType::SIGIL})) {
                consume(TokenType::STAR, "Expected '*' after 'sigil' in return type.");
                ri.typeToken  = consumeType("Expected sigil type name after 'sigil*'.");
                ri.isPointer  = true;
            } else {
                ri.typeToken  = consumeType("Expected a valid return type.");
                ri.isPointer  = false;
            }
            node->returnTypes.push_back(ri);

            // Check if this element is followed by | ruin<Rank> — the Omen suffix.
            // If so, consume it here, set hasOmen, and stop parsing tuple elements.
            if (check(TokenType::PIPE)) {
                advance(); // consume '|'
                consume(TokenType::RUIN, "Expected 'ruin' after '|' in return type.");
                consume(TokenType::LT,   "Expected '<' after 'ruin'.");
                node->omenErrorType = consume(TokenType::IDENT, "Expected rank name inside ruin<...>.");
                consume(TokenType::GT,   "Expected '>' after ruin rank name.");
                node->hasOmen = true;
                break; // Omen terminates the tuple element list
            }
        } while (match({TokenType::COMMA}));
        consume(TokenType::RPAREN, "Expected ')' after tuple return type.");
    } else {
        // Single return type
        ReturnTypeInfo ri;
        if (match({TokenType::SIGIL})) {
            consume(TokenType::STAR, "Expected '*' after 'sigil'.");
            ri.typeToken = consumeType("Expected sigil type name.");
            ri.isPointer = true;
        } else {
            ri.typeToken = consumeType("Expected a valid return type.");
            ri.isPointer = false;
        }
        node->returnTypes.push_back(ri);
    }

    // Optional Omen: | ruin<DiskError>
    if (match({TokenType::PIPE})) {
        consume(TokenType::RUIN, "Expected 'ruin' after '|' in return type.");
        consume(TokenType::LT,   "Expected '<' after 'ruin'.");
        node->omenErrorType = consume(TokenType::IDENT, "Expected rank name inside ruin<...>.");
        consume(TokenType::GT,   "Expected '>' after ruin rank name.");
        node->hasOmen = true;
    }

    // conjure spells are extern declarations — no body.
    if (node->isConjure) {
        consume(TokenType::SEMICOLON, "Expected ';' after conjure declaration.");
        return node;
    }

    // --- Body ---
    consume(TokenType::LBRACE, "Expected '{' before spell body.");
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->body.push_back(parseStatement());
    }
    consume(TokenType::RBRACE, "Expected '}' after spell body.");

    return node;
}

// leyline disk_status_port: rune @ 0x1F7;
// portline disk_data_port: soul16 @ 0x1F0;
//
// isPortline=false -> leyline (MMIO) -> volatile pointer in CodeGen
// isPortline=true  -> portline (PIO) -> inb/outb inline assembly in CodeGen
std::unique_ptr<HardwareDecl> Parser::parseHardwareDecl() {
    auto node = std::make_unique<HardwareDecl>();
    node->token      = previous(); // 'leyline' or 'portline' token
    node->isPortline = (node->token.type == TokenType::PORTLINE);

    node->name    = consume(TokenType::IDENT,    "Expected identifier after hardware keyword.");
    consume(TokenType::COLON, "Expected ':' after hardware name.");
    node->type    = consumeType("Expected a valid type for hardware declaration (e.g., rune, soul16).");
    consume(TokenType::AT,    "Expected '@' before hardware address.");
    node->address = consume(TokenType::INT_LIT,  "Expected address literal after '@'.");
    consume(TokenType::SEMICOLON, "Expected ';' after hardware declaration.");

    return node;
}

// =============================================================================
// STATEMENT PARSERS
// =============================================================================

// Routes to the correct statement parser. Falls through to parseExprOrAssignStmt
// for everything that does not start with a recognized keyword.
std::unique_ptr<Stmt> Parser::parseStatement() {
    // Intercept Primitive Types — with optional pointer declarator.
    // Handles: mark16 x = 5;   AND   mark16* px = &x;
    if (check(TokenType::MARK16) || check(TokenType::MARK32) ||
        check(TokenType::SOUL16) || check(TokenType::SOUL32) ||
        check(TokenType::ADDR)   || check(TokenType::FLOW)   ||
        check(TokenType::RUNE)   || check(TokenType::OATH)   ||
        check(TokenType::SCROLL) || check(TokenType::ABYSS)  ||
        check(TokenType::DECK)) {
        Token typeToken = advance();
        bool isPtr = false;
        if (check(TokenType::STAR)) {
            advance(); // consume '*'
            isPtr = true;
        }
        return parseVarDeclStmt(typeToken, isPtr);
    }

    // Intercept sigil/legion local declarations with explicit keyword.
    // Handles: sigil Device dev = ...;   AND   sigil* Device ptr = ...;
    if (match({TokenType::SIGIL, TokenType::LEGION})) {
        bool isPtr = false;
        if (check(TokenType::STAR)) {
            advance(); // consume '*' — this is sigil* Device ptr = ...
            isPtr = true;
        }
        Token typeToken = consume(TokenType::IDENT, "Expected type name after sigil/legion.");
        return parseVarDeclStmt(typeToken, isPtr);
    }

    // Intercept BARE sigil/legion declarations without the keyword:
    //   Device my_dev = ...;
    //
    // Uses a hardened 3-token lookahead: IDENT IDENT (ASSIGN | SEMICOLON)
    //
    // WHY 3 TOKENS, NOT 2:
    //   The original 2-token check (IDENT IDENT) fired on any two adjacent
    //   identifiers, including across a missing-semicolon boundary:
    //     some_call()        <- missing semicolon
    //     Device dev = ...;  <- IDENT IDENT triggered here, wrong error message
    //
    //   With 3 tokens: the third must be '=' or ';' — the only valid
    //   continuations of a variable declaration. Any other token (LPAREN,
    //   operator, etc.) means this is NOT a declaration and we fall through.
    //
    // NOTE: The duplicate SIGIL/LEGION block that was below this check has
    //       been removed — it was dead code (the first block always returned).
    if (check(TokenType::IDENT) &&
        peekNext().type == TokenType::IDENT &&
        (peekNextNext().type == TokenType::ASSIGN ||
         peekNextNext().type == TokenType::SEMICOLON)) {
        Token typeToken = advance(); // Consume the type name IDENT
        return parseVarDeclStmt(typeToken);
    }
    
    // Lookahead for: Device* ptr = ...;
    if (check(TokenType::IDENT) &&
        peekNext().type == TokenType::STAR &&
        (current + 2 < (int)tokens.size()) &&
        tokens[current + 2].type == TokenType::IDENT &&
        (current + 3 < (int)tokens.size()) &&
        (tokens[current + 3].type == TokenType::ASSIGN || 
         tokens[current + 3].type == TokenType::SEMICOLON)) {
        
        Token typeToken = advance(); // Consume type name
        advance();                   // Consume '*'
        return parseVarDeclStmt(typeToken, true);
    }
    
    if (match({TokenType::IF}))       return parseIfStmt();
    if (match({TokenType::FORE}))     return parseForeStmt();
    if (match({TokenType::WHIRL}))    return parseWhirlStmt();
    if (match({TokenType::YIELD}))    return parseYieldStmt();
    if (match({TokenType::DESTINED})) return parseDestinedStmt();

    if (match({TokenType::SHATTER})) {
        auto node = std::make_unique<ShatterStmt>();
        node->token = previous();
        consume(TokenType::SEMICOLON, "Expected ';' after 'shatter'.");
        return node;
    }

    if (match({TokenType::SURGE})) {
        auto node = std::make_unique<SurgeStmt>();
        node->token = previous();
        consume(TokenType::SEMICOLON, "Expected ';' after 'surge'.");
        return node;
    }

    if (match({TokenType::LBRACE})) {
        // A bare block statement. '{' was just consumed by match().
        // parseBlock() takes over from the next token onward.
        return parseBlock();
    }

    // Detect the divine pattern: IDENT <~ divine ...
    // This requires looking two tokens ahead:
    //   peek()            = IDENT    (the target variable, e.g., my_disk)
    //   peekNext()        = REV_WEAVE  (<~)
    //   tokens[current+2] = DIVINE
    //
    // Added bounds check (current + 2 < tokens.size()) before accessing
    // tokens[current + 2]. Without this, accessing past the end of the vector
    // is undefined behavior and can crash the compiler.
    if (check(TokenType::IDENT) &&
        peekNext().type == TokenType::REV_WEAVE &&
        (current + 2 < (int)tokens.size()) &&
        tokens[current + 2].type == TokenType::DIVINE) {
        return parseDivineStmt();
    }

    return parseExprOrAssignStmt();
}

// Parses the BODY of a block — the contents between { and }.
//
// CONVENTION: The opening '{' must ALREADY be consumed by the caller before
// calling parseBlock(). This is consistent throughout the parser:
//
//   consume(TokenType::LBRACE, "...");  // caller's job
//   node->body = parseBlock();          // parseBlock reads until } and consumes it
//
// parseBlock reads statements until it sees '}', then consumes '}'.
// The caller's consume(LBRACE) ensures error messages point to the right
// source location if '{' is missing.
std::unique_ptr<BlockStmt> Parser::parseBlock() {
    auto node = std::make_unique<BlockStmt>();
    node->token = previous(); // The '{' token — used for error reporting

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        node->statements.push_back(parseStatement());
    }

    consume(TokenType::RBRACE, "Expected '}' to close block.");
    return node;
}

// if (condition) { } elif (condition) { } else { }
//
// 'if' has already been consumed by match() in parseStatement().
// The body is a block — we consume '{' here, then call parseBlock().
// Zero or more elif branches follow, then an optional else.
std::unique_ptr<IfStmt> Parser::parseIfStmt() {
    auto node = std::make_unique<IfStmt>();
    node->token = previous(); // 'if' token

    consume(TokenType::LPAREN, "Expected '(' after 'if'.");
    node->condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after if condition.");
    consume(TokenType::LBRACE, "Expected '{' before if body.");
    node->thenBranch = parseBlock();

    while (match({TokenType::ELIF})) {
        ElifBranch branch;
        consume(TokenType::LPAREN, "Expected '(' after 'elif'.");
        branch.condition = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after elif condition.");
        consume(TokenType::LBRACE, "Expected '{' before elif body.");
        branch.body = parseBlock();
        node->elifBranches.push_back(std::move(branch));
    }

    if (match({TokenType::ELSE})) {
        consume(TokenType::LBRACE, "Expected '{' before else body.");
        node->elseBranch = parseBlock();
    }

    return node;
}

// fore (mark16 i = 0; i < 10; i++) { }
//
// 'fore' has already been consumed. We parse the three-part header inside ( )
// then the body block.
std::unique_ptr<ForeStmt> Parser::parseForeStmt() {
    auto node = std::make_unique<ForeStmt>();
    node->token = previous(); // 'fore' token

    consume(TokenType::LPAREN, "Expected '(' after 'fore'.");

    // Initializer: mark16 i = 0;
    node->initType  = consumeType("Expected a valid type for fore loop variable (e.g., mark16).");
    node->initVar   = consume(TokenType::IDENT,   "Expected loop variable name.");
    consume(TokenType::ASSIGN, "Expected '=' in loop initializer.");
    node->initValue = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after loop initializer.");

    // Condition: i < 10;
    node->condition = parseExpression();
    consume(TokenType::SEMICOLON, "Expected ';' after loop condition.");

    // Increment: parse left side. If '=' follows, build an AssignExpr.
    auto incTarget = parseExpression();
    if (match({TokenType::ASSIGN})) {
        Token eqToken = previous();
        auto incValue = parseExpression();
        node->increment = std::make_unique<AssignExpr>(std::move(incTarget), eqToken, std::move(incValue));
    } else {
        node->increment = std::move(incTarget);
    }

    consume(TokenType::RPAREN, "Expected ')' after fore header.");
    consume(TokenType::LBRACE, "Expected '{' before fore body.");
    node->body = parseBlock();

    return node;
}

// whirl (condition) { }
std::unique_ptr<WhirlStmt> Parser::parseWhirlStmt() {
    auto node = std::make_unique<WhirlStmt>();
    node->token = previous(); // 'whirl' token

    consume(TokenType::LPAREN, "Expected '(' after 'whirl'.");
    node->condition = parseExpression();
    consume(TokenType::RPAREN, "Expected ')' after whirl condition.");
    consume(TokenType::LBRACE, "Expected '{' before whirl body.");
    node->body = parseBlock();

    return node;
}

// yield (ctrl, data);   yield 0;   yield;
//
// 'yield' has already been consumed. We check what follows to determine the form:
//   ';'  -> void return (abyss spells)
//   '('  -> tuple return
//   else -> single value return
//
// CodeGen note: Every yield in a spell that contains a destined block will be
// rewritten during CodeGen into: __ret = <values>; goto __destined_N;
// The YieldStmt node itself does not know about this rewrite — CodeGen handles it
// by scanning the parent spell for destined blocks during emission.
// yield (ctrl, data);   yield 0;   yield;
//
// Uses lookahead to distinguish between a tuple yield (which contains a comma)
// and a parenthesized single-value yield (e.g., yield (a + b) * c;).
std::unique_ptr<YieldStmt> Parser::parseYieldStmt() {
    auto node = std::make_unique<YieldStmt>();
    node->token = previous(); // 'yield' token

    if (check(TokenType::SEMICOLON)) {
        advance(); // consume ';'
        return node; // Void return — values vector is empty
    }

    // LOOKAHEAD: Is this a tuple yield `yield (a, b)` or a math expression `yield (a + b)`?
    bool isTuple = false;
    if (check(TokenType::LPAREN)) {
        int depth = 0;
        for (size_t i = current; i < tokens.size(); i++) {
            if (tokens[i].type == TokenType::LPAREN) depth++;
            else if (tokens[i].type == TokenType::RPAREN) {
                depth--;
                if (depth == 0) break; // Reached the end of the top-level parens
            } else if (tokens[i].type == TokenType::COMMA && depth == 1) {
                isTuple = true; // Found a comma at the top level! It's a tuple.
                break;
            }
        }
    }

    if (isTuple) {
        // Tuple return: yield (ctrl, ruin(DiskError::HardwareFault));
        consume(TokenType::LPAREN, "Expected '(' before tuple yield.");
        do {
            node->values.push_back(parseExpression());
        } while (match({TokenType::COMMA}));
        consume(TokenType::RPAREN, "Expected ')' after tuple yield values.");
    } else {
        // Single value: yield 0; or yield (a + b) * c;
        // We do NOT consume the LPAREN here; we let parseExpression handle the math parens.
        node->values.push_back(parseExpression());
    }

    consume(TokenType::SEMICOLON, "Expected ';' after yield.");
    return node;
}

// destined (condition) { cleanup_body; }
// destined { cleanup_body; }            <- condition is OPTIONAL
//
// The original parser always did consume(LPAREN) which would crash
// on the conditionless form. The spec explicitly states the condition is
// optional. We now check whether '(' or '{' follows and behave accordingly.
std::unique_ptr<DestinedStmt> Parser::parseDestinedStmt() {
    auto node = std::make_unique<DestinedStmt>();
    node->token = previous(); // 'destined' token

    // Optional condition: if '(' follows, parse condition; if '{', skip it.
    if (match({TokenType::LPAREN})) {
        node->hasCondition = true;
        node->condition    = parseExpression();
        consume(TokenType::RPAREN, "Expected ')' after destined condition.");
    } else {
        // No condition — cleanup fires unconditionally before every yield.
        node->hasCondition = false;
        // node->condition remains nullptr
    }

    consume(TokenType::LBRACE, "Expected '{' before destined body.");
    node->body = parseBlock();
    return node;
}

// my_disk <~ divine fetch_sector(own &my_disk, 0x0500) {
//     (ctrl, scroll data)                    => { ... }
//     (ctrl, ruin<DiskError::HardwareFault>) => { ... }
//     (ctrl, ruin err)                       => { ... }
// }
//
// DivineBranch now uses distinct fields for each branch kind instead
// of reusing payloadVar for both the variant name and the error variable.
// This eliminates the ambiguity that would have caused silent semantic bugs.
std::unique_ptr<DivineStmt> Parser::parseDivineStmt() {
    auto node = std::make_unique<DivineStmt>();

    // IDENT <~ divine
    node->targetVar = consume(TokenType::IDENT,     "Expected variable name before '<~'.");
    consume(TokenType::REV_WEAVE,                   "Expected '<~' in divine statement.");
    node->token     = consume(TokenType::DIVINE,    "Expected 'divine' keyword.");

    // The spell call expression: fetch_sector(own &my_disk, 0x0500)
    node->spellCall = parseExpression();

    consume(TokenType::LBRACE, "Expected '{' before divine branches.");

    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        DivineBranch branch;

        consume(TokenType::LPAREN, "Expected '(' before divine pattern.");

        // First element: the ownership rebinding variable (always present)
        branch.ownerVar = consume(TokenType::IDENT, "Expected ownership variable (e.g., 'ctrl').");

        // If ')' follows immediately, this is a payloadless success branch: (ctrl) => { }
        // This is the correct syntax for 'abyss | ruin<T>' omens where the success
        // carries no data. Parsing a payload token here would create a ghost variable —
        // a name registered in the symbol table but never declared in the generated C.
        if (check(TokenType::RPAREN)) {
            branch.isRuin        = false;
            branch.isPayloadless = true;
            consume(TokenType::RPAREN, "Expected ')' after payloadless divine pattern.");
        } else {
            consume(TokenType::COMMA, "Expected ',' after ownership variable.");

            // Second element: the payload pattern
            if (match({TokenType::RUIN})) {
                branch.isRuin = true;

                if (match({TokenType::LT})) {
                    // Specific ruin: ruin<DiskError::HardwareFault>
                    branch.isSpecificRuin = true;
                    branch.rankName    = consume(TokenType::IDENT,  "Expected rank name in ruin<...>.");
                    consume(TokenType::SCOPE,                        "Expected '::' after rank name.");
                    branch.variantName = consume(TokenType::IDENT,  "Expected variant name after '::'.");
                    consume(TokenType::GT,                           "Expected '>' after ruin pattern.");
                } else {
                    // Catch-all ruin: ruin err
                    branch.isSpecificRuin = false;
                    branch.catchAllVar = consume(TokenType::IDENT, "Expected error variable name after 'ruin'.");
                }
            } else {
                // Success branch with payload: scroll data
                branch.isRuin        = false;
                branch.isPayloadless = false;
                branch.successType   = consumeType("Expected a valid type for success branch (e.g., scroll).");
                branch.successVar    = consume(TokenType::IDENT, "Expected success variable name.");
            }

            consume(TokenType::RPAREN, "Expected ')' after divine pattern.");
        }
        consume(TokenType::FAT_ARROW,  "Expected '=>' after divine pattern.");
        consume(TokenType::LBRACE, "Expected '{' before branch body.");
        branch.body = parseBlock();

        node->branches.push_back(std::move(branch));
    }

    consume(TokenType::RBRACE, "Expected '}' after divine branches.");
    return node;
}

// Handles both expression statements and assignment statements.
//
// We parse the left-hand side as an expression first. If '=' follows,
// it is an assignment statement. Otherwise it is a bare expression statement.
//
// Expression statements: process_data(data);    acknowledge_interrupt();
// Assignment statements: ctrl = Disk:Reading;   my_disk = Disk:Idle;
//
// This two-step approach correctly handles complex left-hand sides like
// ctrl->field = value; (a member access expression as the target).
std::unique_ptr<Stmt> Parser::parseExprOrAssignStmt() {
    auto expr = parseExpression();

    if (match({TokenType::ASSIGN})) {
        auto assign   = std::make_unique<AssignStmt>();
        assign->token  = previous(); // '=' token
        assign->target = std::move(expr);
        assign->value  = parseExpression();
        consume(TokenType::SEMICOLON, "Expected ';' after assignment.");
        return assign;
    }

    consume(TokenType::SEMICOLON, "Expected ';' after expression statement.");
    return std::make_unique<ExprStmt>(std::move(expr));
}

std::unique_ptr<Stmt> Parser::parseVarDeclStmt(Token typeToken, bool isPointer) {
    auto node = std::make_unique<VarDeclStmt>();
    node->token = typeToken;

    if (typeToken.type == TokenType::DECK) {
        node->isArray = true;
        consume(TokenType::LBRACKET, "Expected '[' after 'deck'.");
        node->arraySizeToken = consume(TokenType::INT_LIT, "Expected array size literal.");
        consume(TokenType::RBRACKET, "Expected ']' after array size.");
        node->typeToken = consumeType("Expected element type after 'deck[N]'.");
    } else {
        node->isArray = false;
        node->typeToken = typeToken;
    }

    // Store the pointer flag passed from parseStatement().
    // This covers: mark16* px = ...; and sigil* Device ptr = ...;
    node->isPointer = isPointer;

    node->name = consume(TokenType::IDENT, "Expected variable name.");

    if (match({TokenType::ASSIGN})) {
        node->initializer = parseExpression();
    }

    consume(TokenType::SEMICOLON, "Expected ';' after variable declaration.");
    return node;
}

// =============================================================================
// EXPRESSION PARSERS (Pratt Parsing / Precedence Climbing)
// =============================================================================

// Top-level entry. Starts at minimum precedence 1 so all infix operators
// are eligible from the beginning.
std::unique_ptr<Expr> Parser::parseExpression() {
    return parsePrecedence(1);
}

// The core of the Pratt Parser.
//
// HOW IT WORKS:
//   1. Consume the first token and create a PREFIX node (literal, identifier,
//      unary operator, parenthesized expression, etc.)
//   2. While the NEXT token is an infix operator whose precedence >= minPrecedence:
//      a. Consume the operator
//      b. Parse the right operand at (precedence + 1) to get left-associativity
//      c. Wrap left and right into a BinaryExpr (or CallExpr, PostfixExpr, etc.)
//      d. The result becomes the new 'left' for the next iteration
//
// WHY THIS WORKS:
//   By passing (prec + 1) for the right operand, we ensure that the NEXT
//   operator of the SAME precedence goes to a new iteration rather than being
//   consumed by the recursive call. This gives left-associativity: a + b + c
//   becomes (a + b) + c, not a + (b + c).
//
//   Higher-precedence operators consume more tightly: a + b * c correctly
//   becomes a + (b * c) because * has higher precedence than + so the recursive
//   call for b's right side will consume the *.
std::unique_ptr<Expr> Parser::parsePrecedence(int minPrecedence) {
    Token tok = advance(); // Consume the first token of this expression
    std::unique_ptr<Expr> left;

    // --- PREFIX RULES: What can START an expression? ---
    switch (tok.type) {

        // Integer, float, and string literals: 0x1F7, 42, 3.14, "Drive dead"
        case TokenType::INT_LIT:
        case TokenType::FLOAT_LIT:
        case TokenType::STRING_LIT:
            left = std::make_unique<LiteralExpr>(tok);
            break;

        // Boolean literals: kept (true), forsaken (false)
        case TokenType::KEPT:
        case TokenType::FORSAKEN:
            left = std::make_unique<LiteralExpr>(tok);
            break;

        // Identifier — could be:
        //   plain name:          my_disk
        //   stance reference:    Disk:Fault        (IDENT COLON IDENT)
        //   rank variant:        DiskError::Timeout (IDENT SCOPE IDENT)
        case TokenType::IDENT: {
            auto identExpr = std::make_unique<IdentifierExpr>(tok);

            if (match({TokenType::COLON})) {
                // Stance reference: Device:Idle — could be standalone or struct init prefix
                identExpr->stanceName = consume(TokenType::IDENT, "Expected stance name after ':'.");

                // After Device:Idle, check if { follows — this is a stance-prefixed struct init.
                // e.g., Device:Idle { device_id: 0, ... }
                if (check(TokenType::LBRACE) &&
                    (peekNext().type == TokenType::IDENT || peekNext().type == TokenType::RBRACE)) {
                    return parseStructInitExpr(tok, identExpr->stanceName);
                }
            } else if (match({TokenType::SCOPE})) {
                // Rank variant: DiskError::Timeout
                identExpr->variantName = consume(TokenType::IDENT, "Expected variant name after '::'.");
            } else if (check(TokenType::LBRACE) &&
                    (peekNext().type == TokenType::IDENT || peekNext().type == TokenType::RBRACE)) {
                // Struct init without stance: Packet { length: 64, ... }
                Token emptyStance; // default-constructed = empty lexeme
                return parseStructInitExpr(tok, emptyStance);
            }

            left = std::move(identExpr);
            break;
        }

        // Parenthesized expression: (a + b)
        case TokenType::LPAREN:
            left = parseExpression();
            consume(TokenType::RPAREN, "Expected ')' after parenthesized expression.");
            break;

        // Address-of operator: &my_disk, &disk_data_port
        // Semantic analysis distinguishes hardware addresses from regular addresses.
        case TokenType::AMP:
            left = std::make_unique<AddressOfExpr>(tok, parsePrecedence(7));
            break;

        // Unary minus: -x
        // Pointer dereference: *ptr
        case TokenType::MINUS:
        case TokenType::STAR:
        case TokenType::TILDE:
            left = std::make_unique<UnaryExpr>(tok, parsePrecedence(90));
            break;

        // ruin(DiskError::HardwareFault) — error value construction in yield.
        // We parse it as a special call-like expression because it looks like
        // a function call syntactically but has specific semantic meaning.
        case TokenType::RUIN: {
            auto ruinIdent = std::make_unique<IdentifierExpr>(tok);
            consume(TokenType::LPAREN, "Expected '(' after 'ruin'.");
            auto callNode = std::make_unique<CallExpr>(std::move(ruinIdent), tok);
            // The argument is the rank variant: DiskError::HardwareFault
            callNode->args.push_back(parsePrecedence(1));
            callNode->argIsOwned.push_back(false);
            consume(TokenType::RPAREN, "Expected ')' after ruin argument.");
            left = std::move(callNode);
            break;
        }
        
        // cast<TargetType>(expr) or cast<TargetType*>(expr)
        case TokenType::CAST: {
            consume(TokenType::LT, "Expected '<' after 'cast'.");
            Token targetType = consumeType("Expected a valid target type inside 'cast<...>'.");
            
            // Check for optional pointer modifier
            bool isPtr = false;
            if (check(TokenType::STAR)) {
                advance(); // Consume the '*'
                isPtr = true;
            }
            
            consume(TokenType::GT, "Expected '>' after cast target type.");
            consume(TokenType::LPAREN, "Expected '(' after 'cast<Type>'.");
            auto operand = parseExpression();
            consume(TokenType::RPAREN, "Expected ')' after cast expression.");
            
            left = std::make_unique<CastExpr>(targetType, isPtr, std::move(operand));
            break;
        }

        // Prefix increment/decrement: ++i, --val
        case TokenType::PLUS_PLUS:
        case TokenType::MINUS_MINUS:
            left = std::make_unique<UpdateExpr>(parsePrecedence(90), tok, true);
            break;

        default:
            error(tok, "Expected an expression.");
    }

    // --- INFIX RULES: What can FOLLOW an expression? ---
    while (true) {
        int prec = getPrecedence(peek().type);
        if (prec < minPrecedence) break; // Next operator is too low — stop climbing

        Token op = advance(); // Consume the infix operator

        if (op.type == TokenType::QUESTION) {
            // Postfix '?' — Omen unpack. No right operand needed.
            // read_buffer()? -> if ruin, yield it up; if success, unwrap.
            left = std::make_unique<PostfixExpr>(std::move(left), op);
            
        } else if (op.type == TokenType::LBRACKET) {
            // Array subscript: target[index]
            // '[' was just consumed as the infix operator.
            auto idxExpr = std::make_unique<IndexExpr>(std::move(left), parseExpression(), op);
            consume(TokenType::RBRACKET, "Expected ']' after array index.");
            left = std::move(idxExpr);

        } else if (op.type == TokenType::LPAREN) {
            // Function call: callee(args...)
            // '(' was just consumed as the operator — parseCallExpr handles args.
            left = parseCallExpr(std::move(left));

        } else if (op.type == TokenType::ARROW || op.type == TokenType::DOT) {
            // Member access
            Token member;
            if (check(TokenType::STANCE)) {
                member = advance(); // Allow 'stance' keyword as a member name
            } else {
                member = consume(TokenType::IDENT, "Expected member name after '->' or '.'.");
            }
            auto memberExpr = std::make_unique<IdentifierExpr>(member);
            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(memberExpr));

        } else if (op.type == TokenType::PLUS_PLUS || op.type == TokenType::MINUS_MINUS) {
            // Postfix increment/decrement: i++, val--
            // These operators do not consume a right-hand expression!
            left = std::make_unique<UpdateExpr>(std::move(left), op, false);

        } else {
            auto right = parsePrecedence(prec + 1);

            // DESUGAR WEAVE OPERATOR (~>)
            // Intercepts a ~> f(b) AND a ~> f(b)? 
            if (op.type == TokenType::WEAVE) {
                CallExpr* callRight = dynamic_cast<CallExpr*>(right.get());
                PostfixExpr* postRight = nullptr;
                
                // If not a direct call, check if it's wrapped in a '?' Omen unpack
                if (!callRight) {
                    postRight = dynamic_cast<PostfixExpr*>(right.get());
                    if (postRight) {
                        callRight = dynamic_cast<CallExpr*>(postRight->operand.get());
                    }
                }

                if (callRight) {
                    // Inject the left expression as the first argument
                    callRight->args.insert(callRight->args.begin(), std::move(left));
                    callRight->argIsOwned.insert(callRight->argIsOwned.begin(), false);
                    
                    left = std::move(right); 
                    continue; 
                }
            }

            left = std::make_unique<BinaryExpr>(std::move(left), op, std::move(right));
        }
    }

    return left;
}

// Parses the argument list of a function call.
// Called when '(' is detected after an expression in the infix loop above.
// The '(' token has already been consumed.
//
// Handles 'own' on arguments: fetch_sector(own &my_disk, 0x0500)
// The 'own' keyword is recorded in argIsOwned (compile-time tracking only).
// CodeGen strips 'own' and emits the bare argument expression.
std::unique_ptr<Expr> Parser::parseCallExpr(std::unique_ptr<Expr> callee) {
    Token parenToken = previous(); // The '(' token
    auto callNode = std::make_unique<CallExpr>(std::move(callee), parenToken);

    if (!check(TokenType::RPAREN)) {
        do {
            bool owned = match({TokenType::OWN}); // 'own' is optional per argument
            callNode->args.push_back(parseExpression());
            callNode->argIsOwned.push_back(owned);
        } while (match({TokenType::COMMA}));
    }

    consume(TokenType::RPAREN, "Expected ')' after function arguments.");
    return callNode;
}

std::unique_ptr<StructInitExpr> Parser::parseStructInitExpr(Token typeName, Token stanceName) {
    auto node = std::make_unique<StructInitExpr>();
    node->token     = typeName;
    node->typeName  = typeName;
    node->stanceName = stanceName;  // empty lexeme if no stance prefix

    consume(TokenType::LBRACE, "Expected '{' in struct initializer.");

    // Parse field initializers: fieldName: value
    // Supports trailing comma and empty initializer list {}.
    while (!check(TokenType::RBRACE) && !isAtEnd()) {
        StructFieldInit field;
        field.name  = consume(TokenType::IDENT,  "Expected field name in struct initializer.");
        consume(TokenType::COLON, "Expected ':' after field name. Use 'fieldName: value' syntax.");
        field.value = parseExpression();
        node->fields.push_back(std::move(field));

        if (!match({TokenType::COMMA})) break; // trailing comma optional
    }

    consume(TokenType::RBRACE, "Expected '}' after struct initializer fields.");
    return node;
}

// Maps each infix operator token type to its binding power (precedence level).
// Matches the Cgil spec v1.7 operator precedence table exactly.
//
// Higher number = binds tighter = evaluated first.
// 0 means "not an infix operator" — the climb stops when getPrecedence returns 0.
//
// Ref (from spec, highest to lowest):
//   ? (Omen Unpack)         8
//   () function call         8  (same level as ?, so a()? works left-to-right)
//   ->, .                    7
//   *, /                     6
//   +, -                     5
//   ==, !=, >, <             4
//   ~> (Weave)               3
//   <~ (Reverse Weave)       2
//   | (Omen Union)           1
int Parser::getPrecedence(TokenType type) const {
    switch (type) {
        case TokenType::PLUS_PLUS:
        case TokenType::MINUS_MINUS: return 110;
        case TokenType::LBRACKET:  return 100;
        case TokenType::QUESTION:
        case TokenType::LPAREN:    return 90;
        case TokenType::ARROW:
        case TokenType::DOT:       return 80;
        case TokenType::STAR:
        case TokenType::SLASH:
        case TokenType::PERCENT:   return 70; // P1: Modulo
        case TokenType::PLUS:
        case TokenType::MINUS:     return 60;
        case TokenType::LSHIFT:
        case TokenType::RSHIFT:    return 50; // P1: Shifts
        case TokenType::EQ:
        case TokenType::NEQ:
        case TokenType::GT:
        case TokenType::LT:
        case TokenType::GEQ:
        case TokenType::LEQ:       return 40; // P1: Bounds
        case TokenType::AMP:       // Bitwise AND
        case TokenType::CARET:     return 30; // P1: Bitwise XOR
        case TokenType::AMPAMP:    return 25; // P1: Logical AND
        case TokenType::PIPEPIPE:  return 20; // P1: Logical OR
        case TokenType::WEAVE:     return 15;
        case TokenType::REV_WEAVE: return 10;
        case TokenType::PIPE:      return 5;  // Omen union
        default:                   return 0;
    }
}

// =============================================================================
// TOKEN NAVIGATION HELPERS
// =============================================================================

// Return the current token without consuming it.
Token Parser::peek() const {
    return tokens[current];
}

// Return the token one position ahead without consuming either token.
// Used in parseStatement() to detect the 'IDENT <~ divine' pattern.
//
// Returns the last token in the stream (END_OF_FILE) if we are at or
// past the end, rather than accessing tokens[current + 1] unchecked. This
// prevents undefined behavior when peekNext() is called near the end of file.
Token Parser::peekNext() const {
    if (current + 1 >= (int)tokens.size()) return tokens.back();
    return tokens[current + 1];
}

// Look two tokens ahead without consuming any token.
// Mirrors peekNext() with the same bounds-safe pattern.
// Returns END_OF_FILE (the last token in the stream) if at or near the end.
Token Parser::peekNextNext() const {
    if (current + 2 >= (int)tokens.size()) return tokens.back();
    return tokens[current + 2];
}

// Return the most recently consumed token.
Token Parser::previous() const {
    // Prevent UB if current is 0 during error synchronization
    if (current == 0) {
        return tokens.empty() ? Token{TokenType::END_OF_FILE, "", 0, 0} : tokens[0];
    }
    return tokens[current - 1];
}

// True if the current token is END_OF_FILE.
bool Parser::isAtEnd() const {
    return peek().type == TokenType::END_OF_FILE;
}

// Consume the current token and return it. Advances current by 1.
Token Parser::advance() {
    if (!isAtEnd()) current++;
    return previous();
}

// True if the current token matches 'type'. Does NOT consume the token.
bool Parser::check(TokenType type) const {
    if (isAtEnd()) return false;
    return peek().type == type;
}

// If the current token matches any of the given types, consume it and return
// true. Otherwise return false without consuming anything.
bool Parser::match(std::initializer_list<TokenType> types) {
    for (TokenType type : types) {
        if (check(type)) {
            advance();
            return true;
        }
    }
    return false;
}

// Consume the current token if it is 'type'. If not, throw a descriptive
// error pointing at the current source location.
//
// This is called in every place where a specific token MUST appear. The error
// message is written to tell the programmer exactly what was expected and where.
Token Parser::consume(TokenType type, const std::string& message) {
    if (check(type)) return advance();
    error(peek(), message);
}

// Consume the current token only if it is a valid Cgil type.
//
// WHY THIS EXISTS:
//   Several places in the parser need to consume a type token: parameter types,
//   field types, hardware declaration types, and for-loop init types. The naive
//   approach is advance() — just take whatever is next. The problem is that
//   advance() accepts any token, including integer literals, string literals, or
//   keywords that are not types. A typo like `spell foo(42 x)` would silently
//   store the literal 42 as the parameter type. Semantic analysis would then
//   try to look up "42" in the type system and crash with a confusing internal
//   error rather than a clean "expected a type" message at the source location.
//
//   consumeType() fixes this by explicitly checking membership in the set of
//   valid type tokens before consuming. If the check fails, the error is reported
//   immediately at the exact token that was wrong.
//
// ABYSS is included: it is valid as a return type in some positions.
// DECK and TUPLE are included: they appear in type positions in the spec.
// IDENT is included: sigil and legion type names are user-defined identifiers.
Token Parser::consumeType(const std::string& message) {
    if (match({TokenType::MARK16, TokenType::MARK32,
               TokenType::SOUL16, TokenType::SOUL32,
               TokenType::ADDR,   TokenType::FLOW,
               TokenType::RUNE,   TokenType::OATH,
               TokenType::SCROLL, TokenType::ABYSS,
               TokenType::DECK,   TokenType::TUPLE,
               TokenType::IDENT})) {
        return previous();
    }
    error(peek(), message);
}

// =============================================================================
// ERROR HANDLING
// =============================================================================

// Build a formatted error message with source location, then throw.
// [[noreturn]] tells the compiler this function never returns, which suppresses
// spurious "control reaches end of non-void function" warnings in callers.
[[noreturn]] void Parser::error(Token token, const std::string& message) {
    std::string err = "[Line " + std::to_string(token.line) +
                      ":" + std::to_string(token.column) + "] Parse Error";

    if (token.type == TokenType::END_OF_FILE) {
        err += " at end of file: " + message;
    } else {
        err += " at '" + token.lexeme + "': " + message;
    }

    throw std::runtime_error(err);
}

// After a parse error is caught by the top-level parse() loop, synchronize()
// skips tokens until it finds a point where parsing can safely resume.
//
// Safe boundaries (we stop at):
//   - A semicolon or closing brace (end of a statement or block)
//   - A keyword that starts a new top-level declaration
//
// This allows the parser to report multiple errors in one compiler run instead
// of stopping at the very first mistake.
// PACT to the synchronize keyword set to match the full Cgil declaration set.
void Parser::synchronize() {
    advance(); // Skip the token that triggered the error

    while (!isAtEnd()) {
        if (previous().type == TokenType::SEMICOLON ||
            previous().type == TokenType::RBRACE) return;

        switch (peek().type) {
            case TokenType::SPELL:
            case TokenType::WARDEN:
            case TokenType::CONJURE:
            case TokenType::SIGIL:
            case TokenType::LEGION:
            case TokenType::LEYLINE:
            case TokenType::PORTLINE:
            case TokenType::RANK:
            case TokenType::GRIMOIRE:
            case TokenType::PACT:
                return;
            default:
                break;
        }

        advance();
    }
}