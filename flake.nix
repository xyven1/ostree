{
  description = "Dev shell for the ostree s3:// fork";

  # Resolved through the local flake registry and pinned in flake.lock.
  inputs.nixpkgs.url = "nixpkgs";

  outputs = { self, nixpkgs }:
    let
      systems = [ "x86_64-linux" "aarch64-linux" ];
      forAllSystems = f: nixpkgs.lib.genAttrs systems (system: f (import nixpkgs {
        inherit system;
        # nixpkgs marks MinIO insecure because it is unmaintained. It is used
        # here only as a localhost S3 fixture by tests/test-pull-s3.sh.
        config.allowInsecurePredicate = pkg: nixpkgs.lib.getName pkg == "minio";
      }));
    in
    {
      devShells = forAllSystems (pkgs: {
        default = pkgs.mkShell {
          inputsFrom = [ pkgs.ostree ];
          packages = with pkgs; [
            autoconf
            automake
            libtool
            pkg-config
            bison
            curl
            aws-c-auth
            aws-c-http
            aws-c-io
            aws-c-cal
            aws-c-sdkutils
            aws-c-common
            minio
            minio-client
            python3
            valgrind
          ];
        };
      });
    };
}
