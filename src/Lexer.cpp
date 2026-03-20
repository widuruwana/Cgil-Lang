#include "Lexer.h"
#include <cctype>

const std::unordered_map<std::string, TokenType> Lexer::keywords = {
    {"spell", TokenType::SPELL}, {"sigil", TokenType::SIGIL},
    {"rank", TokenType::RANK}, {"legion", TokenType::LEGION},
    {"leyline", TokenType::LEYLINE}, {"portline", TokenType::PORTLINE},
    {"grimoire", TokenType::GRIMOIRE}, {"pact", TokenType::PACT},
    {"conjure", TokenType::CONJURE}, {"endless", TokenType::ENDLESS},
    {"warden", TokenType::WARDEN}, {"yield", TokenType::YIELD},
    {"shatter", TokenType::SHATTER}, {"surge", TokenType::SURGE},
    {"fore", TokenType::FORE}, {"whirl", TokenType::WHIRL},
    {"if", TokenType::IF}, {"elif", TokenType::ELIF},
    {"else", TokenType::ELSE}, {"own", TokenType::OWN},
    {"divine", TokenType::DIVINE}, {"destined", TokenType::DESTINED},
    {"stance", TokenType::STANCE}, {"ruin", TokenType::RUIN},
    {"mark16", TokenType::MARK16}, {"mark32", TokenType::MARK32},
    {"soul16", TokenType::SOUL16}, {"soul32", TokenType::SOUL32},
    {"addr", TokenType::ADDR}, {"flow", TokenType::FLOW},
    {"rune", TokenType::RUNE}, {"oath", TokenType::OATH},
    {"scroll", TokenType::SCROLL}, {"abyss", TokenType::ABYSS},
    {"kept", TokenType::KEPT}, {"forsaken", TokenType::FORSAKEN},
    {"deck", TokenType::DECK}, {"tuple", TokenType::TUPLE}
};

Lexer::Lexer(std::string source) : source(std::move(source)) {}

std::vector<Token> Lexer::tokenize() {
    while (!isAtEnd()) {
        skipWhitespace();
        if (isAtEnd()) break;
        
        start = current;
        startColumn = column; 
        scanToken();
    }
    tokens.push_back({TokenType::END_OF_FILE, "", line, column});
    return tokens;
}

void Lexer::scanToken() {
    char c = advance();

    switch (c) {
        // Single-character tokens
        case '(': addToken(TokenType::LPAREN); break;
        case ')': addToken(TokenType::RPAREN); break;
        case '{': addToken(TokenType::LBRACE); break;
        case '}': addToken(TokenType::RBRACE); break;
        case '[': addToken(TokenType::LBRACKET); break;
        case ']': addToken(TokenType::RBRACKET); break;
        case ';': addToken(TokenType::SEMICOLON); break;
        case ',': addToken(TokenType::COMMA); break;
        case '.': addToken(TokenType::DOT); break;
        case '*': addToken(TokenType::STAR); break;
        case '+': addToken(TokenType::PLUS); break;
        case '?': addToken(TokenType::QUESTION); break;
        case '@': addToken(TokenType::AT); break;
        case '&': addToken(TokenType::AMP); break;
        case '|': addToken(TokenType::PIPE); break;

        // One or two character operators
        case ':': addToken(match(':') ? TokenType::SCOPE : TokenType::COLON); break;
        case '=': addToken(match('=') ? TokenType::EQ : TokenType::ASSIGN); break;
        case '>': addToken(TokenType::GT); break;
        
        case '!': 
            if (match('=')) {
                addToken(TokenType::NEQ);
            } else {
                throw std::runtime_error("Unexpected '!' at line " + std::to_string(line) + ", col " + std::to_string(column));
            }
            break;
            
        case '<': 
            addToken(match('~') ? TokenType::REV_WEAVE : TokenType::LT); 
            break;
        case '~': 
            if (match('>')) {
                addToken(TokenType::WEAVE);
            } else {
                throw std::runtime_error("Unexpected '~' at line " + std::to_string(line) + ". Did you mean '~>'?");
            }
            break;
        case '-': 
            addToken(match('>') ? TokenType::ARROW : TokenType::MINUS); 
            break;

        case '/':
            if (match('/')) {
                while (peek() != '\n' && !isAtEnd()) advance();
            } else {
                addToken(TokenType::SLASH);
            }
            break;

        case '"': string(); break;

        default:
            if (std::isdigit(c)) {
                number();
            } else if (std::isalpha(c) || c == '_') {
                identifier();
            } else {
                throw std::runtime_error("Unexpected character at line " + std::to_string(line) + ", col " + std::to_string(column));
            }
            break;
    }
}

void Lexer::identifier() {
    while (std::isalnum(peek()) || peek() == '_') advance();

    std::string text = source.substr(start, current - start);
    TokenType type = TokenType::IDENT;
    
    auto it = keywords.find(text);
    if (it != keywords.end()) {
        type = it->second;
    }
    addToken(type);
}

// Precondition: scanToken() has already consumed the first digit via advance().
// Therefore, source[current - 1] is guaranteed to be that first digit.
void Lexer::number() {
    if (source[current - 1] == '0' && (peek() == 'x' || peek() == 'X')) {
        advance(); // consume 'x'
        if (!std::isxdigit(peek())) {
            throw std::runtime_error("Malformed hex literal at line " + std::to_string(line) + ", col " + std::to_string(column));
        }
        while (std::isxdigit(peek())) advance();
    } else {
        while (std::isdigit(peek())) advance();
    }
    addToken(TokenType::INT_LIT);
}

void Lexer::string() {
    int stringStartLine = line; // Capture starting line for accurate EOF error reporting

    while (peek() != '"' && !isAtEnd()) {
        if (peek() == '\n') {
            line++;
            column = 0; // advance() will increment this to 1
            advance();
        } else if (peek() == '\\') {
            advance(); // Consume the backslash
            if (!isAtEnd()) advance(); // Consume the escaped character, skipping the quote check
        } else {
            advance(); // Regular character
        }
    }

    if (isAtEnd()) {
        throw std::runtime_error("Unterminated string starting at line " + std::to_string(stringStartLine));
    }

    advance(); // Consume the closing "
    
    /* * CODEGEN CONTRACT NOTE:
     * Escape sequences (e.g. \n) are captured here as raw text (two characters: '\' and 'n'). 
     * When emitting `Cgil_Scroll` string literals during CodeGen, the len field 
     * must calculate the exact byte-length of the string natively interpreted by C,
     * excluding the null terminator. 
     */
    std::string value = source.substr(start + 1, current - start - 2);
    addToken(TokenType::STRING_LIT, value); 
}

bool Lexer::match(char expected) {
    if (isAtEnd()) return false;
    if (source[current] != expected) return false;
    current++;
    column++;
    return true;
}

char Lexer::peek() const {
    if (isAtEnd()) return '\0';
    return source[current];
}

// Reserved for future use in the Parser or extended Lexer lookahead.
// Not currently utilized in the core Lexer loop.
char Lexer::peekNext() const {
    if (current + 1 >= source.length()) return '\0';
    return source[current + 1];
}

char Lexer::advance() {
    column++;
    return source[current++];
}

bool Lexer::isAtEnd() const {
    return current >= source.length();
}

void Lexer::skipWhitespace() {
    while (true) {
        char c = peek();
        switch (c) {
            case ' ':
            case '\r':
            case '\t':
                advance();
                break;
            case '\n':
                line++;
                column = 0; // advance() will increment this to 1
                advance();
                break;
            default:
                return;
        }
    }
}

void Lexer::addToken(TokenType type) {
    std::string text = source.substr(start, current - start);
    tokens.push_back({type, text, line, startColumn});
}

void Lexer::addToken(TokenType type, std::string text) {
    tokens.push_back({type, std::move(text), line, startColumn});
}