<!-- Copyright 2026 David Cornejo -->
<!-- SPDX-License-Identifier: Apache-2.0 -->

# NETCONF event notifications

`NotificationManager` provides the transport-neutral RFC 5277 event service.
The host explicitly registers the mandatory `NETCONF` stream before passing
the manager to `NetconfServer`; notification and interleave capabilities are
advertised only while that stream is configured.

```cpp
yang::netconf::NotificationManager notifications(&nacm_policy);
notifications.AddStream({
    .name = "NETCONF",
    .replay_supported = true,
    .replay_event_limit = 4096,
});

yang::netconf::NetconfServer server(
    datastores, &nacm_policy, url_provider, &notifications);

// Called by an application event source. The payload is exactly one
// notification-specific XML element, without the RFC 5277 wrapper.
notifications.Publish(
    "NETCONF", "ietf-interfaces", "interface-state-change",
    R"xml(<interface-state-change
      xmlns="urn:ietf:params:xml:ns:yang:ietf-interfaces">
      <name>eth0</name>
    </interface-state-change>)xml");
```

`NetconfSession::Poll` drains queued events and applies the negotiated NETCONF
framing. The manager creates the complete `<notification>` document and its
UTC `eventTime`; the transport adapter only sends returned bytes.

Each session has at most one RFC 5277 subscription. The implementation
supports default or named streams, subtree and XPath filters, RFC 3339 start
and stop times with time zones and fractional seconds, bounded replay,
`replayComplete`, `notificationComplete`, and cleanup on session termination.
NACM notification authorization runs before an event enters a session queue.
The two completion markers cannot be filtered out.

Queue count and byte limits are fixed when the manager is constructed. A live
subscription that exceeds either limit is terminated and its queued data is
discarded, giving transports deterministic memory use without blocking event
publishers. A replay that cannot fit is rejected with `resource-denied`.
Hosts should poll writable sessions promptly and treat silent subscription
termination as a resource-exhaustion condition in their operational telemetry.

The library advertises RFC 5277 `:notification` and `:interleave` together and
continues to process ordinary RPCs while a subscription is active. It does not
implement the newer configured-subscription or YANG-Push protocols from RFCs
8639–8641.
