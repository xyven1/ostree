# Experiment: build this tree's libostree fully static (musl, via
# nixpkgs pkgsStatic) and link it into a Rust binary.
#
#   nix build --impure -f static-experiment libostree -o static-experiment/result-lib
#   nix build --impure -f static-experiment consumer   -o static-experiment/result-rs
{ nixpkgs ? (builtins.getFlake "nixpkgs").outPath
, withAws ? true
, withGpgme ? true
}:
let
  pkgs = import nixpkgs { };
  s = pkgs.pkgsStatic;
  lib = pkgs.lib;

  src = builtins.path {
    name = "ostree-src";
    path = ../.;
    filter = path: type:
      let b = baseNameOf path; in
      !(b == ".git" || b == "target" || lib.hasPrefix "result" b
        || (type == "directory" && b == "static-experiment"));
  };

  awsLibs = with s; [ aws-c-auth aws-c-http aws-c-io aws-c-cal aws-c-sdkutils aws-c-common aws-c-compression s2n-tls openssl ];
  # configure's default AWS_LIBS relies on the shared libraries' own
  # dependencies; a static link must spell out the whole closure, in order.
  # It also lands in ostree-1.pc's Libs.private, so consumers inherit it.
  awsStaticLibs = "-laws-c-auth -laws-c-http -laws-c-compression -laws-c-io -ls2n"
    + " -laws-c-cal -laws-c-sdkutils -laws-c-common -lssl -lcrypto -lpthread -ldl";

  # pkgsStatic's full gnupg does not build (openldap can't find SASL statically,
  # pcsclite drops its doc output). gpgme only needs the gpg binary, so build a
  # minimal one. gpgme bakes its store path in and spawns it at run time.
  # Minimal gnupg has no gpg-agent, which gpgme's own test suite needs.
  gpgme = (s.gpgme.override { gnupg = s.gnupg.override { enableMinimal = true; }; })
    .overrideAttrs { doCheck = false; };

  libostree = (s.ostree.override { inherit gpgme; }).overrideAttrs (old: {
    pname = "ostree-fork";
    version = "git";
    inherit src;
    # The nixpkgs patches target the release tarball's installed tests.
    patches = [ ];
    outputs = [ "out" "dev" "man" ];
    buildInputs = lib.filter (p: withGpgme || lib.getName p != "gpgme") old.buildInputs
      ++ lib.optionals withAws awsLibs;
    configureFlags =
      (lib.filter (f: f != "--enable-installed-tests") old.configureFlags)
      ++ lib.optionals withAws [ "--with-aws" ]
      ++ lib.optionals (!withGpgme) [ "--without-gpgme" ];
    env = (old.env or { }) // lib.optionalAttrs withAws { AWS_LIBS = awsStaticLibs; };
    postFixup = "";
    doCheck = false;
  });
  # The consumer needs the whole tree for its `ostree = { path = "../.." }`.
  consumerSrc = builtins.path {
    name = "ostree-consumer-src";
    path = ../.;
    filter = path: type:
      let b = baseNameOf path; in
      !(b == ".git" || b == "target" || lib.hasPrefix "result" b);
  };

  consumer = s.rustPlatform.buildRustPackage {
    pname = "ostree-static-consumer";
    version = "0.1.0";
    src = consumerSrc;
    cargoRoot = "static-experiment/consumer";
    buildAndTestSubdir = "static-experiment/consumer";
    cargoLock.lockFile = ./consumer/Cargo.lock;
    nativeBuildInputs = [ s.buildPackages.pkg-config ];
    # pkg-config --static walks Requires.private, so every dependency's .pc
    # must be visible, not just ostree-1 and glib.
    buildInputs = [ libostree ]
      ++ lib.filter (p: !lib.hasPrefix "python3" (lib.getName p)) libostree.buildInputs;
    env.SYSTEM_DEPS_LINK = "static";
    # Rust's musl target links static-PIE, but pkgsStatic's libstdc++.a
    # (pulled in by hdiffpatch) is not built with -fPIE.
    env.RUSTFLAGS = "-C relocation-model=static";
    doCheck = false;
  };
in
{
  inherit libostree consumer pkgs;
}
