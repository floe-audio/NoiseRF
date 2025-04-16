// Copyright 2025 Sam Windell
// SPDX-License-Identifier: LGPL-3.0

const std = @import("std");
const builtin = @import("builtin");

const project_name = "NoiseRF";
const project_id = "org.floe-audio.noiserf";
const project_vendor = "Floe Audio";
const project_url = "TODO";
const project_description = "Stereo broadband noise reduction - fork of Luciano Dato's Noise Repellent";

const CompileConfig = struct {
    target: std.Build.ResolvedTarget,
    optimize: std.builtin.OptimizeMode,
    flags: []const []const u8,
    clap_inlude_path: std.Build.LazyPath,
    project_version: []const u8,
};

fn addNoiseReductionLibrary(b: *std.Build, compile_config: *const CompileConfig) *std.Build.Step.Compile {
    const lib = b.addStaticLibrary(.{
        .name = "libspecbleach",
        .target = compile_config.target,
        .optimize = compile_config.optimize,
        .pic = true,
    });

    const pffft = b.dependency("pffft", .{});

    lib.addCSourceFiles(.{
        .files = &[_][]const u8{
            "src/processors/specbleach_denoiser.c",
            "src/processors/denoiser/spectral_denoiser.c",
            "src/shared/gain_estimation/gain_estimators.c",
            "src/shared/noise_estimation/adaptive_noise_estimator.c",
            "src/shared/noise_estimation/noise_estimator.c",
            "src/shared/noise_estimation/noise_profile.c",
            "src/shared/post_estimation/postfilter.c",
            "src/shared/post_estimation/spectral_whitening.c",
            "src/shared/pre_estimation/absolute_hearing_thresholds.c",
            "src/shared/pre_estimation/critical_bands.c",
            "src/shared/pre_estimation/masking_estimator.c",
            "src/shared/pre_estimation/noise_scaling_criterias.c",
            "src/shared/pre_estimation/spectral_smoother.c",
            "src/shared/pre_estimation/transient_detector.c",
            "src/shared/stft/fft_transform.c",
            "src/shared/stft/stft_buffer.c",
            "src/shared/stft/stft_processor.c",
            "src/shared/stft/stft_windows.c",
            "src/shared/utils/denoise_mixer.c",
            "src/shared/utils/general_utils.c",
            "src/shared/utils/spectral_features.c",
            "src/shared/utils/spectral_trailing_buffer.c",
            "src/shared/utils/spectral_utils.c",
        },
        .flags = compile_config.flags,
    });
    lib.addCSourceFile(.{
        .file = pffft.path("pffft.c"),
        .flags = compile_config.flags,
    });
    lib.addIncludePath(pffft.path(""));
    lib.linkLibC();
    lib.addIncludePath(b.path("include"));

    return lib;
}

fn addUnitTests(b: *std.Build, compile_config: *const CompileConfig, test_step: *std.Build.Step, plugin_static: *std.Build.Step.Compile) void {
    const tests = b.addExecutable(.{
        .name = "unit-tests",
        .target = compile_config.target,
        .optimize = compile_config.optimize,
    });
    tests.addCSourceFiles(.{
        .files = &[_][]const u8{
            "plugin/tests.c",
        },
        .flags = compile_config.flags,
    });
    tests.linkLibC();
    tests.linkLibrary(plugin_static);
    tests.addIncludePath(b.path("include"));
    tests.addIncludePath(compile_config.clap_inlude_path);
    const run_tests = b.addRunArtifact(tests);
    test_step.dependOn(&run_tests.step);

    if (builtin.os.tag == plugin_static.rootModuleTarget().os.tag and
        builtin.cpu.arch == plugin_static.rootModuleTarget().cpu.arch)
    {
        const install_artifact = b.addInstallArtifact(tests, .{});
        b.default_step.dependOn(&install_artifact.step);
    }
}

fn getLatestVersion(b: *std.Build) []const u8 {
    var version_str: []const u8 = b.run(&.{ "git", "describe", "--tags", "--abbrev=0" });
    version_str = std.mem.trimRight(u8, version_str, " \n\r\t");
    version_str = std.mem.trimLeft(u8, version_str, "v");

    const version = std.SemanticVersion.parse(version_str) catch {
        std.debug.print("Latest tag from git is not a semver: {s}\n", .{version_str});
        std.process.exit(1);
    };

    return b.fmt("{d}.{d}.{d}", .{ version.major, version.minor, version.patch });
}

// The bulk of the plugin is compiled into a static library so it can be built into other steps as needed, such as
// the CLAP shared library and the tests executable.
fn addPluginStatic(b: *std.Build, compile_config: *const CompileConfig) *std.Build.Step.Compile {
    const config = b.addConfigHeader(.{
        .style = .blank,
    }, .{
        .PROJECT_NAME = project_name,
        .PROJECT_VERSION = compile_config.project_version,
        .PROJECT_ID = project_id,
        .PROJECT_VENDOR = project_vendor,
        .PROJECT_URL = project_url,
        .PROJECT_DESCRIPTION = project_description,
    });

    const plugin = b.addStaticLibrary(.{
        .name = "plugin",
        .target = compile_config.target,
        .optimize = compile_config.optimize,
        .pic = true,
    });
    plugin.linkLibrary(addNoiseReductionLibrary(b, compile_config));
    plugin.linkLibC();
    plugin.addIncludePath(compile_config.clap_inlude_path);
    plugin.addIncludePath(b.path("include"));
    plugin.addConfigHeader(config);
    plugin.addCSourceFiles(.{
        .files = &[_][]const u8{
            "plugin/clap_plugin.c",
            "plugin/signal_crossfade.c",
        },
        .flags = compile_config.flags,
    });

    return plugin;
}

fn addClapPlugin(b: *std.Build, compile_config: *const CompileConfig, plugin_static: *std.Build.Step.Compile) *std.Build.Step.Compile {
    const clap_plugin = b.addSharedLibrary(.{
        .name = "NoiseRF",
        .target = compile_config.target,
        .optimize = compile_config.optimize,
        .pic = true,
    });
    clap_plugin.addCSourceFiles(.{
        .files = &[_][]const u8{
            "plugin/clap_entry.c",
        },
        .flags = compile_config.flags,
    });
    clap_plugin.linkLibC();
    clap_plugin.linkLibrary(plugin_static);
    clap_plugin.addIncludePath(compile_config.clap_inlude_path);

    return clap_plugin;
}

pub fn build(b: *std.Build) void {
    const compile_config = CompileConfig{
        .target = b.standardTargetOptions(.{}),
        .optimize = b.standardOptimizeOption(.{}),
        .flags = &[_][]const u8{
            "-fvisibility=hidden",
        },
        .clap_inlude_path = b.dependency("clap", .{}).path("include"),
        .project_version = getLatestVersion(b),
    };

    const plugin_static = addPluginStatic(b, &compile_config);

    const clap_plugin = addClapPlugin(b, &compile_config, plugin_static);

    const install_step = PluginInstallStep.add(b, clap_plugin, &compile_config);

    const test_step = b.step("test", "build and run tests");
    addUnitTests(b, &compile_config, test_step, plugin_static);
    addClapValidatorIfNeeded(b, test_step, install_step);
}

fn addClapValidatorIfNeeded(b: *std.Build, test_step: *std.Build.Step, install_step: *PluginInstallStep) void {
    var clap_validator: ?std.Build.LazyPath = null;

    if (builtin.os.tag == install_step.lib_step.rootModuleTarget().os.tag) {
        switch (builtin.os.tag) {
            .macos => {
                clap_validator = b.dependency("clap_validator_macos", .{}).path("clap-validator");
            },
            .windows => {
                clap_validator = b.dependency("clap_validator_windows", .{}).path("clap-validator.exe");
            },
            .linux => {
                if (isUbuntu() catch false) {
                    clap_validator = b.dependency("clap_validator_ubuntu", .{}).path("clap-validator");
                }
            },
            else => {},
        }
    }

    if (clap_validator) |c| {
        ClapValidatorStep.add(b, test_step, install_step, c);
    }
}

fn isUbuntu() !bool {
    const file = try std.fs.cwd().openFile("/etc/os-release", .{});
    defer file.close();

    var buffer: [1024]u8 = undefined;
    const reader = file.reader();

    while (try reader.readUntilDelimiterOrEof(&buffer, '\n')) |line| {
        if (std.mem.indexOf(u8, line, "ID=ubuntu") != null) {
            return true;
        }
    }

    return false;
}

const ClapValidatorStep = struct {
    step: std.Build.Step,
    plugin_step: *PluginInstallStep,
    clap_validator: std.Build.LazyPath,

    pub fn add(
        b: *std.Build,
        test_step: *std.Build.Step,
        plugin_step: *PluginInstallStep,
        clap_validator: std.Build.LazyPath,
    ) void {
        const validate = b.allocator.create(ClapValidatorStep) catch @panic("out of memory");
        validate.step = std.Build.Step.init(.{
            .id = std.Build.Step.Id.custom,
            .name = "clap-validator",
            .owner = b,
            .makeFn = make,
        });
        validate.plugin_step = plugin_step;
        validate.clap_validator = clap_validator;
        validate.step.dependOn(&plugin_step.step);
        test_step.dependOn(&validate.step);
    }

    fn make(step: *std.Build.Step, prog_node: std.Progress.Node) !void {
        _ = prog_node;
        const self: *ClapValidatorStep = @fieldParentPtr("step", step);
        const b = step.owner;

        const clap_path = self.plugin_step.clap_path.?;
        const clap_validator = self.clap_validator.getPath2(b, step);

        if (builtin.os.tag != .windows) {
            const clap_validator_file = try std.fs.openFileAbsolute(clap_validator, .{});
            defer clap_validator_file.close();
            try clap_validator_file.chmod(0o755);
        }

        // IMPROVE: support clap-validator state tests. For now, we skip the state test for 2 reasons:
        // - clap-validator has bugs (https://github.com/free-audio/clap-validator/issues/18)
        // - The tests impose requirements more than is strictly necessary for the plugin to work. The tests
        //   require state to be byte-for-byte identical which is a stronger requirement that the CLAP spec which
        //   just requires the same result when parsing.

        var out_code: u8 = 0;
        const out = try b.runAllowFail(&.{
            clap_validator,
            "validate",
            "--test-filter",
            ".*state.*",
            "--invert-filter",
            clap_path,
        }, &out_code, .Inherit);

        if (out_code != 0) {
            return step.fail("clap-validator failed with code {d}\n{s}", .{ out_code, out });
        }
    }
};

fn installPath(b: *std.Build, step: *std.Build.Step.Compile) []const u8 {
    const target = step.rootModuleTarget();
    const folder = b.fmt("{s}-{s}", .{
        @tagName(target.cpu.arch),
        @tagName(target.os.tag),
    });
    return b.getInstallPath(.prefix, folder);
}

const PluginInstallStep = struct {
    step: std.Build.Step,
    lib_step: *std.Build.Step.Compile,
    emitted_bin: ?std.Build.LazyPath,
    clap_path: ?[]const u8,
    project_version: []const u8,

    pub fn add(
        b: *std.Build,
        plugin: *std.Build.Step.Compile,
        compile_config: *const CompileConfig,
    ) *PluginInstallStep {
        const install = b.allocator.create(PluginInstallStep) catch @panic("out of memory");
        install.step = std.Build.Step.init(.{
            .id = std.Build.Step.Id.custom,
            .name = "plugin-install",
            .owner = b,
            .makeFn = make,
        });
        install.lib_step = plugin;
        install.emitted_bin = plugin.getEmittedBin();
        install.step.dependOn(&plugin.step);
        install.project_version = compile_config.project_version;
        b.default_step.dependOn(&install.step);
        return install;
    }

    fn make(step: *std.Build.Step, prog_node: std.Progress.Node) !void {
        _ = prog_node;
        const self: *PluginInstallStep = @fieldParentPtr("step", step);
        const b = step.owner;

        const folder = installPath(b, self.lib_step);
        const filename = b.fmt("{s}.clap", .{self.lib_step.name});
        const clap_path = try std.fs.path.join(b.allocator, &[_][]const u8{
            folder,
            filename,
        });
        self.clap_path = clap_path;

        const source = self.emitted_bin.?.getPath2(step.owner, step);

        switch (self.lib_step.rootModuleTarget().os.tag) {
            .macos => {
                // On macOS we need to create a bundle.
                const bundle_exec_path = try std.fs.path.join(b.allocator, &[_][]const u8{
                    clap_path,
                    "Contents",
                    "MacOS",
                    self.lib_step.name,
                });

                _ = try std.fs.updateFileAbsolute(source, bundle_exec_path, .{});

                var bundle_dir = try std.fs.openDirAbsolute(clap_path, .{});
                defer bundle_dir.close();

                const plist_file = try bundle_dir.createFile("Contents/Info.plist", .{});
                defer plist_file.close();
                try self.writePlist(plist_file);

                const pkginfo_file = try bundle_dir.createFile("Contents/PkgInfo", .{});
                defer pkginfo_file.close();
                try pkginfo_file.writeAll("BNDL????");
            },
            .windows, .linux => {
                // For Windows and Linux, we just copy the file but use the clap extension rather than
                // whatever the default would be (dll/lib*.so).
                _ = try std.fs.updateFileAbsolute(source, clap_path, .{});
            },
            else => unreachable,
        }
    }

    fn writePlist(self: *PluginInstallStep, file: std.fs.File) !void {
        std.debug.assert(self.lib_step.rootModuleTarget().os.tag == .macos);

        const writer = file.writer();
        const template = @embedFile("resources/Info.plist");

        std.fmt.format(
            writer,
            template,
            .{
                .exec_name = self.lib_step.name,
                .info_string = project_name,
                .bundle_id = project_id,
                .long_version = self.project_version,
                .bundle_name = project_name,
                .short_version = self.project_version,
                .bundle_version = self.project_version,
                .copyright = "Copyright (c) 2022 Luciano Dato and 2025 Floe Audio",
            },
        ) catch unreachable;
    }
};
