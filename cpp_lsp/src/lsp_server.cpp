#include "json_utils.h"
#include "vx_lexer.h"
#include <iostream>
#include <string>
#include <map>
#include <vector>
#include <memory>
#include <algorithm>
#include <cctype>

using namespace json;

// ========== VX AST Nodes ==========

enum class NodeType {
    FUNCTION,
    VAR_DECL,
    IF,
    WHILE,
    RETURN,
    BINARY_OP,
    CALL,
    IDENT,
    INT_LITERAL,
    STRING_LITERAL,
    BLOCK,
    EXTERN_DECL
};

struct ASTNode {
    NodeType type;
    std::string name;
    std::string type_annotation;
    std::string value;
    int line = 0;
    int column = 0;
    std::vector<ASTNode*> children;
    std::vector<ASTNode*> params;
    std::vector<ASTNode*> body;
    ASTNode* left = nullptr;
    ASTNode* right = nullptr;

    ASTNode() : type(NodeType::IDENT), line(0), column(0), left(nullptr), right(nullptr) {}
    ASTNode(NodeType t, const std::string& n, const std::string& ta, const std::string& v, int ln, int col)
        : type(t), name(n), type_annotation(ta), value(v), line(ln), column(col), left(nullptr), right(nullptr) {}
};

// ========== Document State ==========

struct DocumentState {
    std::string source;
    std::vector<Token> tokens;
    std::vector<ASTNode*> ast;
    std::vector<Value> diagnostics;
};

// ========== LSP Types ==========

struct Position {
    uint32_t line = 0;
    uint32_t character = 0;
};

struct Range {
    Position start;
    Position end;
};

struct Diagnostic {
    Range range;
    int severity; // 1=Error, 2=Warning, 3=Info, 4=Hint
    std::string message;
    std::string source = "vx";
};

struct CompletionItem {
    std::string label;
    int kind = 1; // 1=Text, 3=Function, 4=Constructor, etc.
    std::string detail;
};

// ========== VX Parser ==========

class VXParser {
public:
    VXParser(const std::vector<Token>& tokens) : tokens_(tokens), pos_(0) {}
    
    std::vector<ASTNode*> parse() {
        std::vector<ASTNode*> program;
        while (pos_ < tokens_.size() && tokens_[pos_].type != TokenType::EOF_TOKEN) {
            if (tokens_[pos_].type == TokenType::NL) { pos_++; continue; }
            ASTNode* node = parseStatement();
            if (node) program.push_back(node);
        }
        return program;
    }
    
    const std::vector<Token>& tokens() const { return tokens_; }

private:
    const std::vector<Token>& tokens_;
    size_t pos_;

    bool isAtEnd() const { return pos_ >= tokens_.size() || tokens_[pos_].type == TokenType::EOF_TOKEN; }
    const Token& peek() const { return tokens_[pos_]; }
    
    void advance() { if (!isAtEnd()) pos_++; }
    bool match(TokenType type) { if (!isAtEnd() && peek().type == type) { advance(); return true; } return false; }
    
    void mergeChildren(std::vector<ASTNode*>& dest, const std::vector<ASTNode*>& src) {
        dest.insert(dest.end(), src.begin(), src.end());
    }
    
    ASTNode* parseStatement() {
        if (isAtEnd()) return nullptr;
        
        const Token& tok = peek();
        
        if (tok.type == TokenType::KW && tok.kw_id == KeywordId::EXTERN) {
            advance(); // skip extern
            ASTNode* node = new ASTNode(NodeType::EXTERN_DECL, "", "", "", tok.line, tok.column);
            // Parse function signature
            if (!match(TokenType::LPAREN)) return node;
            advance(); // skip (
            while (!isAtEnd() && peek().type != TokenType::RPAREN) {
                if (peek().type == TokenType::IDENT || peek().type == TokenType::KW) {
                    std::string name = peek().lexeme;
                    advance();
                    std::string ptype;
                    if (match(TokenType::COLON)) { if (!isAtEnd() && peek().type == TokenType::KW) { ptype = peek().lexeme; advance(); } }
                    ASTNode* param = new ASTNode(NodeType::IDENT, name, ptype, "", tok.line, tok.column);
                    node->params.push_back(param);
                }
                if (match(TokenType::COMMA)) advance();
                else break;
            }
            if (!isAtEnd() && peek().type == TokenType::RPAREN) advance();
            if (match(TokenType::ARROW)) { advance(); if (!isAtEnd() && peek().type == TokenType::KW) advance(); }
            return node;
        }
        
        if (tok.type == TokenType::KW && tok.kw_id == KeywordId::FUNC) {
            advance(); // skip func
            std::string name = peek().lexeme;
            advance();
            ASTNode* node = new ASTNode(NodeType::FUNCTION, name, "", "", tok.line, tok.column);
            if (!match(TokenType::LPAREN)) return node;
            advance(); // skip (
            while (!isAtEnd() && peek().type != TokenType::RPAREN) {
                if (peek().type == TokenType::IDENT || peek().type == TokenType::KW) {
                    std::string pname = peek().lexeme;
                    advance();
                    std::string ptype;
                    if (match(TokenType::COLON)) { if (!isAtEnd() && peek().type == TokenType::KW) { ptype = peek().lexeme; advance(); } }
                    ASTNode* param = new ASTNode(NodeType::IDENT, pname, ptype, "", tok.line, tok.column);
                    node->params.push_back(param);
                }
                if (match(TokenType::COMMA)) advance();
                else break;
            }
            if (!isAtEnd() && peek().type == TokenType::RPAREN) advance();
            if (match(TokenType::ARROW)) { advance(); if (!isAtEnd() && peek().type == TokenType::KW) { node->type_annotation = peek().lexeme; advance(); } }
            if (!isAtEnd() && peek().type == TokenType::COLON) advance(); // skip :
            node->body = parseBlock();
            return node;
        }
        
        if (tok.type == TokenType::KW && tok.kw_id == KeywordId::VAR) {
            advance(); // skip var
            std::string name = peek().lexeme;
            advance();
            ASTNode* node = new ASTNode(NodeType::VAR_DECL, name, "", "", tok.line, tok.column);
            if (match(TokenType::COLON)) { if (!isAtEnd() && peek().type == TokenType::KW) { node->type_annotation = peek().lexeme; advance(); } }
            if (match(TokenType::ASSIGN)) { /* value parsed in expression */ }
            node->children.push_back(parseExpression());
            return node;
        }
        
        if (tok.type == TokenType::KW && tok.kw_id == KeywordId::IF) {
            advance(); // skip if
            ASTNode* node = new ASTNode(NodeType::IF, "", "", "", tok.line, tok.column);
            node->children.push_back(parseExpression());
            if (!isAtEnd() && peek().type == TokenType::COLON) advance();
            mergeChildren(node->body, parseBlock());
            return node;
        }
        
        if (tok.type == TokenType::KW && tok.kw_id == KeywordId::WHILE) {
            advance(); // skip while
            ASTNode* node = new ASTNode(NodeType::WHILE, "", "", "", tok.line, tok.column);
            node->children.push_back(parseExpression());
            if (!isAtEnd() && peek().type == TokenType::COLON) advance();
            mergeChildren(node->body, parseBlock());
            return node;
        }
        
        if (tok.type == TokenType::KW && tok.kw_id == KeywordId::RETURN) {
            advance(); // skip return
            ASTNode* node = new ASTNode(NodeType::RETURN, "", "", "", tok.line, tok.column);
            if (!isAtEnd() && peek().type != TokenType::NL && peek().type != TokenType::EOF_TOKEN) {
                node->children.push_back(parseExpression());
            }
            return node;
        }
        
        // Default: parse as expression
        return parseExpression();
    }
    
    std::vector<ASTNode*> parseBlock() {
        std::vector<ASTNode*> block;
        if (!isAtEnd() && peek().type == TokenType::COLON) advance();
        while (!isAtEnd() && peek().type != TokenType::EOF_TOKEN) {
            if (peek().type == TokenType::NL) { advance(); continue; }
            if (isAtEnd()) break;
            ASTNode* stmt = parseStatement();
            if (stmt) block.push_back(stmt);
        }
        return block;
    }
    
    ASTNode* parseExpression() {
        return parseBinary(0);
    }
    
    ASTNode* parseBinary(int minPrec) {
        ASTNode* left = parsePrimary();
        if (!left) return nullptr;
        
        while (!isAtEnd()) {
            std::string op;
            int prec = 0;
            const Token& t = peek();
            
            if (t.type == TokenType::PLUS) { op = "+"; prec = 5; }
            else if (t.type == TokenType::MINUS) { op = "-"; prec = 5; }
            else if (t.type == TokenType::STAR) { op = "*"; prec = 6; }
            else if (t.type == TokenType::SLASH) { op = "/"; prec = 6; }
            else if (t.type == TokenType::PERCENT) { op = "%"; prec = 6; }
            else if (t.type == TokenType::EQ && !isAtEnd() && peek().type == TokenType::EQ) { op = "=="; prec = 3; }
            else if (t.type == TokenType::EQ) { op = "="; prec = 1; }
            else if (t.type == TokenType::NE) { op = "!="; prec = 3; }
            else if (t.type == TokenType::LT) { op = "<"; prec = 4; }
            else if (t.type == TokenType::GT) { op = ">"; prec = 4; }
            else if (t.type == TokenType::LE) { op = "<="; prec = 4; }
            else if (t.type == TokenType::GE) { op = ">="; prec = 4; }
            else if (t.type == TokenType::AND) { op = "and"; prec = 2; }
            else if (t.type == TokenType::OR) { op = "or"; prec = 1; }
            else break;
            
            if (prec < minPrec) break;
            advance();
            ASTNode* right = parseBinary(prec + 1);
            ASTNode* bin = new ASTNode(NodeType::BINARY_OP, op, "", "", t.line, t.column);
            bin->left = left;
            bin->right = right;
            left = bin;
        }
        return left;
    }
    
    ASTNode* parsePrimary() {
        if (isAtEnd()) return nullptr;
        
        const Token& t = peek();
        
        if (t.type == TokenType::INT) {
            advance();
            return new ASTNode(NodeType::INT_LITERAL, "", "", t.lexeme, t.line, t.column);
        }
        if (t.type == TokenType::FLOAT) {
            advance();
            return new ASTNode(NodeType::INT_LITERAL, "", "", t.lexeme, t.line, t.column);
        }
        if (t.type == TokenType::STRING) {
            advance();
            return new ASTNode(NodeType::STRING_LITERAL, "", "", t.lexeme, t.line, t.column);
        }
        if (t.type == TokenType::IDENT || t.type == TokenType::KW) {
            advance();
            ASTNode* node = new ASTNode(NodeType::IDENT, t.lexeme, "", "", t.line, t.column);
            // Check for function call
            if (!isAtEnd() && peek().type == TokenType::LPAREN) {
                advance(); // skip (
                while (!isAtEnd() && peek().type != TokenType::RPAREN) {
                    node->children.push_back(parseExpression());
                    if (!match(TokenType::COMMA)) break;
                    advance();
                }
                if (!isAtEnd() && peek().type == TokenType::RPAREN) advance();
                node->type = NodeType::CALL;
            }
            return node;
        }
        
        advance();
        return nullptr;
    }
};

// ========== Document Manager ==========

class DocumentManager {
public:
    void put(const std::string& uri, const std::string& source) {
        auto& doc = docs_[uri];
        doc.source = source;
        VXLexer lexer(source);
        doc.tokens = lexer.tokenize();
        VXParser parser(doc.tokens);
        doc.ast = parser.parse();
        doc.diagnostics = computeDiagnostics(doc);
    }
    
    const DocumentState* get(const std::string& uri) const {
        auto it = docs_.find(uri);
        return it != docs_.end() ? &it->second : nullptr;
    }
    
private:
    std::map<std::string, DocumentState> docs_;
    
    std::vector<Value> computeDiagnostics(const DocumentState& doc) {
        std::vector<Value> diags;
        // Basic diagnostics from tokens
        return diags;
    }
};

// ========== LSP Response Helpers ==========

Value makeResponse(int id, const Value& result) {
    Value::Object resp;
    resp["jsonrpc"] = Value(std::string("2.0"));
    resp["id"] = Value(id);
    resp["result"] = result;
    return Value(resp);
}

Value makeNotification(const std::string& method, const Value& params) {
    Value::Object notif;
    notif["jsonrpc"] = Value(std::string("2.0"));
    notif["method"] = Value(method);
    notif["params"] = params;
    return Value(notif);
}

// ========== Feature Handlers ==========

class VXLSPHandler {
public:
    VXLSPHandler() = default;
    
    Value handleInitialize(int id) {
        Value::Object caps;
        caps["textDocumentSync"] = Value(1);
        caps["completionProvider"] = Value(true);
        caps["hoverProvider"] = Value(true);
        caps["definitionProvider"] = Value(true);
        caps["documentSymbolProvider"] = Value(true);
        caps["inlayHintProvider"] = Value(true);
        Value::Object result;
        result["capabilities"] = Value(caps);
        return makeResponse(id, Value(result));
    }
    
    Value handleDidOpen(const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        std::string text = td.at("text").as_string();
        docs_.put(uri, text);
        return Value();
    }
    
    Value handleDidChange(const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        const auto& changes = params["contentChanges"].as_array();
        if (!changes.empty()) {
            std::string text = changes[0].as_object().at("text").as_string();
            docs_.put(uri, text);
        }
        return Value();
    }
    
    Value handleCompletion(int id, const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        const auto& pos = params["position"].as_object();
        uint32_t line = static_cast<uint32_t>(pos.at("line").as_int());
        uint32_t col = static_cast<uint32_t>(pos.at("character").as_int());
        
        const DocumentState* doc = docs_.get(uri);
        if (!doc) return makeResponse(id, Value());
        
        Value::Array items;
        
        // Collect symbols from AST
        for (ASTNode* node : doc->ast) {
            if (node->type == NodeType::FUNCTION || node->type == NodeType::VAR_DECL) {
                Value::Object item;
                item["label"] = Value(node->name);
                item["kind"] = Value(node->type == NodeType::FUNCTION ? 3 : 6);
                if (!node->type_annotation.empty()) {
                    item["detail"] = Value(node->type_annotation);
                }
                items.push_back(Value(item));
            }
        }
        
        // Add keywords
        const char* keywords[] = {"if", "elif", "else", "for", "while", "break", "continue",
            "func", "return", "import", "as", "struct", "class", "enum", "union",
            "new", "move", "int", "float", "double", "bool", "void", "true", "false",
            "nil", "loop", "match", "extern", "macro", "mut"};
        for (const char* kw : keywords) {
            Value::Object item;
            item["label"] = Value(kw);
            item["kind"] = Value(14); // Keyword
            items.push_back(Value(item));
        }
        
        Value::Object result;
        result["items"] = Value(items);
        return makeResponse(id, Value(result));
    }
    
    Value handleHover(int id, const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        const auto& pos = params["position"].as_object();
        uint32_t line = static_cast<uint32_t>(pos.at("line").as_int());
        uint32_t col = static_cast<uint32_t>(pos.at("character").as_int());
        
        const DocumentState* doc = docs_.get(uri);
        if (!doc) return makeResponse(id, Value());
        
        for (ASTNode* node : doc->ast) {
            if (!node->name.empty() && node->line == static_cast<int>(line)) {
                Value::Object hover;
                Value::Array contents;
                Value::Object markup;
                markup["kind"] = Value(std::string("markdown"));
                markup["value"] = Value(std::string("```vx\n" + node->name + "\n```"));
                contents.push_back(Value(markup));
                hover["contents"] = Value(contents);
                return makeResponse(id, Value(hover));
            }
        }
        
        return makeResponse(id, Value());
    }
    
    Value handleDefinition(int id, const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        
        const DocumentState* doc = docs_.get(uri);
        if (!doc) return makeResponse(id, Value());
        
        for (ASTNode* node : doc->ast) {
            if (!node->name.empty()) {
                Value::Object def;
                def["uri"] = Value(uri);
                Value::Object range;
                Value::Object start;
                start["line"] = Value(node->line);
                start["character"] = Value(node->column);
                range["start"] = Value(start);
                Value::Object end;
                end["line"] = Value(node->line);
                end["character"] = Value(node->column + 10);
                range["end"] = Value(end);
                def["range"] = Value(range);
                return makeResponse(id, Value(def));
            }
        }
        
        return makeResponse(id, Value());
    }
    
    Value handleDocumentSymbols(int id, const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        const DocumentState* doc = docs_.get(uri);
        if (!doc) return makeResponse(id, Value());
        
        Value::Array symbols;
        for (ASTNode* node : doc->ast) {
            if (node->type == NodeType::FUNCTION || node->type == NodeType::VAR_DECL) {
                Value::Object sym;
                sym["name"] = Value(node->name);
                sym["kind"] = Value(node->type == NodeType::FUNCTION ? 12 : 13); // Function/Variable
                Value::Object range;
                Value::Object start;
                start["line"] = Value(node->line);
                start["character"] = Value(node->column);
                range["start"] = Value(start);
                Value::Object end;
                end["line"] = Value(node->line);
                end["character"] = Value(node->column + 10);
                range["end"] = Value(end);
                sym["range"] = Value(range);
                sym["selectionRange"] = Value(range);
                symbols.push_back(Value(sym));
            }
        }
        
        return makeResponse(id, Value(symbols));
    }
    
    Value handleInlayHints(int id, const Value& params) {
        const auto& td = params["textDocument"].as_object();
        std::string uri = td.at("uri").as_string();
        const DocumentState* doc = docs_.get(uri);
        if (!doc) return makeResponse(id, Value());
        
        Value::Array hints;
        for (ASTNode* node : doc->ast) {
            if (node->type == NodeType::FUNCTION) {
                for (ASTNode* param : node->params) {
                    Value::Object hint;
                    Value::Object pos;
                    pos["line"] = Value(param->line);
                    pos["character"] = Value(param->column);
                    hint["position"] = Value(pos);
                    hint["label"] = Value(param->type_annotation);
                    hint["kind"] = Value(2); // Parameter
                    hints.push_back(Value(hint));
                }
            }
        }
        
        return makeResponse(id, Value(hints));
    }
    
    Value handleDiagnosticsNotify(const std::string& uri) {
        const DocumentState* doc = docs_.get(uri);
        if (!doc) return makeNotification("textDocument/publishDiagnostics", 
            Value({{"uri", Value(uri)}, {"diagnostics", Value()}}));
        return makeNotification("textDocument/publishDiagnostics",
            Value({{"uri", Value(uri)}, {"diagnostics", Value(doc->diagnostics)}}));
    }

private:
    DocumentManager docs_;
};

// ========== Main LSP Server ==========

int main() {
    std::ios::sync_with_stdio(false);
    std::cin.tie(nullptr);
    std::cout.tie(nullptr);
    
    VXLSPHandler handler;
    bool running = true;
    
    while (running) {
        // Read Content-Length header manually to handle \r\n
        int contentLength = 0;
        std::string header;
        char c;
        while (std::cin.get(c)) {
            if (c == '\n') {
                if (!header.empty() && header.back() == '\r') header.pop_back();
                if (header.empty()) break;
                if (header.rfind("Content-Length:", 0) == 0) {
                    contentLength = std::stoi(header.substr(16));
                }
                header.clear();
            } else {
                header += c;
            }
        }
        
        if (contentLength <= 0) continue;
        
        std::string body(contentLength, '\0');
        std::cin.read(&body[0], contentLength);
        
        LSPMessage msg = LSPMessage::parse(body);
        
        if (msg.method == "initialize") {
            int id = msg.id.value_or(0);
            Value resp = handler.handleInitialize(id);
            write_lsp_message(json::serialize(resp));
        }
        else if (msg.method == "initialized") {
            // No response needed
        }
        else if (msg.method == "shutdown") {
            int id = msg.id.value_or(0);
            Value resp = makeResponse(id, Value());
            write_lsp_message(json::serialize(resp));
        }
        else if (msg.method == "exit") {
            running = false;
        }
        else if (msg.method == "textDocument/didOpen") {
            handler.handleDidOpen(msg.params);
            const auto& td = msg.params["textDocument"].as_object();
            std::string uri = td.at("uri").as_string();
            write_lsp_message(json::serialize(handler.handleDiagnosticsNotify(uri)));
        }
        else if (msg.method == "textDocument/didChange") {
            handler.handleDidChange(msg.params);
        }
        else if (msg.method == "textDocument/completion") {
            int id = msg.id.value_or(0);
            Value resp = handler.handleCompletion(id, msg.params);
            write_lsp_message(json::serialize(resp));
        }
        else if (msg.method == "textDocument/hover") {
            int id = msg.id.value_or(0);
            Value resp = handler.handleHover(id, msg.params);
            write_lsp_message(json::serialize(resp));
        }
        else if (msg.method == "textDocument/definition") {
            int id = msg.id.value_or(0);
            Value resp = handler.handleDefinition(id, msg.params);
            write_lsp_message(json::serialize(resp));
        }
        else if (msg.method == "textDocument/documentSymbol") {
            int id = msg.id.value_or(0);
            Value resp = handler.handleDocumentSymbols(id, msg.params);
            write_lsp_message(json::serialize(resp));
        }
        else if (msg.method == "textDocument/inlayHint") {
            int id = msg.id.value_or(0);
            Value resp = handler.handleInlayHints(id, msg.params);
            write_lsp_message(json::serialize(resp));
        }
    }
    
    return 0;
}
