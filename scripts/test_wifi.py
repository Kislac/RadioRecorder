"""Wi-Fi AP smoke tests; run only when the PC can reach the RadioRecorder AP."""

import argparse
import json
import os
import subprocess
import tempfile
import time
import xml.etree.ElementTree as ET
from urllib.request import Request, urlopen


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


def main():
    parser = argparse.ArgumentParser(description="Test the ESP HTTP API via its Wi-Fi AP")
    parser.add_argument("--base-url", default="http://192.168.4.1")
    parser.add_argument("--connect", action="store_true", help="Switch Windows Wi-Fi to the open RadioRecorder AP")
    parser.add_argument("--record", action="store_true", help="Write a short real WAV to the SD card")
    args = parser.parse_args()
    base_url = args.base_url.rstrip("/")
    if args.connect:
        connect_wifi("RadioRecorder")
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
    signal = json.loads(request(base_url, "/api/signal"))
    assert isinstance(signal["rms"], int) and isinstance(signal["enabled"], bool)
    assert b"<html" in request(base_url, "/").lower()
    assert b"<html" in request(base_url, "/files").lower()
    print("PASS: web pages, status and signal API (firmware " + status["version"] + ")")
    if not args.record:
        return
    if not status["ready"] or status["state"] != "idle":
        raise RuntimeError("Recorder not ready or already active; no recording started")
    name = request(base_url, "/api/start", "POST").decode().strip()
    try:
        deadline = time.monotonic() + 8
        while time.monotonic() < deadline:
            state = json.loads(request(base_url, "/api/status"))["state"]
            if state == "recording":
                break
            time.sleep(0.2)
        else:
            raise AssertionError("Recording did not start")
        time.sleep(1)
    finally:
        request(base_url, "/api/stop", "POST")
    deadline = time.monotonic() + 8
    while time.monotonic() < deadline:
        if json.loads(request(base_url, "/api/status"))["state"] == "idle":
            break
        time.sleep(0.2)
    else:
        raise AssertionError("Recording did not stop")
    files = json.loads(request(base_url, "/api/recordings"))["files"]
    if not any(file["name"] == name and file["bytes"] >= 44 for file in files):
        raise AssertionError("New recording not found in file list: " + name)
    print("PASS: recorded and finalized " + name)


if __name__ == "__main__":
    main()