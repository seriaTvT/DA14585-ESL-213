#!/usr/bin/env python3
"""Serve webui/ over HTTPS, so a phone on the LAN can use Web Bluetooth.

Web Bluetooth is gated behind a *secure context*. That means https://, or the
localhost special case - and nothing else. So `python3 -m http.server` is fine
on the machine running it and silently useless from anywhere else: the page
loads, navigator.bluetooth is undefined, and the browser gives no hint why.

This serves the same directory over TLS with a self-signed certificate, which
is enough to make the origin secure. The certificate is not trusted by anything,
so the browser shows an interstitial the first time - click through it
("Advanced" then "Proceed") and Bluetooth works from then on. Chrome keeps the
bypass per-origin, so this is a one-time nuisance per device.

    python3 webui/serve.py            # port 8443
    python3 webui/serve.py 9000

Certificates land in webui/.certs/ (gitignored) and are reused. Delete that
directory to force new ones - which you must do if your LAN address changes,
since the address is baked into the certificate.
"""
import http.server
import ipaddress
import os
import socket
import ssl
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
CERT_DIR = HERE / '.certs'
CERT = CERT_DIR / 'cert.pem'
KEY = CERT_DIR / 'key.pem'
DEFAULT_PORT = 8443


def lan_addresses() -> list[str]:
    """Best-effort list of this machine's LAN addresses.

    Opening a UDP socket to a public address makes the kernel pick the route it
    would really use, which is far more reliable than gethostbyname() - that
    tends to answer 127.0.1.1 on Debian-family systems.
    """
    found = []
    for probe in ('8.8.8.8', '1.1.1.1'):
        try:
            with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
                s.settimeout(0.5)
                s.connect((probe, 80))
                found.append(s.getsockname()[0])
        except OSError:
            pass
    return sorted(set(found))


def make_cert(hosts: list[str]) -> None:
    """Generate a self-signed cert covering every address we might be reached on.

    The addresses go in subjectAltName because browsers have ignored the
    Common Name for years - without a matching SAN entry Chrome rejects the
    certificate outright rather than offering the bypass.
    """
    CERT_DIR.mkdir(exist_ok=True)

    alt = ['DNS:localhost', 'IP:127.0.0.1', 'IP:::1']
    for h in hosts:
        try:
            ipaddress.ip_address(h)
            alt.append(f'IP:{h}')
        except ValueError:
            alt.append(f'DNS:{h}')

    print(f'generating a self-signed certificate for {", ".join(alt)}')
    subprocess.run(
        ['openssl', 'req', '-x509', '-newkey', 'rsa:2048', '-nodes',
         '-keyout', str(KEY), '-out', str(CERT),
         '-days', '825', '-subj', '/CN=hema-epd-clock',
         '-addext', f'subjectAltName={",".join(alt)}'],
        check=True, stderr=subprocess.DEVNULL)
    os.chmod(KEY, 0o600)


def rebuild_faces() -> None:
    """Regenerate faces_data.js from webui/faces/ before serving.

    The bundle is committed so the editor works for anyone who has not run a
    generator, which is exactly the arrangement where a stale copy goes
    unnoticed: the page loads and quietly shows the previous face. Rebuilding
    here keeps editing a .face file to edit-and-refresh.

    Failure is reported and not fatal - a syntax error in one face should not
    stop the server coming up, since the committed bundle is still servable and
    the message says what to fix."""
    tool = HERE.parent / 'tools' / 'genfaces.py'
    if not tool.exists():
        return
    r = subprocess.run([sys.executable, str(tool), '--emit'],
                       capture_output=True, text=True)
    if r.returncode != 0:
        print(f'warning: could not rebuild faces_data.js\n{r.stderr.strip()}')
        print('serving the committed bundle instead.')


def main() -> int:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else DEFAULT_PORT
    hosts = lan_addresses()

    rebuild_faces()

    if not CERT.exists() or not KEY.exists():
        try:
            make_cert(hosts)
        except FileNotFoundError:
            print('error: openssl not found - install it, or serve over '
                  'http://localhost instead (same machine only).')
            return 1
        except subprocess.CalledProcessError as e:
            print(f'error: openssl failed ({e})')
            return 1

    ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    ctx.load_cert_chain(CERT, KEY)

    handler = http.server.SimpleHTTPRequestHandler
    httpd = http.server.ThreadingHTTPServer(('0.0.0.0', port), handler)
    httpd.socket = ctx.wrap_socket(httpd.socket, server_side=True)

    os.chdir(HERE)
    print(f'\nserving {HERE} over https on port {port}\n')
    print(f'  this machine   https://localhost:{port}/')
    for h in hosts:
        print(f'  on the LAN     https://{h}:{port}/')
    print('\nThe certificate is self-signed, so the first visit from each '
          'device\nshows a warning - choose "Advanced" and proceed. Web '
          'Bluetooth then works.\nCtrl-C to stop.\n')

    try:
        httpd.serve_forever()
    except KeyboardInterrupt:
        print('\nstopped')
    return 0


if __name__ == '__main__':
    sys.exit(main())
