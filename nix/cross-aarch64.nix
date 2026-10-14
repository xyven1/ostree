# Cross-compile this ostree fork for aarch64 Linux.
#
#   nix-build nix/cross-aarch64.nix -A ostree     the package (bin/ostree, lib/libostree-1.so, ...)
#   nix-build nix/cross-aarch64.nix -A bundle     a tarball of ostree and everything it needs at
#                                                 run time (glibc, glib, curl, zstd, xz, ...)
#
# The bundle is self-contained: unpack it at / on the device and run the ostree binary named in
# its README.  It does not use the device's own libraries, so the device's distribution and glibc
# version do not matter, and Nix does not need to be installed there.
#
# The source is the committed state of this git tree plus uncommitted changes to tracked files;
# the bsdiff, libglnx and hdiffpatch submodules must be checked out.
{
  pkgs ? import <nixpkgs> { },
  withOpenssl ? true,
}:

let
  # nixpkgs cross-compiles OpenSSL for aarch64 with its generic C-only target, which makes its
  # SHA-256 no faster than GLib's.  When OpenSSL is wanted, build it (and so everything that
  # links it) with the aarch64 target, which uses the CPU's SHA instructions.
  opensslWithAsm = final: prev: {
    openssl =
      if prev.stdenv.hostPlatform.isAarch64 && prev.stdenv.hostPlatform != prev.stdenv.buildPlatform then
        prev.openssl.overrideAttrs (_: {
          configureScript = "./Configure linux-aarch64";
        })
      else
        prev.openssl;
  };

  cross =
    if withOpenssl then
      import pkgs.path {
        localSystem = pkgs.stdenv.buildPlatform.system;
        crossSystem = pkgs.pkgsCross.aarch64-multiplatform.stdenv.hostPlatform.config;
        overlays = [ opensslWithAsm ];
      }
    else
      pkgs.pkgsCross.aarch64-multiplatform;

  src = builtins.fetchGit {
    url = ./..;
    submodules = true;
    shallow = true;
  };
in
rec {
  ostree = cross.callPackage ./ostree-fork.nix { inherit src withOpenssl; };

  bundle =
    pkgs.runCommand "ostree-aarch64-bundle"
      {
        nativeBuildInputs = [
          pkgs.gnutar
          pkgs.zstd
        ];
        closure = pkgs.closureInfo { rootPaths = [ ostree ]; };
      }
      ''
        mkdir -p $out
        # Store paths are absolute; archive them relative to / so the tarball unpacks at /
        sed 's|^/||' $closure/store-paths > paths
        tar --create --zstd --file $out/ostree-aarch64.tar.zst --directory / \
            --owner=0 --group=0 --numeric-owner --files-from paths
        cat > $out/README <<EOF
        ostree for aarch64, with hdiff, zstd and bulk static delta support.

        Install:  zstd -dc ostree-aarch64.tar.zst | tar -x -C /       (as root)
        Run:      ${ostree}/bin/ostree --version

        Contents ($(wc -l < paths) store paths, $(du -sh --apparent-size $out/ostree-aarch64.tar.zst | cut -f1) compressed):
        $(cat $closure/store-paths)
        EOF
      '';
}
