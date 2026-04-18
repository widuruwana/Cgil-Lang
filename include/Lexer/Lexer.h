#pragma once

#include <string>
#include <vector>
#include <unordered_map>
#include <stdexcept>

enum class TokenType {
    // Keywords
    SPELL, SIGIL, RANK, LEGION, LEYLINE, PORTLINE,
    GRIMOIRE, PACT, CONJURE, ENDLESS, WARDEN,
    YIELD, SHATTER, SURGE, FORE, WHIRL,
    IF, ELIF, ELSE, OWN, DIVINE, DESTINED,
    STANCE, RUIN, MARK16, MARK32, SOUL16, SOUL32,
    ADDR, FLOW, RUNE, OATH, SCROLL, ABYSS,
    KEPT, FORSAKEN, DECK, TUPLE, AMPAMP,
    PIPEPIPE, CARET, LSHIFT, RSHIFT, TILDE, 
    LEQ, GEQ, PERCENT, CAST,

    // Operators
    ARROW, FAT_ARROW, DOT, STAR, SLASH, PLUS, MINUS,
    PLUS_PLUS, MINUS_MINUS, EQ, NEQ, GT, LT, WEAVE,
    REV_WEAVE, ASSIGN, PIPE, QUESTION, AMP, SCOPE,

    // Literals & Identifiers
    IDENT, INT_LIT, FLOAT_LIT, STRING_LIT,

    // Delimiters
    LBRACE, RBRACE, LPAREN, RPAREN, LBRACKET, RBRACKET,
    SEMICOLON, COLON, COMMA, AT,

    END_OF_FILE
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;
};

class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> tokenize();

private:
    std::string source;
    std::vector<Token> tokens;
    
    size_t start = 0;
    size_t current = 0;
    int line = 1;
    int column = 1;
    int startColumn = 1;

    static const std::unordered_map<std::string, TokenType> keywords;

    bool isAtEnd() const;
    char advance();
    char peek() const;
    char peekNext() const;
    bool match(char expected);
    void skipWhitespace();
    
    void scanToken();
    void string();
    void number();
    void identifier();
    
    void addToken(TokenType type);
    void addToken(TokenType type, std::string text); 
};