// Copyright 2025 Sam Windell
// SPDX-License-Identifier: LGPL-3.0

const std = @import("std");
const builtin = @import("builtin");

const project_name = "NoiseRF";
const project_version = "0.1.0";
const project_id = "org.floe-audio.noiserf";
const project_vendor = "Floe Audio";
const project_url = "TODO";
const project_description = "Stereo broadband noise reduction - fork of Luciano Dato's Noise Repellent";

pub fn build(b: *std.Build) void {
    const target = b.standardTargetOptions(.{});

    const optimize = b.standardOptimizeOption(.{});

    const lib = b.addStaticLibrary(.{
        .name = "libspecbleach",
        .target = target,
        .optimize = optimize,
        .pic = true,
    });

    const pffft = b.dependency("pffft", .{});

    const flags = &[_][]const u8{
        "-fvisibility=hidden",
    };

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
        .flags = flags,
    });
    lib.addCSourceFile(.{
        .file = pffft.path("pffft.c"),
        .flags = flags,
    });
    lib.addIncludePath(pffft.path(""));
    lib.linkLibC();

    const clap = b.dependency("clap", .{});

    const config = b.addConfigHeader(.{
        .style = .blank,
    }, .{
        .PROJECT_NAME = project_name,
        .PROJECT_VERSION = project_version,
        .PROJECT_ID = project_id,
        .PROJECT_VENDOR = project_vendor,
        .PROJECT_URL = project_url,
        .PROJECT_DESCRIPTION = project_description,
    });

    const plugin = b.addSharedLibrary(.{
        .name = "NoiseRF",
        .target = target,
        .optimize = optimize,
        .pic = true,
    });
    plugin.linkLibrary(lib);
    plugin.linkLibC();
    plugin.addIncludePath(clap.path("include"));
    plugin.addIncludePath(b.path("include"));
    plugin.addConfigHeader(config);
    plugin.addCSourceFiles(.{
        .files = &[_][]const u8{
            "plugin/clap_plugin.c",
            "plugin/signal_crossfade.c",
        },
        .flags = flags,
    });

    const test_step = b.step("test", "");

    const install = PluginInstallStep.add(b, plugin) catch unreachable;
    addClapValidatorIfNeeded(b, test_step, install) catch unreachable;
}

fn addClapValidatorIfNeeded(b: *std.Build, test_step: *std.Build.Step, install_step: *PluginInstallStep) !void {
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
        ClapValidatorStep.add(b, test_step, install_step, c) catch unreachable;
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
    ) !void {
        const validate = b.allocator.create(ClapValidatorStep) catch unreachable;
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

    pub fn add(
        b: *std.Build,
        plugin: *std.Build.Step.Compile,
    ) !*PluginInstallStep {
        const install = b.allocator.create(PluginInstallStep) catch unreachable;
        install.step = std.Build.Step.init(.{
            .id = std.Build.Step.Id.custom,
            .name = "plugin-install",
            .owner = b,
            .makeFn = make,
        });
        install.lib_step = plugin;
        install.emitted_bin = plugin.getEmittedBin();
        install.step.dependOn(&plugin.step);
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
                .long_version = project_version,
                .bundle_name = project_name,
                .short_version = project_version,
                .bundle_version = project_version,
                .copyright = "Copyright (c) 2022 Luciano Dato and 2025 Floe Audio",
            },
        ) catch unreachable;
    }
};
