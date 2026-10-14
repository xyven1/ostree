# A lean build of this ostree fork, meant for cross-compiling to a device.
#
# It builds straight from the git tree (running autogen.sh), with zstd and HDiffPatch enabled, and
# leaves out what an update client does not need: GPG, libsoup, SELinux, systemd integration,
# FUSE, man pages, API docs and introspection.  Fetching is done with libcurl, checksums with
# OpenSSL.
{
  lib,
  stdenv,
  autoconf,
  automake,
  libtool,
  pkg-config,
  bison,
  which,
  glib,
  curl,
  xz,
  zstd,
  e2fsprogs,
  libarchive,
  util-linuxMinimal,
  openssl,
  # The source tree, including the bsdiff, libglnx and hdiffpatch submodules
  src,
  # Use OpenSSL's SHA-256 instead of GLib's.  Every object written is hashed, and with a CPU
  # that has SHA instructions OpenSSL is several times faster: on an aarch64 device applying a
  # delta took a quarter to a half less time.  The OpenSSL passed in must be built with its
  # assembly (see cross-aarch64.nix); a C-only one is slower than GLib.
  withOpenssl ? true,
}:

stdenv.mkDerivation {
  pname = "ostree-hdiff";
  version = "2026.5";

  inherit src;

  strictDeps = true;

  nativeBuildInputs = [
    autoconf
    automake
    libtool
    pkg-config
    bison
    which
    glib # glib-mkenums, glib-compile-resources
  ];

  buildInputs = [
    glib
    curl
    xz
    zstd
    e2fsprogs
    libarchive
    util-linuxMinimal # libmount
  ]
  ++ lib.optional withOpenssl openssl;

  preConfigure = ''
    patchShebangs autogen.sh
    env NOCONFIGURE=1 ./autogen.sh
  '';

  configureFlags = [
    "--with-curl"
    "--without-soup"
    "--without-soup3"
    "--with-zstd"
    "--with-hdiffpatch"
    "--with-libarchive"
    "--with-libmount"
    "--without-gpgme"
    "--without-selinux"
    "--without-avahi"
    "--without-libsystemd"
    "--disable-rofiles-fuse"
    "--disable-man"
  ]
  ++ lib.optional withOpenssl "--with-crypto=openssl";

  enableParallelBuilding = true;

  # The test suite needs a running system (user xattrs, a local web server) and cannot run when
  # cross-compiling anyway.
  doCheck = false;

  meta = {
    description = "ostree with hdiff, zstd and bulk static deltas";
    license = lib.licenses.lgpl2Plus;
    platforms = lib.platforms.linux;
  };
}
