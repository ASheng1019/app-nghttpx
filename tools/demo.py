#!/usr/bin/env python3
# SPDX-License-Identifier: BSD-3-Clause
"""Run a local ARM64/KVM HTTP/3-to-HTTP/1.1 demonstration, without QA fixtures."""
import argparse
import json
import os
from pathlib import Path
import platform
import secrets
import shutil
import socket
import subprocess
import threading
import time
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer

BODY = b"HTTP/3 over Unikraft -> HTTP/1.1 backend\n"


def udp_port():
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
        sock.bind(('127.0.0.1', 0))
        return sock.getsockname()[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--image', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True,
                        help='new private directory for credentials and logs')
    parser.add_argument('--client-prefix', type=Path,
                        help='optional prefix with bin/{curl,openssl} and lib/')
    parser.add_argument('--port', type=int, default=4433,
                        help='localhost UDP port; 0 selects an unused port')
    parser.add_argument('--keep-running', action='store_true')
    args = parser.parse_args()
    if platform.machine() not in ('aarch64', 'arm64'):
        parser.error('this recipe is qualified on ARM64 Linux/KVM')
    if not 0 <= args.port <= 65535:
        parser.error('--port must be between 0 and 65535')
    image = args.image.resolve(strict=True)
    if not os.access('/dev/kvm', os.R_OK | os.W_OK):
        parser.error('/dev/kvm must be accessible to the current user')
    qemu = shutil.which('qemu-system-aarch64')
    cpio = shutil.which('cpio')
    env = dict(os.environ, OPENSSL_CONF='/dev/null')
    if args.client_prefix:
        prefix = args.client_prefix.resolve(strict=True)
        curl, openssl = (str(prefix/'bin'/name) for name in ('curl', 'openssl'))
        env['LD_LIBRARY_PATH'] = str(prefix/'lib') + (
            ':'+env['LD_LIBRARY_PATH'] if env.get('LD_LIBRARY_PATH') else '')
    else:
        curl, openssl = shutil.which('curl'), shutil.which('openssl')
    if (not all((qemu, cpio, curl, openssl)) or
            not all(os.access(p, os.X_OK) for p in (curl, openssl) if p)):
        parser.error('install QEMU, GNU cpio, OpenSSL 3 and an HTTP/3 curl')
    version = subprocess.check_output([curl, '--version'], env=env, text=True)
    if 'HTTP3' not in version.split():
        parser.error('selected curl has no HTTP3 feature; use --client-prefix')
    os.umask(0o077)
    output = args.output.resolve()
    try:
        output.mkdir(mode=0o700, parents=True)
    except FileExistsError:
        parser.error('output already exists; choose a new private directory')
    rootfs = output/'rootfs'
    rootfs.mkdir()
    requests = []

    class Handler(BaseHTTPRequestHandler):
        protocol_version = 'HTTP/1.1'

        def do_GET(self):
            requests.append(self.request_version)
            self.send_response(200)
            self.send_header('Content-Type', 'text/plain')
            self.send_header('Content-Length', str(len(BODY)))
            self.end_headers()
            self.wfile.write(BODY)

        def log_message(self, *unused):
            pass

    backend = ThreadingHTTPServer(('127.0.0.1', 0), Handler)
    worker = threading.Thread(target=backend.serve_forever, daemon=True)
    worker.start()
    process = serial = None
    result = dict(status='FAIL', image=str(image))
    host_port = args.port or udp_port()
    control_port = udp_port()
    while control_port == host_port:
        control_port = udp_port()
    token = secrets.token_hex(32)
    try:
        commands = [
            ['req', '-x509', '-newkey', 'rsa:2048', '-noenc', '-days', '2',
             '-subj', '/CN=Local demo CA', '-addext',
             'basicConstraints=critical,CA:TRUE', '-keyout', str(output/'ca.key'),
             '-out', str(output/'ca.pem')],
            ['req', '-new', '-newkey', 'rsa:2048', '-noenc',
             '-subj', '/CN=localhost', '-keyout', str(rootfs/'server.key'),
             '-out', str(output/'server.csr')],
            ['x509', '-req', '-in', str(output/'server.csr'), '-CA',
             str(output/'ca.pem'), '-CAkey', str(output/'ca.key'),
             '-CAcreateserial', '-days', '2', '-sha256', '-extfile',
             str(output/'server.ext'), '-out', str(rootfs/'server.pem')],
        ]
        (output/'server.ext').write_text(
            'subjectAltName=DNS:localhost\nbasicConstraints=critical,CA:FALSE\n'
            'keyUsage=critical,digitalSignature,keyEncipherment\n'
            'extendedKeyUsage=serverAuth\n')
        with (output/'cert.log').open('x') as log:
            for command in commands:
                subprocess.run([openssl, *command], env=env, check=True,
                               stdout=log, stderr=subprocess.STDOUT)
        (rootfs/'nghttpx.conf').write_text(
            f'frontend=10.0.2.15,4433;quic\nbackend=10.0.2.2,{backend.server_port}\n'
            'private-key-file=/server.key\ncertificate-file=/server.pem\n'
            'workers=1\nsingle-process=yes\nsingle-thread=yes\nno-quic-bpf=yes\n'
            'errorlog-file=/dev/stderr\nlog-level=NOTICE\n')
        (rootfs/'native-runtime.conf').write_text(
            'control=10.0.2.15,4434\ndrain-seconds=5\n'
            + (Path(__file__).resolve().parents[1]/
               'recommended-runtime.conf').read_text())
        (rootfs/'native-epoch.txt').write_text(f'{int(time.time())}\n')
        (rootfs/'native-control.token').write_text(token+'\n')
        names = b'\0'.join(p.name.encode() for p in sorted(rootfs.iterdir()))+b'\0'
        with (output/'demo.cpio').open('xb') as archive:
            subprocess.run([cpio, '--null', '-o', '--format=newc', '--quiet'],
                           input=names, cwd=rootfs, stdout=archive, check=True)
        serial = (output/'serial.log').open('x')
        process = subprocess.Popen([
            qemu, '-machine', 'virt,gic-version=host', '-cpu', 'host', '-accel',
            'kvm', '-m', '768M', '-smp', '1', '-display', 'none', '-monitor',
            'none', '-serial', 'stdio', '-no-reboot', '-kernel', str(image),
            '-initrd', str(output/'demo.cpio'), '-netdev',
            f'user,id=n0,host=10.0.2.2,dhcpstart=10.0.2.15,'
            f'hostfwd=udp:127.0.0.1:{host_port}-10.0.2.15:4433,'
            f'hostfwd=udp:127.0.0.1:{control_port}-10.0.2.15:4434', '-device',
            'virtio-net-device,netdev=n0,csum=off,guest_csum=off,gso=off,'
            'host_tso4=off,host_tso6=off,host_ecn=off,host_ufo=off'],
            stdin=subprocess.DEVNULL, stdout=serial, stderr=subprocess.STDOUT)
        deadline = time.monotonic()+20
        while 'NATIVE_NGHTTPX READY' not in (output/'serial.log').read_text():
            if process.poll() is not None or time.monotonic() >= deadline:
                raise RuntimeError('guest did not become ready; see serial.log')
            time.sleep(.05)
        command = [curl, '--http3-only', '--silent', '--show-error', '--fail',
                   '--noproxy', '*', '--cacert', str(output/'ca.pem'), '--resolve',
                   f'localhost:{host_port}:127.0.0.1', '--max-time', '15',
                   '--output', str(output/'response.txt'), '--write-out',
                   '%{http_version}', f'https://localhost:{host_port}/']
        client = subprocess.run(command, env=env, capture_output=True, timeout=20)
        (output/'client.log').write_bytes(client.stderr)
        if (client.returncode or client.stdout != b'3' or
                (output/'response.txt').read_bytes() != BODY or
                requests != ['HTTP/1.1']):
            raise RuntimeError('HTTP/3/backend verification failed; see client.log')
        result.update(status='PASS', frontend='HTTP/3', backend='HTTP/1.1',
                      certificate_verified=True, host_port=host_port)
        print('DEMO PASS: verified TLS, HTTP/3 frontend, HTTP/1.1 backend', flush=True)
        print(BODY.decode().strip(), flush=True)
        print(f'Private credentials and logs: {output}', flush=True)
        if args.keep_running:
            print(f'Listening at https://localhost:{host_port}; Ctrl-C to stop.', flush=True)
            while process.poll() is None:
                time.sleep(.2)
            raise RuntimeError('guest exited unexpectedly; see serial.log')
    except KeyboardInterrupt:
        pass
    finally:
        if process and process.poll() is None:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as sock:
                sock.sendto(token.encode(), ('127.0.0.1', control_port))
            try:
                process.wait(timeout=10)
            except subprocess.TimeoutExpired:
                process.terminate()
                process.wait(timeout=5)
        if serial:
            serial.close()
        if process and result['status'] == 'PASS':
            stopped = 'NATIVE_NGHTTPX EXIT result=0' in (
                output/'serial.log').read_text()
            result['shutdown'] = 'graceful' if stopped else 'forced'
            if not stopped or process.returncode != 0:
                result['status'] = 'FAIL'
        backend.shutdown()
        backend.server_close()
        worker.join(timeout=2)
        (output/'result.json').write_text(json.dumps(result, indent=2)+'\n')
    return 0 if result['status'] == 'PASS' else 1


if __name__ == '__main__':
    raise SystemExit(main())
