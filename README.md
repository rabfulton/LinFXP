# LinFXP

A native Qt 6 desktop application for local/remote file transfers on Linux,
designed for Manjaro, X11, and XFCE. Choose **This computer** or a saved server
independently in each pane. Uploads and downloads use SFTP on the desktop;
remote-to-remote transfers run on the destination server, keeping the file data
off your desktop connection.

## Build and run

From the project directory:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j4
./build/linfxp
```

Local build dependencies: C++20 compiler, CMake, pkg-config, Qt 6 Widgets and
Concurrent, libssh >= 0.11, and libsecret. The default build also enables tests,
requiring Qt 6 Test, Python 3, and Paramiko; use `-DBUILD_TESTING=OFF` to build
only the application. Password storage requires a running Secret Service keyring,
such as GNOME Keyring. If none is available, uncheck **Remember secret** to use
credentials for the current session.

Optional installation after reviewing the application:

```sh
cmake --install build --prefix "$HOME/.local"
```

The desktop launcher uses `linfxp`, so `$HOME/.local/bin` must be in `PATH` when
installing with that prefix.

## Manjaro installation and updates

Build a pacman package from the current source:

```sh
python3 scripts/package.py --build
```

The script creates an immutable source archive, a checksum-verified `PKGBUILD`,
and `.SRCINFO` under `dist/linfxp-VERSION-REVISION/`. It runs `makepkg`, including
the GUI and localhost SSH/SFTP tests, and prints the exact installation command:

```sh
sudo pacman -U /absolute/path/to/linfxp-VERSION-REVISION-x86_64.pkg.tar.zst
```

This installs `/usr/bin/linfxp`, a desktop launcher under
`/usr/share/applications/`, documentation under `/usr/share/doc/linfxp/`, and the
MIT licence under `/usr/share/licenses/linfxp/`.
LinFXP will appear in XFCE's Internet/Network application menu. Saved connections,
queues, backups, and keyring credentials stay in your user account; none are
included in the package. Python is a local test dependency. Server-to-server
transfers additionally require Python 3 and OpenSSH on the destination;
desktop uploads/downloads require only SFTP on the remote endpoint.

The build script never runs sudo, installs dependencies, or installs the package.
On a fresh Manjaro machine, prepare the build dependencies with:

```sh
sudo pacman -Syu --needed base-devel cmake ninja pkgconf qt6-base libssh libsecret python python-paramiko
```

`gnome-keyring` is optional if you already have another Secret Service provider.
Review the generated `PKGBUILD` before building or installing if moving the source
to another machine. To generate just the archive/recipe, omit `--build`.

After source changes, close the running application, rerun the build command,
and install the newly printed package with `pacman -U`. Each run automatically
uses the next unused local package revision (`0.3.0-1`, `0.3.0-2`, ...), so pacman
can track upgrades without changing the application version for every edit.
Keep `dist/` between builds for that revision history. Set `--release N` if
building on another checkout where you need a revision newer than the installed
one; the script refuses to overwrite an existing output directory.

For a release with new features, update `VERSION` and `CHANGELOG.md`, then build
again. CMake, the application's `linfxp --version` output, and package metadata
all read the same version. Package revisions start again at 1 for a new version.
Builds have been verified on x86_64; the recipe also allows
native aarch64 builds, which have not been tested here.

This is a **local package**, not a subscribed update channel: `pacman -Syu` or
Pamac will not fetch newer LinFXP builds automatically. A hosted source repository
plus release tags would enable a stable upstream; an AUR recipe could then let an
AUR helper rebuild updates. A signed binary package repository would enable
normal pacman/Pamac updates. Hosting the source on GitHub alone does not enable
automatic package updates. The generated local recipe currently uses the source
directory as its homepage; replace that URL with your repository URL before
publishing a public package recipe.

Keep a previous package if you want to roll back. Installing it with `pacman -U`
replaces the application files; compatibility with settings written by later
versions must be checked before a downgrade. To remove the application:

```sh
sudo pacman -R linfxp
```

This package does not remove your user settings or credentials on uninstall.

## Use

1. For a remote connection, add a server using **New…** in either pane.
   Each pane starts with no selected connection.
   Enter the host, SSH port, username, initial folder, and either a password or
   private key file. Passwords and key passphrases can be stored in the desktop
   keyring; connection settings contain no passwords.
2. Choose **This computer** or a saved server in either pane and click **Open**.
   Use the **…** button beside the path to choose a local directory, or type a
   path and press Enter. Verify the host-key fingerprint on first remote connection.
3. Double-click folders to browse, or enter a remote path and press Enter.
   Click **Name**, **Size**, or **Modified** to sort a directory listing; click
   the same header again to reverse the order. Size and date sorts use raw values.
4. Select files/folders in the source pane and click **Queue selected →**.
   Folders are scanned recursively, and each file appears as its own queue row,
   retaining its relative folder path. Empty directories are preserved.
   Symbolic links and special files are excluded.
   Select individual queue rows and click **Remove selected** to exclude them.
5. Click **Start queue**. The **Method** column identifies Upload, Download,
   Local copy, or Server → server. Uploads/downloads/local copies run in the
   desktop application: keep the window open until they finish, or cancel and
   resume later. Server → server jobs are dispatched to the destination and run
   there independently; those submitted jobs continue after closing the GUI.
6. Reopen LinFXP to monitor submitted jobs. Double-click a queue row for details.
   Use **Retry / resume** after a failed or cancelled transfer.
   The destination directory automatically refreshes when its submitted queue
   finishes. It keeps the folder you are currently browsing.

Uploads and downloads need only SFTP on the remote host; shell access and remote
Python are unnecessary. Passwords, key files, and your local SSH agent can be used.
For Server → server transfers, the destination additionally needs Python 3,
an OpenSSH client (`ssh`), SSH command execution, and network access to the source.
Neither server needs a service, root access, or a Python package installed.

## Transfer behaviour

- New output directories follow the destination SSH session's original umask.
  Completed files use mode `0666` filtered by that umask: the usual `0022` gives
  `0644` files and `0755` directories; `0002` gives `0664` and `0775`.
  Credential/job files and incomplete downloads stay private. Existing
  destination directories are not chmodded, and source executable bits are
  not copied. Downloads completed by older builds retain their old permissions
  until repaired separately.

- The remote worker speaks SFTP v3 over the destination's OpenSSH client with
  pipelined reads. Progress counts acknowledged, flushed file bytes.
- Uploads/downloads use libssh's pipelined SFTP directly from the desktop.
  Local-to-local copies are supported too. Each batch uses one connection per
  remote endpoint and bounded memory; files are not staged on a third host.
  Desktop checkpoints are stored under the Qt application data directory,
  normally `~/.local/share/LinFXP/LinFXP/desktop-jobs/`. Credentials are never
  written to these checkpoints. Interrupted jobs can be retried after reopening.
- Completed files are created with the destination's normal creation permissions;
  incomplete files are private. Existing destination directories retain their
  permissions. Modification times are copied, but source executable bits are not.
- Completed files are published atomically. Existing destination files are
  never overwritten. An existing file makes the job fail; select another
  destination folder or resolve the conflict before retrying.
- Incomplete files are stored alongside the destination as hidden
  `.filename.linfxp-JOBID.part` files. Cancel leaves those files for resume.
- File queue rows share an underlying transfer batch so a folder uses one SFTP
  connection. **Remove selected** removes local waiting entries before dispatch;
  after dispatch it asks the worker to skip selected files. An active selected
  file is stopped, and the worker continues with the remaining files. Removing
  completed entries only removes them from the display; downloaded files are
  retained. Removed files remain excluded when retrying a failed batch.
- **Cancel batch** stops the batch containing the selected row(s), and
  **Retry / resume** retries their batches while retaining completed files.
  Transfers already dispatched by an older application version must be cancelled
  before removing individual entries; new transfers support remote removal.
- Resume uses durable checkpoints and checks the source size and modification
  time. It assumes source content has not changed while retaining the same size
  and timestamp; it does not perform a full checksum of the existing prefix.
- Remote workers and job metadata live in `~/.local/share/linfxp/` on the
  destination. Completed metadata and cancelled partial files are retained.
  **Clear finished** removes completed entries from the desktop queue only.
- Unexpected worker termination or a destination reboot is detected on the next
  status check. Retry resumes the job. A monitoring connection failure alone
  does not cancel a remote download.
- Profiles and the desktop queue are saved under the platform's Qt application
  config directory, normally `~/.config/LinFXP/LinFXP/state.json`.
- For Server → server jobs, source credentials are sent to the destination over authenticated SSH.
  Passwords/passphrases are held in process memory and provided to OpenSSH by
  an askpass helper. Private keys are written to temporary mode-0600 files on
  the destination and removed on normal completion, failure, or cancellation.
  An abrupt kill/power loss can leave a temporary key behind in that job's
  directory; remove it when recovering the server. The destination account must
  be trusted with source credentials.
- First connections require host-key confirmation. Changed host keys are
  rejected. The destination worker pins the source key verified by the GUI.
- For Server → server jobs, local SSH-agent authentication is supported for the destination. Source key
  authentication requires an explicit private key file so the detached worker
  can authenticate without the desktop. No agent forwarding is used.

This initial version does not implement drag-and-drop, automatic timed retries,
SSH jump hosts/config aliases, bandwidth limits, or interactive MFA.
Manifests capture source filenames, sizes, and timestamps at queue time; requeue
if the source changes. Endpoint details are captured with each job; authentication
credentials are refreshed from its saved connection when retrying.

Each pane remembers its selected connection independently. If the same saved
connection is selected in both panes, editing it in one pane creates a separate
copy for that pane. Existing connection settings are backed up to timestamped
`connections-*.json.bak` files beside `state.json` before profile changes.

## Validation

```sh
python3 -m unittest discover -s tests -v
# Or, after building:
ctest --test-dir build --output-on-failure
```

Tests use Paramiko (test dependency only) to run a temporary SSH/SFTP server on
localhost. They exercise the native libssh client, remote script upload and
execution, password/key authentication, Unicode and quoted filenames, short
SFTP reads, cancellation/resume, detached queue execution, destination conflicts,
path validation, changed host keys, changed source files, and dead-worker status.
They do not connect to your saved servers.

`build/test_profiles` runs Qt GUI regression tests with temporary settings. It
checks independent pane setup, shared-profile edits, selection persistence,
connection backups, preservation of legacy settings, and preview isolation.

Render an interface preview with example data:

```sh
QT_QPA_PLATFORM=offscreen QT_QPA_PLATFORMTHEME=none QT_STYLE_OVERRIDE=Fusion \
  ./build/linfxp --screenshot /tmp/linfxp-preview.png
```

## Publishing

Commit the source, tests, documentation, and packaging scripts. `.gitignore`
excludes build directories, generated packages, local tooling configuration,
and common credential files. Review the staged files before your first push;
ignore rules do not remove files that Git already tracks.

Do not commit saved connection settings, SSH keys, or keyring data. Screenshots
should use example data, as in the preview command above. Put built packages and
source archives in GitHub Releases rather than committing `build/` or `dist/`.
Rebuild packages after these licence changes: existing archives are immutable
and still contain the previous licence placeholder.

## Licence

LinFXP is licensed under the [MIT License](LICENSE).
Third-party dependencies retain their own licences.
