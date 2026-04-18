#include "../../include/Lexer/Lexer.h"
#include <cctype>

const std::unordered_map<std::string, TokenType> Lexer::keywords = {
    {"spell", TokenType::SPELL},
    {"sigil", TokenType::SIGIL},
    {"rank", TokenType::RANK},
    {"legion", TokenType::LEGION},
    {"leyline", TokenType::LEYLINE},
    {"portline", TokenType::PORTLINE},
    {"grimoire", TokenType::GRIMOIRE},
    {"pact", TokenType::PACT},
    {"conjure", TokenType::CONJURE},
    {"endless", TokenType::ENDLESS},
    {"warden", TokenType::WARDEN},
    {"yield", TokenType::YIELD},
    {"shatter", TokenType::SHATTER},
    {"surge", TokenType::SURGE},
    {"fore", TokenType::FORE},
    {"whirl", TokenType::WHIRL},
    {"if", TokenType::IF},
    {"elif", TokenType::ELIF},
    {"else", TokenType::ELSE},
    {"own", TokenType::OWN},
    {"divine", TokenType::DIVINE},
    {"destined", TokenType::DESTINED},
    {"stance", TokenType::STANCE},
    {"ruin", TokenType::RUIN},
    {"mark16", TokenType::MARK16},
    {"mark32", TokenType::MARK32},
    {"soul16", TokenType::SOUL16},
    {"soul32", TokenType::SOUL32},
    {"addr", TokenType::ADDR},
    {"flow", TokenType::FLOW},
    {"rune", TokenType::RUNE},
    {"oath", TokenType::OATH},
    {"scroll", TokenType::SCROLL},
    {"abyss", TokenType::ABYSS},
    {"kept", TokenType::KEPT},
    {"forsaken", TokenType::FORSAKEN},
    {"deck", TokenType::DECK},
    {"tuple", TokenType::TUPLE},
    {"cast", TokenType::CAST}
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
        case '?': addToken(TokenType::QUESTION); break;
        case '@': addToken(TokenType::AT); break;
        case '^': addToken(TokenType::CARET); break;
        case '%': addToken(TokenType::PERCENT); break;

        // One or two character operators
        case ':': addToken(match(':') ? TokenType::SCOPE : TokenType::COLON); break;
        case '=': 
            if (match('=')) addToken(TokenType::EQ);
            else if (match('>')) addToken(TokenType::FAT_ARROW);
            else addToken(TokenType::ASSIGN);
            break;
            
        case '!': 
            if (match('=')) addToken(TokenType::NEQ);
            else throw std::runtime_error("Unexpected '!' at line " + std::to_string(line) + ", col " + std::to_string(column));
            break;

        case '&': 
            addToken(match('&') ? TokenType::AMPAMP : TokenType::AMP);
            break;

        case '|': 
            addToken(match('|') ? TokenType::PIPEPIPE : TokenType::PIPE);
            break;

        case '~': 
            addToken(match('>') ? TokenType::WEAVE : TokenType::TILDE);
            break;

        case '<': 
            if (match('~')) addToken(TokenType::REV_WEAVE);
            else if (match('<')) addToken(TokenType::LSHIFT);
            else if (match('=')) addToken(TokenType::LEQ);
            else addToken(TokenType::LT); 
            break;

        case '>': 
            if (match('>')) addToken(TokenType::RSHIFT);
            else if (match('=')) addToken(TokenType::GEQ);
            else addToken(TokenType::GT);
            break;

        case '+': addToken(match('+') ? TokenType::PLUS_PLUS : TokenType::PLUS); break;
        case '-': 
            if (match('-')) addToken(TokenType::MINUS_MINUS);
            else if (match('>')) addToken(TokenType::ARROW);
            else addToken(TokenType::MINUS); 
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
        addToken(TokenType::INT_LIT);
    } else {
        while (std::isdigit(peek())) advance();
        
        // --- THE FLOAT ---
        // Look for a decimal point followed by at least one digit (e.g., 3.14)
        if (peek() == '.' && std::isdigit(peekNext())) {
            advance(); // Consume the '.'
            while (std::isdigit(peek())) advance();
            addToken(TokenType::FLOAT_LIT);
            return;
        }
        
        addToken(TokenType::INT_LIT);
    }
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
                column = 0;
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