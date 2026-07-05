// ==================== AArch64 (ARM64) 架构代码生成后端 ====================

const std = @import("std");
const codebuf = @import("codebuf");

pub const Backend = struct {
    pub const word_size: u32 = 8;
    pub const RESULT: usize = 0;  // x0
    pub const SCRATCH: usize = 1; // x1
    pub const SCRATCH2: usize = 2; // x2
    pub const FP: usize = 29;     // x29
    pub const LR: usize = 30;     // x30
    pub const ARG_REGS: [6]usize = .{ 0, 1, 2, 3, 4, 5 }; // x0-x5

    pub const Cond = enum(u8) { eq = 0, ne = 1, lt = 2, gt = 3, le = 4, ge = 5 };

    fn condToArm(cond: Cond) u8 {
        return switch (cond) {
            .eq => 0,  .ne => 1,  .lt => 11, .gt => 12, .le => 13, .ge => 10,
        };
    }

    fn emitU32(cb: *codebuf.CodeBuffer, val: u32) !void {
        var buf: [4]u8 = undefined;
        std.mem.writeInt(u32, &buf, val, .little);
        try cb.appendSlice(&buf);
    }

    // MOVZ Rd, #imm16, lsl #shift
    fn emitMovz(cb: *codebuf.CodeBuffer, rd: u8, imm16: u16, shift: u6) !void {
        try emitU32(cb, 0xD2800000 | @as(u32, rd) | (@as(u32, imm16) << 5) | (@as(u32, shift / 16) << 21));
    }

    // MOVK Rd, #imm16, lsl #shift
    fn emitMovk(cb: *codebuf.CodeBuffer, rd: u8, imm16: u16, shift: u6) !void {
        try emitU32(cb, 0xF2800000 | @as(u32, rd) | (@as(u32, imm16) << 5) | (@as(u32, shift / 16) << 21));
    }

    // STR Xt, [Xn, #imm7*8] (unsigned offset, scaled by 8)
    fn emitStrOffset(cb: *codebuf.CodeBuffer, rt: u8, rn: u8, imm7: u16) !void {
        try emitU32(cb, 0xF9000000 | @as(u32, rt) | (@as(u32, rn) << 5) | (@as(u32, imm7 & 0x7F) << 10));
    }

    // LDR Xt, [Xn, #imm7*8]
    fn emitLdrOffset(cb: *codebuf.CodeBuffer, rt: u8, rn: u8, imm7: u16) !void {
        try emitU32(cb, 0xF9400000 | @as(u32, rt) | (@as(u32, rn) << 5) | (@as(u32, imm7 & 0x7F) << 10));
    }

    // STR Xt, [Xn, #-imm7*8]! (pre-index)
    fn emitStrPreIdx(cb: *codebuf.CodeBuffer, rt: u8, rn: u8, imm7: u16) !void {
        try emitU32(cb, 0xF8000C00 | @as(u32, rt) | (@as(u32, rn) << 5) | (@as(u32, imm7 & 0x7F) << 10));
    }

    // STP Xt1, Xt2, [Xn, #-imm7*8]! (pre-index)
    fn emitStpPreIdx(cb: *codebuf.CodeBuffer, rt1: u8, rt2: u8, rn: u8, imm7: u16) !void {
        try emitU32(cb, 0xA9800000 | (@as(u32, imm7 & 0x7F) << 15) | (@as(u32, rt2) << 10) | (@as(u32, rn) << 5) | @as(u32, rt1));
    }

    // LDP Xt1, Xt2, [Xn], #imm7*8 (post-index)
    fn emitLdpPostIdx(cb: *codebuf.CodeBuffer, rt1: u8, rt2: u8, rn: u8, imm7: u16) !void {
        try emitU32(cb, 0xA8C00000 | (@as(u32, imm7 & 0x7F) << 15) | (@as(u32, rt2) << 10) | (@as(u32, rn) << 5) | @as(u32, rt1));
    }

    // ADD Xd, Xn, Xm
    fn emitAddRegReg(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0x8B000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // SUB Xd, Xn, Xm
    fn emitSubRegReg(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0xCB000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // SUB Xd, Xn, #imm12 (12-bit unsigned, shifted by 0 or 12) - helper
    fn emitSubImmHelper(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, imm12: u16) !void {
        try emitU32(cb, 0xD1000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, imm12 & 0xFFF) << 10));
    }

    // SUBS Xd, Xn, Xm (for cmp = SUBS xzr, Xn, Xm)
    fn emitSubsRegReg(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0xEB000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // MUL Xd, Xn, Xm
    fn emitMulRegReg(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0x9B007C00 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // SDIV Xd, Xn, Xm
    fn emitSdivRegReg(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0x9AC00C00 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // CSEL Xd, Xn, Xm, cond (condition select)
    fn emitCsel(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8, cond: u8) !void {
        try emitU32(cb, 0x9A800000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16) | (@as(u32, cond) << 12));
    }

    // CSET Xd, cond
    fn emitCset(cb: *codebuf.CodeBuffer, rd: u8, cond: u8) !void {
        try emitU32(cb, 0x9A9F07E0 | @as(u32, rd) | (@as(u32, cond) << 12));
    }

    // MOV Xd, Xm (alias for ORR Xd, XZR, Xm)
    fn emitMovX(cb: *codebuf.CodeBuffer, rd: u8, rm: u8) !void {
        try emitU32(cb, 0xAA0003E0 | @as(u32, rd) | (@as(u32, rm) << 16));
    }

    // MVN Xd, Xm (bitwise NOT, alias for ORN Xd, XZR, Xm)
    fn emitMvn(cb: *codebuf.CodeBuffer, rd: u8, rm: u8) !void {
        try emitU32(cb, 0xAA2003E0 | @as(u32, rd) | (@as(u32, rm) << 16));
    }

    // AND Xd, Xn, Xm
    fn emitAndReg(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0x8A000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // ORR Xd, Xn, Xm
    fn emitOrr(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0xAA000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // EOR Xd, Xn, Xm
    fn emitEor(cb: *codebuf.CodeBuffer, rd: u8, rn: u8, rm: u8) !void {
        try emitU32(cb, 0xCA000000 | @as(u32, rd) | (@as(u32, rn) << 5) | (@as(u32, rm) << 16));
    }

    // B offset26 (unconditional branch)
    fn emitB(cb: *codebuf.CodeBuffer, offset26: u32) !void {
        try emitU32(cb, 0x14000000 | (offset26 & 0x3FFFFFF));
    }

    // B.cond offset19
    fn emitBCond(cb: *codebuf.CodeBuffer, cond: u8, offset19: u32) !void {
        try emitU32(cb, 0x54000000 | @as(u32, cond) | ((offset19 & 0x7FFFF) << 5));
    }

    // BL offset26
    fn emitBl(cb: *codebuf.CodeBuffer, offset26: u32) !void {
        try emitU32(cb, 0x94000000 | (offset26 & 0x3FFFFFF));
    }

    // RET
    fn emitRetInstr(cb: *codebuf.CodeBuffer) !void {
        try emitU32(cb, 0xD65F03C0);
    }

    // SVC #0
    fn emitSvc(cb: *codebuf.CodeBuffer) !void {
        try emitU32(cb, 0xD4000001);
    }

    // ===== 后端通用接口 =====

    pub fn emitStoreToSlot(cb: *codebuf.CodeBuffer, offset: i32, reg: usize) !void {
        const imm7: u16 = @intCast(@divExact(-offset, @as(i32, 8)) & 0x7F);
        try emitStrOffset(cb, @intCast(reg), FP, imm7);
    }
    pub fn emitLoadFromSlot(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
        const imm7: u16 = @intCast(@divExact(-offset, @as(i32, 8)) & 0x7F);
        try emitLdrOffset(cb, @intCast(reg), FP, imm7);
    }
    pub fn emitMovRegReg(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitMovX(cb, @intCast(dst), @intCast(src));
    }
    pub fn emitLoadImm64(cb: *codebuf.CodeBuffer, reg: usize, value: u64) !void {
        const rd = @as(u8, @intCast(reg));
        try emitMovz(cb, rd, @truncate(value), 0);
        var remaining = value >> 16;
        var shift: u6 = 16;
        while (remaining > 0) : ({ remaining >>= 16; shift += 16; }) {
            try emitMovk(cb, rd, @truncate(remaining), shift);
        }
    }
    pub fn emitXorReg(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = @as(u8, @intCast(reg));
        try emitEor(cb, r, r, r);
    }
    pub fn emitTest(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = @as(u8, @intCast(reg));
        try emitSubsRegReg(cb, 31, r, 0); // cmp reg, #0 (SUBS XZR, reg, #0)
    }
    pub fn emitNot(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = @as(u8, @intCast(reg));
        try emitSubsRegReg(cb, 31, r, 0);
        try emitCset(cb, r, 0); // cset reg, eq
    }
    pub fn emitNeg(cb: *codebuf.CodeBuffer, reg: usize) !void {
        const r = @as(u8, @intCast(reg));
        try emitSubRegReg(cb, r, 31, r); // SUB reg, XZR, reg
    }
    pub fn emitLeaFP(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
        _ = offset;
        const r = @as(u8, @intCast(reg));
        try emitMovX(cb, r, FP);
    }
    pub fn emitSubImm(cb: *codebuf.CodeBuffer, reg: usize, value: i32) !void {
        try emitSubImmHelper(cb, @intCast(reg), @intCast(reg), @intCast(value));
    }
    pub fn emitAdd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitAddRegReg(cb, @intCast(dst), @intCast(dst), @intCast(src));
    }
    pub fn emitSub(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitSubRegReg(cb, @intCast(dst), @intCast(dst), @intCast(src));
    }
    pub fn emitMul(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitMulRegReg(cb, @intCast(dst), @intCast(dst), @intCast(src));
    }
    pub fn emitDiv(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitSdivRegReg(cb, @intCast(dst), @intCast(dst), @intCast(src));
    }
    pub fn emitMod(cb: *codebuf.CodeBuffer, _dst: usize, src: usize, rem: usize) !void {
        // rem not used in simplest approach - use SDIV then MSUB
        // For simplicity: SDIV rem, dst, src; MSUB rem, rem, src, dst
        const dd = @as(u8, @intCast(_dst));
        const ss = @as(u8, @intCast(src));
        const rr = @as(u8, @intCast(rem));
        try emitSdivRegReg(cb, rr, dd, ss);
        // MSUB rr, rr, ss, dd → rr = dd - rr * ss
        // MSUB encoding: 0x9B008000 | rd | rn<<5 | rm<<16
        try emitU32(cb, 0x9B008000 | @as(u32, rr) | (@as(u32, dd) << 5) | (@as(u32, ss) << 16));
    }
    pub fn emitCmp(cb: *codebuf.CodeBuffer, a: usize, b: usize) !void {
        try emitSubsRegReg(cb, 31, @intCast(a), @intCast(b)); // SUBS XZR, a, b
    }
    pub fn emitSetCond(cb: *codebuf.CodeBuffer, reg: usize, cond: Cond) !void {
        try emitCset(cb, @intCast(reg), condToArm(cond));
    }
    pub fn emitAnd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitAndReg(cb, @intCast(dst), @intCast(dst), @intCast(src));
    }
    pub fn emitOr(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
        try emitOrr(cb, @intCast(dst), @intCast(dst), @intCast(src));
    }
    pub fn emitJmpRel32(cb: *codebuf.CodeBuffer) !usize {
        try emitB(cb, 0);
        return cb.len() - 4;
    }
    pub fn emitJccRel32(cb: *codebuf.CodeBuffer, cond: Cond) !usize {
        try emitBCond(cb, condToArm(cond), 0);
        return cb.len() - 4;
    }
    pub fn emitCallRel32(cb: *codebuf.CodeBuffer) !usize {
        try emitBl(cb, 0);
        return cb.len() - 4;
    }
    pub fn emitRet(cb: *codebuf.CodeBuffer) !void { try emitRetInstr(cb); }
    pub fn emitSyscall(cb: *codebuf.CodeBuffer) !void { try emitSvc(cb); }
    pub fn emitLoadFP(cb: *codebuf.CodeBuffer, reg: usize) !void {
        try emitMovX(cb, @intCast(reg), FP);
    }
    pub fn emitPrologue(cb: *codebuf.CodeBuffer, frame_size: i32) !void {
        const imm7: u16 = @intCast(@divExact(frame_size, 8));
        // stp x29, x30, [sp, #-frame_size]!
        try emitStpPreIdx(cb, FP, LR, 31, imm7);
        // mov x29, sp
        try emitMovX(cb, FP, 31);
    }
    pub fn emitEpilogueReturn(cb: *codebuf.CodeBuffer, frame_size: i32, ret_reg: ?usize) !void {
        if (ret_reg) |r| try emitMovX(cb, 0, @intCast(r));
        const imm7: u16 = @intCast(@divExact(frame_size, 8));
        try emitLdpPostIdx(cb, FP, LR, 31, imm7);
        try emitRetInstr(cb);
    }
    pub fn emitBuiltin(cb: *codebuf.CodeBuffer, name: []const u8) !void {
        if (std.mem.eql(u8, name, "exit")) {
            try emitMovz(cb, 8, 93, 0);  // x8 = 93 (exit)
            try emitSvc(cb);
        } else if (std.mem.eql(u8, name, "out")) {
            try emitMovz(cb, 8, 64, 0);  // x8 = 64 (write)
            try emitMovz(cb, 0, 1, 0);  // x0 = 1 (stdout)
            // x1 = pointer, x2 = size
            // For now, stub: write 4 bytes from sp
            try emitAddRegReg(cb, 1, 31, FP);
            try emitMovz(cb, 2, 4, 0);  // x2 = 4
            try emitSvc(cb);
            try emitRetInstr(cb);
        } else {
            try emitRetInstr(cb);
        }
    }
};
