"""Wi-Fi AP smoke tests; run only when the PC can reach the RadioRecorder AP."""

import argparse
from datetime import datetime
import io
import json
import os
import subprocess
import tempfile
import time
import uuid
import zipfile
import xml.etree.ElementTree as ET
from urllib.request import Request, urlopen, build_opener, ProxyHandler
from urllib.parse import urlsplit

from scripts.oled_viewer import find_device
from scripts.test_device import delete_test_recording
from scripts.test_support import TestRun, firmware_at_least, validate_performance_snapshot


def wifi_profile(ssid):
    namespace = "http://www.microsoft.com/networking/WLAN/profile/v1"
    ET.register_namespace("", namespace)
    root = ET.Element(f"{{{namespace}}}WLANProfile")
    ET.SubElement(root, f"{{{namespace}}}name").text = ssid
    configuration = ET.SubElement(root, f"{{{namespace}}}SSIDConfig")
    ET.SubElement(ET.SubElement(configuration, f"{{{namespace}}}SSID"), f"{{{namespace}}}name").text = ssid
    ET.SubElement(root, f"{{{namespace}}}connectionType").text = "ESS"
    ET.SubElement(root, f"{{{namespace}}}connectionMode").text = "manual"
    security = ET.SubElement(ET.SubElement(root, f"{{{namespace}}}MSM"), f"{{{namespace}}}security")
    authentication = ET.SubElement(security, f"{{{namespace}}}authEncryption")
    ET.SubElement(authentication, f"{{{namespace}}}authentication").text = "open"
    ET.SubElement(authentication, f"{{{namespace}}}encryption").text = "none"
    ET.SubElement(authentication, f"{{{namespace}}}useOneX").text = "false"
    return ET.tostring(root, encoding="utf-8", xml_declaration=True)


def connect_wifi(ssid):
    if os.name != "nt":
        raise RuntimeError("Automatic Wi-Fi connection is supported only on Windows")
    with tempfile.NamedTemporaryFile(suffix=".xml", delete=False) as profile:
        path = profile.name
        profile.write(wifi_profile(ssid))
    try:
        subprocess.run(["netsh", "wlan", "add", "profile", f"filename={path}", "user=current"], check=True)
    finally:
        os.unlink(path)
    subprocess.run(["netsh", "wlan", "connect", f"name={ssid}", f"ssid={ssid}"], check=True)
    print("Requested Wi-Fi connection to " + ssid)


def request(base_url, path, method="GET"):
    with urlopen(Request(base_url + path, method=method), timeout=5) as response:
        return response.read()


def test_api(base_url, args, report):
    if args.connect:
        connect_wifi("RadioRecorder")
        report.detail("Requested Wi-Fi connection to RadioRecorder")
        deadline = time.monotonic() + 20
        while time.monotonic() < deadline:
            try:
                request(base_url, "/api/status")
                break
            except OSError:
                time.sleep(0.5)
        else:
            raise TimeoutError("ESP AP not reachable after connecting; check VPN and Wi-Fi")
    status = json.loads(request(base_url, "/api/status"))
    assert status["version"] and status["state"] in ("idle", "starting", "recording")
    assert isinstance(status["meterReady"], bool)
    assert 0 <= status["screenOffMinutes"] <= 999
    wifi = json.loads(request(base_url, "/api/wifi/status"))
    assert wifi["mode"] in ("OFF", "AP", "CONNECTING", "STA", "AP+STA")
    assert wifi["ip"] and 0 <= wifi["autoOffMinutes"] <= 999
    assert "password" not in wifi
    scan = json.loads(request(base_url, "/api/wifi/scan"))
    assert scan["state"] in ("idle", "scanning", "failed", "complete")
    wifi = json.loads(request(base_url, "/api/wifi/status"))
    assert wifi["mode"] in ("OFF", "AP", "CONNECTING", "STA", "AP+STA")
    assert 0 <= wifi["autoOffMinutes"] <= 999
    assert "password" not in wifi
    wifi = json.loads(request(base_url, "/api/wifi/status"))
    assert wifi["mode"] in ("OFF", "AP", "CONNECTING", "STA", "AP+STA")
    assert 0 <= wifi["autoOffMinutes"] <= 999
    assert "password" not in wifi
    signal = json.loads(request(base_url, "/api/signal"))
    assert isinstance(signal["rms"], int) and isinstance(signal["enabled"], bool)
    settings = json.loads(request(base_url, "/api/settings"))
    assert 0 <= settings["screenOffMinutes"] <= 999
    assert 0 <= settings["wifiAutoOffMinutes"] <= 999
    if firmware_at_least(status["version"], (0, 1, 90)):
        performance = json.loads(request(base_url, "/api/performance"))
        report.detail("HTTP /api/performance " + json.dumps(performance, sort_keys=True))
        assert settings["performanceEnabled"] is performance["enabled"]
        report.detail("Performance: " + validate_performance_snapshot(performance))
    assert settings["wifi"]["mode"] in ("OFF", "AP", "CONNECTING", "STA", "AP+STA")
    assert 0 <= settings["wifiAutoOffMinutes"] <= 999
    assert settings["wifi"]["mode"] in ("OFF", "AP", "CONNECTING", "STA", "AP+STA")
    assert 0 <= settings["wifiAutoOffMinutes"] <= 999
    assert settings["wifi"]["mode"] in ("OFF", "AP", "CONNECTING", "STA", "AP+STA")
    pages = (request(base_url, "/"), request(base_url, "/files"), request(base_url, "/settings"))
    page_lengths = tuple(map(len, pages))
    assert all(length > 0 for length in page_lengths)
    assert b'href="/settings"' in pages[0]
    assert b'href="/"' in pages[2]
    report.detail("HTTP /api/status " + json.dumps(status, sort_keys=True))
    report.detail("HTTP /api/settings " + json.dumps(settings, sort_keys=True))
    report.detail("HTTP /api/wifi/status " + json.dumps(wifi, sort_keys=True))
    report.detail("HTTP /api/wifi/scan " + json.dumps(scan, sort_keys=True))
    report.detail("HTTP /api/wifi/status " + json.dumps(wifi, sort_keys=True))
    report.detail("HTTP /api/wifi/status " + json.dumps(wifi, sort_keys=True))
    report.detail("HTTP /api/signal " + json.dumps(signal, sort_keys=True))
    report.detail("HTTP pages: /={} bytes, /files={} bytes, /settings={} bytes".format(*page_lengths))
    return status


def test_performance_switch(base_url, report):
    original = json.loads(request(base_url, "/api/performance"))["enabled"]
    target = not original
    try:
        changed = json.loads(request(base_url, "/api/performance?enabled=" + str(int(target)), "POST"))
        assert changed["enabled"] is target
        assert json.loads(request(base_url, "/api/settings"))["performanceEnabled"] is target
        validate_performance_snapshot(changed)
    finally:
        restored = json.loads(request(base_url, "/api/performance?enabled=" + str(int(original)), "POST"))
        assert restored["enabled"] is original
        report.detail("Performance diagnostics restored to " + str(original))
    return "NVS setting toggled and original state restored"


def recording_metadata_found(files, path):
    return any(file.get("path") == path and file.get("bytes", 0) >= 44 for file in files)


def test_record_and_delete(base_url, status, report):
    if not status["ready"] or status["state"] != "idle":
        raise RuntimeError("Recorder not ready or already active; no recording started")
    if not firmware_at_least(status["version"], (0, 1, 26)):
        raise RuntimeError("record-and-delete requires firmware 0.1.26 or newer; found " + status["version"])

    with find_device() as serial_device:
        report.detail("Connected to serial port " + serial_device.port + " for safe test-file cleanup")
        name = request(base_url, "/api/start", "POST").decode().strip()
        failure = None
        try:
            deadline = time.monotonic() + 8
            while time.monotonic() < deadline:
                current = json.loads(request(base_url, "/api/status"))
                if current["state"] == "recording":
                    break
                time.sleep(0.2)
            else:
                raise AssertionError("Recording did not start")
            time.sleep(1)
        except Exception as error:
            failure = error
        finally:
            try:
                request(base_url, "/api/stop", "POST")
                report.detail("HTTP POST /api/stop requested")
            except Exception as error:
                if failure is None:
                    failure = error

        idle = False
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            try:
                if json.loads(request(base_url, "/api/status"))["state"] == "idle":
                    idle = True
                    break
            except Exception as error:
                if failure is None:
                    failure = error
            time.sleep(0.2)
        if not idle and failure is None:
            failure = TimeoutError("Recording did not stop")

        if idle:
            try:
                files = json.loads(request(base_url, "/api/recordings"))["files"]
                if not recording_metadata_found(files, name):
                    raise AssertionError("New recording not found in file list: " + name)
                report.detail("HTTP verified WAV metadata for " + name)
            except Exception as error:
                if failure is None:
                    failure = error
            try:
                delete_test_recording(serial_device, name, report)
            except Exception as error:
                if failure is None:
                    failure = error

        if failure is not None:
            raise failure
        return name + " recorded, verified and deleted"


def test_file_operations(base_url, report):
    root = "/FileTest-" + datetime.now().strftime("%Y%m%d-%H%M%S") + "-" + uuid.uuid4().hex[:8]
    created = False
    last_job = 0
    payload = bytes(range(256)) * 4
    opener = build_opener(ProxyHandler({}))

    def api(path, data=None):
        body = json.dumps(data).encode() if data is not None else None
        request = Request(base_url + path, data=body, headers={"Content-Type": "application/json"}, method="POST" if body is not None else "GET")
        with opener.open(request, timeout=10) as response:
            return json.loads(response.read())

    def wait_job(job, ready=False):
        deadline = time.monotonic() + 30
        while time.monotonic() < deadline:
            current = api("/api/files/jobs/status?id=" + str(job["id"]))
            if current["state"] == "complete" or ready and current["state"] == "ready":
                return current
            if current["state"] in ("failed", "cancelled"):
                raise AssertionError(current.get("error") or current["state"])
            time.sleep(0.05)
        raise TimeoutError("File operation did not finish: " + str(job["id"]))

    def start(action, source="", destination="", **extra):
        nonlocal last_job
        job = api("/api/files/jobs", {"action": action, "source": source, "destination": destination, **extra})
        last_job = job["id"]
        return job

    def transfer_url(job, kind):
        port = job.get("transferPort", 0)
        if not port:
            return base_url + "/transfer/" + kind + "?id=" + str(job["id"])
        host = urlsplit(base_url).hostname
        if ":" in host:
            host = "[" + host + "]"
        return "http://" + host + ":" + str(port) + "/" + kind + "?id=" + str(job["id"])

    try:
        status = api("/api/status")
        if not status.get("ready") or status["state"] != "idle" or status.get("playback") != "idle":
            raise AssertionError("File tests require an idle ready SD device")
        api("/api/files/mkdir", {"path": root})
        created = True
        report.detail("FILE TEST created exact folder " + root)
        api("/api/files/mkdir", {"path": root + "/Sub"})
        upload = wait_job(start("upload", destination=root + "/probe.bin", bytes=len(payload)), ready=True)
        with opener.open(Request(transfer_url(upload, "upload"), data=payload, headers={"Content-Type": "application/octet-stream"}, method="PUT"), timeout=10) as response:
            response.read()
        wait_job(upload)
        wait_job(start("copy", root + "/probe.bin", root + "/copy.bin"))
        api("/api/files/rename", {"source": root + "/copy.bin", "destination": root + "/renamed.bin"})
        wait_job(start("move", root + "/renamed.bin", root + "/Sub/moved.bin"))
        bundle = wait_job(start("zip", root), ready=True)
        with opener.open(transfer_url(bundle, "zip"), timeout=15) as response:
            archive_bytes = response.read()
        wait_job(bundle)
        with zipfile.ZipFile(io.BytesIO(archive_bytes)) as archive:
            if archive.testzip() is not None:
                raise AssertionError("ZIP integrity failed")
            label = root[1:]
            if archive.read(label + "/probe.bin") != payload or archive.read(label + "/Sub/moved.bin") != payload:
                raise AssertionError("Copied or uploaded content differs")
        report.detail("FILE TEST verified upload/copy/rename/move/ZIP bytes in " + root)
    finally:
        if created:
            current = api("/api/files/jobs/status")
            if current.get("id") == last_job and current.get("state") in ("preparing", "ready", "running", "verifying"):
                api("/api/files/jobs/cancel?id=" + str(last_job), {})
                deadline = time.monotonic() + 10
                while api("/api/files/jobs/status?id=" + str(last_job))["state"] in ("preparing", "ready", "running", "verifying"):
                    if time.monotonic() >= deadline:
                        raise TimeoutError("Test operation cleanup remains busy")
                    time.sleep(0.05)
            wait_job(start("delete", root, confirm=True))
            items = api("/api/files?filter=" + root[1:])["items"]
            if any(item["path"] == root for item in items):
                raise AssertionError("Exact test folder cleanup failed")
            report.detail("FILE TEST removed exact verified test folder " + root)
    return "upload, copy, rename, move and ZIP verified; exact test folder removed"


def main():
    parser = argparse.ArgumentParser(description="Test the ESP HTTP API via its Wi-Fi AP")
    parser.add_argument("--base-url", default="http://192.168.4.1")
    parser.add_argument("--connect", action="store_true", help="Switch Windows Wi-Fi to the open RadioRecorder AP")
    parser.add_argument("--record", action="store_true", help="Write a short real WAV to the SD card")
    parser.add_argument("--preserve-performance", action="store_true", help="Keep diagnostics collecting throughout a parent online suite")
    parser.add_argument("--file-manager", action="store_true", help="Opt in to run-specific SD upload/copy/move/rename/ZIP/delete tests with exact cleanup")
    args = parser.parse_args()
    base_url = args.base_url.rstrip("/")
    report = TestRun("wifi")
    try:
        passed, status = report.case("Wi-Fi pages and read-only APIs", lambda: test_api(base_url, args, report))
        if passed and args.record:
            report.case("Record, verify and delete test WAV", lambda: test_record_and_delete(base_url, status, report))
        elif passed:
            report.skip("Wi-Fi recording/deletion", "pass --record to opt in")
        elif args.record:
            report.skip("Wi-Fi recording/deletion", "API checks failed")
        if args.file_manager:
            if passed and firmware_at_least(str(status.get("version", "")), (0, 1, 76)):
                report.case("File manager read/write operations and exact cleanup", lambda: test_file_operations(base_url, report))
            else:
                report.skip("File manager read/write operations", "requires reachable ready API and firmware 0.1.76+")
        if args.record and not args.preserve_performance:
            if passed and firmware_at_least(str(status.get("version", "")), (0, 1, 90)):
                report.case("Performance setting NVS write and restore", lambda: test_performance_switch(base_url, report))
            else:
                report.skip("Performance setting NVS write and restore", "requires reachable firmware 0.1.90+")
    finally:
        successful = report.summary()
    return 0 if successful else 1


if __name__ == "__main__":
    raise SystemExit(main())