"""In-memory file-manager API and streaming transfers for the offline preview."""

from functools import cmp_to_key
from datetime import datetime, timezone
import hashlib
import json
from pathlib import PurePosixPath
import re
import threading
import time
import zipfile


def valid_path(path, root=True):
    if not isinstance(path, str) or not path.startswith("/") or len(path.encode("utf-8")) > 220:
        return False
    if path == "/":
        return root
    for name in path[1:].split("/"):
        if not name or name in (".", "..") or name.endswith((".", " ")) or name.lower().startswith(".rr-"):
            return False
        if re.search(r'[\x00-\x1f\x7f\\:"<>|?*]', name):
            return False
        if re.fullmatch(r"(?:con|prn|aux|nul|com[1-9]|lpt[1-9])", name.split(".")[0], re.I):
            return False
    return True


class PreviewFileManager:
    def __init__(self, device):
        self.device = device
        self.modified = {}
        self.job = {"id": 0, "state": "idle"}
        self.cancel = threading.Event()
        self.lock = threading.RLock()
        self.worker = None
        self.capacity = 8 * 1024**3

    def busy(self):
        return self.job["state"] in ("preparing", "ready", "running", "verifying")

    def exists(self, path):
        return any(name.lower() == path.lower() for name in (*self.device.directories, *self.device.files))

    def ready(self):
        return self.device.sd_ready and not self.device.recording and not self.device.playing and not self.busy()

    def stamp(self, path):
        if path in self.modified:
            return self.modified[path]
        name = PurePosixPath(path).name
        if re.fullmatch(r"\d{4}-\d{2}-\d{2}", name):
            return int(datetime.strptime(name, "%Y-%m-%d").replace(tzinfo=timezone.utc).timestamp())
        match = re.search(r"(\d{6})_(\d{6})", name)
        try:
            return int(datetime.strptime("".join(match.groups()), "%y%m%d%H%M%S").replace(tzinfo=timezone.utc).timestamp()) if match else 0
        except ValueError:
            return 0

    def listing(self, params):
        directory = params.get("dir", "/")
        if not valid_path(directory) or directory != "/" and directory not in self.device.directories:
            return 404, "Directory not found"
        if self.device.recording or not self.device.sd_ready:
            return 409, "SD unavailable or recording in progress"
        sort, order = params.get("sort", "date"), params.get("order", "desc")
        if sort not in ("date", "name", "size") or order not in ("asc", "desc"):
            return 400, "Invalid sort order"
        items = []
        for folder, names in ((True, self.device.directories), (False, self.device.files)):
            for path in names:
                if str(PurePosixPath(path).parent) != directory or PurePosixPath(path).name.startswith(".rr-"):
                    continue
                items.append({"name": PurePosixPath(path).name, "path": path, "directory": folder,
                              "bytes": 0 if folder else len(self.device.files[path]), "modified": self.stamp(path)})
        revision = hashlib.sha256(json.dumps(sorted(items, key=lambda item: item["path"]), sort_keys=True).encode()).hexdigest()[:16]
        if params.get("revision") and params["revision"] != revision:
            return 409, "Directory changed; refresh the list"

        def compare(first, second):
            if first["directory"] != second["directory"]:
                return -1 if first["directory"] else 1
            field = "modified" if sort == "date" else "bytes" if sort == "size" and not first["directory"] else "name"
            left, right = first[field], second[field]
            if field == "name":
                left, right = left.lower(), right.lower()
            if left != right:
                direction = -1 if order == "desc" and (field != "name" or sort == "name") else 1
                return direction * (-1 if left < right else 1)
            direction = -1 if sort == "date" and order == "desc" else 1
            return direction * (-1 if first["name"].lower() < second["name"].lower() else 1 if first["name"].lower() > second["name"].lower() else 0)

        items.sort(key=cmp_to_key(compare))
        after = params.get("after", "")
        start = 0
        if after:
            matches = [index for index, item in enumerate(items) if item["name"] == after]
            if not matches:
                return 409, "Directory changed; refresh the list"
            start = matches[0] + 1
        query = params.get("filter", "").lower()
        visible = [item for item in items[start:] if query in item["name"].lower()]
        total = sum(query in item["name"].lower() for item in items)
        if "offset" in params and not after:
            visible = visible[int(params["offset"]):]
        page = visible[:20]
        return 200, {"items": page, "total": total, "next": page[-1]["name"] if len(visible) > 20 else None,
                     "revision": revision, "capacity": self.capacity,
                     "used": sum(map(len, self.device.files.values()))}

    def api(self, method, path, params):
        if method == "GET" and path == "/api/files":
            return self.listing(params)
        if method == "GET" and path == "/api/files/jobs/status":
            return (200, dict(self.job)) if int(params.get("id", 0)) in (0, self.job["id"]) else (404, {})
        if method == "POST" and path == "/api/files/jobs/cancel":
            if not self.busy() or int(params.get("id", 0)) != self.job["id"]:
                return 409, "Operation is not active"
            self.cancel.set()
            if self.job["state"] == "ready":
                self.job.update(state="cancelled", error="Cancelled")
            return 200, {}
        if method != "POST" or path not in ("/api/files/mkdir", "/api/files/rename", "/api/files/jobs"):
            return None
        if not self.ready():
            return 409, "SD, recorder, playback or file operation busy"
        if path == "/api/files/mkdir":
            target = params.get("path", "")
            if not valid_path(target, False) or str(PurePosixPath(target).parent) not in self.device.directories | {"/"}:
                return 400, "Invalid folder path"
            if self.exists(target):
                return 409, "Destination already exists"
            self.device.directories.add(target)
            self.modified[target] = int(time.time())
            return 200, {"ok": True}
        source, destination = params.get("source", ""), params.get("destination", "")
        action = "move" if path.endswith("rename") else params.get("action", "")
        if action not in ("copy", "move", "delete", "zip", "upload"):
            return 400, "Invalid operation"
        if action != "upload" and (not valid_path(source, False) or not self.exists(source)):
            return 404, "Source not found or invalid"
        if action in ("copy", "move", "upload"):
            if not valid_path(destination, False) or str(PurePosixPath(destination).parent) not in self.device.directories | {"/"}:
                return 400, "Invalid destination"
            if action != "upload" and (source.lower() == destination.lower() or destination.lower().startswith(source.lower() + "/")):
                return 400, "Invalid destination"
            if self.exists(destination):
                return 409, "Destination already exists"
        if action == "delete" and params.get("confirm") is not True:
            return 400, "Missing deletion confirmation"
        files = [name for name in self.device.files if name == source or name.startswith(source + "/")]
        directories = sorted(name for name in self.device.directories if name == source or name.startswith(source + "/"))
        if path.endswith("rename"):
            renamed = {destination + name[len(source):]: self.device.files[name] for name in files}
            timestamps = {destination + name[len(source):]: self.stamp(name) for name in (*files, *directories)}
            for name in files:
                del self.device.files[name]
            self.device.directories.difference_update(directories)
            self.device.directories.update(destination + name[len(source):] for name in directories)
            self.device.files.update(renamed)
            self.modified.update(timestamps)
            return 200, {"ok": True}
        count = 1 if action == "upload" else len(files)
        size = params.get("bytes", 0) if action == "upload" else sum(len(self.device.files[name]) for name in files)
        if not isinstance(size, int) or isinstance(size, bool) or not 0 <= size <= 0xffffffff:
            return 400, "Invalid file size"
        if action in ("copy", "upload") and self.capacity - sum(map(len, self.device.files.values())) < size + 65536:
            return 409, "Not enough free space"
        self.cancel = threading.Event()
        self.job = {"id": self.job["id"] + 1, "state": "preparing", "action": action, "source": source,
                    "destination": destination, "totalBytes": size, "completedBytes": 0, "totalFiles": count,
                    "completedFiles": 0, "sentBytes": 0, "archiveBytes": 0, "transferPort": 0, "error": ""}
        if action in ("zip", "upload"):
            self.job["state"] = "ready"
        else:
            self.worker = threading.Thread(target=self.run, args=(files, directories), daemon=True)
            self.worker.start()
        return 202, dict(self.job)

    def run(self, files, directories):
        self.job["state"] = "running"
        action, source, destination = (self.job[name] for name in ("action", "source", "destination"))
        staged = {}
        try:
            for name in files:
                data = self.device.files[name]
                for offset in range(0, len(data), 4096):
                    if self.cancel.wait(0.001):
                        raise InterruptedError("Cancelled; deletion may be partial")
                    self.job["completedBytes"] += len(data[offset:offset + 4096])
                staged[destination + name[len(source):]] = data
                self.job["completedFiles"] += 1
            if self.cancel.is_set():
                raise InterruptedError("Cancelled")
            with self.device.lock:
                if action in ("move", "delete"):
                    for name in files:
                        del self.device.files[name]
                    self.device.directories.difference_update(directories)
                if action in ("copy", "move"):
                    self.device.files.update(staged)
                    self.device.directories.update(destination + name[len(source):] for name in directories)
                    for name in staged:
                        self.modified[name] = self.stamp(source + name[len(destination):])
            self.job["state"] = "complete"
        except Exception as error:
            self.job.update(state="cancelled" if self.cancel.is_set() else "failed", error=str(error))

    def stream_zip(self, handler, job_id):
        if self.job["id"] != job_id or self.job["action"] != "zip" or self.job["state"] != "ready":
            handler.reply(409, "Transfer is not ready")
            return
        self.job["state"] = "running"
        source = self.job["source"]
        label = PurePosixPath(source).name
        files = [(name, data) for name, data in self.device.files.items() if name == source or name.startswith(source + "/")]
        directories = sorted(name for name in self.device.directories if name == source or name.startswith(source + "/"))
        names = [(label + name[len(source):] + "/", b"") for name in directories] + [(label + name[len(source):], data) for name, data in files]
        self.job["archiveBytes"] = 22 + sum(30 + 16 + 46 + len(name.encode()) * 2 + len(data) for name, data in names)
        handler.send_response(200)
        handler.send_header("Content-Type", "application/zip")
        handler.send_header("Content-Disposition", 'attachment; filename="archive.zip"')
        handler.send_header("Content-Length", str(self.job["archiveBytes"]))
        handler.end_headers()
        manager = self

        class Sink:
            def write(self, data):
                if manager.cancel.is_set():
                    raise InterruptedError("Cancelled")
                handler.wfile.write(data)
                manager.job["sentBytes"] += len(data)
                return len(data)

            def flush(self):
                handler.wfile.flush()

        self.job["state"] = "running"
        try:
            with zipfile.ZipFile(Sink(), "w", zipfile.ZIP_STORED, allowZip64=False) as archive:
                for name, data in names:
                    with archive.open(name, "w") as output:
                        for offset in range(0, len(data), 4096):
                            output.write(data[offset:offset + 4096])
                            self.job["completedBytes"] += len(data[offset:offset + 4096])
                    if not name.endswith("/"):
                        self.job["completedFiles"] += 1
            self.job["state"] = "complete"
        except (OSError, InterruptedError) as error:
            self.job.update(state="cancelled" if self.cancel.is_set() else "failed", error=str(error))

    def upload(self, handler, job_id):
        if self.job["id"] != job_id or self.job["action"] != "upload" or self.job["state"] != "ready":
            handler.reply(409, "Transfer is not ready")
            return
        expected = self.job["totalBytes"]
        if int(handler.headers.get("Content-Length", -1)) != expected:
            handler.reply(400, "Invalid file size")
            return
        self.job["state"] = "running"
        self.job["state"] = "running"
        output = bytearray()
        try:
            while len(output) < expected:
                if self.cancel.is_set():
                    raise InterruptedError("Cancelled")
                data = handler.rfile.read(min(4096, expected - len(output)))
                if not data:
                    raise OSError("Connection closed")
                output.extend(data)
                self.job["completedBytes"] = len(output)
            self.device.files[self.job["destination"]] = bytes(output)
            self.modified[self.job["destination"]] = int(time.time())
            self.job.update(state="complete", completedFiles=1)
            handler.reply(200, {"ok": True})
        except Exception as error:
            self.job.update(state="cancelled" if self.cancel.is_set() else "failed", error=str(error))
            handler.reply(500, str(error))