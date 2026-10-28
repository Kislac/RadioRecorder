"""Wi-Fi AP smoke tests; run only when the PC can reach the RadioRecorder AP."""

import argparse
import json
import os
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET
from urllib.request import Request, urlopen

from scripts.oled_viewer import find_device
from scripts.test_device import delete_test_recording
from scripts.test_support import TestRun, firmware_at_least


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
    signal = json.loads(request(base_url, "/api/signal"))
    assert isinstance(signal["rms"], int) and isinstance(signal["enabled"], bool)
    settings = json.loads(request(base_url, "/api/settings"))
    assert 0 <= settings["screenOffMinutes"] <= 999
    pages = (request(base_url, "/"), request(base_url, "/files"), request(base_url, "/settings"))
    page_lengths = tuple(map(len, pages))
    assert all(length > 0 for length in page_lengths)
    assert b'href="/settings"' in pages[0]
    assert b'href="/"' in pages[2]
    report.detail("HTTP /api/status " + json.dumps(status, sort_keys=True))
    report.detail("HTTP /api/settings " + json.dumps(settings, sort_keys=True))
    report.detail("HTTP /api/signal " + json.dumps(signal, sort_keys=True))
    report.detail("HTTP pages: /={} bytes, /files={} bytes, /settings={} bytes".format(*page_lengths))
    return status


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
                if not any(file["name"] == name and file["bytes"] >= 44 for file in files):
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


def main():
    parser = argparse.ArgumentParser(description="Test the ESP HTTP API via its Wi-Fi AP")
    parser.add_argument("--base-url", default="http://192.168.4.1")
    parser.add_argument("--connect", action="store_true", help="Switch Windows Wi-Fi to the open RadioRecorder AP")
    parser.add_argument("--record", action="store_true", help="Write a short real WAV to the SD card")
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
    finally:
        successful = report.summary()
    return 0 if successful else 1


if __name__ == "__main__":
    raise SystemExit(main())