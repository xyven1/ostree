{
  description = "ostree with native s3:// remotes: package, tests and a dev shell with a local S3 server";

  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixos-unstable";
  # libglnx and bsdiff are git submodules.
  inputs.self.submodules = true;

  outputs =
    { self, nixpkgs }:
    let
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs systems (
          system:
          f (
            import nixpkgs {
              inherit system;
              # nixpkgs marks MinIO insecure because it is unmaintained. It is
              # only used here as a local S3 server for tests and development.
              config.allowInsecurePredicate = pkg: nixpkgs.lib.getName pkg == "minio";
            }
          )
        );

      awsLibs =
        pkgs: with pkgs; [
          aws-c-auth
          aws-c-cal
          aws-c-common
          aws-c-http
          aws-c-io
          aws-c-sdkutils
        ];

      # nixpkgs' ostree, built from this tree with s3:// support. Its patches
      # target the released version, and its installed tests aren't needed.
      ostreeFor =
        pkgs:
        pkgs.ostree.overrideAttrs (old: {
          pname = "ostree-s3";
          version = "0-unstable-${self.shortRev or self.dirtyShortRev or "dirty"}";
          src = self;
          patches = [ ];
          outputs = [
            "out"
            "dev"
            "man"
          ];
          configureFlags = builtins.filter (f: f != "--enable-installed-tests") old.configureFlags ++ [
            "--with-aws"
          ];
          buildInputs = old.buildInputs ++ awsLibs pkgs;
          postFixup = "";
        });

      # Tools for the s3:// tests: a local S3 server and its client.
      testTools =
        pkgs: with pkgs; [
          curl
          minio
          minio-client
          python3
        ];

      # Wrappers to run a local S3 server (MinIO) to pull from.
      mockScripts =
        pkgs:
        let
          common = ''
            state=''${S3_MOCK_DIR:-''${XDG_RUNTIME_DIR:-/tmp}/ostree-s3-mock}
            export MC_CONFIG_DIR="$state/mc"
          '';
        in
        [
          (pkgs.writeShellApplication {
            name = "s3-mock-start";
            runtimeInputs = with pkgs; [
              coreutils
              curl
              minio
              minio-client
              python3
            ];
            text = ''
              # Start a local S3 server (MinIO) with one bucket.
              #   s3-mock-start [BUCKET]   (default bucket: ostree)
              # State lives in $S3_MOCK_DIR (default $XDG_RUNTIME_DIR/ostree-s3-mock).
              ${common}
              user=ostreemock
              secret=ostreemocksecret
              bucket=''${1:-ostree}
              if test -f "$state/pid" && kill -0 "$(cat "$state/pid")" 2>/dev/null; then
                echo "already running; see $state/env" >&2
                exit 1
              fi
              mkdir -p "$state/data"
              port=$(python3 -c 'import socket; s = socket.socket(); s.bind(("127.0.0.1", 0)); print(s.getsockname()[1])')
              MINIO_ROOT_USER=$user MINIO_ROOT_PASSWORD=$secret \
                minio server --quiet --address "127.0.0.1:$port" "$state/data" > "$state/minio.log" 2>&1 &
              echo $! > "$state/pid"
              for _ in $(seq 1 100); do
                curl -sf -o /dev/null "http://127.0.0.1:$port/minio/health/live" && break
                sleep 0.1
              done
              mc --quiet alias set mock "http://127.0.0.1:$port" "$user" "$secret" > /dev/null
              mc --quiet mb --ignore-existing "mock/$bucket" > /dev/null
              cat > "$state/env" <<EOF
              export AWS_ACCESS_KEY_ID=$user
              export AWS_SECRET_ACCESS_KEY=$secret
              export AWS_EC2_METADATA_DISABLED=true
              export S3_MOCK_ENDPOINT=http://127.0.0.1:$port
              EOF
              echo "MinIO on http://127.0.0.1:$port, bucket '$bucket' (log: $state/minio.log)"
              echo
              echo "  source $state/env"
              echo "  s3-mock-publish path/to/archive-repo $bucket/repo"
              echo "  ostree remote add --set=gpg-verify=false origin 's3://$bucket/repo?endpoint=http://127.0.0.1:$port&region=us-east-1'"
            '';
          })
          (pkgs.writeShellApplication {
            name = "s3-mock-publish";
            runtimeInputs = [ pkgs.minio-client ];
            text = ''
              # Copy an archive-mode repo into the local S3 server.
              #   s3-mock-publish REPO BUCKET/PREFIX
              ${common}
              if test $# -ne 2; then
                echo "usage: s3-mock-publish REPO BUCKET/PREFIX" >&2
                exit 2
              fi
              mc --quiet mirror --overwrite --remove "$1/" "mock/$2/" > /dev/null
              echo "published $1 to s3://$2"
            '';
          })
          (pkgs.writeShellApplication {
            name = "s3-mock-stop";
            runtimeInputs = [ pkgs.coreutils ];
            text = ''
              # Stop the local S3 server; pass --clean to delete its data too.
              ${common}
              if test -f "$state/pid"; then
                kill "$(cat "$state/pid")" 2>/dev/null || true
                rm -f "$state/pid" "$state/env"
              fi
              if test "''${1:-}" = --clean; then
                rm -rf "$state"
              fi
            '';
          })
        ];
    in
    {
      packages = forAllSystems (pkgs: {
        default = ostreeFor pkgs;
      });

      checks = forAllSystems (pkgs: {
        # The s3:// unit tests and the pulls from MinIO and from a faulty server.
        s3-tests = (ostreeFor pkgs).overrideAttrs (old: {
          pname = "ostree-s3-tests";
          doCheck = true;
          nativeCheckInputs = (old.nativeCheckInputs or [ ]) ++ testTools pkgs;
          preCheck = ''
            patchShebangs buildutil tests
          '';
          checkPhase = ''
            runHook preCheck
            # The test harness defaults to /var/tmp, which the sandbox lacks.
            export TEST_TMPDIR=$TMPDIR
            make check TESTS="tests/test-fetcher-s3 tests/test-pull-s3.sh tests/test-pull-s3-faults.sh" \
              || { cat test-suite.log; exit 1; }
            runHook postCheck
          '';
        });
      });

      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ (ostreeFor pkgs) ];
          packages =
            testTools pkgs
            ++ mockScripts pkgs
            ++ (with pkgs; [
              clang-tools
              valgrind
            ]);
          shellHook = ''
            echo "Build:  env NOCONFIGURE=1 ./autogen.sh && ./configure --with-curl --with-aws && make -j\$(nproc)"
            echo "Tests:  make check TESTS='tests/test-fetcher-s3 tests/test-pull-s3.sh tests/test-pull-s3-faults.sh'"
            echo "Local S3 server: s3-mock-start, s3-mock-publish, s3-mock-stop"
          '';
        };
      });

      formatter = forAllSystems (pkgs: pkgs.nixfmt-tree);
    };
}
