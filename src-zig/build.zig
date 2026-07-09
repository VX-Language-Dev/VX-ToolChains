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

    // Run steps for each binary
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

    // Tests
    const lib_unit_tests = b.addTest(.{
        .root_module = b.createModule(.{
            .root_source_file = b.path("src/lib.zig"),
            .target = target,
            .optimize = optimize,
        }),
    });
    const run_lib_tests = b.addRunArtifact(lib_unit_tests);
    const test_step = b.step("test", "Run unit tests");
    test_step.dependOn(&run_lib_tests.step);
}
