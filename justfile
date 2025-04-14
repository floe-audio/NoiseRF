default:
    zig build -Doptimize=ReleaseFast
    mkdir -p release
    cd zig-out/x86_64-linux-gnu && zip -r ../../release/NoiseRF-Linux-x86_64.zip *
