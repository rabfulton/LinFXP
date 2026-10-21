"""Detached Linux transfer worker. Only Python 3 and OpenSSH are required.

SSH supplies the encrypted transport; this module speaks SFTP v3 on its pipes.
No source shell access, Python packages, or remote installation is required.
"""
import base64
import json
import os
from pathlib import Path
import struct
import subprocess
import sys
import time


def u32(n):
    return struct.pack('>I', n)


def string(value):
    if isinstance(value, str):
        value = value.encode('utf-8')
    return u32(len(value)) + value


def take_string(data, offset=0):
    size = struct.unpack_from('>I', data, offset)[0]
    end = offset + 4 + size
    if end > len(data):
        raise RuntimeError('Truncated SFTP response')
    return data[offset + 4:end], end


class SFTP:
    def __init__(self, source, directory):
        self.directory = Path(directory)
        known = self.directory / 'known_hosts'
        known.write_text(source['known_host'])
        known.chmod(0o600)
        askpass = self.directory / 'askpass'
        askpass.write_text('#!/bin/sh\nprintf "%s\\n" "$LINFXP_SECRET"\n')
        askpass.chmod(0o700)
        env = os.environ.copy()
        env.update(SSH_ASKPASS=str(askpass), SSH_ASKPASS_REQUIRE='force',
                   DISPLAY='linfxp:0', LINFXP_SECRET=source.get('secret', ''))
        cmd = ['ssh', '-F', '/dev/null', '-T', '-p', str(source['port']),
               '-l', source['user'], '-o', 'StrictHostKeyChecking=yes',
               '-o', 'UserKnownHostsFile=' + str(known),
               '-o', 'GlobalKnownHostsFile=/dev/null',
               '-o', 'ConnectTimeout=20', '-o', 'ServerAliveInterval=15',
               '-o', 'ServerAliveCountMax=3', '-o', 'NumberOfPasswordPrompts=1']
        if source.get('key_data'):
            key = self.directory / 'identity'
            key.write_bytes(base64.b64decode(source['key_data'], validate=True))
            key.chmod(0o600)
            cmd += ['-i', str(key), '-o', 'IdentitiesOnly=yes',
                    '-o', 'PreferredAuthentications=publickey']
        else:
            cmd += ['-o', 'PreferredAuthentications=password,keyboard-interactive']
        if not source['host'] or source['host'].startswith('-'):
            raise ValueError('Invalid source host')
        cmd += ['-s', source['host'], 'sftp']
        self.error_log = open(self.directory / 'ssh.log', 'ab', buffering=0)
        self.process = subprocess.Popen(cmd, stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                                        stderr=self.error_log, env=env, start_new_session=True)
        try:
            self.sequence = 0
            self.send_packet(b'\x01' + u32(3))
            packet = self.packet()
            if len(packet) < 5 or packet[0] != 2 or struct.unpack_from('>I', packet, 1)[0] != 3:
                raise RuntimeError('The source did not negotiate SFTP version 3')
        except BaseException:
            self.close()
            raise

    def read_exact(self, size):
        result = bytearray()
        while len(result) < size:
            part = self.process.stdout.read(size - len(result))
            if not part:
                detail = (self.directory / 'ssh.log').read_text(errors='replace')[-2000:]
                raise RuntimeError('SFTP connection closed. ' + detail.strip())
            result.extend(part)
        return bytes(result)

    def packet(self):
        length = struct.unpack('>I', self.read_exact(4))[0]
        if length < 1 or length > 16 * 1024 * 1024:
            raise RuntimeError('Invalid SFTP packet length')
        return self.read_exact(length)

    def send_packet(self, packet):
        self.process.stdin.write(u32(len(packet)) + packet)
        self.process.stdin.flush()

    def request(self, kind, body):
        self.sequence += 1
        self.send_packet(bytes([kind]) + u32(self.sequence) + body)
        return self.sequence

    def response(self):
        packet = self.packet()
        if len(packet) < 5:
            raise RuntimeError('Invalid SFTP response')
        return packet[0], struct.unpack_from('>I', packet, 1)[0], packet[5:]

    def call(self, kind, body, expected):
        seq = self.request(kind, body)
        typ, received, data = self.response()
        if received != seq:
            raise RuntimeError('Unexpected SFTP request ID')
        if typ == 101:
            code = struct.unpack_from('>I', data)[0]
            if code:
                message, _ = take_string(data, 4)
                raise RuntimeError(message.decode(errors='replace'))
        if typ != expected:
            raise RuntimeError('Unexpected SFTP response type')
        return data

    def download(self, entry, temporary, offset, checkpoint, cancelled):
        handle, _ = take_string(self.call(3, string(entry['source']) + u32(1) + u32(0), 102))
        try:
            attrs = self.call(8, string(handle), 105)
            flags = struct.unpack_from('>I', attrs)[0]
            cursor = 4
            size = struct.unpack_from('>Q', attrs, cursor)[0] if flags & 1 else None
            cursor += 8 if flags & 1 else 0
            cursor += 8 if flags & 2 else 0
            cursor += 4 if flags & 4 else 0
            mtime = struct.unpack_from('>II', attrs, cursor)[1] if flags & 8 else None
            if size != entry['size'] or (mtime is not None and mtime != entry['mtime']):
                raise RuntimeError('Source changed since queueing: ' + entry['source'])
            with open(temporary, 'r+b' if temporary.exists() else 'w+b') as output:
                # Only a fully acknowledged window is a valid resume point.
                output.truncate(offset)
                while offset < size:
                    if cancelled():
                        raise InterruptedError('Cancelled')
                    pending = {}
                    end = min(size, offset + 32 * 65536)
                    position = offset
                    while position < end:
                        length = min(65536, end - position)
                        seq = self.request(5, string(handle) + struct.pack('>Q', position) + u32(length))
                        pending[seq] = (position, length)
                        position += length
                    while pending:
                        if cancelled():
                            raise InterruptedError('Cancelled')
                        kind, seq, data = self.response()
                        if seq not in pending:
                            raise RuntimeError('Unexpected SFTP read response')
                        position, length = pending.pop(seq)
                        if kind != 103:
                            raise RuntimeError('Source read failed before expected end of file')
                        chunk, _ = take_string(data)
                        if not chunk or len(chunk) > length:
                            raise RuntimeError('Invalid SFTP data length')
                        output.seek(position)
                        output.write(chunk)
                        if len(chunk) < length:
                            seq = self.request(5, string(handle) + struct.pack('>Q', position + len(chunk))
                                               + u32(length - len(chunk)))
                            pending[seq] = (position + len(chunk), length - len(chunk))
                    output.flush()
                    os.fsync(output.fileno())
                    offset = end
                    checkpoint(offset)
        finally:
            # On cancellation/error there may be outstanding replies; killing the
            # transport is safer than trying to reuse a desynchronised stream.
            if not cancelled() and sys.exc_info()[0] is None:
                self.call(4, string(handle), 101)

    def close(self):
        if self.process.poll() is None:
            self.process.terminate()
            try:
                self.process.wait(timeout=3)
            except subprocess.TimeoutExpired:
                self.process.kill()
                self.process.wait()
        self.process.stdin.close()
        self.process.stdout.close()
        self.error_log.close()


def atomic_json(path, data):
    temporary = path.with_suffix('.new')
    with open(temporary, 'w') as stream:
        json.dump(data, stream)
        stream.flush()
        os.fsync(stream.fileno())
    os.replace(temporary, path)


def target_path(root, relative):
    part = Path(relative)
    if part.is_absolute() or '..' in part.parts or not part.parts:
        raise ValueError('Unsafe destination path')
    target = root / part
    # Refuse existing symlinks at every level, including the filename.
    current = root
    if root.is_symlink():
        raise ValueError('Destination root is a symlink')
    for component in part.parts:
        current = current / component
        if current.is_symlink():
            raise ValueError('Destination contains a symlink: ' + str(current))
    if not target.resolve().is_relative_to(root.resolve()):
        raise ValueError('Destination escapes selected folder')
    return target


def transfer_directory(path, transfer_umask):
    """Create output directories with the destination's normal creation policy."""
    previous = os.umask(transfer_umask)
    try:
        path.mkdir(parents=True, exist_ok=True)
    finally:
        os.umask(previous)


def run_job(job, directory, source, transfer_umask=None):
    original_umask = os.umask(0o077)
    if transfer_umask is None:
        transfer_umask = original_umask
    directory = Path(directory)
    status_path = directory / 'status.json'
    old = json.loads(status_path.read_text()) if status_path.exists() else {}
    offsets = old.get('offsets', {})
    completed = old.get('completed', [])
    skipped = old.get('skipped', [])
    status = dict(state='Running', bytes=0, total=sum(e['size'] for e in job['files']),
                  current='', current_index=None, error='', offsets=offsets, completed=completed, skipped=skipped,
                  pid=os.getpid(), updated=time.time())
    root = Path(job['destination'])
    client = None
    queue_lock = None
    skip_cache = set()
    skip_mtime = None
    def skip_requested(index):
        nonlocal skip_cache, skip_mtime
        try:
            timestamp = (directory / 'skip.json').stat().st_mtime_ns
        except FileNotFoundError:
            return False
        if timestamp != skip_mtime:
            skip_cache = set(json.loads((directory / 'skip.json').read_text()))
            skip_mtime = timestamp
        return str(index) in skip_cache
    def save():
        status['updated'] = time.time()
        status['bytes'] = sum(e['size'] if str(i) in completed else offsets.get(str(i), 0)
                              for i, e in enumerate(job['files']))
        atomic_json(status_path, status)
    def cancelled():
        return (directory / 'cancel').exists() or (status['current_index'] is not None and skip_requested(status['current_index']))
    try:
        # Every dispatched job waits on the destination, so closing the GUI
        # does not strand queued transfers. One job per destination at a time.
        import fcntl
        queue_lock = open(directory.parent / 'queue.lock', 'w')
        status['state'] = 'Remote queue'
        save()
        while True:
            if cancelled():
                raise InterruptedError('Cancelled')
            # Preserve dispatch order, even when the scheduler starts a later
            # worker first. Earlier jobs release their turn on any final state.
            earlier = False
            for manifest in directory.parent.glob('*/manifest.json'):
                if manifest.parent == directory:
                    continue
                try:
                    other = json.loads(manifest.read_text())
                    state = json.loads((manifest.parent / 'status.json').read_text()).get('state')
                    if (other.get('created', 0), other['id']) < (job.get('created', 0), job['id']) and state in ('Starting', 'Running', 'Remote queue'):
                        # A dead worker must not permanently block the queue.
                        with open(manifest.parent / 'lock', 'a') as prior_lock:
                            try:
                                fcntl.flock(prior_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                            except BlockingIOError:
                                earlier = True
                                break
                except (OSError, ValueError, KeyError):
                    continue
            if not earlier:
                try:
                    fcntl.flock(queue_lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                    break
                except BlockingIOError:
                    pass
            time.sleep(0.5)
        status['state'] = 'Running'
        transfer_directory(root, transfer_umask)
        for relative in job.get('directories', []):
            transfer_directory(target_path(root, relative), transfer_umask)
        save()
        for i, entry in enumerate(job['files']):
            status['current_index'] = None
            if (directory / 'cancel').exists():
                raise InterruptedError('Cancelled')
            if str(i) in skipped or skip_requested(i):
                if str(i) not in skipped:
                    skipped.append(str(i))
                save()
                continue
            target = target_path(root, entry['relative'])
            if str(i) in completed:
                if not target.is_file() or target.stat().st_size != entry['size']:
                    raise RuntimeError('Previously completed file changed: ' + str(target))
                continue
            if target.exists():
                raise FileExistsError('Destination already exists: ' + str(target)
                                      + '. Choose another folder or remove the existing file.')
            transfer_directory(target.parent, transfer_umask)
            temporary = target.with_name('.' + target.name + '.linfxp-' + job['id'] + '.part')
            if temporary.is_symlink():
                raise ValueError('Partial file is a symlink')
            offset = min(offsets.get(str(i), 0), temporary.stat().st_size if temporary.exists() else 0)
            status['current'] = entry['relative']
            status['current_index'] = i
            save()
            def checkpoint(value):
                offsets[str(i)] = value
                save()
            try:
                if client is None:
                    client = SFTP(source, directory)
                client.download(entry, temporary, offset, checkpoint, cancelled)
                if cancelled():
                    raise InterruptedError('Cancelled')
            except InterruptedError:
                if (directory / 'cancel').exists():
                    raise
                client.close()
                client = None
                skipped.append(str(i))
                save()
                continue
            # Hard-link publication refuses to overwrite a file created meanwhile.
            # Partial files remain private until ready; resumed partials from
            # older workers also receive the corrected final permissions.
            temporary.chmod(0o666 & ~transfer_umask)
            os.link(temporary, target)
            temporary.unlink()
            os.utime(target, (entry['mtime'], entry['mtime']))
            completed.append(str(i))
            offsets.pop(str(i), None)
            save()
        status['state'] = 'Completed'
    except InterruptedError as error:
        status.update(state='Cancelled', error=str(error))
    except Exception as error:
        status.update(state='Failed', error=str(error))
    finally:
        if client:
            client.close()
        if queue_lock:
            queue_lock.close()
        for name in ('identity', 'askpass', 'known_hosts'):
            (directory / name).unlink(missing_ok=True)
        save()
        os.umask(original_umask)


def main():
    transfer_umask = os.umask(0o077)
    payload = json.load(sys.stdin)
    if sys.argv[1:] == ['--status']:
        job_id = payload['id']
        if len(job_id) != 32 or any(c not in '0123456789abcdef' for c in job_id):
            raise ValueError('Invalid job ID')
        directory = Path.home() / '.local/share/linfxp/jobs' / job_id
        if not (directory / 'status.json').exists():
            print(json.dumps({'state': 'Unknown'}))
            return
        status = json.loads((directory / 'status.json').read_text())
        import fcntl
        if status['state'] in ('Running', 'Starting', 'Remote queue'):
            with open(directory / 'lock', 'a') as lock:
                try:
                    fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
                except BlockingIOError:
                    pass
                else:
                    status.update(state='Failed', error='Worker stopped unexpectedly. Retry / resume this transfer.')
        print(json.dumps(status))
        return
    job = payload['job']
    if len(job['id']) != 32 or any(c not in '0123456789abcdef' for c in job['id']):
        raise ValueError('Invalid job ID')
    directory = Path.home() / '.local/share/linfxp/jobs' / job['id']
    directory.mkdir(parents=True, exist_ok=True)
    import fcntl
    lock = open(directory / 'lock', 'w')
    try:
        fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)
    except BlockingIOError:
        print(json.dumps({'started': True, 'id': job['id'], 'existing': True}), flush=True)
        return
    if (directory / 'status.json').exists():
        prior = json.loads((directory / 'status.json').read_text())
        if prior.get('state') == 'Completed':
            print(json.dumps({'started': True, 'id': job['id'], 'existing': True}), flush=True)
            return
    atomic_json(directory / 'manifest.json', job)
    removed = {str(i) for i in job.get('removed_files', []) if int(i) >= 0}
    if removed:
        skip_path = directory / 'skip.json'
        existing = set(json.loads(skip_path.read_text())) if skip_path.exists() else set()
        atomic_json(skip_path, sorted(existing | removed))
    (directory / 'cancel').unlink(missing_ok=True)
    # The parent acknowledges only after the detached process has initialised.
    ready_read, ready_write = os.pipe()
    pid = os.fork()
    if pid:
        os.close(ready_write)
        ready = os.read(ready_read, 1)
        os.close(ready_read)
        if ready != b'1':
            raise RuntimeError('Could not initialise remote worker')
        print(json.dumps({'started': True, 'id': job['id']}), flush=True)
        return
    os.close(ready_read)
    os.setsid()
    null = os.open(os.devnull, os.O_RDWR)
    for fd in (0, 1, 2):
        os.dup2(null, fd)
    if null > 2:
        os.close(null)
    prior = json.loads((directory / 'status.json').read_text()) if (directory / 'status.json').exists() else {}
    prior.update(state='Starting', pid=os.getpid())
    atomic_json(directory / 'status.json', prior)
    os.write(ready_write, b'1')
    os.close(ready_write)
    try:
        run_job(job, directory, payload['source'], transfer_umask)
    finally:
        lock.close()
    os._exit(0)


if __name__ == '__main__':
    main()
