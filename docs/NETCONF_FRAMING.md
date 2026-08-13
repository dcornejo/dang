<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NETCONF framing and sessions

`yang/netconf_framing.h` provides the transport-neutral portion of RFC 6242.
It accepts arbitrary byte-buffer boundaries, negotiates the NETCONF base
version from the client hello, decodes requests, dispatches them through
`NetconfServer`, and frames replies.

```cpp
#include <yang/netconf_framing.h>

yang::netconf::NetconfSession session(
    server, 1001, authenticated_username, 16 * 1024 * 1024);

write_to_transport(session.Start());

while (auto bytes = read_from_transport()) {
  auto output = session.Receive(*bytes);
  for (const std::string& framed_reply : output.bytes_to_send) {
    write_to_transport(framed_reply);
  }
  if (output.error || output.close_transport) {
    close_transport();
    break;
  }
}

// Also call this from the connection event loop when no bytes arrive.
if (session.Poll().close_transport) {
  close_transport();
}
```

Both hello messages always use `]]>]]>`. If both peers advertise base 1.1,
subsequent messages use chunked framing; otherwise they use the base 1.0 end
marker. The decoder supports partial headers, partial data, multiple chunks,
and multiple messages in one input buffer.

Malformed chunk sizes, leading zeroes, zero-size chunks, missing framing
markers, unsupported hello documents, and configured size-limit violations
close the session. The size limit applies to the reassembled XML message, not
to framing bytes. Unexpected transport closure releases locks and triggers the
RFC 6241 session-loss behavior for non-persistent confirmed commits.

Session IDs must be nonzero and unique among active sessions; `valid()` reports
whether registration succeeded. `kill-session` asynchronously marks the target
for termination. The host calls `Poll()` from its event loop (as well as
`Receive()` when bytes arrive) and closes the transport when
`close_transport` is true. Resource cleanup happens before the successful kill
reply is returned, so it is not delayed by a quiet or blocked transport.

The framing class deliberately consumes and produces byte strings only.
`NetconfTransportAdapter` adds the event-loop boundary for authenticated SSH
or TLS streams, timeouts, bounded queues, backpressure, cancellation, and Call
Home connection factories while leaving cryptography and socket I/O to the
host. See `NETCONF_TRANSPORT.md`.
