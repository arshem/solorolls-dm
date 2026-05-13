import os
Import("env")

def merge_bin(source, target, env):
    build_dir  = env.subst("$BUILD_DIR")
    proj_name  = env.subst("$PIOENV")
    idf_path   = env.subst("$IDF_PATH")
    boot_bin   = os.path.join(build_dir, "bootloader.bin")
    part_bin   = os.path.join(build_dir, "partitions.bin")
    app_bin    = os.path.join(build_dir, f"{proj_name}.bin")
    merged_bin = os.path.join(build_dir, "firmware-merged.bin")

    # ESP32-S3 flash offsets
    cmd = (
        f"python -m esptool --chip esp32s3 merge_bin "
        f"--output {merged_bin} "
        f"0x0000 {boot_bin} "
        f"0x8000 {part_bin} "
        f"0x10000 {app_bin}"
    )
    print(f"\n>>> Merging firmware → {merged_bin}")
    ret = env.Execute(cmd)
    if ret == 0:
        size = os.path.getsize(merged_bin)
        print(f"    merged: {size/1024:.1f} kB\n")
    else:
        print("    merge failed (esptool not on PATH?)\n")

env.AddPostAction("$BUILD_DIR/${PIOENV}.bin", merge_bin)
