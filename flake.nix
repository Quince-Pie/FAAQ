{
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/nixpkgs-unstable";

  outputs =
    { nixpkgs, ... }:
    let
      forAllSystems =
        f:
        nixpkgs.lib.genAttrs nixpkgs.lib.platforms.unix (
          system:
          f {
            pkgs = import nixpkgs { inherit system; };
          }
        );
    in
    {
      formatter = forAllSystems ({ pkgs }: pkgs.nixfmt-rfc-style);
      devShells = forAllSystems (
        { pkgs }:
        let
          tools =
            with pkgs;
            [
              bear
              cmake
              poop
              meson
              ninja
              ccache
              cmocka
              python3
              llvmPackages_20.clang-tools
            ]
            ++ lib.optionals stdenv.isLinux [
              valgrind
              gdb
            ];
          llvmToolchain = with pkgs.llvmPackages_20; [
            clang-tools
            clang
            lldb
            llvm
            bintools
          ];
          gccToolchain = with pkgs; [
            gcc15
            # ccls
          ];
        in
        {
          default = pkgs.mkShell {
            stdenv = pkgs.gcc15Stdenv;
            packages = tools ++ gccToolchain;
            hardeningDisable = ["all"];
          };

          gccMold = pkgs.mkShell {
            stdenv = pkgs.stdenvAdapters.useMoldLinker pkgs.gcc15Stdenv;
            packages = tools ++ gccToolchain;
          };

          gccGold = pkgs.mkShell {
            stdenv = pkgs.stdenvAdapters.useGoldLinker pkgs.gcc15Stdenv;
            packages = tools ++ gccToolchain;
          };

          llvm = pkgs.mkShell {
            stdenv = pkgs.llvmPackages_20.stdenv;
            packages = tools ++ llvmToolchain;
          };

          llvmMold = pkgs.mkShell {
            stdenv = pkgs.stdenvAdapters.useMoldLinker pkgs.llvmPackages_20.stdenv;
            packages = tools ++ llvmToolchain;
          };

          llvmGold = pkgs.mkShell {
            stdenv = pkgs.stdenvAdapters.useGoldLinker pkgs.llvmPackages_20.stdenv;
            packages = tools ++ llvmToolchain;
          };
        }
      );
    };
}
