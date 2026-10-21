# Changes

## 0.3.0

- Choose This computer in either pane for local browsing, uploads, downloads,
  and local copies. Remote-to-remote jobs retain direct destination-side downloads.
- Desktop transfers use pipelined SFTP with no shell/Python dependency on the
  remote host. They support per-file progress, cancel/resume, removal, empty
  directories, safe publication, and normal destination permissions.
- Desktop transfer checkpoints persist separately from credentials and recover
  interrupted jobs after restart. Keep the application open for local transfers.

## 0.2.1

- Transfer output now uses the destination's original umask, normally producing
  files with mode 0644 and directories with mode 0755. Credential files, job
  metadata, and incomplete downloads remain private. Existing destination
  directory permissions are preserved.

## 0.2.0

- Native Qt interface for direct SFTP downloads to a remote Linux server.
- Independent source/destination profiles, desktop keyring credential storage,
  persisted pane selections, and backups of edited connection settings.
- Sortable directory listings and automatic destination refresh after transfers.
- Individual file queue rows, progress, removal, batch cancellation, and resume.
- Detached remote workers; submitted queues continue when the GUI is closed.
- Arch/Manjaro package build workflow and a desktop launcher.

This is the first packaged release. Development builds previously reported 0.1.0.
