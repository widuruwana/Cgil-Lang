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
    KEPT, FORSAKEN, DECK, TUPLE,

    // Operators
    ARROW, DOT, STAR, SLASH, PLUS, MINUS,
    EQ, NEQ, GT, LT, WEAVE, REV_WEAVE, ASSIGN,
    PIPE, QUESTION, AMP, SCOPE,

    // Literals & Identifiers
    IDENT, INT_LIT, STRING_LIT,

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

/* * PARSER CONTRACT NOTE:
 * The Lexer emits 'LT' for the '<' character. It is the Parser's responsibility 
 * to determine from context whether 'LT' represents a less-than operator 
 * (e.g., `if (a < b)`) or a pattern/generic boundary (e.g., `ruin<DiskError>`).
 */

class Lexer {
public:
    explicit Lexer(std::string source);
    std::vector<Token> tokenize();

private:
    std::string source;
    std::vector<Token> tokens;
    
    int start = 0;
    int current = 0;
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