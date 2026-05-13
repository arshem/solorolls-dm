import time
Import("env")

def before_upload(source, target, env):
    print("\n>>> Plug in device now — uploading in 5 seconds...")
    for i in range(5, 0, -1):
        print(f"    {i}...")
        time.sleep(1)
    print("    uploading!\n")

env.AddPreAction("upload", before_upload)
