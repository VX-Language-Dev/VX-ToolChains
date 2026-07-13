const std = @import("std");

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});
    const optimize = b.standardOptimizeOption(.{});

    // Library module
    const vx_vm_module = b.createModule(.{
        .root_source_file = b.path("src/lib.zig"),
        .target = target,
        .optimize = optimize,
    });

    // Static library
    _ = b.addLibrary(.{
        .linkage = .static,
        .name = "vx_vm",
        .root_module = vx_vm_module,
    });

    // Executable: vxc
    {
        const exe_mod = b.createModule(.{
            .root_source_file = b.path("src/vxc.zig"),
            .target = target,
            .optimize = optimize,
        });
        exe_mod.addImport("vx_vm", vx_vm_module);
        const exe = b.addExecutable(.{
            .name = "vxc",
            .root_module = exe_mod,
        });
        b.installArtifact(exe);
    }

    // Executable: vlnk
    {
        const exe_mod = b.createModule(.{
            .root_source_file = b.path("src/vlnk.zig"),
            .target = target,
            .optimize = optimize,
        });
        exe_mod.addImport("vx_vm", vx_vm_module);
        const exe = b.addExecutable(.{
            .name = "vlnk",
            .root_module = exe_mod,
        });
        b.installArtifact(exe);
    }

    // Executable: vpm
    {
        const exe_mod = b.createModule(.{
            .root_source_file = b.path("src/vpm.zig"),
            .target = target,
            .optimize = optimize,
        });
        exe_mod.addImport("vx_vm", vx_vm_module);
        const exe = b.addExecutable(.{
            .name = "vpm",
            .root_module = exe_mod,
        });
        b.installArtifact(exe);
    }

    // Executable: vxde (反编译器)
    {
        const exe_mod = b.createModule(.{
            .root_source_file = b.path("src/vxde.zig"),
            .target = target,
            .optimize = optimize,
        });
        exe_mod.addImport("vx_vm", vx_vm_module);
        const exe = b.addExecutable(.{
            .name = "vxde",
            .root_module = exe_mod,
        });
        b.installArtifact(exe);
    }

    // Executable: vdlnk (反链接器)
    {
        const exe_mod = b.createModule(.{
            .root_source_file = b.path("src/vdlnk.zig"),
            .target = target,
            .optimize = optimize,
        });
        exe_mod.addImport("vx_vm", vx_vm_module);
        const exe = b.addExecutable(.{
            .name = "vdlnk",
            .root_module = exe_mod,
        });
        b.installArtifact(exe);
    }

    // Run steps
    {
        const run_exe_mod = b.createModule(.{
            .root_source_file = b.path("src/vxc.zig"),
            .target = target,
            .optimize = optimize,
        });
        run_exe_mod.addImport("vx_vm", vx_vm_module);
        const run_vxc = b.addRunArtifact(
            b.addExecutable(.{
                .name = "run-vxc",
                .root_module = run_exe_mod,
            }),
        );
        run_vxc.step.dependOn(b.getInstallStep());
        if (b.args) |args| {
            run_vxc.addArgs(args);
        }
        const run_vxc_step = b.step("run-vxc", "Run vxc compiler");
        run_vxc_step.dependOn(&run_vxc.step);
    }

    // Library unit tests (包含所有模块测试)
    const lib_unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/lib.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_lib_tests = b.addRunArtifact(lib_unit_tests);

    // 新增模块独立测试
    const new_modules = &[_]struct { name: []const u8, path: []const u8 }{
        .{ .name = "linker", .path = "src/linker.zig" },
        .{ .name = "logger", .path = "src/logger.zig" },
        .{ .name = "cache", .path = "src/cache.zig" },
        .{ .name = "parallel_build", .path = "src/parallel_build.zig" },
        .{ .name = "vxsetting", .path = "src/vxsetting.zig" },
        .{ .name = "target_profile", .path = "src/target_profile.zig" },
        .{ .name = "delinker", .path = "src/delinker.zig" },
        .{ .name = "builder", .path = "src/builder.zig" },
        .{ .name = "lsp_state", .path = "src/lsp/state.zig" },
        .{ .name = "lsp_diagnostics", .path = "src/lsp/diagnostics.zig" },
        .{ .name = "lsp_completion", .path = "src/lsp/completion.zig" },
        .{ .name = "lsp_hover", .path = "src/lsp/hover.zig" },
        .{ .name = "lsp_goto", .path = "src/lsp/goto.zig" },
        .{ .name = "lsp_symbols", .path = "src/lsp/symbols.zig" },
        .{ .name = "lsp_inlay_hints", .path = "src/lsp/inlay_hints.zig" },
        .{ .name = "lsp_backend", .path = "src/lsp/backend.zig" },
    };

    const test_step = b.step("test", "Run all unit tests");
    test_step.dependOn(&run_lib_tests.step);

    for (new_modules) |mod| {
        const mod_test = b.addTest(.{
            .root_module = b.createModule(.{
                .root_source_file = b.path(mod.path),
                .target = target,
                .optimize = optimize,
            }),
        });
        const run_mod_test = b.addRunArtifact(mod_test);
        test_step.dependOn(&run_mod_test.step);

        const mod_test_step = b.step(
            b.fmt("test-{s}", .{mod.name}),
            b.fmt("Run {s} tests", .{mod.name}),
        );
        mod_test_step.dependOn(&run_mod_test.step);
    }
}
