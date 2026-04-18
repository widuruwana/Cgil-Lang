#pragma once

#include "../Lexer/Lexer.h"
#include "../Parser/AST.h"
#include <vector>
#include <memory>
#include <stdexcept>
#include <initializer_list>

// =============================================================================
// CGIL PARSER
// =============================================================================
//
// WHAT THIS CLASS DOES:
//   Takes the flat token stream produced by the Lexer and builds the Abstract
//   Syntax Tree defined in AST.h. After this class runs, the Parser is done —
//   nothing in the compiler touches the token stream again.
//
// ALGORITHM: RECURSIVE DESCENT
//   The parser is a hand-written recursive descent parser. There is one private
//   function for each grammar rule in Cgil. Each function:
//     1. Looks at the current token (peek())
//     2. Consumes the tokens that belong to its rule (advance(), consume())
//     3. Returns the corresponding AST node
//     4. Throws a runtime_error if something unexpected appears
//
//   Functions call each other recursively. For example, parseSpellDecl() calls
//   parseStatement() for each statement in the body, which calls parseExpression()
//   for expressions inside those statements.
//
// EXPRESSIONS: PRATT PARSING (PRECEDENCE CLIMBING)
//   Expressions are parsed using a technique called Pratt Parsing, also called
//   precedence climbing. The key insight is that each infix operator has a
//   numeric precedence. The parsePrecedence(minPrecedence) function:
//     1. Parses a prefix (literal, identifier, unary operator)
//     2. Loops: while the NEXT operator's precedence >= minPrecedence, consume
//        it and parse its right operand at a higher minimum precedence
//
//   This naturally handles left-associativity and operator priority without
//   needing separate functions for each precedence level. The getPrecedence()
//   function maps each Cgil operator to its level from the spec.
//
// ERROR RECOVERY: PANIC MODE
//   When parseStatement() or parseDeclaration() hits an unexpected token,
//   the error() function throws a std::runtime_error. The top-level parse()
//   loop catches this, reports it to stderr, then calls synchronize() to skip
//   tokens until the next safe recovery point (a new declaration keyword or
//   a semicolon). This lets the parser find and report multiple errors in one
//   compiler run rather than stopping at the first mistake.
//
// CONVENTION: OPENING BRACE OWNERSHIP
//   parseBlock() does NOT consume the opening '{'. The caller is always
//   responsible for consuming '{' with consume(LBRACE, ...) before calling
//   parseBlock(). parseBlock() reads statements until '}' and consumes '}'.
//   This rule is followed consistently throughout the parser.
//
// =============================================================================
class Parser {
public:
    explicit Parser(const std::vector<Token>& tokens);

    // Main entry point. Parses the entire token stream into a list of top-level
    // declarations. Errors are caught, reported to stderr, and recovered from.
    // Returns whatever declarations were successfully parsed even if errors occurred.
    std::vector<std::unique_ptr<Decl>> parse();

private:
    std::vector<Token> tokens;  // The complete token stream from the Lexer
    int current = 0;            // Index of the current (not yet consumed) token

    // =========================================================================
    // DECLARATION PARSERS
    // Each handles one top-level Cgil construct.
    // =========================================================================

    // Routes to the right parser based on the current keyword.
    std::unique_ptr<Decl>         parseDeclaration();

    // grimoire <header.h>;   pact <header.h>;
    std::unique_ptr<GrimoireDecl>  parseGrimoireDecl();

    // rank DiskError { Timeout, HardwareFault, InvalidSector }
    std::unique_ptr<RankDecl>      parseRankDecl();

    // sigil Disk { stance Idle; soul16 sector_count; }
    std::unique_ptr<SigilDecl>     parseSigilDecl();

    // legion SectorCache { mark16 sector_id; flow read_time; }
    std::unique_ptr<LegionDecl>    parseLegionDecl();

    // spell / warden spell / conjure spell / conjure endless spell
    // isTopLevel = false when parsing a bound spell inside a sigil body.
    std::unique_ptr<SpellDecl>     parseSpellDecl(bool isTopLevel = true);

    // leyline / portline declarations
    std::unique_ptr<HardwareDecl>  parseHardwareDecl();

    // =========================================================================
    // STATEMENT PARSERS
    // Each handles one kind of statement inside a spell body.
    // =========================================================================

    // Routes to the correct statement parser based on the current token.
    std::unique_ptr<Stmt>          parseStatement();

    // Parses a { } block. IMPORTANT: The '{' must already be consumed by the
    // caller before this is called. parseBlock reads until '}' and consumes it.
    std::unique_ptr<BlockStmt>     parseBlock();

    // if / elif / else
    std::unique_ptr<IfStmt>        parseIfStmt();

    // fore (init; condition; increment) { }
    std::unique_ptr<ForeStmt>      parseForeStmt();

    // whirl (condition) { }
    std::unique_ptr<WhirlStmt>     parseWhirlStmt();

    // yield expr;   yield (a, b);   yield;
    std::unique_ptr<YieldStmt>     parseYieldStmt();

    // destined (condition) { }   or   destined { }   (condition optional)
    std::unique_ptr<DestinedStmt>  parseDestinedStmt();

    // my_disk <~ divine fetch_sector(...) { branches }
    std::unique_ptr<DivineStmt>    parseDivineStmt();

    // Handles expression statements (f();) and assignment statements (x = y;).
    std::unique_ptr<Stmt>          parseExprOrAssignStmt();
    std::unique_ptr<Stmt>          parseVarDeclStmt(Token typeToken, bool isPointer = false);
    
    // =========================================================================
    // EXPRESSION PARSERS (Pratt / Precedence Climbing)
    // =========================================================================

    // Top-level entry for expression parsing. Starts at lowest precedence.
    std::unique_ptr<Expr>          parseExpression();

    // Core Pratt parser. minPrecedence controls which operators are consumed.
    // Called recursively with increasing minPrecedence for right-hand operands.
    std::unique_ptr<Expr>          parsePrecedence(int minPrecedence);

    // Called when a '(' is seen after an expression — parses arguments and
    // builds a CallExpr. The '(' has already been consumed before this is called.
    std::unique_ptr<Expr>          parseCallExpr(std::unique_ptr<Expr> callee);

    std::unique_ptr<StructInitExpr> parseStructInitExpr(Token typeName, Token stanceName);

    // Returns the infix binding power (precedence level) of a token type.
    // Returns 0 for tokens that are not infix operators (stops the climb).
    // Levels match the Cgil spec v1.7 operator precedence table exactly.
    int getPrecedence(TokenType type) const;

    // =========================================================================
    // TOKEN NAVIGATION HELPERS
    // =========================================================================

    // Look at the current token without consuming it.
    Token peek() const;

    // Look one token ahead without consuming either token.
    // Used for the divine detection lookahead in parseStatement().
    // Returns END_OF_FILE token if at or past the end of the stream.
    Token peekNext() const;

    // Look two tokens ahead without consuming any token.
    // Used for the 3-token IDENT IDENT ASSIGN/SEMICOLON lookahead in
    // parseStatement() to harden bare sigil declaration detection.
    // Returns END_OF_FILE token if at or past the end of the stream.
    Token peekNextNext() const;

    // Return the most recently consumed token.
    Token previous() const;

    // True if the current token is END_OF_FILE.
    bool isAtEnd() const;

    // Consume and return the current token. Advances current by 1.
    Token advance();

    // True if the current token matches the given type (does NOT consume).
    bool check(TokenType type) const;

    // If the current token matches any of the given types, consume it and
    // return true. Otherwise return false without consuming.
    bool match(std::initializer_list<TokenType> types);

    // Consume the current token if it matches 'type', or throw a descriptive
    // error pointing at the current source location. This is the primary
    // error-detection mechanism — every "this token MUST appear here" check
    // uses consume() rather than manually calling advance().
    Token consume(TokenType type, const std::string& message);

    // Consume the current token ONLY if it is a valid Cgil type keyword or an
    // IDENT (for sigil type names). Throws a descriptive error if the current
    // token is not a type — preventing garbage like integer literals or keywords
    // from being silently accepted as type tokens during parameter and field parsing.
    //
    // Valid types: mark16, mark32, soul16, soul32, addr, flow, rune, oath,
    //              scroll, abyss, deck, tuple, IDENT (for sigil/legion names)
    //
    // Without this, a typo like `spell foo(42 x)` would accept the literal 42
    // as a type, causing a confusing crash in semantic analysis rather than a
    // clear "expected a type" error at the point of the mistake.
    Token consumeType(const std::string& message);

    // =========================================================================
    // ERROR HANDLING
    // =========================================================================

    // Build a formatted error message with line and column info, then throw.
    // [[noreturn]] tells the compiler this never returns normally, which
    // suppresses "control reaches end of non-void function" warnings in callers.
    [[noreturn]] void error(Token token, const std::string& message);

    // After catching an error, skip tokens until a safe restart point.
    // Safe boundaries: semicolons, closing braces, new declaration keywords.
    // This enables multi-error reporting in one compiler pass.
    void synchronize();
};