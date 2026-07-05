// ==================== RISC-V 架构代码生成后端 (RV32 / RV64) ====================

const std = @import("std");
const codebuf = @import("codebuf");

// RV64 后端
pub const rv64 = struct {
    pub const Backend = struct {
        pub const word_size: u32 = 8;
        pub const RESULT: usize = 10; // a0 (x10)
        pub const SCRATCH: usize = 11; // a1 (x11)
        pub const SCRATCH2: usize = 12; // a2 (x12)
        pub const FP: usize = 8; // s0/fp (x8)
        pub const RA: usize = 1; // ra (x1)
        pub const ARG_REGS: [6]usize = .{ 10, 11, 12, 13, 14, 15 }; // a0-a5

        pub const Cond = enum(u8) { eq = 0, ne = 1, lt = 2, gt = 3, le = 4, ge = 5 };

        fn emitU32(cb: *codebuf.CodeBuffer, val: u32) !void {
            var buf: [4]u8 = undefined;
            std.mem.writeInt(u32, &buf, val, .little);
            try cb.appendSlice(&buf);
        }

        // R-type: funct7 | rs2(5) | rs1(5) | funct3(3) | rd(5) | opcode(7)
        fn rType(funct7: u6, rs2: u5, rs1: u5, funct3: u3, rd: u5, opcode: u7) u32 {
            return (@as(u32, funct7) << 25) | (@as(u32, rs2) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        // I-type: imm12(12) | rs1(5) | funct3(3) | rd(5) | opcode(7)
        fn iType(imm12: u12, rs1: u5, funct3: u3, rd: u5, opcode: u7) u32 {
            return (@as(u32, imm12) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        // S-type: imm[11:5](7) | rs2(5) | rs1(5) | funct3(3) | imm[4:0](5) | opcode(7)
        fn sType(imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) u32 {
            const upper = (imm12 >> 5) & 0x7F;
            const lower = imm12 & 0x1F;
            return (@as(u32, upper) << 25) | (@as(u32, rs2) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, lower) << 7) | opcode;
        }
        // B-type: imm[12|10:5](7) | rs2(5) | rs1(5) | funct3(3) | imm[4:1|11](5) | opcode(7)
        fn bType(imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) u32 {
            const imm12_u32 = @as(u32, imm12);
            const b12 = (imm12_u32 >> 12) & 1;
            const b10_5 = (imm12 >> 5) & 0x3F;
            const b4_1 = (imm12 >> 1) & 0xF;
            const b11 = (imm12 >> 11) & 1;
            const upper = (b12 << 6) | b10_5;
            const lower = (b4_1 << 1) | b11;
            return (@as(u32, upper) << 25) | (@as(u32, rs2) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, lower) << 7) | opcode;
        }
        // U-type: imm[31:12](20) | rd(5) | opcode(7)
        fn uType(imm20: u20, rd: u5, opcode: u7) u32 {
            return (@as(u32, imm20) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        // J-type: imm[20|10:1|11|19:12](20) | rd(5) | opcode(7)
        fn jType(imm20: u20, rd: u5, opcode: u7) u32 {
            const j20 = (imm20 >> 19) & 1; // bit 20
            const j10_1 = (imm20 >> 1) & 0x3FF; // bits 10:1
            const j11 = (imm20 >> 10) & 1; // bit 11
            const j19_12 = (imm20 >> 11) & 0xFF; // bits 19:12
            const encoding = (j20 << 19) | (j10_1 << 9) | (j11 << 8) | j19_12;
            return (@as(u32, encoding) << 12) | (@as(u32, rd) << 7) | opcode;
        }

        fn emitR(cb: *codebuf.CodeBuffer, funct7: u6, rs2: u5, rs1: u5, funct3: u3, rd: u5, opcode: u7) !void {
            try emitU32(cb, rType(funct7, rs2, rs1, funct3, rd, opcode));
        }
        fn emitI(cb: *codebuf.CodeBuffer, imm12: u12, rs1: u5, funct3: u3, rd: u5, opcode: u7) !void {
            try emitU32(cb, iType(imm12, rs1, funct3, rd, opcode));
        }
        fn emitS(cb: *codebuf.CodeBuffer, imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) !void {
            try emitU32(cb, sType(imm12, rs2, rs1, funct3, opcode));
        }
        fn emitB(cb: *codebuf.CodeBuffer, imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) !void {
            try emitU32(cb, bType(imm12, rs2, rs1, funct3, opcode));
        }
        fn emitU(cb: *codebuf.CodeBuffer, imm20: u20, rd: u5, opcode: u7) !void {
            try emitU32(cb, uType(imm20, rd, opcode));
        }
        fn emitJ(cb: *codebuf.CodeBuffer, imm20: u20, rd: u5, opcode: u7) !void {
            try emitU32(cb, jType(imm20, rd, opcode));
        }

        // ADD rd, rs1, rs2
        fn emitAddR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 0, rd, 0x33);
        }
        // SUB rd, rs1, rs2
        fn emitSubR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x20, rs2, rs1, 0, rd, 0x33);
        }
        // MUL rd, rs1, rs2
        fn emitMulR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x01, rs2, rs1, 0, rd, 0x33);
        }
        // DIV rd, rs1, rs2
        fn emitDivR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x01, rs2, rs1, 0, rd, 0x33); // funct7 = 0x01 for M extension
        }
        // REM rd, rs1, rs2
        fn emitRemR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x01, rs2, rs1, 0, rd, 0x33);
        }
        // AND rd, rs1, rs2
        fn emitAndR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 7, rd, 0x33);
        }
        // OR rd, rs1, rs2
        fn emitOrR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 6, rd, 0x33);
        }
        // XOR rd, rs1, rs2
        fn emitXorR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 4, rd, 0x33);
        }
        // SLT rd, rs1, rs2 (set less than, signed)
        fn emitSltR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 2, rd, 0x33);
        }
        // ADDI rd, rs1, imm12
        fn emitAddi(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 0, rd, 0x13);
        }
        // SLTI rd, rs1, imm12 (set less than immediate, signed)
        fn emitSlti(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 2, rd, 0x13);
        }
        // XORI rd, rs1, imm12
        fn emitXori(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 4, rd, 0x13);
        }
        // ORI rd, rs1, imm12
        fn emitOri(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 6, rd, 0x13);
        }
        // ANDI rd, rs1, imm12
        fn emitAndi(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 7, rd, 0x13);
        }
        // LUI rd, imm20
        fn emitLui(cb: *codebuf.CodeBuffer, rd: u5, imm20: u20) !void {
            try emitU(cb, imm20, rd, 0x37);
        }
        // SLLI rd, rs1, shamt
        fn emitSlli(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, shamt: u6) !void {
            try emitI(cb, @intCast(shamt), rs1, 1, rd, 0x13);
        }

        // LD rd, offset(rs1)  (RV64: I-type, opcode=0x03, funct3=3)
        fn emitLd(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 3, rd, 0x03);
        }
        // SD rs2, offset(rs1) (RV64: S-type, opcode=0x23, funct3=3)
        fn emitSd(cb: *codebuf.CodeBuffer, rs2: u5, rs1: u5, imm12: u12) !void {
            try emitS(cb, imm12, rs2, rs1, 3, 0x23);
        }
        // LW rd, offset(rs1) (RV32/RV64: I-type, opcode=0x03, funct3=2)
        fn emitLw(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 2, rd, 0x03);
        }
        // SW rs2, offset(rs1) (S-type, opcode=0x23, funct3=2)
        fn emitSw(cb: *codebuf.CodeBuffer, rs2: u5, rs1: u5, imm12: u12) !void {
            try emitS(cb, imm12, rs2, rs1, 2, 0x23);
        }

        // JAL rd, offset20
        fn emitJal(cb: *codebuf.CodeBuffer, rd: u5, imm20: u20) !void {
            try emitJ(cb, imm20, rd, 0x6F);
        }
        // JALR rd, rs1, imm12
        fn emitJalr(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 0, rd, 0x67);
        }
        // ECALL
        fn emitEcall(cb: *codebuf.CodeBuffer) !void {
            try emitU32(cb, 0x00000073);
        }

        // BEQ rs1, rs2, offset12
        fn emitBeq(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 0, 0x63);
        }
        // BNE
        fn emitBne(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 1, 0x63);
        }
        // BLT
        fn emitBlt(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 4, 0x63);
        }
        // BGE
        fn emitBge(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 5, 0x63);
        }

        // MV rd, rs (ADDI rd, rs, 0)
        fn emitMv(cb: *codebuf.CodeBuffer, rd: u5, rs: u5) !void {
            try emitAddi(cb, rd, rs, 0);
        }

        // LI rd, imm (load immediate, can be multiple instrs)
        fn emitLi(cb: *codebuf.CodeBuffer, rd: u5, imm: u64) !void {
            if (imm <= 2047) {
                try emitAddi(cb, rd, 0, @intCast(imm));
                return;
            }
            // LUI + ADDI
            const upper = @as(u32, @intCast((imm + 0x800) >> 12));
            var lower = @as(i32, @intCast(imm & 0xFFF));
            if (lower > 2047) lower -= 4096; // sign-extend
            if (upper > 0) {
                try emitLui(cb, rd, @intCast(upper & 0xFFFFF));
                if (lower != 0) {
                    try emitAddi(cb, rd, rd, @intCast(lower & 0xFFF));
                }
            } else {
                try emitAddi(cb, rd, 0, @intCast(imm & 0xFFF));
            }
        }

        // ===== 后端通用接口 =====

        pub fn emitStoreToSlot(cb: *codebuf.CodeBuffer, offset: i32, reg: usize) !void {
            try emitSd(cb, @intCast(reg), FP, @intCast((-offset) & 0xFFF));
        }
        pub fn emitLoadFromSlot(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
            try emitLd(cb, @intCast(reg), FP, @intCast((-offset) & 0xFFF));
        }
        pub fn emitMovRegReg(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitMv(cb, @intCast(dst), @intCast(src));
        }
        pub fn emitLoadImm64(cb: *codebuf.CodeBuffer, reg: usize, value: u64) !void {
            try emitLi(cb, @intCast(reg), value);
        }
        pub fn emitXorReg(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            try emitXorR(cb, r, r, r);
        }
        pub fn emitTest(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            try emitAddi(cb, 0, r, 0); // dummy read (sets flags in SW emulation)
            // RISC-V has no flags; we just compare with zero
        }
        pub fn emitNot(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            // xori r, r, -1; then sltiu r, r, 1 (equal to zero check)
            try emitXori(cb, r, r, 0x7FF); // NOT via XOR with -1 (lower 12 bits)
            // Actually use: r = (r == 0) ? 1 : 0
            // sltiu r, r, 1
            try emitSlti(cb, r, r, 1);
        }
        pub fn emitNeg(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            try emitSubR(cb, r, 0, r); // SUB rd, x0, rs
        }
        pub fn emitLeaFP(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
            _ = offset;
            try emitMv(cb, @intCast(reg), FP);
        }
        pub fn emitSubImm(cb: *codebuf.CodeBuffer, reg: usize, value: i32) !void {
            try emitAddi(cb, @intCast(reg), @intCast(reg), @intCast((-value) & 0xFFF)); // addi reg, reg, -value
        }
        pub fn emitAdd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitAddR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitSub(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitSubR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitMul(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitMulR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitDiv(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitDivR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitMod(cb: *codebuf.CodeBuffer, _dst: usize, src: usize, rem: usize) !void {
            const dd = @as(u5, @intCast(_dst));
            const ss = @as(u5, @intCast(src));
            const rr = @as(u5, @intCast(rem));
            // DIV rr, dd, ss; MUL tmp, rr, ss; SUB rr, dd, tmp
            try emitDivR(cb, rr, dd, ss);
            try emitMulR(cb, SCRATCH2, rr, ss);
            try emitSubR(cb, rr, dd, SCRATCH2);
        }
        pub fn emitCmp(cb: *codebuf.CodeBuffer, a: usize, b: usize) !void {
            const aa = @as(u5, @intCast(a));
            const bb = @as(u5, @intCast(b));
            // RISC-V: use SLT and SLTU to determine comparison
            // For branch purposes, we store result in SCRATCH2
            try emitSltR(cb, SCRATCH2, aa, bb);
            // also compute reverse slt for equality
            try emitXorR(cb, 0, aa, bb); // x0 = xor of a,b (for eq/ne)
        }
        pub fn emitSetCond(cb: *codebuf.CodeBuffer, reg: usize, cond: Cond) !void {
            const r = @as(u5, @intCast(reg));
            const aa = @as(u5, @intCast(SCRATCH));
            const bb = @as(u5, @intCast(SCRATCH2));
            switch (cond) {
                .eq => {
                    // seqz r, a → sltiu r, a, 1
                    try emitSlti(cb, r, aa, 1);
                },
                .ne => {
                    // snez r, a → sltu r, x0, a
                    try emitSltR(cb, r, 0, aa);
                },
                .lt => {
                    try emitMv(cb, r, SCRATCH2); // SLT result
                },
                .ge => {
                    // not lt
                    try emitSlti(cb, r, SCRATCH2, 1); // r = (aa >= bb) ? 1 : 0
                },
                .gt => {
                    // blt bb, aa → SLT r, bb, aa
                    try emitSltR(cb, r, bb, aa);
                },
                .le => {
                    // bge bb, aa
                    try emitSlti(cb, r, bb, 1); // simplified: use bge check
                },
            }
        }
        pub fn emitAnd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitAndR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitOr(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitOrR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitJmpRel32(cb: *codebuf.CodeBuffer) !usize {
            // JAL x0, offset → unconditional jump
            try emitJal(cb, 0, 0);
            return cb.len() - 4;
        }
        pub fn emitJccRel32(cb: *codebuf.CodeBuffer, cond: Cond) !usize {
            const rs1: u5 = SCRATCH;
            const rs2: u5 = SCRATCH2;
            switch (cond) {
                .eq => try emitBeq(cb, rs1, rs2, 0),
                .ne => try emitBne(cb, rs1, rs2, 0),
                .lt => try emitBlt(cb, rs1, rs2, 0),
                .ge => try emitBge(cb, rs1, rs2, 0),
                .le => try emitBge(cb, rs2, rs1, 0), // a <= b → b >= a
                .gt => try emitBlt(cb, rs2, rs1, 0), // a > b → b < a
            }
            return cb.len() - 4;
        }
        pub fn emitCallRel32(cb: *codebuf.CodeBuffer) !usize {
            try emitJal(cb, RA, 0);
            return cb.len() - 4;
        }
        pub fn emitRet(cb: *codebuf.CodeBuffer) !void {
            try emitJalr(cb, 0, RA, 0); // jalr x0, ra, 0
        }
        pub fn emitSyscall(cb: *codebuf.CodeBuffer) !void {
            try emitEcall(cb);
        }
        pub fn emitLoadFP(cb: *codebuf.CodeBuffer, reg: usize) !void {
            try emitMv(cb, @intCast(reg), FP);
        }
        pub fn emitPrologue(cb: *codebuf.CodeBuffer, frame_size: i32) !void {
            // addi sp, sp, -frame_size
            try emitAddi(cb, 2, 2, @intCast((-frame_size) & 0xFFF));
            // sd ra, frame_size-8(sp)
            try emitSd(cb, RA, 2, @intCast((frame_size - 8) & 0xFFF));
            // sd fp, frame_size-16(sp)
            try emitSd(cb, FP, 2, @intCast((frame_size - 16) & 0xFFF));
            // addi fp, sp, frame_size (or addi fp, sp, 0)
            try emitAddi(cb, FP, 2, 0);
        }
        pub fn emitEpilogueReturn(cb: *codebuf.CodeBuffer, frame_size: i32, ret_reg: ?usize) !void {
            if (ret_reg) |r| try emitMv(cb, 10, @intCast(r));
            try emitLd(cb, RA, 2, @intCast((frame_size - 8) & 0xFFF));
            try emitLd(cb, FP, 2, @intCast((frame_size - 16) & 0xFFF));
            try emitAddi(cb, 2, 2, @intCast(frame_size & 0xFFF));
            try emitJalr(cb, 0, RA, 0);
        }
        pub fn emitBuiltin(cb: *codebuf.CodeBuffer, name: []const u8) !void {
            if (std.mem.eql(u8, name, "exit")) {
                try emitAddi(cb, 17, 0, 93);
                try emitEcall(cb);
            } else if (std.mem.eql(u8, name, "out")) {
                try emitAddi(cb, 17, 0, 64);
                try emitAddi(cb, 10, 0, 1);
                try emitMv(cb, 11, FP);
                try emitAddi(cb, 12, 0, 4);
                try emitEcall(cb);
                try emitJalr(cb, 0, RA, 0);
            } else {
                try emitJalr(cb, 0, RA, 0);
            }
        }
    };
};

pub const rv32 = struct {
    pub const Backend = struct {
        pub const word_size: u32 = 4;
        pub const RESULT: usize = 10;
        pub const SCRATCH: usize = 11;
        pub const SCRATCH2: usize = 12;
        pub const FP: usize = 8;
        pub const RA: usize = 1;
        pub const ARG_REGS: [6]usize = .{ 10, 11, 12, 13, 14, 15 };
        pub const Cond = enum(u8) { eq = 0, ne = 1, lt = 2, gt = 3, le = 4, ge = 5 };

        fn emitU32(cb: *codebuf.CodeBuffer, val: u32) !void {
            var buf: [4]u8 = undefined;
            std.mem.writeInt(u32, &buf, val, .little);
            try cb.appendSlice(&buf);
        }
        fn rType(funct7: u6, rs2: u5, rs1: u5, funct3: u3, rd: u5, opcode: u7) u32 {
            return (@as(u32, funct7) << 25) | (@as(u32, rs2) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        fn iType(imm12: u12, rs1: u5, funct3: u3, rd: u5, opcode: u7) u32 {
            return (@as(u32, imm12) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        fn sType(imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) u32 {
            const upper = (imm12 >> 5) & 0x7F;
            const lower = imm12 & 0x1F;
            return (@as(u32, upper) << 25) | (@as(u32, rs2) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, lower) << 7) | opcode;
        }
        fn bType(imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) u32 {
            const imm12_u32 = @as(u32, imm12);
            const b12 = (imm12_u32 >> 12) & 1;
            const b10_5 = (imm12 >> 5) & 0x3F;
            const b4_1 = (imm12 >> 1) & 0xF;
            const b11 = (imm12 >> 11) & 1;
            const upper = (b12 << 6) | b10_5;
            const lower = (b4_1 << 1) | b11;
            return (@as(u32, upper) << 25) | (@as(u32, rs2) << 20) | (@as(u32, rs1) << 15) | (@as(u32, funct3) << 12) | (@as(u32, lower) << 7) | opcode;
        }
        fn uType(imm20: u20, rd: u5, opcode: u7) u32 {
            return (@as(u32, imm20) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        fn jType(imm20: u20, rd: u5, opcode: u7) u32 {
            const j20 = (imm20 >> 19) & 1;
            const j10_1 = (imm20 >> 1) & 0x3FF;
            const j11 = (imm20 >> 10) & 1;
            const j19_12 = (imm20 >> 11) & 0xFF;
            const encoding = (j20 << 19) | (j10_1 << 9) | (j11 << 8) | j19_12;
            return (@as(u32, encoding) << 12) | (@as(u32, rd) << 7) | opcode;
        }
        fn emitR(cb: *codebuf.CodeBuffer, funct7: u6, rs2: u5, rs1: u5, funct3: u3, rd: u5, opcode: u7) !void {
            try emitU32(cb, rType(funct7, rs2, rs1, funct3, rd, opcode));
        }
        fn emitI(cb: *codebuf.CodeBuffer, imm12: u12, rs1: u5, funct3: u3, rd: u5, opcode: u7) !void {
            try emitU32(cb, iType(imm12, rs1, funct3, rd, opcode));
        }
        fn emitS(cb: *codebuf.CodeBuffer, imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) !void {
            try emitU32(cb, sType(imm12, rs2, rs1, funct3, opcode));
        }
        fn emitB(cb: *codebuf.CodeBuffer, imm12: u12, rs2: u5, rs1: u5, funct3: u3, opcode: u7) !void {
            try emitU32(cb, bType(imm12, rs2, rs1, funct3, opcode));
        }
        fn emitU(cb: *codebuf.CodeBuffer, imm20: u20, rd: u5, opcode: u7) !void {
            try emitU32(cb, uType(imm20, rd, opcode));
        }
        fn emitJ(cb: *codebuf.CodeBuffer, imm20: u20, rd: u5, opcode: u7) !void {
            try emitU32(cb, jType(imm20, rd, opcode));
        }

        fn emitAddR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 0, rd, 0x33);
        }
        fn emitSubR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x20, rs2, rs1, 0, rd, 0x33);
        }
        fn emitMulR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x01, rs2, rs1, 0, rd, 0x33);
        }
        fn emitDivR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x01, rs2, rs1, 0, rd, 0x33);
        }
        fn emitAndR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 7, rd, 0x33);
        }
        fn emitOrR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 6, rd, 0x33);
        }
        fn emitXorR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 4, rd, 0x33);
        }
        fn emitSltR(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, rs2: u5) !void {
            try emitR(cb, 0x00, rs2, rs1, 2, rd, 0x33);
        }
        fn emitAddi(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 0, rd, 0x13);
        }
        fn emitSlti(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 2, rd, 0x13);
        }
        fn emitXori(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 4, rd, 0x13);
        }
        fn emitLui(cb: *codebuf.CodeBuffer, rd: u5, imm20: u20) !void {
            try emitU(cb, imm20, rd, 0x37);
        }
        fn emitLw(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 2, rd, 0x03);
        }
        fn emitSw(cb: *codebuf.CodeBuffer, rs2: u5, rs1: u5, imm12: u12) !void {
            try emitS(cb, imm12, rs2, rs1, 2, 0x23);
        }
        fn emitJal(cb: *codebuf.CodeBuffer, rd: u5, imm20: u20) !void {
            try emitJ(cb, imm20, rd, 0x6F);
        }
        fn emitJalr(cb: *codebuf.CodeBuffer, rd: u5, rs1: u5, imm12: u12) !void {
            try emitI(cb, imm12, rs1, 0, rd, 0x67);
        }
        fn emitEcall(cb: *codebuf.CodeBuffer) !void {
            try emitU32(cb, 0x00000073);
        }
        fn emitBeq(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 0, 0x63);
        }
        fn emitBne(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 1, 0x63);
        }
        fn emitBlt(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 4, 0x63);
        }
        fn emitBge(cb: *codebuf.CodeBuffer, rs1: u5, rs2: u5, imm12: u12) !void {
            try emitB(cb, imm12, rs2, rs1, 5, 0x63);
        }
        fn emitMv(cb: *codebuf.CodeBuffer, rd: u5, rs: u5) !void {
            try emitAddi(cb, rd, rs, 0);
        }
        fn emitLi(cb: *codebuf.CodeBuffer, rd: u5, imm: u32) !void {
            if (imm <= 2047) {
                try emitAddi(cb, rd, 0, @intCast(imm));
                return;
            }
            const upper = (imm + 0x800) >> 12;
            if (upper > 0) {
                try emitLui(cb, rd, @intCast(upper & 0xFFFFF));
                const lower = @as(i32, @intCast(imm & 0xFFF));
                if (lower > 2047) try emitAddi(cb, rd, rd, @intCast(lower & 0xFFF));
            } else {
                try emitAddi(cb, rd, 0, @intCast(imm & 0xFFF));
            }
        }

        pub fn emitStoreToSlot(cb: *codebuf.CodeBuffer, offset: i32, reg: usize) !void {
            try emitSw(cb, @intCast(reg), FP, @intCast((-offset) & 0xFFF));
        }
        pub fn emitLoadFromSlot(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
            try emitLw(cb, @intCast(reg), FP, @intCast((-offset) & 0xFFF));
        }
        pub fn emitMovRegReg(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitMv(cb, @intCast(dst), @intCast(src));
        }
        pub fn emitLoadImm64(cb: *codebuf.CodeBuffer, reg: usize, value: u64) !void {
            try emitLi(cb, @intCast(reg), @intCast(value & 0xFFFFFFFF));
        }
        pub fn emitXorReg(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            try emitXorR(cb, r, r, r);
        }
        pub fn emitTest(_: *codebuf.CodeBuffer, _: usize) !void {}
        pub fn emitNot(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            try emitXori(cb, r, r, 0x7FF);
            try emitSlti(cb, r, r, 1);
        }
        pub fn emitNeg(cb: *codebuf.CodeBuffer, reg: usize) !void {
            const r = @as(u5, @intCast(reg));
            try emitSubR(cb, r, 0, r);
        }
        pub fn emitLeaFP(cb: *codebuf.CodeBuffer, reg: usize, offset: i32) !void {
            _ = offset;
            try emitMv(cb, @intCast(reg), FP);
        }
        pub fn emitSubImm(cb: *codebuf.CodeBuffer, reg: usize, value: i32) !void {
            try emitAddi(cb, @intCast(reg), @intCast(reg), @intCast((-value) & 0xFFF));
        }
        pub fn emitAdd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitAddR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitSub(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitSubR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitMul(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitMulR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitDiv(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitDivR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitMod(cb: *codebuf.CodeBuffer, _dst: usize, src: usize, rem: usize) !void {
            const dd = @as(u5, @intCast(_dst));
            const ss = @as(u5, @intCast(src));
            const rr = @as(u5, @intCast(rem));
            try emitDivR(cb, rr, dd, ss);
            try emitMulR(cb, SCRATCH2, rr, ss);
            try emitSubR(cb, rr, dd, SCRATCH2);
        }
        pub fn emitCmp(cb: *codebuf.CodeBuffer, a: usize, b: usize) !void {
            try emitSltR(cb, SCRATCH2, @intCast(a), @intCast(b));
            try emitXorR(cb, 0, @intCast(a), @intCast(b));
        }
        pub fn emitSetCond(cb: *codebuf.CodeBuffer, reg: usize, cond: Cond) !void {
            const r = @as(u5, @intCast(reg));
            const aa = @as(u5, @intCast(SCRATCH));
            const bb = @as(u5, @intCast(SCRATCH2));
            switch (cond) {
                .eq => try emitSlti(cb, r, aa, 1),
                .ne => try emitSltR(cb, r, 0, aa),
                .lt => try emitMv(cb, r, SCRATCH2),
                .ge => try emitSlti(cb, r, SCRATCH2, 1),
                .gt => try emitSltR(cb, r, bb, aa),
                .le => try emitSlti(cb, r, bb, 1),
            }
        }
        pub fn emitAnd(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitAndR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitOr(cb: *codebuf.CodeBuffer, dst: usize, src: usize) !void {
            try emitOrR(cb, @intCast(dst), @intCast(dst), @intCast(src));
        }
        pub fn emitJmpRel32(cb: *codebuf.CodeBuffer) !usize {
            try emitJal(cb, 0, 0);
            return cb.len() - 4;
        }
        pub fn emitJccRel32(cb: *codebuf.CodeBuffer, cond: Cond) !usize {
            const rs1: u5 = SCRATCH;
            const rs2: u5 = SCRATCH2;
            switch (cond) {
                .eq => try emitBeq(cb, rs1, rs2, 0),
                .ne => try emitBne(cb, rs1, rs2, 0),
                .lt => try emitBlt(cb, rs1, rs2, 0),
                .ge => try emitBge(cb, rs1, rs2, 0),
                .le => try emitBge(cb, rs2, rs1, 0),
                .gt => try emitBlt(cb, rs2, rs1, 0),
            }
            return cb.len() - 4;
        }
        pub fn emitCallRel32(cb: *codebuf.CodeBuffer) !usize {
            try emitJal(cb, RA, 0);
            return cb.len() - 4;
        }
        pub fn emitRet(cb: *codebuf.CodeBuffer) !void {
            try emitJalr(cb, 0, RA, 0);
        }
        pub fn emitSyscall(cb: *codebuf.CodeBuffer) !void {
            try emitEcall(cb);
        }
        pub fn emitLoadFP(cb: *codebuf.CodeBuffer, reg: usize) !void {
            try emitMv(cb, @intCast(reg), FP);
        }
        pub fn emitPrologue(cb: *codebuf.CodeBuffer, frame_size: i32) !void {
            const neg_fs: u12 = @intCast((-frame_size) & 0xFFF);
            const fs_minus8 = @as(u12, @intCast((frame_size - 4) & 0xFFF));
            const fs_minus16 = @as(u12, @intCast((frame_size - 8) & 0xFFF));
            try emitAddi(cb, 2, 2, neg_fs);
            try emitSw(cb, RA, 2, fs_minus8);
            try emitSw(cb, FP, 2, fs_minus16);
            try emitAddi(cb, FP, 2, 0);
        }
        pub fn emitEpilogueReturn(cb: *codebuf.CodeBuffer, frame_size: i32, ret_reg: ?usize) !void {
            if (ret_reg) |r| try emitMv(cb, 10, @intCast(r));
            const fs_minus8 = @as(u12, @intCast((frame_size - 4) & 0xFFF));
            const fs_minus16 = @as(u12, @intCast((frame_size - 8) & 0xFFF));
            const fs_u12 = @as(u12, @intCast(frame_size & 0xFFF));
            try emitLw(cb, RA, 2, fs_minus8);
            try emitLw(cb, FP, 2, fs_minus16);
            try emitAddi(cb, 2, 2, fs_u12);
            try emitJalr(cb, 0, RA, 0);
        }
        pub fn emitBuiltin(cb: *codebuf.CodeBuffer, name: []const u8) !void {
            if (std.mem.eql(u8, name, "exit")) {
                try emitAddi(cb, 17, 0, 1);
                try emitEcall(cb);
            } else if (std.mem.eql(u8, name, "out")) {
                try emitAddi(cb, 17, 0, 64);
                try emitAddi(cb, 10, 0, 1);
                try emitMv(cb, 11, FP);
                try emitAddi(cb, 12, 0, 4);
                try emitEcall(cb);
                try emitJalr(cb, 0, RA, 0);
            } else {
                try emitJalr(cb, 0, RA, 0);
            }
        }
    };
};
