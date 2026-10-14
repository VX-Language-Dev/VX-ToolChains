/**
 * tree-sitter-vx 外部扫描器
 *
 * 负责产出缩进相关的三个外部 token（与 VX 参考词法分析器
 * vx-toolkit/src/compiler/lexer.cpp 的 handle_indent 行为一致）：
 *
 *   - _newline:  语句边界处的换行（跨过空行与整行注释后再判定）
 *   - _indent:   当前行缩进深于栈顶时入栈并产出
 *   - _dedent:   当前行缩进浅于栈顶时出栈并产出（可连续多次）
 *
 * 行为要点：
 *   - 空行与 `#` 整行注释不参与缩进比较（与参考实现一致）
 *   - 制表符按 4 列计（与参考实现一致）
 *   - 括号内（( [ {）不产生缩进/换行 token：grammar 在括号上下文中
 *     不接受任何外部 token，扫描器自然不会被调用，换行由 extras 消费
 *   - token 均为零宽（mark_end 在跳过空白之前），被跳过的字符随后
 *     由内部 lexer 以 extras 重新扫描
 */

#include "tree_sitter/parser.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdlib.h>

enum TokenType {
    NEWLINE,
    INDENT,
    DEDENT,
};

// 最大缩进层级（序列化缓冲区上限内）
#define MAX_INDENTS 128

typedef struct {
    uint16_t indents[MAX_INDENTS];
    uint32_t size;
} Scanner;

static inline void skip(TSLexer *lexer) { lexer->advance(lexer, true); }

void *tree_sitter_vx_external_scanner_create(void) {
    Scanner *scanner = calloc(1, sizeof(Scanner));
    scanner->indents[0] = 0;
    scanner->size = 1;
    return scanner;
}

void tree_sitter_vx_external_scanner_destroy(void *payload) {
    free(payload);
}

unsigned tree_sitter_vx_external_scanner_serialize(void *payload, char *buffer) {
    Scanner *scanner = (Scanner *)payload;
    unsigned size = 0;
    // 从 1 开始：栈底 0 不参与序列化（反序列化时固定重建）
    for (uint32_t i = 1; i < scanner->size && size < TREE_SITTER_SERIALIZATION_BUFFER_SIZE; i++, size++) {
        buffer[size] = (char)scanner->indents[i];
    }
    return size;
}

void tree_sitter_vx_external_scanner_deserialize(void *payload, const char *buffer, unsigned length) {
    Scanner *scanner = (Scanner *)payload;
    scanner->indents[0] = 0;
    scanner->size = 1;
    for (unsigned i = 0; i < length && scanner->size < MAX_INDENTS; i++) {
        scanner->indents[scanner->size++] = (uint16_t)(unsigned char)buffer[i];
    }
}

bool tree_sitter_vx_external_scanner_scan(void *payload, TSLexer *lexer, const bool *valid_symbols) {
    Scanner *scanner = (Scanner *)payload;

    if (!valid_symbols[NEWLINE] && !valid_symbols[INDENT] && !valid_symbols[DEDENT]) {
        return false;
    }

    // 零宽 token：先标记结束，跳过的空白随后由 extras 重新扫描
    lexer->mark_end(lexer);

    bool found_eol = false;
    uint32_t indent = 0;

    for (;;) {
        if (lexer->lookahead == '\n') {
            found_eol = true;
            indent = 0;
            skip(lexer);
        } else if (lexer->lookahead == '\r') {
            skip(lexer);
        } else if (lexer->lookahead == ' ') {
            if (found_eol) {
                indent++;
            }
            skip(lexer);
        } else if (lexer->lookahead == '\t') {
            if (found_eol) {
                indent += 4; // 与参考实现一致：制表符按 4 列计
            }
            skip(lexer);
        } else if (lexer->lookahead == '#') {
            if (!found_eol) {
                // 语句后的行尾注释：交还内部 lexer（comment 作为 extras 消费）
                return false;
            }
            // 整行注释：跳过且不参与缩进比较
            while (lexer->lookahead && lexer->lookahead != '\n') {
                skip(lexer);
            }
        } else if (lexer->eof(lexer)) {
            found_eol = true;
            indent = 0;
            break;
        } else {
            break;
        }
    }

    if (!found_eol) {
        return false;
    }

    uint32_t current = scanner->indents[scanner->size - 1];

    if (valid_symbols[INDENT] && indent > current && scanner->size < MAX_INDENTS) {
        scanner->indents[scanner->size++] = (uint16_t)indent;
        lexer->result_symbol = INDENT;
        return true;
    }

    if (valid_symbols[DEDENT] && indent < current && scanner->size > 1) {
        scanner->size--;
        lexer->result_symbol = DEDENT;
        return true;
    }

    if (valid_symbols[NEWLINE]) {
        lexer->result_symbol = NEWLINE;
        return true;
    }

    return false;
}
