const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    const root_module = b.createModule(.{
        .root_source_file = b.path("src/main.zig"),
        .target = target,
        .optimize = optimize,
    });

    const vxobj_module = b.createModule(.{
        .root_source_file = b.path("src/vxobj.zig"),
        .target = target,
        .optimize = optimize,
    });

    const platform_io_module = b.createModule(.{
        .root_source_file = b.path("src/platform_io.zig"),
        .target = target,
        .optimize = optimize,
    });

    const elf_linker_module = b.createModule(.{
        .root_source_file = b.path("src/elf_linker.zig"),
        .target = target,
        .optimize = optimize,
    });

    const macho_linker_module = b.createModule(.{
        .root_source_file = b.path("src/macho_linker.zig"),
        .target = target,
        .optimize = optimize,
    });

    const pe_linker_module = b.createModule(.{
        .root_source_file = b.path("src/pe_linker.zig"),
        .target = target,
        .optimize = optimize,
    });

    const typeir_module = b.createModule(.{
        .root_source_file = b.path("src/typeir.zig"),
        .target = target,
        .optimize = optimize,
    });

    const codebuf_module = b.createModule(.{
        .root_source_file = b.path("src/codebuf.zig"),
        .target = target,
        .optimize = optimize,
    });

    const codegen_x86_64_module = b.createModule(.{
        .root_source_file = b.path("src/codegen_x86_64.zig"),
        .target = target,
        .optimize = optimize,
    });

    const codegen_aarch64_module = b.createModule(.{
        .root_source_file = b.path("src/codegen_aarch64.zig"),
        .target = target,
        .optimize = optimize,
    });

    const codegen_arm32_module = b.createModule(.{
        .root_source_file = b.path("src/codegen_arm32.zig"),
        .target = target,
        .optimize = optimize,
    });

    const codegen_riscv_module = b.createModule(.{
        .root_source_file = b.path("src/codegen_riscv.zig"),
        .target = target,
        .optimize = optimize,
    });

    const codegen_module = b.createModule(.{
        .root_source_file = b.path("src/codegen.zig"),
        .target = target,
        .optimize = optimize,
    });

    codegen_module.addImport("typeir", typeir_module);
    codegen_module.addImport("codebuf", codebuf_module);
    codegen_module.addImport("codegen_x86_64", codegen_x86_64_module);
    codegen_module.addImport("codegen_aarch64", codegen_aarch64_module);
    codegen_module.addImport("codegen_arm32", codegen_arm32_module);
    codegen_module.addImport("codegen_riscv", codegen_riscv_module);

    codegen_x86_64_module.addImport("codebuf", codebuf_module);
    codegen_aarch64_module.addImport("codebuf", codebuf_module);
    codegen_arm32_module.addImport("codebuf", codebuf_module);
    codegen_riscv_module.addImport("codebuf", codebuf_module);

    const linker_module = b.createModule(.{
        .root_source_file = b.path("src/linker.zig"),
        .target = target,
        .optimize = optimize,
    });

    linker_module.addImport("vxobj", vxobj_module);
    linker_module.addImport("elf_linker", elf_linker_module);
    linker_module.addImport("macho_linker", macho_linker_module);
    linker_module.addImport("pe_linker", pe_linker_module);
    linker_module.addImport("platform_io", platform_io_module);
    linker_module.addImport("typeir", typeir_module);
    linker_module.addImport("codegen", codegen_module);

    root_module.addImport("linker", linker_module);
    root_module.addImport("vxobj", vxobj_module);
    root_module.addImport("platform_io", platform_io_module);

    elf_linker_module.addImport("platform_io", platform_io_module);
    macho_linker_module.addImport("platform_io", platform_io_module);
    pe_linker_module.addImport("platform_io", platform_io_module);

    const vxlinker = b.addExecutable(.{
        .name = "vxlinker",
        .root_module = root_module,
        .linkage = .static,
    });

    b.installArtifact(vxlinker);

    const run_cmd = b.addRunArtifact(vxlinker);
    run_cmd.step.dependOn(b.getInstallStep());

    if (b.args) |args| {
        run_cmd.addArgs(args);
    }

    const run_step = b.step("run", "Run the vxlinker");
    run_step.dependOn(&run_cmd.step);

    const unit_tests = b.addTest(.{
        .root_module = vxobj_module,
    });

    const run_unit_tests = b.addRunArtifact(unit_tests);

    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_unit_tests.step);
}
