# Static libostree experiment

Question: can libostree (this tree, including s3:// and hdiffpatch) be built
fully static, and can it be linked statically into a Rust binary?

Answer: yes to both, with musl via nixpkgs `pkgsStatic`.

```
static-experiment/build.sh libostree   # -> result-lib (static bin/ostree, lib/libostree-1.a)
static-experiment/build.sh consumer    # -> result-rs (static Rust binary using the ostree crate)
static-experiment/inspect-lib.sh       # outputs, features, ostree-1.pc
static-experiment/run-consumer.sh      # file:// and http:// pull, commit, checkout
```

## Results

- `bin/ostree` (24 MB) and `rofiles-fuse` are `statically linked` musl ELFs; no
  `.so` is produced. Features: zstd, hdiffpatch, libcurl, aws, gpgme,
  libarchive, selinux, sign-ed25519, libmount, p2p.
- `consumer/` is a Rust binary built from the in-tree `ostree` crate (0.20,
  glib/gio 0.20) with `SYSTEM_DEPS_LINK=static`. It is 22 MB stripped and
  static. It creates an archive repo, commits, pulls over file:// and over
  http:// (the static curl fetcher), then checks out.
- Usage: `ostree-static-consumer [-b|--branch <ref>] <workdir> [remote-url]`.
  With a URL, `<ref>` can be any branch on that remote; the contents check
  runs only when the pulled commit is the tool's own test commit.
- Both binaries run in `bwrap --tmpfs / --clearenv --unshare-all` with only the
  binary bound in: no /nix, /usr, /lib or /bin/sh. The CLI runs init, commit,
  log and fsck there.

## What it took

1. **gnupg.** pkgsStatic's full gnupg does not build: openldap's configure
   cannot find Cyrus SASL statically, and pcsclite fails to produce its doc
   output. gpgme is built against `gnupg.override { enableMinimal = true; }`
   with `doCheck = false` (its tests need gpg-agent, which minimal omits).
   `--arg withGpgme false` builds `--without-gpgme` instead.
2. **AWS_LIBS.** configure's default list relies on shared-library
   dependencies. A static link needs the full ordered closure:
   `-laws-c-auth -laws-c-http -laws-c-compression -laws-c-io -ls2n -laws-c-cal
   -laws-c-sdkutils -laws-c-common -lssl -lcrypto -lpthread -ldl` (OpenSSL is
   the crypto backend in pkgsStatic).
3. **ostree-1.pc.** On fork main `AWS_LIBS` was missing from `Libs.private`, so
   `pkg-config --static` consumers could not link. `configure.ac` now appends
   it, as the `s3-remotes-upstream` branch already does. The other libraries
   are covered by `Requires.private`; e2p and libmount are used only by the
   CLI.
4. **static-PIE.** Rust's musl target links static-PIE (`rcrt1.o`), but
   pkgsStatic's `libstdc++.a` is not built with `-fPIE`. It is pulled in by
   hdiffpatch (C++). The consumer uses `-C relocation-model=static`.
   Alternatives: build without hdiffpatch, or use a PIC libstdc++.
5. **Requires.private closure.** `pkg-config --static` needs every dependency's
   `.pc` file visible, so the consumer takes libostree's buildInputs as its own.

## Caveats

- GPG verification is not self-contained. gpgme spawns `gpg` from a path
  baked in at build time (here a Nix store path). Off Nix, either ship gpg at
  that path or use `--without-gpgme` and ed25519 signatures.
- Other build-time paths are baked in: `$datadir/ostree/trusted.gpg.d`, and
  glib's GIO module and locale dirs. A static GIO cannot load modules anyway;
  ostree does not need them with the curl backend.
- Not tested yet: https:// and s3:// pulls from the static binary. These
  depend on finding a CA bundle at run time (curl/OpenSSL and s2n each probe
  their own paths; `tls-ca-path` / `SSL_CERT_FILE`), and on aws-c-auth's
  credential providers.
- glibc static was not tried. NSS and `dlopen` make it a worse target than
  musl.
- Licensing: libostree and glib are LGPL-2.0+. Distributing a statically
  linked proprietary binary requires letting users relink, for example by
  shipping object files.
