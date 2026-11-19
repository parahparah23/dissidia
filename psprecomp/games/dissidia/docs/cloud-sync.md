# Cloud sync of saves

Keeps the save folders of several installs (Windows, Linux, Android; any region) in step through a synced **folder**, a
**WebDAV** server (Nextcloud, ownCloud, most NAS boxes) or **Google Drive**. It is built in: no extra program.

## Using it

- **Settings menu** (F1 / Guide button) > **Cloud** page: pick the provider and fill in what it needs.
  - *Folder*: any folder another program keeps in sync (Syncthing, the Drive / OneDrive / Dropbox desktop apps, a NAS share).
  - *WebDAV*: the address of a folder on the server (it must be inside an existing folder), user and password. For
    Nextcloud / ownCloud create an **app password** (Settings > Security) and use that.
  - *Google Drive*: **Sign in...** shows a code; enter it at google.com/device on a phone or computer. The game only gets
    Google's `drive.file` permission: it creates a folder called "Dissidia PSPRecomp Saves" in your Drive and can only see
    files it created itself, never your other files. (Google's device sign-in does not allow the hidden `drive.appdata`
    scope, which is why the folder is visible. You can delete the folder at any time.)
- **Settings > Saves** (also **Triangle** on the game's save list) shows the status, **Sync now**, and the choices when a save
  changed on two devices (*Needs your choice*); its **Backups** hold every save a sync replaced. The game is paused while the menu is
  open, so a download never lands while it reads a save, and the save list behind it is refreshed when the menu closes.
- `PSPRECOMP_CLOUD=0` switches the whole feature off (nothing is read, started or contacted).

## When it syncs

| When | What |
|---|---|
| Start of the game | a full sync (uploads and downloads), in the background. The game waits for it (up to 20 s) before its first save dialog, so a download never replaces a save the game is reading |
| A few seconds after the game saves | a full sync: your save goes up and newer saves from your other devices come down. The game waits for a running sync before its next save dialog, so a download never lands while it reads a save. A save changed on two devices is a conflict, as always |
| **Sync now** | a full sync |

*Sync Automatically: Off* leaves only **Sync now**. Replays are left out unless *Include Replays* is on.

## Conflicts

Each install remembers the content hash of every save as of its last sync. A save is uploaded when only this device changed it,
downloaded when only the cloud copy changed, and a **conflict** when both did (or there is no history to tell, e.g. a new
install that already has its own save). A conflict is **never resolved silently**: the save list's hint says
"Triangle: Manage saves (1 needs a choice)", and Settings > Saves (and Settings > Cloud) list it under *Needs your choice*. Its screen
shows both sides: this device and the cloud copy with the name of the device that uploaded it, when each was saved (the newer one
marked, and the cursor starts on it) and the end of the save's own description (Dissidia: the player and play time). Choose *Keep
this device's save* (the cloud copy is kept once as `<slot>.bak` in the cloud), *Use the cloud save* (yours goes into a
`cloud-conflict` backup) or *Decide later*; *Keep the newer save of each* settles all of them at once. A sync never deletes a save,
locally or in the cloud.

**Rolling back.** Every local save a sync replaces is moved into a dated backup first (`cloud-download`, `cloud-conflict`; see
[savedata.md](savedata.md#backups)), and the saves are copied once a day before the start-up sync. *Settings > Saves > Restore a backup*
puts any of them back; the restored save is then uploaded, so the other devices get it at their next sync.

Saves are matched by their slot name without the region ID (`GameData00`, `FCard03`...), so a US and an EU install sync with each
other; a downloaded save is renamed to the installed game's ID and its `PARAM.SFO` signed again (`psp_savemgr`).

The device name in a conflict is the other device's host name (Windows: the computer name; Android: the maker and model);
`PSPRECOMP_DEVICE_NAME` sets this device's.

**Deleting.** A sync never deletes on its own. A save deleted (or moved to another slot) in Settings > Saves leaves a deletion marker in
the cloud (`<slot>.meta` with `deleted=1`; the `.save` is kept once as `<slot>.bak`). Another device whose copy is unchanged since its
last sync moves it into its backups (`deleted`); one that changed it since keeps it and uploads it again. The delete is not done in the
cloud when the cloud copy changed since this device's last sync (it then comes back). Devices with an older runtime ignore the marker
and upload their copy again.

## Where things are stored

| What | Where |
|---|---|
| Settings, secrets (WebDAV password, Google sign-in token) | `<workdir>/cloud.ini` (mode 0600 on Linux/Android; keep it private). Not in `settings.ini`, which is logged and copied around |
| Last-synced hashes, this device's ID | `<workdir>/cloud/state.txt` |
| Replaced local saves | `ms0/PSP/SAVEDATA_BACKUP/<date>-<time>_<reason>/` (savedata.md, Backups) |
| In the cloud | per slot `<slot>.save` (the files in one container), `<slot>.meta` (content hash, container checksum, mtime, device ID and name, disc ID, the save's title and description), optional `<slot>.bak` |

The `.save` is written first and the `.meta` last, so a half-finished upload is never taken for a newer save; a download is
checked against both hashes before it touches anything.

## Google Drive client (for forks)

`runtime/src/hle/psp_cloud_keys.h` holds this project's OAuth client (type *TVs and Limited Input devices*, scope
`drive.file`). Google does not treat that client secret as confidential. A fork should create its own in the Google Cloud
console (APIs & Services: enable the Drive API, OAuth consent screen with the `drive.file` scope and a privacy policy page,
create the client) and replace the two values, or set `PSPRECOMP_GOOGLE_CLIENT_ID` / `_SECRET`.

## Code

| File | Role |
|---|---|
| `psp_cloudsync.{h,cpp}` | provider interface, folder provider, SHA-256, save container, the three-way sync and conflict handling |
| `psp_cloud_providers.{h,cpp}` | WebDAV and Google Drive (device sign-in, refresh, a "Dissidia PSPRecomp Saves" folder in Drive) |
| `psp_http.{h,cpp}` | blocking HTTP/1.1 client; HTTPS through mbedTLS with the embedded Mozilla CA bundle (`assets/ca`), certificate and host name checked |
| `psp_cloud_service.{h,cpp}` | configuration, worker thread, status, debounced push after a save |
| `psp_cloud_glue.{h,cpp}` | the few calls the runtime makes (boot, wait before the first save dialog, after a save, shutdown) |
| `psp_saves_page.cpp`, `psp_settings.cpp` | Settings > Saves (conflicts, backups) and Settings > Cloud |

HTTPS needs **mbedTLS 3.6+** (optional at build time: without it Folder sync still works and https reports "no TLS support").
The toolchain bundle builds it from a pinned source (`toolchain/versions.json`); for a developer build pass
`-DPSPRECOMP_MBEDTLS_PREFIX=<install prefix>`.

## Tests (no network, no accounts)

`test_cloudsync` (two installs through a folder: upload, cross-region download, conflicts, backups, damaged downloads, unsafe
containers), `tests/run_http.sh` (HTTP/TLS client against a local server: chunked and close-delimited bodies, redirects,
timeouts, truncation, an untrusted certificate and a wrong host name refused), `tests/run_cloud.sh` (WebDAV and Google Drive
against a fake server, the service with its real worker thread, sign-in, refresh, revoked sign-in), `test_savemgr` (the conflict
screens with a fake service).
