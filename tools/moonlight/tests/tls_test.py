#!/usr/bin/env python3
"""Only local ephemeral fixture servers and generated test certificates."""
import http.server
from pathlib import Path
import ssl
import subprocess
import sys
import tempfile
import threading


class Handler(http.server.BaseHTTPRequestHandler):
    def do_GET(self):
        name = dict(self.connection.getpeercert()["subject"][0])["commonName"]
        body = name.encode()
        self.send_response(200)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def log_message(self, *args):
        pass

    def handle(self):
        try:
            super().handle()
        except (ConnectionError, ssl.SSLError):
            pass  # Expected when the client's SPKI check aborts the impostor.


def certificate(directory, name):
    directory.mkdir()
    subprocess.run(["openssl", "req", "-x509", "-newkey", "rsa:2048", "-nodes", "-days", "1",
                    "-keyout", str(directory / "key.pem"), "-out", str(directory / "client.pem"),
                    "-subj", f"/CN={name}"], check=True, capture_output=True)


def server(identity, clients):
    context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
    context.load_cert_chain(identity / "client.pem", identity / "key.pem")
    for client in clients:
        context.load_verify_locations(client / "client.pem")
    context.verify_mode = ssl.CERT_REQUIRED
    instance = http.server.ThreadingHTTPServer(("127.0.0.1", 0), Handler)
    instance.socket = context.wrap_socket(instance.socket, server_side=True)
    thread = threading.Thread(target=instance.serve_forever, daemon=True)
    thread.start()
    return instance, thread


with tempfile.TemporaryDirectory(prefix="fogplay-moonlight-tls-") as temporary:
    root = Path(temporary)
    first, second, host, impostor = [root / name for name in ("first", "second", "host", "impostor")]
    for directory, name in ((first, "ClientOne"), (second, "ClientTwo"), (host, "MoonlightHost"), (impostor, "OtherHost")):
        certificate(directory, name)
    servers = [server(identity, [first, second]) for identity in (host, impostor)]
    try:
        urls = [f"https://127.0.0.1:{instance.server_port}/serverinfo?synthetic=1" for instance, _ in servers]
        subprocess.run([sys.argv[1], str(first), str(host / "client.pem"), *urls, str(second)], check=True)
    finally:
        for instance, thread in servers:
            instance.shutdown()
            instance.server_close()
            thread.join()
