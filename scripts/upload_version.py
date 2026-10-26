from pathlib import Path
from SCons.Script import COMMAND_LINE_TARGETS

Import("env")

counter_file = Path(env.subst("$PROJECT_DIR")) / "firmware-build.txt"
build_number = int(counter_file.read_text(encoding="ascii").strip())

if "upload" in COMMAND_LINE_TARGETS:
    build_number += 1
    print(f"Firmware upload build: {build_number}")

    def finish_upload(target, source, env):
        counter_file.write_text(f"{build_number}\n", encoding="ascii")
        print(f"Firmware upload completed: {build_number}")

    env.AddPostAction("upload", finish_upload)

env.Append(CPPDEFINES=[("FIRMWARE_BUILD", build_number)])