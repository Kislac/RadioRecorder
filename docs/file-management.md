# File management

Firmware 0.1.76 provides shared SD file services for the OLED and the web file manager. The main recording page no longer lists files; use `/files`.

## Browsing and sorting

Both interfaces put folders before files. The default is newest modified date first; valid SD timestamps are preferred, with dated recording/folder names used when timestamps are absent. Equal or unknown dates use descending names, so numbered recordings remain newest first. The web offers name/date/size and ascending/descending choices, folder-local search, and capacity/free-space information. Folder grouping never changes.

Sorting happens on the ESP before pagination. Web pages contain at most 20 items and use an `after` filename cursor plus a directory `revision`; a changed directory requires refresh. Only a bounded candidate page is kept in normal RAM. OLED pages use the same order in five rows.

## Web operations

Select individual rows or all loaded rows for copy, move, or delete. Rename and new-folder dialogs validate names. A destination-folder picker supports copy/move; for a single item its destination name is editable, including copies within the same folder. File and folder copies/moves are supported. Existing destinations are never overwritten.

Upload accepts one or more browser files. Download remains available for files, while folders have ZIP download. WAV files retain browser, speaker, and radio DAC playback; PTT is not activated. Arbitrarily renamed/moved WAVs are discoverable without recording-name prefixes. Playback controls are expandable above the list.

The operation panel shows preparation, file/byte counts, progress, verification, average rate, estimated remaining time and cancellation. Preparation has no guessed percentage. Verification is indeterminate. Batch cancellation stops the current operation and remaining queue; already completed earlier items remain completed.

## OLED operations

Selecting a file opens **Speaker / Radio DAC / Delete / Back**. Delete opens a separate confirmation with **Cancel** selected initially. Successful deletion refreshes the directory and clamps selection to a valid item. Unsupported audio files can still be deleted but cannot be played. Folders remain browsable; rename/copy/move/upload and folder deletion are web-only. Settings remains the last home tile.

## Streaming and safety

Copy/upload use a private `.rr-job-*` staging path, with byte-count/CRC verification before the final rename. Only a staging path created by the current run is cleaned on failure/cancel; existing data is not overwritten or deleted. Copy leaves its source unchanged. Move uses the SD filesystem rename rather than copying the data. Recursive delete cannot be rolled back and may be partial after cancellation or SD errors.

Long operations run in a yielding worker. The normal HTTP service, OLED/main loop, and 10-second watchdog remain active. A worker waiting for a ZIP/upload client is not subscribed to the task watchdog; its connection deadline is 60 seconds. Upload data inactivity times out after 15 seconds. Recording/playback cannot start while a file job is active, and mutations require idle recording/playback. Direct SD downloads are excluded during jobs.

ZIP is streamed on independent HTTP port **82**, using stored entries (no compression), UTF-8 names, data descriptors and a single data/CRC pass. Only per-entry metadata and a 4096-byte data buffer are held on the ESP. The browser's native downloader saves the archive; JavaScript never builds a multi-gigabyte Blob. Progress measures ESP-transmitted ZIP bytes, not confirmed PC-disk writes. The browser reports actual local-save completion. ZIP32 limits are **less than 4 GiB including headers** and **65535 entries including folders**; larger archives fail before streaming. Metadata memory may impose a lower practical entry limit.

Paths are absolute, at most 220 UTF-8 bytes, with tree depth at most 16. Root deletion/rename, dot segments, control characters, FAT-reserved names, forbidden separators, trailing dots/spaces, and the private `.rr-` namespace are rejected. A folder cannot be copied/moved into its own descendant. Unexpected power loss may leave private staging data and FAT corruption; recovery may require checking the card on a PC. The open AP remains unauthenticated: use only a trusted test network.

## API

All modifying requests below use JSON. Job responses are HTTP 202; mkdir/rename success is HTTP 200. Invalid paths/requests are rejected, existing targets and busy resources report conflicts.

| Endpoint | Request |
| --- | --- |
| `GET /api/files` | `dir`, `sort=date/name/size`, `order=asc/desc`, optional `filter`, `after`, `revision` |
| `POST /api/files/mkdir` | `{"path":"/New folder"}` |
| `POST /api/files/rename` | `{"source":"/old.wav","destination":"/new.wav"}` |
| `POST /api/files/jobs` | `action=copy/move/delete/zip/upload`, `source`, `destination`; delete requires `confirm:true`; upload requires `bytes` |
| `GET /api/files/jobs/status` | optional `id`; returns state, file/byte counters and transfer port |
| `POST /api/files/jobs/cancel?id=N` | `{}` |
| `GET http://ESP:82/zip?id=N` | native ZIP transfer after job becomes ready |
| `PUT http://ESP:82/upload?id=N` | raw file body with exact Content-Length; CORS preflight supported |

The older `/api/files/delete` and `/api/files/download` endpoints remain for compatibility. The new UI uses jobs for recursive deletion and large ZIP transfers. In offline preview the transfer routes use the same localhost server at `/transfer/zip` and `/transfer/upload`; simulated files live in PC memory, so use small fixture data rather than operational gigabyte uploads.

## Verification

`python -m unittest test.test_file_manager test.test_oled_viewer test.test_web_preview test.test_language -v` exercises native path/sort/ZIP rules, bounded pagination, changed-directory detection, conflicts, copy/move/rename/delete/upload, cancellation, no-space failures, renamed WAV discovery and home-page list removal. The native ZIP output is verified by Python's ZIP reader with over a hundred entries, and 2-GiB/ZIP32 counters are checked without allocating gigabytes.

Run the complete offline suite/build and standard authorized online suite per [testing](testing.md). Real AP file-operation and multi-gigabyte throughput/cancellation checks require a reachable ESP network. Tests must use dedicated run-specific paths and clean only their own verified artifacts; never delete user recordings to make a test pass.

The opt-in `scripts.test_wifi --file-manager` smoke test creates a unique `FileTest-*` folder, verifies uploaded/copied/renamed/moved bytes through the streamed ZIP, and deletes that exact folder in cleanup. It is included in the write-enabled standard online suite only when the ESP HTTP API is reachable.