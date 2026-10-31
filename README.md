# Native HTTP/3 reverse proxy on Unikraft

This application runs nghttpx as a native Unikraft unikernel. It terminates
HTTP/3/QUIC and TLS, then forwards requests to existing HTTP/1.1 or HTTP/2
origins. The origins do not need QUIC support.

```text
HTTP/3 client -- QUIC/UDP --> Unikraft VM -- HTTP/1.1 or HTTP/2/TCP --> origin
                            nghttpx
                            ngtcp2 + nghttp3 + OpenSSL
                            libev + musl + lwIP
```

The target entrypoint in [main.c](main.c) calls `app_nghttpx_run()` once.
[deployment.c](deployment.c) configures the explicitly selected clock, network
and optional management hooks, then calls `uk_nghttpx_run(options)`.
Protocol processing, TLS and backend proxying remain in library ports.
This repository supplies deployment support and a host-side demonstration,
not another QUIC implementation.

## Requirements

The qualified recipe uses **ARM64 Linux with usable KVM**, one vCPU and 768 MiB
of guest memory. Host requirements:

- Git, GNU make, binutils, Clang 19, a host C compiler, flex, bison, Perl,
  Python 3.10+, patch, wget, unzip, tar and xz/gzip support.
- `qemu-system-aarch64` and GNU `cpio`; the current user must have read/write
  access to `/dev/kvm`.
- OpenSSL 3 and a curl whose `curl --version` lists **HTTP3**. A distribution's
  default curl may lack that feature. See curl's [HTTP/3 client build guide](https://curl.se/docs/http3.html)
  if a separate client build is needed. The host client is independent of the
  guest TLS/QUIC ports.

The demo uses QEMU user networking with localhost UDP forwarding. It needs no
TAP device, bridge, host routes or root privileges once KVM is accessible.
No compiler, client binary, VM image or private credential is bundled here.

## Get the pinned dependencies

Run the following from **this app-nghttpx checkout**. The separate workspace
supplies the fixed core and library commits; continue using this application
checkout for the build and demo commands.

If the application already sits in that workspace, set DEPS_ROOT to its parent
directory and skip the clone below. `UK_ROOT` and `LIBS_ROOT` allow the same
application to build against either layout.

```sh
APP_ROOT="$PWD"
DEPS_ROOT="$(dirname "$APP_ROOT")/http3-deps"
git clone https://github.com/ASheng1019/unikraft-http3.git "$DEPS_ROOT"
git -C "$DEPS_ROOT" checkout --detach 228a6fce6fa66266e68a4324925fb69ec77ac3cb
git -C "$DEPS_ROOT" submodule update --init --recursive -- unikraft libs
```

The pinned workspace records the source archives in `sources.lock.json` and
exact development checkouts in Git submodules. Its `forks/` development
repositories are not needed to build the application. Avoid updating the
submodules to floating branch heads when reproducing these results.

## Build

Use a new output directory when changing source versions or profiles:

```sh
OUTDIR="$DEPS_ROOT/build/nghttpx-demo"
mkdir -p "$OUTDIR"
make UK_ROOT="$DEPS_ROOT/unikraft" LIBS_ROOT="$DEPS_ROOT/libs" \
  O="$OUTDIR" C="$OUTDIR/config" COMPILER=clang-19 defconfig
make UK_ROOT="$DEPS_ROOT/unikraft" LIBS_ROOT="$DEPS_ROOT/libs" \
  O="$OUTDIR" C="$OUTDIR/config" COMPILER=clang-19 -j8
```

The image is `$OUTDIR/app-nghttpx_qemu-arm64`. `defconfig` and `Kraftfile`
describe the same deployment profile. The Make recipe above is the direct
source-build path; it does not require Kraft or preinstalled local QA tools.

The selected versions are nghttpx/nghttp2 1.70.0, ngtcp2 1.25.0, nghttp3 1.18.0,
OpenSSL 3.5.9, lwIP 2.2.1, musl 1.2.3 and LLVM 21.1.8 runtimes. ARM64 core
prerequisites reuse the original authors' #1875/#1876 fixes. See the workspace
source lock and contribution plan for the other reused contributor work.

## Run and check HTTP/3

```sh
python3 "$APP_ROOT/tools/demo.py" \
  --image "$OUTDIR/app-nghttpx_qemu-arm64" \
  --output "$DEPS_ROOT/build/demo-run" --keep-running
```

This [script](tools/demo.py) starts a localhost HTTP/1.1 origin, generates a
private test CA and a localhost server certificate, creates the initrd, starts
the guest, and sends a certificate-verified `curl --http3-only` request. It
checks the negotiated HTTP version, response body and origin request version:

```text
DEMO PASS: verified TLS, HTTP/3 frontend, HTTP/1.1 backend
HTTP/3 over Unikraft -> HTTP/1.1 backend
...
Listening at https://localhost:4433; Ctrl-C to stop.
```

The VM remains available until Ctrl-C; omitting `--keep-running` runs the check
and shuts it down automatically. Each attempt requires a **new** output path;
the script refuses to overwrite an earlier deployment. Use `--port 0` to pick
an unused localhost port, or `--port 8443` to select another one.

If curl and OpenSSL are installed in a separate prefix, add
`--client-prefix /absolute/client-prefix`; the script uses its `bin/` and
`lib/` for host clients only. No system library is replaced. The one-request
demo uses stock QEMU user networking. The workspace's high-frequency/backend
fault qualification selects an explicit experimental host libslirp build;
those broader results are not implied by this demo.

While the demo runs, a second terminal can send another request:

```sh
DEMO_DIR=/absolute/path/printed/by/the/demo
curl --http3-only --noproxy '*' --cacert "$DEMO_DIR/ca.pem" \
  --resolve localhost:4433:127.0.0.1 https://localhost:4433/
```

Use the same client prefix/environment if curl is not installed system-wide.
Inspect `result.json`, `response.txt`, `client.log` and `serial.log` in the
private output directory. The serial log should contain `NATIVE_NGHTTPX READY`;
a normal authenticated shutdown ends with `NATIVE_NGHTTPX EXIT result=0`.

## Deployment contract

The generated `rootfs/` also provides a concrete example for a different origin:

| File in the initrd | Purpose |
|---|---|
| `/nghttpx.conf` | QUIC listener, origins, TLS files and single-worker settings |
| `/server.pem`, `/server.key` | Server certificate and private key |
| `/native-epoch.txt` | Fresh trusted Unix epoch in seconds; required only with APPNGHTTPX_BOOT_EPOCH |
| `/native-control.token` | 64 hexadecimal characters; required only with APPNGHTTPX_CONTROL |
| `/native-runtime.conf` | PMTUD/ECN, control address and drain timeout |

The demo guest uses `10.0.2.15:4433`; `10.0.2.2` reaches the host origin through
QEMU user networking. It forwards only the HTTP/3 port and an authenticated
shutdown port to localhost. `recommended-runtime.conf` documents the PMTUD
ceiling and ECN defaults; upstream nghttpx settings live in `/nghttpx.conf`.
Unknown or unsupported profile options are rejected at startup.

Keep the generated keys, token and initrd private. The CA is only for this local
demo. Regenerate boot data on each launch: the supplied epoch anchors realtime
to the guest monotonic clock, it is not an RTC, synchronization or clock
correction mechanism. The app's explicitly selected clock wrappers affect
the whole image. Selecting the nghttpx library itself adds no wrappers.
Shutdown/STATS are optional application management, with separate feature
switches; service lifecycle hooks expose no private nghttpx objects.

To use another origin, edit the generated backend entry and rebuild the initrd
with GNU cpio (or Unikraft's mkcpio); regenerate `/native-epoch.txt` immediately
before boot. An application/configuration change does not require protocol
code changes. The demo deliberately supplies a simple local HTTP/1.1 origin;
the broader workspace tests additionally cover HTTP/2 and verified TLS origins.

## Select deployment capabilities

All deployment feature switches default to **n** in Config.uk. The provided
ARM64 defconfig/Kraftfile explicitly enable boot epoch, management, shutdown,
STATS and named-interface IPv6 support for the demonstrated profile. FD scans
remain disabled. Build a fresh image after changing these selections.

| Selection | Effect |
|---|---|
| APPNGHTTPX_BOOT_EPOCH | Compile/link clock.c and the three global libc wrappers; otherwise use platform realtime, ignoring native-epoch.txt |
| APPNGHTTPX_CONTROL | Compile the UDP management adapter and require native-control.token; disabling it removes the socket/token requirement and both management commands |
| APPNGHTTPX_SHUTDOWN | Accept authenticated stop, start bounded drain and request upstream graceful shutdown |
| APPNGHTTPX_STATS | Return read-only counters and available allocator data; can be used with shutdown disabled |
| APPNGHTTPX_FD_STATS | Explicitly scan 4096 FDs per STATS request; otherwise fds=-1 and fd_scan_limit=0 |
| APPNGHTTPX_IPV6 | Parse named-interface deployment settings and call the independent lwIP IPv6 helper |

For a service without a management endpoint, clear CONTROL, SHUTDOWN, STATS
and FD_STATS, omit native-control.token, control and drain-seconds. HTTP/3 is
unchanged; the host/VM supervisor owns teardown. The demo helper assumes the
provided shutdown-enabled profile; it must not be used to claim graceful exit
for an unmanaged image. For STATS-only clear SHUTDOWN and omit drain-seconds.
Management requires at least shutdown or STATS. Options for disabled features
are rejected rather than silently ignored.

Clearing BOOT_EPOCH removes all three --wrap flags and their implementations.
It requires another working realtime provider: this ARM64 generic-timer target
currently supplies only an epoch-zero wall clock, so it fails startup without
one. The isolated platform-clock test application is a test provider, not an
RTC implementation. CLOCK_MONOTONIC/QUIC timers are never reanchored.

IPv6 setup does not choose the first NIC. Provide an explicit interface, e.g.
the qualified single-virtio image uses en1 (the lwIP loopback occupies index0):

```ini
ipv6-interface=en1
ipv6-multicast-rx=yes
ipv6-address=fd00:1::2
ipv6-slaac=no
ipv6-dad-timeout-ms=6000
```

The multicast setting is an explicit assertion for this named transport, not
a capability detected for every uknetdev. Enable it only when receive-side
multicast works without a hardware filter callback (as qualified here with
virtio). The lwIP helper handles MLD/link-local/SLAAC/static address operations
on the TCP/IP thread and waits for link-local/static DAD. Duplicate addresses,
missing interfaces, unavailable multicast and DAD timeout fail startup. SLAAC
does not wait for or guarantee acquisition of a global address. Configuration
is not rolled back after failure. Another network initializer may configure
IPv6 instead; leave these deployment options absent in that case.

## Scope and upstream status

The current profile is ARM64/QEMU KVM, one application worker, epoll,
individual UDP datagrams and TCP origins. Multiple workers, x86_64/other
platforms, 0-RTT, QUIC DATAGRAM, WebTransport and GSO/GRO need separate work and
qualification. The full workspace has 11 ordinary-image HTTP/3 regression
suites; a successful demo is a smoke test, not a benchmark or production SLO.

The pinned development composition includes the separately retained PMTU
black-hole recovery and 50 ms TCP ACK-deadline policy commits. Initial library
submission branches exclude those policies. They are not prerequisites for the
one-request demo, and the full policy-specific regression results must not be
attributed to the initial base ports.

This application is a candidate native example. Official library names,
repository/catalog placement, the proposed deployment interfaces and final
upstream CI still require maintainer agreement. Clock/control live in this app;
common named-interface IPv6 mechanics live in the lwIP port. MIT/BSD/Zlib and other upstream
notices remain in their respective ports; this application uses BSD-3-Clause
([COPYING](COPYING)).
