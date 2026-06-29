Import("env")
import subprocess
import os
import shutil
import atexit

def generate_uf2():
    build_dir = env.subst("$BUILD_DIR")
    hex_file  = os.path.join(build_dir, "firmware.hex")
    uf2_file  = os.path.join(build_dir, "firmware.uf2")
    downloads = os.path.expanduser("~/Downloads/firmware.uf2")

    uf2conv = os.path.expanduser(
        "~/.platformio/packages/framework-arduinoadafruitnrf52/tools/uf2conv/uf2conv.py"
    )

    if not os.path.isfile(hex_file):
        return

    result = subprocess.run(
        ["python", uf2conv, "-f", "0xADA52840", "-c", "-o", uf2_file, hex_file],
        capture_output=True, text=True
    )

    if result.returncode == 0:
        shutil.copy2(uf2_file, downloads)
        print(f"[UF2] Created: {uf2_file}")
        print(f"[UF2] Copied to: {downloads}")
    else:
        print("[UF2] Error:", result.stderr)

atexit.register(generate_uf2)
