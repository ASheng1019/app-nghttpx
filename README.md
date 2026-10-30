# nghttpx HTTP/3 application for Unikraft

A thin ARM64 application calling lib-nghttpx through <nghttpx/unikraft.h>.
The service, TLS, protocol and network adapters belong to their library ports.
Makefile uses sibling unikraft/ and libs/ by default; override UK_ROOT and
LIBS_ROOT for another layout. defconfig and Kraftfile select the same profile.

```sh
make COMPILER=clang-19 defconfig
make COMPILER=clang-19 -j8
```

Use the pinned [HTTP/3 workspace](https://github.com/ASheng1019/unikraft-http3)
for dependency checkouts, build instructions and integration tests. Supply a
private initrd with /nghttpx.conf, certificates, private key and trusted boot
epoch/control token in /native-boot.txt. recommended-runtime.conf shows public
runtime defaults; credentials and deployment outputs must remain private.

Qualified profile: ARM64/QEMU KVM, one worker, HTTP/3 frontend, HTTP/1.1 or
HTTP/2 TCP backend, OpenSSL 3.5.9, lwIP 2.2.1, LLVM 21.1.8 runtime. Unsupported
profiles include multiple workers, 0-RTT, QUIC DATAGRAM and WebTransport.
The glue is BSD-3-Clause; see COPYING and each dependency's own license.
