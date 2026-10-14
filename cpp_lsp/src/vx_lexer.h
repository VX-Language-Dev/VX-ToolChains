#pragma once
#include <string>
#include <vector>
#include <cstdint>

// VX Token types
enum class TokenType : uint32_t {
    EOF_TOKEN = 0,
    NL = 1,
    INDENT = 2,
    DEDENT = 3,
    IDENT = 10,
    INT = 11,
    FLOAT = 12,
    STRING = 13,
    KW = 20,
    PLUS = 30,
    MINUS = 31,
    STAR = 32,
    SLASH = 33,
    PERCENT = 34,
    POWER = 35,
    EQ = 36,
    NE = 37,
    LT = 38,
    GT = 39,
    LE = 40,
    GE = 41,
    ASSIGN = 42,
    PLUSEQ = 43,
    MINUSEQ = 44,
    STAREQ = 45,
    SLASHEQ = 46,
    ARROW = 50,
    AND = 51,
    OR = 52,
    NOT = 53,
    LPAREN = 60,
    RPAREN = 61,
    LBRACKET = 62,
    RBRACKET = 63,
    LBRACE = 64,
    RBRACE = 65,
    COLON = 66,
    COMMA = 67,
    DOT = 68,
    HASH = 69
};

enum class KeywordId : uint32_t {
    IF = 100,
    ELIF = 101,
    ELSE = 102,
    FOR = 103,
    WHILE = 104,
    BREAK = 105,
    CONTINUE = 106,
    FUNC = 107,
    RETURN = 108,
    TRUE = 109,
    FALSE = 110,
    NIL = 111,
    VAR = 112,
    MUT = 113,
    IMPORT = 114,
    STRUCT = 115,
    CLASS = 116,
    NEW = 117,
    EXTERN = 118,
    INT_T = 119,
    FLOAT_T = 120,
    BOOL_T = 121,
    VOID_T = 122,
    AND = 123,
    OR = 124,
    NOT_KW = 125,
    AS = 126,
    IN = 127,
    LOOP = 128,
    MATCH = 129,
    ENUM = 130,
    UNION = 131,
    MACRO = 132,
    MOVE = 133
};

struct Token {
    TokenType type;
    std::string lexeme;
    int line;
    int column;
    KeywordId kw_id;

    Token() : type(TokenType::EOF_TOKEN), line(0), column(0), kw_id(KeywordId::IF) {}
    Token(TokenType t, const std::string& l, int ln, int col, KeywordId kw = KeywordId::IF)
        : type(t), lexeme(l), line(ln), column(col), kw_id(kw) {}
};

class VXLexer {
public:
    VXLexer(const std::string& source);
    std::vector<Token> tokenize();

private:
    const std::string& source_;
    size_t pos_;
    int line_;
    int column_;
    std::vector<Token> tokens_;

    char peek();
    char peek_next();
    bool match(char expected);
    bool match_next(char expected);
    void advance();
    void skip_whitespace();
    void skip_comment();
    Token make_token(TokenType type, const std::string& lexeme);
    KeywordId check_keyword(const std::string& ident);
    void scan_identifier();
    void scan_number();
    void scan_string();
};

// Keyword keyword table
extern const std::vector<std::pair<std::string, KeywordId>> KEYWORD_TABLE;

// Convert token type to string for debugging
std::string token_type_name(TokenType type);
