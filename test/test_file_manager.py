import os
import http.client
import io
import json
from pathlib import Path
import shutil
import subprocess
from tempfile import TemporaryDirectory
import unittest
import threading
import zipfile
from types import SimpleNamespace

from scripts.preview_web import PreviewDevice, PreviewServer
from scripts.test_wifi import test_file_operations

ROOT = Path(__file__).resolve().parents[1]


class FileRulesTests(unittest.TestCase):
    def test_preview_sorted_cursor_pages_and_directory_revision(self):
      device = PreviewDevice()
      device.files = {f"/file{index:03d}.wav": b"x" * index for index in range(250)}
      device.directories = {"/2026-11-01", "/2026-10-31"}
      device.file_manager.modified.update({name: index for index, name in enumerate(device.files)})
      status, page = device.api("GET", "/api/files", {})
      self.assertEqual(status, 200)
      self.assertEqual([item["name"] for item in page["items"][:3]], ["2026-11-01", "2026-10-31", "file249.wav"])
      collected = list(page["items"])
      while page["next"]:
        status, page = device.api("GET", "/api/files", {"after": page["next"], "revision": page["revision"]})
        self.assertEqual(status, 200)
        collected.extend(page["items"])
      self.assertEqual(len(collected), 252)
      self.assertEqual(len({item["path"] for item in collected}), 252)
      device.files["/new.wav"] = b"new"
      self.assertEqual(device.api("GET", "/api/files", {"revision": page["revision"]})[0], 409)
      status, filtered = device.api("GET", "/api/files", {"filter": "file00", "sort": "name", "order": "asc"})
      self.assertEqual((status, filtered["total"]), (200, 10))

    def test_preview_mutations_conflicts_busy_and_cancellation(self):
      device = PreviewDevice()
      manager = device.file_manager
      self.assertEqual(device.api("POST", "/api/files/mkdir", {"path": "/Target"})[0], 200)
      self.assertEqual(device.api("POST", "/api/files/mkdir", {"path": "/target"})[0], 409)
      self.assertEqual(device.api("POST", "/api/files/rename", {"source": "/REC00001.wav", "destination": "/Target/new.wav"})[0], 200)
      self.assertIn("/Target/new.wav", device.files)
      self.assertNotIn("/REC00001.wav", device.files)
      self.assertEqual(device.api("POST", "/api/files/jobs", {"action": "copy", "source": "/Target", "destination": "/Target/child"})[0], 400)
      self.assertEqual(device.api("POST", "/api/files/jobs", {"action": "copy", "source": "/Target", "destination": "/Copied"})[0], 202)
      manager.worker.join(2)
      self.assertEqual(device.files["/Copied/new.wav"], device.files["/Target/new.wav"])
      device.files["/large.wav"] = bytes(2 * 1024 * 1024)
      status, job = device.api("POST", "/api/files/jobs", {"action": "copy", "source": "/large.wav", "destination": "/cancelled.wav"})
      self.assertEqual(status, 202)
      self.assertEqual(device.api("POST", "/api/start", {})[0], 409)
      self.assertEqual(device.api("POST", "/api/files/jobs/cancel", {"id": str(job["id"])})[0], 200)
      manager.worker.join(2)
      self.assertEqual(manager.job["state"], "cancelled")
      self.assertNotIn("/cancelled.wav", device.files)
      self.assertEqual(device.api("POST", "/api/files/jobs", {"action": "delete", "source": "/Target"})[0], 400)
      self.assertEqual(device.api("POST", "/api/files/jobs", {"action": "delete", "source": "/Target", "confirm": True})[0], 202)
      manager.worker.join(2)
      self.assertNotIn("/Target", device.directories)
      status, pending = device.api("POST", "/api/files/jobs", {"action": "zip", "source": "/Copied"})
      self.assertEqual(status, 202)
      self.assertEqual(device.api("POST", "/api/files/jobs/cancel", {"id": str(pending["id"])})[0], 200)
      self.assertFalse(manager.busy())
      manager.capacity = 65536
      self.assertEqual(device.api("POST", "/api/files/jobs", {"action": "copy", "source": "/Copied/new.wav", "destination": "/full.wav"})[0], 409)
      self.assertNotIn("/full.wav", device.files)
      manager.capacity = 8 * 1024**3
      device.files["/odd123456_123456.wav"] = device.wav
      self.assertEqual(device.api("GET", "/api/files", {})[0], 200)
      self.assertIn("/Copied/new.wav", [item["path"] for item in device.api("GET", "/api/recordings", {})[1]["files"]])

    def test_http_streaming_zip_and_upload_jobs(self):
      server = PreviewServer(("127.0.0.1", 0))
      thread = threading.Thread(target=server.serve_forever, daemon=True)
      thread.start()
      self.addCleanup(server.server_close)
      self.addCleanup(server.shutdown)

      def request(method, path, data=None, raw=False):
        client = http.client.HTTPConnection(*server.server_address, timeout=5)
        body = data if raw else json.dumps(data) if data is not None else None
        client.request(method, path, body, {"Content-Type": "application/octet-stream" if raw else "application/json"})
        response = client.getresponse()
        result = response.status, response.read(), dict(response.getheaders())
        client.close()
        return result

      server.device.directories.update({"/Bundle", "/Bundle/Empty"})
      for index in range(120):
        server.device.files[f"/Bundle/file{index}.wav"] = bytes(range(256)) * 4
      status, payload, _ = request("POST", "/api/files/jobs", {"action": "zip", "source": "/Bundle"})
      self.assertEqual(status, 202)
      job = json.loads(payload)
      status, content, headers = request("GET", "/transfer/zip?id=" + str(job["id"]))
      self.assertEqual(status, 200)
      self.assertEqual(int(headers["Content-Length"]), len(content))
      with zipfile.ZipFile(io.BytesIO(content)) as archive:
        self.assertIsNone(archive.testzip())
        self.assertEqual(len(archive.namelist()), 122)
        self.assertIn("Bundle/Empty/", archive.namelist())
      self.assertEqual(server.device.file_manager.job["state"], "complete")
      status, payload, _ = request("POST", "/api/files/jobs", {"action": "upload", "destination": "/uploaded.wav", "bytes": 7})
      self.assertEqual(status, 202)
      self.assertEqual(request("PUT", "/transfer/upload?id=" + str(json.loads(payload)["id"]), b"payload", True)[0], 200)
      self.assertEqual(server.device.files["/uploaded.wav"], b"payload")
      self.assertIn("exact test folder removed", test_file_operations(
        "http://127.0.0.1:" + str(server.server_address[1]), SimpleNamespace(detail=lambda message: None)))
      self.assertFalse(any(path.startswith("/FileTest-") for path in server.device.directories))

    def test_native_path_sorting_and_bounded_page_rules(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            self.skipTest("Host g++ required for file manager rules")
        harness = r'''
#include <cassert>
#include "file_rules.h"
int main() {
  assert(managedPathValid("/2026-11-01/record.wav"));
  for (const char *path : {"/../x", "/x/", "/x//y", "/x/CON.txt", "/x/name.", "/x/name ", "/.rr-private", "/x/a?b"}) assert(!managedPathValid(path));
  assert(!managedDestinationValid("/folder", "/folder/child"));
  assert(!managedDestinationValid("/folder", "/FOLDER"));
  assert(managedDestinationValid("/folder", "/other/folder"));
  std::vector<ManagedFileInfo> entries;
  for (int index=0; index<300; ++index) {
    ManagedFileInfo entry;
    entry.name = "item" + std::to_string(index);
    entry.modified = index;
    entry.directory = index % 2 == 0;
    managedPageInsert(entries, entry, 6, "date", true);
    assert(entries.size() <= 6);
  }
  assert(entries.front().name == "item298");
  ManagedFileInfo file; file.name="new.wav"; file.modified=1000;
  assert(managedFileBefore(entries.front(), file));
  assert(!managedFileBefore(file, entries.front()));
  ManagedFileInfo older=file; older.modified=999;
  assert(managedFileBefore(file, older));
  assert(managedFileBefore(older, file, "date", false));
  file.bytes=10; older.bytes=20;
  assert(managedFileBefore(older, file, "size", true));
  file.modified=older.modified=0; file.name="REC00002.wav"; older.name="REC00001.wav";
  assert(managedFileBefore(file,older,"date",true));
}
'''
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / "rules.cpp").write_text(harness, encoding="utf-8")
            executable = directory / ("rules.exe" if os.name == "nt" else "rules")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                                       str(directory / "rules.cpp"), "-o", str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            result = subprocess.run([str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_native_streaming_zip_format_and_gigabyte_counters(self):
        compiler = shutil.which("g++")
        fallback = Path.home() / ".platformio/packages/toolchain-gccmingw32/bin/g++.exe"
        if compiler is None and fallback.is_file():
            compiler = str(fallback)
        if compiler is None:
            self.skipTest("Host g++ required for native ZIP regression")
        harness = r'''
#include <cassert>
#include <fstream>
#include <vector>
#include "zip_format.h"
struct Output {
  std::vector<uint8_t> bytes;
  bool word(uint16_t value) { bytes.push_back(value); bytes.push_back(value >> 8); return true; }
  bool dword(uint32_t value) { return word(value) && word(value >> 16); }
  bool text(const std::string &value) { bytes.insert(bytes.end(), value.begin(), value.end()); return true; }
};
int main(int argc, char **argv) {
  assert(argc==2);
  ManagedZipTotals large;
  assert(large.add(12, 2UL*1024*1024*1024));
  assert(large.archiveBytes > 2ULL*1024*1024*1024);
  assert(!large.add(12, 2UL*1024*1024*1024));
  ManagedZipTotals many;
  for (int index=0; index<65535; ++index) assert(many.add(1,0));
  assert(!many.add(1,0));
  Output output;
  std::vector<uint32_t> offsets, crcs;
  std::vector<std::string> names;
  const std::string payload="streamed content";
  ManagedZipTotals total;
  for (int index=0; index<121; ++index) {
    std::string name=index==0 ? "Bundle/Empty/" : "Bundle/file"+std::to_string(index)+".wav";
    uint32_t size=index==0 ? 0 : payload.size();
    assert(total.add(name.size(),size));
    names.push_back(name); offsets.push_back(output.bytes.size());
    assert(managedZipLocal(output,name));
    uint32_t crc=index==0 ? 0 : ~managedZipCrc(0xffffffffU,reinterpret_cast<const uint8_t *>(payload.data()),payload.size());
    crcs.push_back(crc);
    if (size) output.text(payload);
    assert(managedZipDescriptor(output,crc,size));
  }
  uint32_t central=output.bytes.size();
  for (size_t index=0; index<names.size(); ++index) assert(managedZipCentral(output,names[index],crcs[index],index==0 ? 0 : payload.size(),offsets[index],index==0));
  assert(managedZipEnd(output,names.size(),output.bytes.size()-central,central));
  assert(output.bytes.size()==total.archiveBytes);
  std::ofstream archive(argv[1],std::ios::binary);
  archive.write(reinterpret_cast<const char *>(output.bytes.data()),output.bytes.size());
}
'''
        environment = dict(os.environ)
        environment["PATH"] = str(Path(compiler).parent) + os.pathsep + environment.get("PATH", "")
        with TemporaryDirectory() as folder:
            directory = Path(folder)
            (directory / "zip.cpp").write_text(harness, encoding="utf-8")
            executable = directory / ("zip.exe" if os.name == "nt" else "zip")
            compiled = subprocess.run([compiler, "-std=c++11", "-Wall", "-Wextra", "-Werror", "-I", str(ROOT / "include"),
                                       str(directory / "zip.cpp"), "-o", str(executable)], capture_output=True, text=True, env=environment)
            self.assertEqual(compiled.returncode, 0, compiled.stdout + compiled.stderr)
            archive_path = directory / "native.zip"
            result = subprocess.run([str(executable), str(archive_path)], capture_output=True, env=environment)
            self.assertEqual(result.returncode, 0, result.stderr)
            with zipfile.ZipFile(archive_path) as archive:
                self.assertIsNone(archive.testzip())
                self.assertEqual(len(archive.namelist()), 121)
                self.assertEqual(archive.read("Bundle/file120.wav"), b"streamed content")