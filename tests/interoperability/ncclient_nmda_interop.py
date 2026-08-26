#!/usr/bin/python3
# Copyright 2026 David Cornejo
# SPDX-License-Identifier: Apache-2.0

import pathlib
import ssl
import sys

from ncclient import manager
from ncclient.operations.rpc import RPCError
from ncclient.xml_ import to_ele


NMDA = "urn:ietf:params:xml:ns:yang:ietf-netconf-nmda"
DATASTORES = "urn:ietf:params:xml:ns:yang:ietf-datastores"
ORIGIN = "urn:ietf:params:xml:ns:yang:ietf-origin"
APPLIANCE = "urn:example:appliance"


def dispatch(session, body: str) -> str:
    return session.dispatch(to_ele(body)).xml


def require(text: str, value: str, stage: str) -> None:
    if value not in text:
        raise RuntimeError(f"{stage} missing {value!r}: {text}")


def main() -> int:
    if len(sys.argv) != 4:
        print("usage: CLIENT HOST PORT CREDENTIAL-DIR", file=sys.stderr)
        return 2
    host, port_text, credential_text = sys.argv[1:]
    credentials = pathlib.Path(credential_text)
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
        running = dispatch(session, f"""
          <get-data xmlns="{NMDA}" xmlns:ds="{DATASTORES}">
            <datastore>ds:running</datastore>
            <subtree-filter><system xmlns="{APPLIANCE}"/></subtree-filter>
          </get-data>""")
        require(running, "edge-1", "running get-data")

        with session.locked("candidate"):
            edited = dispatch(session, f"""
              <edit-data xmlns="{NMDA}" xmlns:ds="{DATASTORES}">
                <datastore>ds:candidate</datastore>
                <config><system xmlns="{APPLIANCE}">
                  <hostname>ncclient-nmda</hostname>
                </system></config>
              </edit-data>""")
            require(edited, "<ok", "candidate edit-data")
            candidate = dispatch(session, f"""
              <get-data xmlns="{NMDA}" xmlns:ds="{DATASTORES}">
                <datastore>ds:candidate</datastore>
              </get-data>""")
            require(candidate, "ncclient-nmda", "candidate get-data")
            session.commit()

        intended = dispatch(session, f"""
          <get-data xmlns="{NMDA}" xmlns:ds="{DATASTORES}">
            <datastore>ds:intended</datastore>
          </get-data>""")
        require(intended, "ncclient-nmda", "intended get-data")

        operational = dispatch(session, f"""
          <get-data xmlns="{NMDA}" xmlns:ds="{DATASTORES}"
                    xmlns:or="{ORIGIN}">
            <datastore>ds:operational</datastore>
            <subtree-filter><system xmlns="{APPLIANCE}"/></subtree-filter>
            <origin-filter>or:intended</origin-filter>
            <with-origin/>
          </get-data>""")
        require(operational, "ncclient-nmda", "operational get-data")
        require(operational, ORIGIN, "operational origin namespace")
        require(operational, "intended", "operational origin value")

        try:
            dispatch(session, f"""
              <get-data xmlns="{NMDA}" xmlns:ds="{DATASTORES}">
                <datastore>ds:running</datastore><with-origin/>
              </get-data>""")
            raise RuntimeError("with-origin on running unexpectedly succeeded")
        except RPCError as error:
            if error.tag != "invalid-value":
                raise RuntimeError(
                    f"with-origin on running returned {error.tag}: {error}"
                ) from error

        print("ncclient get-data running/subtree: PASS")
        print("ncclient edit-data candidate/lock/commit: PASS")
        print("ncclient intended visibility: PASS")
        print("ncclient operational origin filter/metadata: PASS")
        print("ncclient invalid with-origin rejection: PASS")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
