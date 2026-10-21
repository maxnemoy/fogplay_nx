#!/usr/bin/env python3
"""Local certificate-rejection fixture; never contacts external services."""
import pathlib
import socket
import ssl
import sys

context = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
context.minimum_version = ssl.TLSVersion.TLSv1_2
context.maximum_version = ssl.TLSVersion.TLSv1_2
context.load_cert_chain(sys.argv[1], sys.argv[2])
with socket.socket() as server:
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    server.settimeout(30)
    pathlib.Path(sys.argv[3]).write_text(str(server.getsockname()[1]))
    accepted, _ = server.accept()
    accepted.settimeout(5)
    try:
        with context.wrap_socket(accepted, server_side=True) as connection:
            data = connection.recv(1024)
            if data:
                raise RuntimeError("Untrusted client unexpectedly sent application data")
    except ssl.SSLError as error:
        print("Expected rejected TLS handshake:", error)
    finally:
        accepted.close()
