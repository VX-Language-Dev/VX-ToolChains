#include "vx_lexer.h"
#include <cctype>
#include <algorithm>

const std::vector<std::pair<std::string, KeywordId>> KEYWORD_TABLE = {
    {"if", KeywordId::IF}, {"elif", KeywordId::ELIF}, {"else", KeywordId::ELSE},
    {"for", KeywordId::FOR}, {"while", KeywordId::WHILE}, {"break", KeywordId::BREAK},
    {"continue", KeywordId::CONTINUE}, {"func", KeywordId::FUNC}, {"return", KeywordId::RETURN},
    {"true", KeywordId::TRUE}, {"false", KeywordId::FALSE}, {"nil", KeywordId::NIL},
    {"var", KeywordId::VAR}, {"mut", KeywordId::MUT}, {"import", KeywordId::IMPORT},
    {"struct", KeywordId::STRUCT}, {"class", KeywordId::CLASS}, {"new", KeywordId::NEW},
    {"extern", KeywordId::EXTERN}, {"int", KeywordId::INT_T}, {"float", KeywordId::FLOAT_T},
    {"bool", KeywordId::BOOL_T}, {"void", KeywordId::VOID_T}, {"and", KeywordId::AND},
    {"or", KeywordId::OR}, {"not", KeywordId::NOT_KW}, {"as", KeywordId::AS},
    {"in", KeywordId::IN}, {"loop", KeywordId::LOOP}, {"match", KeywordId::MATCH},
    {"enum", KeywordId::ENUM}, {"union", KeywordId::UNION}, {"macro", KeywordId::MACRO},
    {"move", KeywordId::MOVE}
};

std::string token_type_name(TokenType type) {
    switch (type) {
        case TokenType::EOF_TOKEN: return "EOF";
        case TokenType::NL: return "NL";
        case TokenType::INDENT: return "INDENT";
        case TokenType::DEDENT: return "DEDENT";
        case TokenType::IDENT: return "IDENT";
        case TokenType::INT: return "INT";
        case TokenType::FLOAT: return "FLOAT";
        case TokenType::STRING: return "STRING";
        case TokenType::KW: return "KW";
        case TokenType::PLUS: return "PLUS";
        case TokenType::MINUS: return "MINUS";
        case TokenType::STAR: return "STAR";
        case TokenType::SLASH: return "SLASH";
        case TokenType::PERCENT: return "PERCENT";
        case TokenType::POWER: return "POWER";
        case TokenType::EQ: return "EQ";
        case TokenType::NE: return "NE";
        case TokenType::LT: return "LT";
        case TokenType::GT: return "GT";
        case TokenType::LE: return "LE";
        case TokenType::GE: return "GE";
        case TokenType::ASSIGN: return "ASSIGN";
        case TokenType::PLUSEQ: return "PLUSEQ";
        case TokenType::MINUSEQ: return "MINUSEQ";
        case TokenType::STAREQ: return "STAREQ";
        case TokenType::SLASHEQ: return "SLASHEQ";
        case TokenType::ARROW: return "ARROW";
        case TokenType::AND: return "AND";
        case TokenType::OR: return "OR";
        case TokenType::NOT: return "NOT";
        case TokenType::LPAREN: return "LPAREN";
        case TokenType::RPAREN: return "RPAREN";
        case TokenType::LBRACKET: return "LBRACKET";
        case TokenType::RBRACKET: return "RBRACKET";
        case TokenType::LBRACE: return "LBRACE";
        case TokenType::RBRACE: return "RBRACE";
        case TokenType::COLON: return "COLON";
        case TokenType::COMMA: return "COMMA";
        case TokenType::DOT: return "DOT";
        case TokenType::HASH: return "HASH";
        default: return "UNKNOWN";
    }
}

VXLexer::VXLexer(const std::string& source)
    : source_(source), pos_(0), line_(1), column_(1) {}

char VXLexer::peek() {
    if (pos_ >= source_.size()) return '\0';
    return source_[pos_];
}

char VXLexer::peek_next() {
    if (pos_ + 1 >= source_.size()) return '\0';
    return source_[pos_ + 1];
}

bool VXLexer::match(char expected) {
    if (peek() == expected) { advance(); return true; }
    return false;
}

bool VXLexer::match_next(char expected) {
    if (peek_next() == expected) return true;
    return false;
}

void VXLexer::advance() {
    if (source_[pos_] == '\n') { line_++; column_ = 1; }
    else { column_++; }
    pos_++;
}

void VXLexer::skip_whitespace() {
    while (pos_ < source_.size()) {
        char c = peek();
        if (c == ' ' || c == '\r' || c == '\t') { advance(); }
        else if (c == '\n') { advance(); }
        else if (c == '#') { skip_comment(); }
        else { break; }
    }
}

void VXLexer::skip_comment() {
    while (pos_ < source_.size() && peek() != '\n') {
        advance();
    }
}

Token VXLexer::make_token(TokenType type, const std::string& lexeme) {
    return Token(type, lexeme, line_, column_);
}

KeywordId VXLexer::check_keyword(const std::string& ident) {
    for (const auto& [kw, id] : KEYWORD_TABLE) {
        if (kw == ident) return id;
    }
    return KeywordId::IF; // default
}

void VXLexer::scan_identifier() {
    std::string ident;
    while (pos_ < source_.size() && (std::isalnum(source_[pos_]) || source_[pos_] == '_')) {
        ident += source_[pos_];
        advance();
    }
    KeywordId kw = check_keyword(ident);
    if (kw != KeywordId::IF) {
        tokens_.push_back(Token(TokenType::KW, ident, line_, column_ - ident.size(), kw));
    } else {
        tokens_.push_back(Token(TokenType::IDENT, ident, line_, column_ - ident.size()));
    }
}

void VXLexer::scan_number() {
    std::string num;
    bool is_float = false;
    while (pos_ < source_.size() && (std::isdigit(source_[pos_]) || source_[pos_] == '.')) {
        if (source_[pos_] == '.') is_float = true;
        num += source_[pos_];
        advance();
    }
    tokens_.push_back(is_float ? make_token(TokenType::FLOAT, num) : make_token(TokenType::INT, num));
}

void VXLexer::scan_string() {
    advance(); // skip opening quote
    std::string str;
    while (pos_ < source_.size() && peek() != '"') {
        if (peek() == '\\') {
            advance();
            if (peek() == 'n') str += '\n';
            else if (peek() == 't') str += '\t';
            else if (peek() == '\\') str += '\\';
            else if (peek() == '"') str += '"';
            else str += peek();
            advance();
        } else {
            str += peek();
            advance();
        }
    }
    if (pos_ < source_.size()) advance(); // skip closing quote
    tokens_.push_back(make_token(TokenType::STRING, str));
}

std::vector<Token> VXLexer::tokenize() {
    tokens_.clear();
    pos_ = 0;
    line_ = 1;
    column_ = 1;

    while (pos_ < source_.size()) {
        skip_whitespace();
        if (pos_ >= source_.size()) break;

        char c = peek();

        // Single and multi-character operators
        switch (c) {
            case '+':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::PLUSEQ, "+=")); }
                else { advance(); tokens_.push_back(make_token(TokenType::PLUS, "+")); }
                break;
            case '-':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::MINUSEQ, "-=")); }
                else if (match_next('>')) { advance(); advance(); tokens_.push_back(make_token(TokenType::ARROW, "->")); }
                else { advance(); tokens_.push_back(make_token(TokenType::MINUS, "-")); }
                break;
            case '*':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::STAREQ, "*=")); }
                else { advance(); tokens_.push_back(make_token(TokenType::STAR, "*")); }
                break;
            case '/':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::SLASHEQ, "/=")); }
                else { advance(); tokens_.push_back(make_token(TokenType::SLASH, "/")); }
                break;
            case '%':
                advance(); tokens_.push_back(make_token(TokenType::PERCENT, "%"));
                break;
            case '^':
                advance(); tokens_.push_back(make_token(TokenType::POWER, "^"));
                break;
            case '=':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::EQ, "==")); }
                else { advance(); tokens_.push_back(make_token(TokenType::ASSIGN, "=")); }
                break;
            case '!':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::NE, "!=")); }
                else { advance(); tokens_.push_back(make_token(TokenType::NOT, "!")); }
                break;
            case '<':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::LE, "<=")); }
                else { advance(); tokens_.push_back(make_token(TokenType::LT, "<")); }
                break;
            case '>':
                if (match_next('=')) { advance(); advance(); tokens_.push_back(make_token(TokenType::GE, ">=")); }
                else { advance(); tokens_.push_back(make_token(TokenType::GT, ">")); }
                break;
            case ':':
                advance(); tokens_.push_back(make_token(TokenType::COLON, ":"));
                break;
            case ',':
                advance(); tokens_.push_back(make_token(TokenType::COMMA, ","));
                break;
            case '.':
                advance(); tokens_.push_back(make_token(TokenType::DOT, "."));
                break;
            case '(':
                advance(); tokens_.push_back(make_token(TokenType::LPAREN, "("));
                break;
            case ')':
                advance(); tokens_.push_back(make_token(TokenType::RPAREN, ")"));
                break;
            case '[':
                advance(); tokens_.push_back(make_token(TokenType::LBRACKET, "["));
                break;
            case ']':
                advance(); tokens_.push_back(make_token(TokenType::RBRACKET, "]"));
                break;
            case '{':
                advance(); tokens_.push_back(make_token(TokenType::LBRACE, "{"));
                break;
            case '}':
                advance(); tokens_.push_back(make_token(TokenType::RBRACE, "}"));
                break;
            case '&':
                advance(); tokens_.push_back(make_token(TokenType::AND, "&"));
                break;
            case '|':
                advance(); tokens_.push_back(make_token(TokenType::OR, "|"));
                break;
            case '#':
                skip_comment();
                break;
            case '"':
                scan_string();
                break;
            default:
                if (std::isdigit(c)) scan_number();
                else if (std::isalpha(c) || c == '_') scan_identifier();
                else { advance(); } // skip unknown chars
        }
    }

    tokens_.push_back(Token(TokenType::EOF_TOKEN, "", line_, column_));
    return tokens_;
}
