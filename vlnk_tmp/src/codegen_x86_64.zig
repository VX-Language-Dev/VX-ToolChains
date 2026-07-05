// ==================== x86_64 架构代码生成后端 ====================

const std = @import("std");
const codebuf = @import("codebuf");

pub const Backend = struct {
    pub const word_size: u32 = 8;
    pub const ARG_REGS = [6]usize{ 7, 6, 2, 1, 8, 9 }; // rdi, rsi, rdx, rcx, r8, r9
    pub const RESULT: usize = 0; // rax
    pub const SCRATCH: usize = 3; // rbx
    pub const SCRATCH2: usize = 1; // rcx
    pub const FP: usize = 5; // rbp

    pub const Cond = enum(u8) { eq = 0, ne = 1, lt = 2, gt = 3, le = 4, ge = 5 };

    pub const Reg = enum(u8) {
        rax = 0, rcx = 1, rdx = 2, rbx = 3, rsp = 4, rbp = 5, rsi = 6, rdi = 7,
        r8 = 8, r9 = 9, r10 = 10, r11 = 11, r12 = 12, r13 = 13, r14 = 14, r15 = 15,
    };

    fn toReg(reg: usize) Reg { return @as(Reg, @enumFromInt(@as(u8, @intCast(reg)))); }

    pub fn emitRexW(cb: *codebuf.CodeBuffer, r: u8, m: u8) !void {
        const w: u8 = 0x48;
        const rr: u8 = if (r >= 8) 0x04 else 0x00;
        const mm: u8 = if (m >= 8) 0x01 else 0x00;
        try cb.append(w | rr | mm);
    }

    pub fn modRM(mod: u8, reg: u8, rm: u8) u8 {
        return ((mod & 3) << 6) | ((reg & 7) << 3) | (rm & 7);
    }

    pub fn emitPushReg(cb: *codebuf.CodeBuffer, reg: Reg) !void {
        const r = @intFromEnum(reg);
        if (r >= 8) try cb.append(0x41);
        try cb.append(0x50 + (r & 7));
    }

    pub fn emitPopReg(cb: *codebuf.CodeBuffer, reg: Reg) !void {
        const r = @intFromEnum(reg);
        if (r >= 8) try cb.append(0x41);
        try cb.append(0x58 + (r & 7));
    }

    pub fn emitMovRegImm64(cb: *codebuf.CodeBuffer, reg: Reg, value: u64) !void {
        const r = @intFromEnum(reg);
        try emitRexW(cb, r, 0);
        try cb.append(0xB8 + (r & 7));
        try cb.emitU64LE(value);
    }

    pub fn emitMemOperand(cb: *codebuf.CodeBuffer, reg: u8, base: u8, offset: i32) !void {
        const r = reg & 7;
        const b = base & 7;
        if (offset == 0 and b != 5) {
            try cb.append(modRM(0, r, b));
        } else if (offset >= -128 and offset <= 127) {
            try cb.append(modRM(1, r, b));
            try cb.append(@bitCast(@as(i8, @intCast(offset))));
        } else {
            try cb.append(modRM(2, r, b));
            try cb.emitI32LE(offset);
        }
    }

    pub fn emitMovRegMem(cb: *codebuf.CodeBuffer, dst: Reg, base: Reg, offset: i32) !void {
        const d = @intFromEnum(dst);
        const b = @intFromEnum(base);
        try emitRexW(cb, d, b);
        try cb.append(0x8B);
        try emitMemOperand(cb, d, b, offset);
    }

    pub fn emitMovMemReg(cb: *codebuf.CodeBuffer, base: Reg, offset: i32, src: Reg) !void {
        const s = @intFromEnum(src);
        const b = @intFromEnum(base);
        try emitRexW(cb, s, b);
        try cb.append(0x89);
        try emitMemOperand(cb, s, b, offset);
    }

    pub fn emitMovRegReg(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        const d = @intFromEnum(toReg(dst));
        const s = @intFromEnum(toReg(src));
        try emitRexW(cb, s, d);
        try cb.append(0x89);
        try cb.append(modRM(3, s, d));
    }

    pub fn emitAluRegReg(cb: *codebuf.CodeBuffer, opcode: u8, dst: Reg, src: Reg) !void {
        const d = @intFromEnum(dst);
        const s = @intFromEnum(src);
        try emitRexW(cb, s, d);
        try cb.append(opcode);
        try cb.append(modRM(3, s, d));
    }

    pub fn emitAddRegReg(cb: *codebuf.CodeBuffer, dst: Reg, src: Reg) !void { try emitAluRegReg(cb, 0x01, dst, src); }
    pub fn emitSubRegReg(cb: *codebuf.CodeBuffer, dst: Reg, src: Reg) !void { try emitAluRegReg(cb, 0x29, dst, src); }
    pub fn emitAndRegReg(cb: *codebuf.CodeBuffer, dst: Reg, src: Reg) !void { try emitAluRegReg(cb, 0x21, dst, src); }
    pub fn emitOrRegReg(cb: *codebuf.CodeBuffer, dst: Reg, src: Reg) !void { try emitAluRegReg(cb, 0x09, dst, src); }
    pub fn emitXorRegReg(cb: *codebuf.CodeBuffer, dst: Reg, src: Reg) !void { try emitAluRegReg(cb, 0x31, dst, src); }
    pub fn emitCmpRegReg(cb: *codebuf.CodeBuffer, a: Reg, b: Reg) !void { try emitAluRegReg(cb, 0x39, a, b); }
    pub fn emitTestRegReg(cb: *codebuf.CodeBuffer, a: Reg, b: Reg) !void { try emitAluRegReg(cb, 0x85, a, b); }

    pub fn emitIMulRegReg(cb: *codebuf.CodeBuffer, dst: Reg, src: Reg) !void {
        const d = @intFromEnum(dst);
        const s = @intFromEnum(src);
        try emitRexW(cb, d, s);
        try cb.appendSlice(&[_]u8{ 0x0F, 0xAF });
        try cb.append(modRM(3, d, s));
    }

    pub fn emitIDivReg(cb: *codebuf.CodeBuffer, divisor: Reg) !void {
        const d = @intFromEnum(divisor);
        try emitRexW(cb, 0, d);
        try cb.append(0xF7);
        try cb.append(modRM(3, 7, d));
    }

    pub fn emitNegReg(cb: *codebuf.CodeBuffer, reg: Reg) !void {
        const r = @intFromEnum(reg);
        try emitRexW(cb, 0, r);
        try cb.append(0xF7);
        try cb.append(modRM(3, 3, r));
    }

    pub fn emitSubImm32(cb: *codebuf.CodeBuffer, reg: Reg, value: i32) !void {
        const r = @intFromEnum(reg);
        try emitRexW(cb, 0, r);
        try cb.append(0x81);
        try cb.append(modRM(3, 5, r));
        try cb.emitI32LE(value);
    }

    pub fn emitSetcc(cb: *codebuf.CodeBuffer, reg: Reg, cc: u8) !void {
        const r = @intFromEnum(reg);
        if (r >= 8) try cb.append(0x41);
        try cb.append(0x0F);
        try cb.append(0x90 + cc);
        try cb.append(modRM(3, 0, r & 7));
        if (r >= 8) try cb.append(0x41);
        try cb.append(0x0F);
        try cb.append(0xB6);
        try cb.append(modRM(3, r & 7, r & 7));
    }

    pub fn emitJmpRel32(cb: *codebuf.CodeBuffer) !usize {
        try cb.append(0xE9);
        const pos = cb.len();
        try cb.emitU32LE(0);
        return pos;
    }

    pub fn emitJccRel32(cb: *codebuf.CodeBuffer, cond: Cond) !usize {
        const cc: u8 = switch (cond) {
            .eq => 4, .ne => 5, .lt => 12, .gt => 15, .le => 14, .ge => 13,
        };
        try cb.append(0x0F);
        try cb.append(0x80 + cc);
        const pos = cb.len();
        try cb.emitU32LE(0);
        return pos;
    }

    pub fn emitCallRel32(cb: *codebuf.CodeBuffer) !usize {
        try cb.append(0xE8);
        const pos = cb.len();
        try cb.emitU32LE(0);
        return pos;
    }

    pub fn emitRet(cb: *codebuf.CodeBuffer) !void { try cb.append(0xC3); }
    pub fn emitSyscall(cb: *codebuf.CodeBuffer) !void { try cb.appendSlice(&[_]u8{ 0x0F, 0x05 }); }

    pub fn emitLeaRbp(cb: *codebuf.CodeBuffer, dst: Reg, offset: i32) !void {
        const d = @intFromEnum(dst);
        const b = @intFromEnum(Reg.rbp);
        try emitRexW(cb, d, b);
        try cb.append(0x8D);
        try emitMemOperand(cb, d, b, offset);
    }

    pub fn emitBuiltin(cb: *codebuf.CodeBuffer, name: []const u8) !void {
        if (std.mem.eql(u8, name, "exit")) {
            try emitMovRegImm64(cb, .rax, 60);
            try emitSyscall(cb);
        } else if (std.mem.eql(u8, name, "out")) {
            try emitPushReg(cb, .rbp);
            try emitMovRegReg(cb, @intFromEnum(Reg.rbp), @intFromEnum(Reg.rsp));
            try emitSubImm32(cb, .rsp, 16);
            try emitMovMemReg(cb, .rbp, -8, .rdi);
            try emitMovRegImm64(cb, .rax, 1);
            try emitMovRegImm64(cb, .rdi, 1);
            try emitLeaRbp(cb, .rsi, -8);
            try emitMovRegImm64(cb, .rdx, 4);
            try emitSyscall(cb);
            try emitMovRegReg(cb, @intFromEnum(Reg.rsp), @intFromEnum(Reg.rbp));
            try emitPopReg(cb, .rbp);
            try emitRet(cb);
        } else {
            try emitRet(cb);
        }
    }

    // ========== 后端通用接口 ==========

    pub fn emitStoreToSlot(cb: *codebuf.CodeBuffer, offset: i32, reg: usize) !void {
        try emitMovMemReg(cb, .rbp, offset, toReg(reg));
    }
    pub fn emitLoadFromSlot(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
        try emitMovRegMem(cb, toReg(reg), .rbp, offset);
    }
    pub fn emitLoadFP(cb: *codebuf.CodeBuffer, reg: usize) !void {
        try emitMovRegReg(cb, reg, @intFromEnum(Reg.rbp));
    }
    pub fn emitTest(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = toReg(reg);
        try emitTestRegReg(cb, r, r);
    }
    pub fn emitNot(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = toReg(reg);
        try emitTestRegReg(cb, r, r);
        try emitSetcc(cb, r, 4);
    }
    pub fn emitNeg(cb: *codebuf.CodeBuffer, reg: usize) !void {
        try emitNegReg(cb, toReg(reg));
    }
    pub fn emitLeaFP(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
        try emitLeaRbp(cb, toReg(reg), offset);
    }
    pub fn emitSubImm(cb: *codebuf.CodeBuffer, reg: usize, value: i32) !void {
        try emitSubImm32(cb, toReg(reg), value);
    }
    pub fn emitLoadImm64(cb: *codebuf.CodeBuffer, reg: usize, value: u64) !void {
        try emitMovRegImm64(cb, toReg(reg), value);
    }
    pub fn emitXorReg(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = toReg(reg);
        try emitXorRegReg(cb, r, r);
    }
    pub fn emitAdd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitAddRegReg(cb, toReg(dst), toReg(src));
    }
    pub fn emitSub(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitSubRegReg(cb, toReg(dst), toReg(src));
    }
    pub fn emitMul(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitIMulRegReg(cb, toReg(dst), toReg(src));
    }
    pub fn emitDiv(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        _ = dst;
        try emitIDivReg(cb, toReg(src));
    }
    pub fn emitMod(cb: *codebuf.CodeBuffer, _: usize, src: usize, _: usize) !void {
        try emitIDivReg(cb, toReg(src));
    }
    pub fn emitCmp(cb: *codebuf.CodeBuffer, a: usize, b: usize) !void {
        try emitCmpRegReg(cb, toReg(a), toReg(b));
    }
    pub fn emitSetCond(cb: *codebuf.CodeBuffer, reg: usize, cond: Cond) !void {
        const cc: u8 = switch (cond) {
            .eq => 4, .ne => 5, .lt => 12, .gt => 15, .le => 14, .ge => 13,
        };
        try emitSetcc(cb, toReg(reg), cc);
    }
    pub fn emitAnd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitAndRegReg(cb, toReg(dst), toReg(src));
    }
    pub fn emitOr(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitOrRegReg(cb, toReg(dst), toReg(src));
    }
    pub fn emitPrologue(cb: *codebuf.CodeBuffer, frame_size: i32) !void {
        try emitPushReg(cb, .rbp);
        try emitMovRegReg(cb, @intFromEnum(Reg.rbp), @intFromEnum(Reg.rsp));
        try emitSubImm32(cb, .rsp, frame_size);
    }
    pub fn emitEpilogueReturn(cb: *codebuf.CodeBuffer, frame_size: i32, ret_reg: ?usize) !void {
        _ = frame_size;
        _ = ret_reg;
        try emitMovRegReg(cb, @intFromEnum(Reg.rsp), @intFromEnum(Reg.rbp));
        try emitPopReg(cb, .rbp);
        try emitRet(cb);
    }
};
