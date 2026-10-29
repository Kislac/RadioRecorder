"""Read and edit a UV-5R through the ESP USB serial port, without a programming cable."""

import argparse
from datetime import datetime
import json
from pathlib import Path
import re
import time

from scripts.oled_viewer import find_device
from scripts.test_support import TestRun


class RadioClient:
    def __init__(self, device, log=None):
        self.device = device
        self.log = log
        self.sequence = int(time.monotonic() * 1000) % 900000
        self.buffer = b""
        device.timeout = 0.1

    def request(self, command, timeout=12, **params):
        self.sequence = (self.sequence + 1) % 999999
        payload = json.dumps({"id": self.sequence, "command": command, **params},
                             separators=(",", ":"), ensure_ascii=True, allow_nan=False).encode("ascii")
        if len(payload) > 4500:
            raise ValueError("Serial request exceeds 4500 bytes")
        frame = b"\n@RR " + payload + b"\n"
        if self.device.write(frame) != len(frame):
            raise OSError("Incomplete serial request")
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            while b"\n" in self.buffer:
                line, self.buffer = self.buffer.split(b"\n", 1)
                if not line.startswith(b"RR_REPLY "):
                    continue
                parts = line.split(b" ", 2)
                if len(parts) != 3 or parts[1] != str(self.sequence).encode("ascii"):
                    continue
                response = json.loads(parts[2])
                if not isinstance(response, dict):
                    raise ValueError("Invalid ESP response")
                if response.get("error"):
                    raise RuntimeError(response["error"])
                return response
            data = self.device.read(1024)
            self.buffer += data
            if len(self.buffer) > 131072:
                raise ValueError("Oversized serial response")
        raise TimeoutError("No matching ESP reply; install firmware 0.1.47+ and close other COM tools")

    def wait(self, timeout=150):
        deadline = time.monotonic() + timeout
        previous = None
        while time.monotonic() < deadline:
            state = self.request("status")
            progress = (state.get("stage"), state.get("bytesRead"), state.get("bytesWritten"))
            if progress != previous and self.log:
                self.log.log(f"RADIO: {progress[0]} read={progress[1]} written={progress[2]}")
            previous = progress
            if state.get("state") == "done":
                return state
            if state.get("state") not in ("reading", "writing"):
                raise RuntimeError("Radio operation failed or was interrupted")
            time.sleep(0.2)
        raise TimeoutError("ESP radio operation did not complete; do not retry an uncertain write automatically")

    def read_radio(self):
        self.request("read", confirm=True)
        self.wait(timeout=60)
        return self.request("settings")

    def image(self, revision):
        output = bytearray()
        total = 128
        while len(output) < total:
            count = min(128, total - len(output))
            response = self.request("image", offset=len(output), count=count)
            if response.get("revision") != revision or response.get("offset") != len(output):
                raise ValueError("Radio snapshot changed during image download")
            reported = response.get("total")
            if reported not in (6152, 6472) or total != 128 and reported != total:
                raise ValueError("Invalid or changed image length")
            total = reported
            chunk = bytes.fromhex(response.get("data", ""))
            if len(chunk) != count:
                raise ValueError("Incomplete image chunk")
            output.extend(chunk)
        return bytes(output)


def load_patch(path):
    text = Path(path).read_text(encoding="utf-8-sig")
    if len(text.encode("utf-8")) > 4096:
        raise ValueError("Patch exceeds 4096 bytes")
    patch = json.loads(text, parse_constant=lambda value: (_ for _ in ()).throw(ValueError("Invalid number")))
    if not isinstance(patch, dict) or set(patch) != {"revision", "changes"}:
        raise ValueError("Patch must contain only revision and changes")
    if not isinstance(patch["revision"], str) or not re.fullmatch(r"[0-9a-f]{8}", patch["revision"]):
        raise ValueError("Patch revision must match the saved radio snapshot")
    changes = patch["changes"]
    if not isinstance(changes, list) or not 1 <= len(changes) <= 32:
        raise ValueError("Patch requires 1-32 explicit changes")
    for change in changes:
        if (not isinstance(change, dict) or not {"group", "field", "value"} <= set(change) or
                not set(change) <= {"group", "field", "value", "index"} or
                change["group"] not in ("settings", "vfoA", "vfoB", "channel", "dtmf") or
                not isinstance(change["field"], str)):
            raise ValueError("Invalid change; use the ESP schema for supported fields")
        if change["group"] == "channel":
            if type(change.get("index")) is not int or not 0 <= change["index"] <= 127:
                raise ValueError("Channel change requires index 0-127")
        elif "index" in change:
            raise ValueError("Only channel changes accept an index")
    return patch


def save_snapshot(client, state, directory):
    directory = Path(directory)
    directory.mkdir(parents=True, exist_ok=False)
    image = client.image(state["revision"])
    with (directory / "radio.img").open("xb") as output:
        output.write(image)
    with (directory / "radio.json").open("x", encoding="utf-8") as output:
        json.dump(state, output, ensure_ascii=False, indent=2, allow_nan=False)
    with (directory / "patch.json").open("x", encoding="utf-8") as output:
        json.dump({"revision": state["revision"], "changes": []}, output, indent=2)
    return str(directory)


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--port", help="ESP COM port, or auto (close monitor/viewer first)")
    parser.add_argument("--log-dir", type=Path)
    commands = parser.add_subparsers(dest="command")
    commands.add_parser("status", help="Read ESP status only; does not activate radio UART")
    commands.add_parser("schema", help="List writable groups, fields and ranges")
    read = commands.add_parser("read", help="Clone radio and save JSON/image/patch template")
    read.add_argument("--confirm-read", default="CANCEL")
    read.add_argument("--output", type=Path, help="New directory for backup files")
    write = commands.add_parser("write", help="Apply a patch with fresh backup, preflight and verification")
    write.add_argument("--patch", required=True, type=Path)
    write.add_argument("--confirm-write", default="CANCEL")
    args = parser.parse_args(argv)
    command = args.command or "status"
    run = TestRun("radio-programmer", args.log_dir)
    format_json = lambda value: json.dumps(value, ensure_ascii=False, indent=2, allow_nan=False)
    if command == "read" and args.confirm_read != "READ UV5R":
        run.skip("radio read", "Explicit --confirm-read 'READ UV5R' required")
        run.summary()
        return 2
    if command == "write" and args.confirm_write != "WRITE UV5R":
        run.skip("radio write", "Explicit --confirm-write 'WRITE UV5R' required")
        run.summary()
        return 2
    patch = None
    if command == "write":
        valid, patch = run.case("validate patch file", lambda: load_patch(args.patch), result_formatter=format_json)
        if not valid:
            run.summary()
            return 1
    connected, device = run.case("open ESP serial", lambda: find_device(None if args.port in (None, "auto") else args.port))
    if not connected:
        run.summary()
        return 1
    try:
        client = RadioClient(device, run)
        ready, hello = run.case("serial protocol", lambda: client.request("hello"), result_formatter=format_json)
        if ready and hello.get("protocol") != 1:
            raise RuntimeError("Unsupported ESP serial protocol")
        if ready and command in ("status", "schema"):
            run.case(command, lambda: client.request(command), result_formatter=format_json)
        elif ready:
            valid, state = run.case("read radio", client.read_radio, result_formatter=format_json)
            if valid:
                stamp = datetime.now().strftime("%Y%m%d-%H%M%S-%f")
                directory = args.output if command == "read" and args.output else run.log_path.parent / ("radio-backup-" + stamp)
                backed_up, _ = run.case("save original backup", lambda: save_snapshot(client, state, directory))
                if command == "write" and backed_up:
                    def perform_write():
                        if patch["revision"] != state.get("revision"):
                            raise ValueError("Radio differs from patch backup; review changes against a fresh read")
                        client.request("write", patch=patch, confirm="WRITE UV5R")
                        completed = client.wait()
                        if not completed.get("writeVerified"):
                            raise RuntimeError("Radio write was not verified")
                        return save_snapshot(client, client.request("settings"), directory.parent / (directory.name + "-verified"))
                    run.case("write and verify radio", perform_write)
    except Exception as error:
        run.case("radio operation", lambda: (_ for _ in ()).throw(error))
    finally:
        device.close()
    return 0 if run.summary() else 1


if __name__ == "__main__":
    raise SystemExit(main())