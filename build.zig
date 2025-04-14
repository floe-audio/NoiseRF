// Copyright 2025 Sam Windell
// SPDX-License-Identifier: LGPL-3.0

const std = @import("std");

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

    switch (target.result.os.tag) {
        .linux => {
            LinuxPluginInstallStep.add(b, plugin) catch unreachable;
        },
        .windows => {
            WindowsPluginStep.add(b, plugin) catch unreachable;
        },
        .macos => {
            MacosPluginInstallStep.add(b, plugin) catch unreachable;
        },
        else => {},
    }
}

fn installPath(b: *std.Build, step: *std.Build.Step.Compile) []const u8 {
    // It's called linuxTriple but it can be used for any OS.
    const folder = step.rootModuleTarget().linuxTriple(b.allocator) catch @panic("OOM");
    return b.getInstallPath(.prefix, folder);
}

const MacosPluginInstallStep = struct {
    step: std.Build.Step,
    lib_step: *std.Build.Step.Compile,
    emitted_bin: ?std.Build.LazyPath,

    pub fn add(
        b: *std.Build,
        plugin: *std.Build.Step.Compile,
    ) !void {
        const install = b.allocator.create(MacosPluginInstallStep) catch unreachable;
        install.step = std.Build.Step.init(.{
            .id = std.Build.Step.Id.custom,
            .name = "macos-make-bundle",
            .owner = b,
            .makeFn = make,
        });
        install.lib_step = plugin;
        install.emitted_bin = plugin.getEmittedBin();
        install.step.dependOn(&plugin.step);
        b.default_step.dependOn(&install.step);
    }

    fn make(step: *std.Build.Step, prog_node: std.Progress.Node) !void {
        _ = prog_node;
        const self: *MacosPluginInstallStep = @fieldParentPtr("step", step);
        const b = step.owner;

        const folder = installPath(b, self.lib_step);

        const bundle_name = b.fmt("{s}.clap", .{self.lib_step.name});
        const bundle_path = try std.fs.path.join(b.allocator, &[_][]const u8{
            folder,
            bundle_name,
        });

        const bundle_exec_path = try std.fs.path.join(b.allocator, &[_][]const u8{
            bundle_path,
            "Contents",
            "MacOS",
            self.lib_step.name,
        });

        const source = self.emitted_bin.?.getPath2(step.owner, step);

        _ = try std.fs.updateFileAbsolute(source, bundle_exec_path, .{});

        var bundle_dir = try std.fs.openDirAbsolute(bundle_path, .{});
        defer bundle_dir.close();

        const plist_file = try bundle_dir.createFile("Contents/Info.plist", .{});
        defer plist_file.close();
        try self.writePlist(plist_file);

        const pkginfo_file = try bundle_dir.createFile("Contents/PkgInfo", .{});
        defer pkginfo_file.close();
        try pkginfo_file.writeAll("BNDL????");
    }

    fn writePlist(self: *MacosPluginInstallStep, file: std.fs.File) !void {
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

const LinuxPluginInstallStep = struct {
    step: std.Build.Step,
    lib_step: *std.Build.Step.Compile,
    emitted_bin: ?std.Build.LazyPath,

    pub fn add(
        b: *std.Build,
        plugin: *std.Build.Step.Compile,
    ) !void {
        const install = b.allocator.create(LinuxPluginInstallStep) catch unreachable;
        install.step = std.Build.Step.init(.{
            .id = std.Build.Step.Id.custom,
            .name = "linux-finalise",
            .owner = b,
            .makeFn = make,
        });
        install.lib_step = plugin;
        install.emitted_bin = plugin.getEmittedBin();
        install.step.dependOn(&plugin.step);
        b.default_step.dependOn(&install.step);
    }

    fn make(step: *std.Build.Step, prog_node: std.Progress.Node) !void {
        _ = prog_node;
        const self: *LinuxPluginInstallStep = @fieldParentPtr("step", step);
        const b = step.owner;

        const folder = installPath(b, self.lib_step);
        const filename = b.fmt("{s}.clap", .{self.lib_step.name});
        const clap_path = try std.fs.path.join(b.allocator, &[_][]const u8{
            folder,
            filename,
        });

        const source = self.emitted_bin.?.getPath2(step.owner, step);

        _ = try std.fs.updateFileAbsolute(source, clap_path, .{});
    }
};

// For Windows, we need to do the same as Linux, except we are simply replacing the dll extension with a clap extension.
const WindowsPluginStep = struct {
    step: std.Build.Step,
    lib_step: *std.Build.Step.Compile,
    emitted_bin: ?std.Build.LazyPath,

    pub fn add(
        b: *std.Build,
        plugin: *std.Build.Step.Compile,
    ) !void {
        const install = b.allocator.create(WindowsPluginStep) catch unreachable;
        install.step = std.Build.Step.init(.{
            .id = std.Build.Step.Id.custom,
            .name = "windows-finalise",
            .owner = b,
            .makeFn = make,
        });
        install.lib_step = plugin;
        install.emitted_bin = plugin.getEmittedBin();
        install.step.dependOn(&plugin.step);
        b.default_step.dependOn(&install.step);
    }

    fn make(step: *std.Build.Step, prog_node: std.Progress.Node) !void {
        _ = prog_node;
        const self: *WindowsPluginStep = @fieldParentPtr("step", step);
        const b = step.owner;

        const folder = installPath(b, self.lib_step);
        const filename = b.fmt("{s}.clap", .{self.lib_step.name});
        const clap_path = try std.fs.path.join(b.allocator, &[_][]const u8{
            folder,
            filename,
        });

        const source = self.emitted_bin.?.getPath2(step.owner, step);

        _ = try std.fs.updateFileAbsolute(source, clap_path, .{});
    }
};
