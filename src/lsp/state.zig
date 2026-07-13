const std = @import("std");
const Allocator = std.mem.Allocator;

const Stmt = @import("../parser/ast.zig").Stmt;
const Token = @import("../token.zig").Token;

pub const DocumentState = struct {
    source: []u8,
    tokens: []Token,
    ast: []Stmt,

    pub fn init(allocator: Allocator) DocumentState {
        _ = allocator;
        return DocumentState{
            .source = "",
            .tokens = &[0]Token{},
            .ast = &[0]Stmt{},
        };
    }
};

pub const BackendState = struct {
    allocator: Allocator,
    documents: std.StringHashMap(DocumentState),

    pub fn init(allocator: Allocator) BackendState {
        return BackendState{
            .allocator = allocator,
            .documents = std.StringHashMap(DocumentState).init(allocator),
        };
    }

    pub fn deinit(self: *BackendState) void {
        var it = self.documents.iterator();
        while (it.next()) |entry| {
            entry.value_ptr.*.deinit();
        }
        self.documents.deinit();
    }
};
