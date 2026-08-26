#!/usr/bin/python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

import pathlib
import ssl
import sys

from ncclient import manager


def main() -> int:
    if len(sys.argv) != 5:
        print("usage: CLIENT HOST PORT CREDENTIAL-DIR MARKER-DIR", file=sys.stderr)
        return 2
    host, port_text, credential_text, marker_text = sys.argv[1:]
    credentials = pathlib.Path(credential_text)
    markers = pathlib.Path(marker_text)
    with manager.connect_tls(
        host=host,
        port=int(port_text),
        keyfile=str(credentials / "alice-key.pem"),
        certfile=str(credentials / "alice-cert.pem"),
        ca_certs=str(credentials / "ca-cert.pem"),
        protocol=ssl.PROTOCOL_TLS_CLIENT,
        server_hostname="localhost",
        check_hostname=True,
        timeout=10,
    ) as session:
        reply = session.create_subscription()
        if not reply.ok:
            raise RuntimeError(f"create-subscription failed: {reply}")
        (markers / "subscribed").write_text("ready\n", encoding="ascii")
        allowed = session.take_notification(timeout=10)
        if allowed is None or "yang-library-update" not in allowed.notification_xml:
            raise RuntimeError(f"permitted notification missing: {allowed}")
        if "ncclient-interop" not in allowed.notification_xml:
            raise RuntimeError(f"permitted notification content missing: {allowed}")
        denied = session.take_notification(timeout=2)
        if denied is not None:
            raise RuntimeError(f"denied notification was delivered: {denied}")
        print("ncclient subscription: PASS")
        print("received yang-library-update: PASS")
        print("suppressed yang-library-change: PASS")
        (markers / "client-done").write_text("done\n", encoding="ascii")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
