import hashlib
import importlib.util
import json
import os
from pathlib import Path
import socket
import shlex
import stat
import subprocess
import tempfile
import threading
import time
import unittest
import uuid

import paramiko

ROOT = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('LINFXP_TEST_BUILD_DIR', ROOT / 'build'))
spec = importlib.util.spec_from_file_location('worker', ROOT / 'worker/worker.py')
worker = importlib.util.module_from_spec(spec)
spec.loader.exec_module(worker)


class Authentication(paramiko.ServerInterface):
    deny_exec = False
    def __init__(self, public_key, root):
        self.public_key = public_key
        self.root = root

    def check_auth_password(self, username, password):
        return paramiko.AUTH_SUCCESSFUL if (username, password) == ('test', 'test-secret') else paramiko.AUTH_FAILED

    def check_auth_publickey(self, username, key):
        return paramiko.AUTH_SUCCESSFUL if username == 'test' and key == self.public_key else paramiko.AUTH_FAILED

    def get_allowed_auths(self, username):
        return 'publickey,password'

    def check_channel_request(self, kind, chanid):
        return paramiko.OPEN_SUCCEEDED if kind == 'session' else paramiko.OPEN_FAILED_ADMINISTRATIVELY_PROHIBITED

    def check_channel_exec_request(self, channel, command):
        if self.deny_exec:
            return False
        parts = shlex.split(command.decode())
        if len(parts) not in (2, 3) or parts[0] != 'python3':
            return False
        if parts[1] == '-c' and len(parts) == 3 and "d=Path.home()/'.local/share/linfxp/jobs'/i" in parts[2]:
            arguments = ['-c', parts[2]]
        elif parts[1].startswith('/.local/share/linfxp/worker-'):
            arguments = [str(self.root / parts[1].lstrip('/'))] + parts[2:]
        else:
            return False
        def execute():
            data = bytearray()
            while True:
                chunk = channel.recv(32768)
                if not chunk:
                    break
                data.extend(chunk)
            result = subprocess.run(['python3'] + arguments, input=bytes(data),
                                    capture_output=True, env=dict(os.environ, HOME=str(self.root)), timeout=15)
            channel.sendall(result.stdout)
            channel.sendall_stderr(result.stderr)
            channel.send_exit_status(result.returncode)
            channel.close()
        threading.Thread(target=execute, daemon=True).start()
        return True


class Handle(paramiko.SFTPHandle):
    delay = 0
    def read(self, offset, length):
        time.sleep(self.delay)
        # Exercise legal short DATA packets and the worker's retry ranges.
        return super().read(offset, min(length, 32768))

    def write(self, offset, data):
        time.sleep(self.delay)
        return super().write(offset, data)


class Filesystem(paramiko.SFTPServerInterface):
    def __init__(self, server, *args, root, **kwargs):
        super().__init__(server, *args, **kwargs)
        self.root = root

    def local(self, path):
        result = (self.root / path.lstrip('/')).resolve()
        if not result.is_relative_to(self.root):
            raise PermissionError('outside test root')
        return result

    def canonicalize(self, path):
        try:
            return '/' + str(self.local(path).relative_to(self.root)) if path != '.' else '/'
        except OSError:
            return '/'

    def list_folder(self, path):
        try:
            result = []
            for p in self.local(path).iterdir():
                attrs = paramiko.SFTPAttributes.from_stat(p.lstat())
                attrs.filename = p.name
                result.append(attrs)
            return result
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def stat(self, path):
        try:
            return paramiko.SFTPAttributes.from_stat(self.local(path).stat())
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def lstat(self, path):
        try:
            parent = self.local(str(Path(path).parent))
            return paramiko.SFTPAttributes.from_stat((parent / Path(path).name).lstat())
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def chattr(self, path, attr):
        try:
            # SFTP truncation must preserve the acknowledged prefix. Paramiko's
            # convenience helper opens with w+, which incorrectly erases it.
            if attr._flags & attr.FLAG_SIZE:
                with open(self.local(path), 'r+b') as file:
                    file.truncate(attr.st_size)
                attr._flags &= ~attr.FLAG_SIZE
            paramiko.SFTPServer.set_file_attr(str(self.local(path)), attr)
            return paramiko.SFTP_OK
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def open(self, path, flags, attr):
        try:
            writable = bool(flags & (os.O_WRONLY | os.O_RDWR))
            fd = os.open(self.local(path), flags, attr.st_mode if attr.st_mode is not None else 0o666)
            stream = os.fdopen(fd, 'r+b' if flags & os.O_RDWR else 'wb' if writable else 'rb')
            handle = Handle(flags)
            if writable:
                handle.writefile = stream
                if flags & os.O_RDWR:
                    handle.readfile = stream
            else:
                handle.readfile = stream
            handle.stat = lambda: paramiko.SFTPAttributes.from_stat(os.fstat(stream.fileno()))
            return handle
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def mkdir(self, path, attr):
        try:
            self.local(path).mkdir(mode=attr.st_mode or 0o700)
            return paramiko.SFTP_OK
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)

    def rename(self, old, new):
        try:
            os.link(self.local(old), self.local(new))
            self.local(old).unlink()
            return paramiko.SFTP_OK
        except OSError as e:
            return paramiko.SFTPServer.convert_errno(e.errno)


class Transfers(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.temporary = tempfile.TemporaryDirectory(prefix='linfxp-tests-')
        cls.base = Path(cls.temporary.name)
        cls.source_root = cls.base / 'source'
        cls.source_root.mkdir()
        cls.identity = cls.base / 'source-key'
        subprocess.run(['ssh-keygen', '-q', '-t', 'ed25519', '-N', 'test-passphrase', '-f', str(cls.identity)], check=True)
        cls.public_key = paramiko.Ed25519Key.from_private_key_file(str(cls.identity), password='test-passphrase')
        cls.host_key = paramiko.RSAKey.generate(2048)
        cls.socket = socket.socket()
        cls.socket.bind(('127.0.0.1', 0))
        cls.socket.listen(10)
        cls.port = cls.socket.getsockname()[1]
        cls.connections = []
        cls.stopping = threading.Event()
        def server():
            while not cls.stopping.is_set():
                try:
                    client, _ = cls.socket.accept()
                except OSError:
                    return
                transport = paramiko.Transport(client)
                cls.connections.append(transport)
                transport.add_server_key(cls.host_key)
                transport.set_subsystem_handler('sftp', paramiko.SFTPServer, Filesystem, root=cls.source_root)
                def handshake(t=transport):
                    try:
                        t.start_server(server=Authentication(cls.public_key, cls.source_root))
                    except (EOFError, OSError, paramiko.SSHException):
                        t.close()
                threading.Thread(target=handshake, daemon=True).start()
        threading.Thread(target=server, daemon=True).start()

    @classmethod
    def tearDownClass(cls):
        cls.stopping.set()
        cls.socket.close()
        for connection in cls.connections:
            connection.close()
        cls.temporary.cleanup()

    def setUp(self):
        self.case = self.base / uuid.uuid4().hex
        self.case.mkdir()
        self.destination = self.case / 'destination'
        self.directory = self.case / 'job'
        self.directory.mkdir()
        self.source = dict(host='127.0.0.1', port=self.port, user='test', secret='test-secret',
                           known_host=f'[127.0.0.1]:{self.port} {self.host_key.get_name()} {self.host_key.get_base64()}\n')

    def file(self, name, contents):
        p = self.source_root / name
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(contents)
        return dict(source='/' + name, relative=name, size=len(contents), mtime=int(p.stat().st_mtime))

    def job(self, files, directories=None):
        return dict(id=uuid.uuid4().hex, created=time.time_ns() // 1000000,
                    destination=str(self.destination), files=files, directories=directories or [])

    def status(self, directory=None):
        return json.loads(((directory or self.directory) / 'status.json').read_text())

    def wait(self, directory, states, timeout=20):
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            if (directory / 'status.json').exists():
                status = self.status(directory)
                if status['state'] in states:
                    return status
            time.sleep(0.05)
        self.fail('Timed out waiting for ' + str(states))

    def launch(self, job, source=None, umask=None):
        home = self.case / 'home'
        home.mkdir(exist_ok=True)
        payload = json.dumps(dict(job=job, source=source or self.source))
        options = {} if umask is None else {'umask': umask}
        result = subprocess.run(['python3', str(ROOT / 'worker/worker.py')], input=payload, text=True,
                                capture_output=True, env=dict(os.environ, HOME=str(home)), timeout=10,
                                **options)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['started'])
        return home / '.local/share/linfxp/jobs' / job['id']

    def test_password_unicode_short_reads_and_empty_directory(self):
        content = os.urandom(4 * 1024 * 1024 + 17)
        entry = self.file('photos/space "quotes" [*] café 🏕.bin', content)
        worker.run_job(self.job([entry], ['empty']), self.directory, self.source)
        self.assertEqual(self.status()['state'], 'Completed', self.status())
        self.assertEqual((self.destination / entry['relative']).read_bytes(), content)
        self.assertTrue((self.destination / 'empty').is_dir())
        for secret in ('identity', 'known_hosts', 'askpass'):
            self.assertFalse((self.directory / secret).exists())
        self.assertNotIn('test-secret', (self.directory / 'status.json').read_text())

    def test_detached_output_permissions_and_private_job_metadata(self):
        for mask, file_mode, directory_mode in ((0o022, 0o644, 0o755), (0o002, 0o664, 0o775)):
            with self.subTest(umask=oct(mask)):
                entry = self.file(f'permissions-{mask}/nested/data.bin', b'usable output')
                job = self.job([entry], [f'permissions-{mask}/empty'])
                directory = self.launch(job, umask=mask)
                status = self.wait(directory, ['Completed', 'Failed'])
                self.assertEqual(status['state'], 'Completed', status)
                target = self.destination / entry['relative']
                self.assertEqual(stat.S_IMODE(target.stat().st_mode), file_mode)
                for path in (target.parent, target.parent.parent, self.destination / f'permissions-{mask}/empty'):
                    self.assertEqual(stat.S_IMODE(path.stat().st_mode), directory_mode)
                self.assertEqual(stat.S_IMODE(directory.stat().st_mode), 0o700)
                for name in ('manifest.json', 'status.json', 'lock'):
                    self.assertEqual(stat.S_IMODE((directory / name).stat().st_mode), 0o600)

    def test_existing_destination_directory_permissions_are_preserved(self):
        self.destination.mkdir(mode=0o700)
        folder = self.destination / 'existing'
        folder.mkdir(mode=0o750)
        entry = self.file('existing/data.bin', b'keep directory policy')
        job = self.job([entry], ['existing'])
        directory = self.launch(job, umask=0o022)
        status = self.wait(directory, ['Completed', 'Failed'])
        self.assertEqual(status['state'], 'Completed', status)
        self.assertEqual(stat.S_IMODE(self.destination.stat().st_mode), 0o700)
        self.assertEqual(stat.S_IMODE(folder.stat().st_mode), 0o750)
        self.assertEqual(stat.S_IMODE((folder / 'data.bin').stat().st_mode), 0o644)

    def test_resumed_private_partial_gets_normal_final_permissions(self):
        entry = self.file('resumed-permissions.bin', b'correct permissions after resume')
        job = self.job([entry])
        self.destination.mkdir()
        partial = self.destination / ('.' + entry['relative'] + '.linfxp-' + job['id'] + '.part')
        partial.write_bytes(b'correct')
        partial.chmod(0o600)
        worker.atomic_json(self.directory / 'status.json', {'offsets': {'0': 7}})
        worker.run_job(job, self.directory, self.source, 0o022)
        self.assertEqual(self.status()['state'], 'Completed', self.status())
        self.assertEqual(stat.S_IMODE((self.destination / entry['relative']).stat().st_mode), 0o644)

    def test_encrypted_private_key(self):
        source = dict(self.source, secret='test-passphrase', key_data=worker.base64.b64encode(self.identity.read_bytes()).decode())
        entry = self.file('encrypted-key.bin', os.urandom(1025))
        worker.run_job(self.job([entry]), self.directory, source)
        self.assertEqual(self.status()['state'], 'Completed', self.status())
        self.assertFalse((self.directory / 'identity').exists())

    def test_wrong_password(self):
        entry = self.file('wrong-password.bin', b'never transferred')
        worker.run_job(self.job([entry]), self.directory, dict(self.source, secret='wrong'))
        self.assertEqual(self.status()['state'], 'Failed')
        self.assertIn('Permission denied', self.status()['error'])

    def test_changed_host_key_rejected(self):
        other = paramiko.RSAKey.generate(2048)
        source = dict(self.source, known_host=f'[127.0.0.1]:{self.port} {other.get_name()} {other.get_base64()}\n')
        entry = self.file('wrong-host.bin', b'never transferred')
        worker.run_job(self.job([entry]), self.directory, source)
        self.assertEqual(self.status()['state'], 'Failed')
        self.assertIn('HOST IDENTIFICATION HAS CHANGED', self.status()['error'])

    def test_existing_file_never_overwritten(self):
        entry = self.file('exists.bin', b'new')
        self.destination.mkdir()
        (self.destination / 'exists.bin').write_bytes(b'original')
        worker.run_job(self.job([entry]), self.directory, self.source)
        self.assertEqual(self.status()['state'], 'Failed')
        self.assertEqual((self.destination / 'exists.bin').read_bytes(), b'original')

    def test_destination_symlink_and_traversal_rejected(self):
        self.destination.mkdir()
        outside = self.case / 'outside'
        outside.mkdir()
        (self.destination / 'link').symlink_to(outside, target_is_directory=True)
        for name in ('../outside/x', '/absolute', 'link/x'):
            with self.subTest(name=name), self.assertRaises(ValueError):
                worker.target_path(self.destination, name)

    def test_source_change_rejected(self):
        entry = self.file('changed.bin', b'old')
        (self.source_root / 'changed.bin').write_bytes(b'new version')
        worker.run_job(self.job([entry]), self.directory, self.source)
        self.assertEqual(self.status()['state'], 'Failed')
        self.assertIn('Source changed', self.status()['error'])

    def test_detach_cancel_and_resume_preserves_checkpoint(self):
        content = os.urandom(12 * 1024 * 1024)
        entry = self.file('resume.bin', content)
        job = self.job([entry])
        Handle.delay = 0.003
        try:
            directory = self.launch(job)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                s = self.status(directory)
                if s.get('bytes', 0) >= 2 * 1024 * 1024:
                    break
                time.sleep(0.02)
            (directory / 'cancel').touch()
            status = self.wait(directory, ['Cancelled'])
            self.assertGreater(status['offsets']['0'], 0)
            self.assertLess(status['offsets']['0'], len(content))
            checkpoint = status['offsets']['0']
            partial = self.destination / ('.resume.bin.linfxp-' + job['id'] + '.part')
            # Assert resumed operation keeps the acknowledged prefix; replacing
            # this file's beginning would change this inode's modification bytes.
            self.assertEqual(partial.read_bytes()[:checkpoint], content[:checkpoint])
            self.launch(job)
            result = self.wait(directory, ['Completed', 'Failed'], timeout=30)
            self.assertEqual(result['state'], 'Completed', result)
            self.assertEqual(hashlib.sha256((self.destination / 'resume.bin').read_bytes()).digest(), hashlib.sha256(content).digest())
            self.assertFalse(partial.exists())
        finally:
            Handle.delay = 0

    def test_remote_queue_continues_without_gui(self):
        Handle.delay = 0.002
        try:
            a = self.job([self.file('queue-a.bin', os.urandom(4 * 1024 * 1024))])
            b = self.job([self.file('queue-b.bin', os.urandom(1024))])
            b['created'] = a['created'] + 1
            first = self.launch(a)
            second = self.launch(b)
            self.assertEqual(self.wait(first, ['Completed', 'Failed'])['state'], 'Completed')
            self.assertEqual(self.wait(second, ['Completed', 'Failed'])['state'], 'Completed')
        finally:
            Handle.delay = 0

    def test_removed_waiting_file_is_skipped_and_other_files_download(self):
        first = self.file('removed-waiting.bin', b'removed')
        second = self.file('kept-waiting.bin', b'kept')
        job = self.job([first, second])
        worker.atomic_json(self.directory / 'skip.json', ['0'])
        # Removed entries must not even be opened at the source.
        (self.source_root / first['relative']).unlink()
        worker.run_job(job, self.directory, self.source)
        status = self.status()
        self.assertEqual(status['state'], 'Completed', status)
        self.assertEqual(status['skipped'], ['0'])
        self.assertEqual(status['completed'], ['1'])
        self.assertFalse((self.destination / first['relative']).exists())
        self.assertEqual((self.destination / second['relative']).read_bytes(), b'kept')

    def test_remove_active_file_continues_batch_and_retry_does_not_restore_it(self):
        first = self.file('removed-active.bin', os.urandom(12 * 1024 * 1024))
        second = self.file('kept-after-remove.bin', os.urandom(1025))
        third = self.file('retry-after-remove.bin', os.urandom(1025))
        job = self.job([first, second, third])
        # Force a failure after removal and one successful file, then retry.
        self.destination.mkdir()
        conflict = self.destination / third['relative']
        conflict.write_bytes(b'conflict')
        Handle.delay = 0.003
        try:
            directory = self.launch(job)
            deadline = time.monotonic() + 15
            while time.monotonic() < deadline:
                status = self.status(directory)
                if status.get('bytes', 0) >= 2 * 1024 * 1024:
                    break
                time.sleep(0.02)
            self.assertEqual(status['current_index'], 0)
            worker.atomic_json(directory / 'skip.json', ['0'])
            status = self.wait(directory, ['Failed'])
            self.assertEqual(status['skipped'], ['0'])
            self.assertIn('1', status['completed'])
            self.assertFalse((self.destination / first['relative']).exists())
            conflict.unlink()
            self.launch(job)
            status = self.wait(directory, ['Completed', 'Failed'])
            self.assertEqual(status['state'], 'Completed', status)
            self.assertEqual(status['skipped'], ['0'])
            self.assertFalse((self.destination / first['relative']).exists())
            self.assertEqual((self.destination / third['relative']).read_bytes(), (self.source_root / third['relative']).read_bytes())
        finally:
            Handle.delay = 0

    def test_native_libssh_browsing(self):
        binary = BUILD / 'linfxp'
        if not binary.exists():
            self.skipTest('Build native app to enable libssh integration')
        self.file('gui-browse.bin', b'listing')
        home = self.case / 'gui-home'
        (home / '.ssh').mkdir(parents=True)
        (home / '.ssh/known_hosts').write_text(self.source['known_host'])
        profile = dict(self.source, id='test', name='Test', password=True, path='/')
        result = subprocess.run([str(binary), '--probe'], input=json.dumps(profile), text=True,
                                capture_output=True, env=dict(os.environ, HOME=str(home), QT_QPA_PLATFORM='offscreen',
                                                             QT_QPA_PLATFORMTHEME='none', QT_STYLE_OVERRIDE='Fusion'), timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn('gui-browse.bin', [e['name'] for e in json.loads(result.stdout)])

    def desktop(self, job, source, target, **options):
        binary = BUILD / 'linfxp'
        home = self.case / 'desktop-home'
        (home / '.ssh').mkdir(parents=True, exist_ok=True)
        (home / '.ssh/known_hosts').write_text(self.source['known_host'])
        result = subprocess.run([str(binary), '--probe'],
            input=json.dumps(dict(action='desktop', job=job, source=source, target=target, **options)),
            text=True,capture_output=True,timeout=30,
            env=dict(os.environ,HOME=str(home),XDG_DATA_HOME=str(home / 'data'),
                     QT_QPA_PLATFORM='offscreen',QT_QPA_PLATFORMTHEME='none',QT_STYLE_OVERRIDE='Fusion'))
        self.assertEqual(result.returncode,0,result.stdout+result.stderr)
        return json.loads(result.stdout)

    def test_desktop_download_with_short_reads_and_permissions(self):
        data=os.urandom(4*1024*1024+37)
        entry=self.file('desktop-download/folder/quotes "[*] 🏕.bin',data)
        self.destination.mkdir()
        status=self.desktop(self.job([entry],['desktop-download/empty']),self.source,{'local':True})
        self.assertEqual(status['state'],'Completed',status)
        output=self.destination / entry['relative']
        self.assertEqual(output.read_bytes(),data)
        self.assertEqual(stat.S_IMODE(output.stat().st_mode),0o644)
        self.assertTrue((self.destination / 'desktop-download/empty').is_dir())
        self.assertEqual(int(output.stat().st_mtime),entry['mtime'])

    def test_desktop_upload_preserves_data_paths_and_remote_permissions(self):
        source=self.case / 'upload-source.bin';data=os.urandom(4*1024*1024+91);source.write_bytes(data)
        folder='upload-'+uuid.uuid4().hex
        (self.source_root / folder).mkdir()
        job=self.job([dict(source=str(source),relative='nested/spaces "[x] café.bin',size=len(data),mtime=int(source.stat().st_mtime))],['empty'])
        job['destination']='/'+folder
        status=self.desktop(job,{'local':True},self.source)
        self.assertEqual(status['state'],'Completed',status)
        output=self.source_root / folder / job['files'][0]['relative']
        self.assertEqual(output.read_bytes(),data)
        self.assertEqual(stat.S_IMODE(output.stat().st_mode),0o644)
        self.assertEqual(stat.S_IMODE(output.parent.stat().st_mode),0o755)
        self.assertTrue((self.source_root / folder / 'empty').is_dir())

    def test_desktop_cancel_resume_and_skip(self):
        data=os.urandom(6*1024*1024+100)
        first=self.file('desktop-resume.bin',data);second=self.file('desktop-removed.bin',b'excluded')
        self.destination.mkdir();job=self.job([first,second]);job['removed_files']=[1]
        status=self.desktop(job,self.source,{'local':True},cancel_after=1)
        self.assertEqual(status['state'],'Cancelled',status)
        self.assertGreater(status['offsets']['0'],0)
        partial=self.destination / ('.desktop-resume.bin.linfxp-'+job['id']+'.part')
        self.assertEqual(stat.S_IMODE(partial.stat().st_mode),0o600)
        status=self.desktop(job,self.source,{'local':True})
        self.assertEqual(status['state'],'Completed',status)
        self.assertEqual((self.destination / first['relative']).read_bytes(),data)
        self.assertFalse((self.destination / second['relative']).exists())
        self.assertEqual(status['skipped'],['1'])

    def test_desktop_upload_resume(self):
        data=os.urandom(6*1024*1024+99);source=self.case / 'resume-upload';source.write_bytes(data)
        folder='upload-resume-'+uuid.uuid4().hex;(self.source_root / folder).mkdir()
        entry=dict(source=str(source),relative='resume.bin',size=len(data),mtime=int(source.stat().st_mtime))
        job=self.job([entry]);job['destination']='/'+folder
        Handle.delay=0.003
        try:
            status=self.desktop(job,{'local':True},self.source,cancel_after=1)
        finally:
            Handle.delay=0
        self.assertEqual(status['state'],'Cancelled',status)
        self.assertGreater(status['offsets']['0'],0)
        status=self.desktop(job,{'local':True},self.source)
        self.assertEqual(status['state'],'Completed',status)
        self.assertEqual((self.source_root / folder / 'resume.bin').read_bytes(),data)

    def test_desktop_upload_existing_file_is_not_overwritten(self):
        source=self.case / 'conflict';source.write_bytes(b'new')
        folder='conflict-'+uuid.uuid4().hex;(self.source_root / folder).mkdir()
        existing=self.source_root / folder / 'same.bin';existing.write_bytes(b'original')
        job=self.job([dict(source=str(source),relative='same.bin',size=3,mtime=int(source.stat().st_mtime))]);job['destination']='/'+folder
        status=self.desktop(job,{'local':True},self.source)
        self.assertEqual(status['state'],'Failed',status)
        self.assertEqual(existing.read_bytes(),b'original')

    def test_desktop_encrypted_key_transfers_work_on_sftp_only_host(self):
        auth=dict(self.source,password=False,key=str(self.identity),secret='test-passphrase')
        entry=self.file('sftp-only-download.bin',os.urandom(131073));self.destination.mkdir()
        folder='sftp-only-'+uuid.uuid4().hex;(self.source_root / folder).mkdir()
        Authentication.deny_exec=True
        try:
            status=self.desktop(self.job([entry]),auth,{'local':True})
            self.assertEqual(status['state'],'Completed',status)
            local=self.destination / entry['relative']
            upload=self.job([dict(source=str(local),relative='upload.bin',size=local.stat().st_size,mtime=int(local.stat().st_mtime))])
            upload['destination']='/'+folder
            status=self.desktop(upload,{'local':True},auth)
            self.assertEqual(status['state'],'Completed',status)
            self.assertEqual((self.source_root / folder / 'upload.bin').read_bytes(),local.read_bytes())
        finally:
            Authentication.deny_exec=False
        for journal in (self.case / 'desktop-home/data').rglob('status.json'):
            self.assertNotIn('test-passphrase',journal.read_text())

    def test_desktop_remove_waiting_file_continues_batch(self):
        a=self.file('desktop-kept.bin',b'keep');b=self.file('desktop-skip.bin',b'skip')
        self.destination.mkdir();job=self.job([a,b])
        status=self.desktop(job,self.source,{'local':True},remove_index=1)
        self.assertEqual(status['state'],'Completed',status)
        self.assertEqual((self.destination / a['relative']).read_bytes(),b'keep')
        self.assertFalse((self.destination / b['relative']).exists())

    def test_desktop_download_rejects_destination_symlink(self):
        entry=self.file('unsafe/link/data.bin',b'refuse link');self.destination.mkdir()
        outside=self.case / 'outside';outside.mkdir();(self.destination / 'unsafe').symlink_to(outside,target_is_directory=True)
        status=self.desktop(self.job([entry]),self.source,{'local':True})
        self.assertEqual(status['state'],'Failed',status)
        self.assertEqual(list(outside.iterdir()),[])

    def test_native_dispatch_upload_and_remote_execution(self):
        binary = BUILD / 'linfxp'
        if not binary.exists():
            self.skipTest('Build native app to enable dispatch integration')
        entry = self.file('native-transfer.bin', os.urandom(262145))
        job = self.job([entry])
        home = self.case / 'native-home'
        (home / '.ssh').mkdir(parents=True)
        (home / '.ssh/known_hosts').write_text(self.source['known_host'])
        profile = dict(self.source, password=True, action='launch', payload=dict(job=job, source=self.source))
        result = subprocess.run([str(binary), '--probe'], input=json.dumps(profile), text=True, capture_output=True,
                                env=dict(os.environ, HOME=str(home), QT_QPA_PLATFORM='offscreen',
                                         QT_QPA_PLATFORMTHEME='none', QT_STYLE_OVERRIDE='Fusion'), timeout=20)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(json.loads(result.stdout)['started'])
        directory = self.source_root / '.local/share/linfxp/jobs' / job['id']
        status = self.wait(directory, ['Completed', 'Failed'])
        self.assertEqual(status['state'], 'Completed', status)
        self.assertEqual((self.destination / entry['relative']).read_bytes(), (self.source_root / entry['relative']).read_bytes())

    def test_native_recursive_manifest_excludes_symlinks(self):
        binary = BUILD / 'linfxp'
        if not binary.exists():
            self.skipTest('Build native app to enable manifest integration')
        self.file('manifest/folder/data.bin', b'folder contents')
        (self.source_root / 'manifest/empty').mkdir()
        (self.source_root / 'manifest/link').symlink_to('folder', target_is_directory=True)
        home = self.case / 'manifest-home'
        (home / '.ssh').mkdir(parents=True)
        (home / '.ssh/known_hosts').write_text(self.source['known_host'])
        profile = dict(self.source, password=True, action='manifest', path='/manifest')
        result = subprocess.run([str(binary), '--probe'], input=json.dumps(profile), text=True, capture_output=True,
                                env=dict(os.environ, HOME=str(home), QT_QPA_PLATFORM='offscreen',
                                         QT_QPA_PLATFORMTHEME='none', QT_STYLE_OVERRIDE='Fusion'), timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        manifest = json.loads(result.stdout)
        self.assertEqual([e['relative'] for e in manifest['files']], ['folder/data.bin'])
        self.assertEqual(set(manifest['directories']), {'folder', 'empty'})
        self.assertEqual(manifest['skipped'], 1)

    def test_native_remove_merges_remote_skip_requests(self):
        binary = BUILD / 'linfxp'
        if not binary.exists():
            self.skipTest('Build native app to enable removal integration')
        job = self.job([])
        directory = self.source_root / '.local/share/linfxp/jobs' / job['id']
        directory.mkdir(parents=True)
        worker.atomic_json(directory / 'skip.json', ['0'])
        home = self.case / 'skip-home'
        (home / '.ssh').mkdir(parents=True)
        (home / '.ssh/known_hosts').write_text(self.source['known_host'])
        profile = dict(self.source, password=True, action='skip', job_id=job['id'], indices=['1', '2'])
        result = subprocess.run([str(binary), '--probe'], input=json.dumps(profile), text=True, capture_output=True,
                                env=dict(os.environ, HOME=str(home), QT_QPA_PLATFORM='offscreen',
                                         QT_QPA_PLATFORMTHEME='none', QT_STYLE_OVERRIDE='Fusion'), timeout=10)
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertEqual(json.loads((directory / 'skip.json').read_text()), ['0', '1', '2'])
        self.assertEqual(stat.S_IMODE((directory / 'skip.json').stat().st_mode), 0o600)

    def test_destination_gui_autorefresh_over_ssh(self):
        binary = BUILD / 'test_profiles'
        if not binary.exists():
            self.skipTest('Build Qt test executable to enable refresh integration')
        self.file('autorefresh-proof.bin', b'refresh listing')
        home = self.case / 'refresh-home'
        (home / '.ssh').mkdir(parents=True)
        (home / '.ssh/known_hosts').write_text(self.source['known_host'])
        result = subprocess.run([str(binary), 'destinationAutoRefreshLoadsLocalSftp'],
                                text=True, capture_output=True,
                                env=dict(os.environ, HOME=str(home), LINFXP_TEST_PORT=str(self.port),
                                         QT_QPA_PLATFORM='offscreen', QT_QPA_PLATFORMTHEME='none', QT_STYLE_OVERRIDE='Fusion'), timeout=15)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertNotIn('SKIP', result.stdout)

    def test_dead_worker_detected_after_restart(self):
        job = self.job([])
        home = self.case / 'status-home'
        directory = home / '.local/share/linfxp/jobs' / job['id']
        directory.mkdir(parents=True)
        worker.atomic_json(directory / 'status.json', dict(state='Running', pid=99999999, bytes=2097152, offsets={'0': 2097152}))
        result = subprocess.run(['python3', str(ROOT / 'worker/worker.py'), '--status'],
                                input=json.dumps({'id': job['id']}), text=True, capture_output=True,
                                env=dict(os.environ, HOME=str(home)), timeout=5)
        self.assertEqual(result.returncode, 0, result.stderr)
        status = json.loads(result.stdout)
        self.assertEqual(status['state'], 'Failed')
        self.assertEqual(status['offsets']['0'], 2097152)


if __name__ == '__main__':
    unittest.main()
