{
  description = "C23 development environment — GCC 15 / LLVM 22, GNU ld / mold / lld";

  # Pinned to the exact revision this host's system closure was built from: every
  # store path the shells need is then already local or already on
  # cache.nixos.org, and locking costs no network round trip.
  # To follow a channel instead: github:NixOS/nixpkgs/nixos-26.05
  inputs.nixpkgs.url = "github:NixOS/nixpkgs/293d6abedf0478e681a4dfcfcb35b30fc796a32f";

  outputs =
    { nixpkgs, ... }:
    let
      # poop needs perf_event_open and mold cannot emit Mach-O, so the matrix is
      # Linux by construction rather than silently degraded on Darwin.
      systems = [
        "x86_64-linux"
        "aarch64-linux"
      ];

      # flake-utils' eachSystem, in one line of nixpkgs' own lib. Hand-rolling it
      # with builtins.listToAttrs to avoid forcing nixpkgs.lib measured identical
      # (+1.1k values out of 3.3M): the package sets below force lib regardless.
      forAllSystems = nixpkgs.lib.genAttrs systems;

      # One package set per system, memoised across every output: without this,
      # devShells and formatter each instantiate nixpkgs separately and
      # `nix flake check` costs 1.7x the CPU and heap.
      # `config` and `overlays` are pinned rather than using
      # nixpkgs.legacyPackages, which layers pkgs/top-level/impure-overlays.nix on
      # top and would read ~/.config/nixpkgs/overlays under `--impure`.
      pkgsFor = forAllSystems (
        system:
        import nixpkgs {
          inherit system;
          config = { };
          overlays = [ ];
        }
      );
    in
    {
      devShells = forAllSystems (
        system:
        let
          pkgs = pkgsFor.${system};
          inherit (pkgs) lib;

          # Newest *released* LLVM in this pin. llvmPackages_23 is a pre-release
          # git snapshot: not in the binary cache, so it would build from source.
          llvm = pkgs.llvmPackages_22;

          # The compiler is chosen by replacing the stdenv, never by adding a
          # second compiler to `packages`: that would leave $CC/$CXX on the
          # default stdenv and put two `cc`s on PATH.
          compilers = {
            gcc = pkgs.gcc15Stdenv; # 15.2.0 — already defaults to -std=gnu23
            clang = llvm.stdenv; # 22.1.5 — libstdc++; llvm.libcxxStdenv for libc++
          };

          # Alternative linkers. Both drivers resolve `-fuse-ld=NAME` to `ld.NAME`
          # on PATH, and cc-wrapper restores the caller's PATH before exec'ing the
          # real compiler, so listing the package is all that is required. Neither
          # ships a bare `ld`, so the stdenv's own linker is never shadowed.
          # This is the mechanism stdenvAdapters.useMoldLinker uses
          # (env.NIX_CFLAGS_LINK), minus the cc/bintools wrapper fork — which lets
          # all three shells of a toolchain share one gcc-wrapper and one stdenv.
          linkers = {
            mold = pkgs.mold;
            lld = llvm.lld;
          };

          tools = [
            # build drivers
            pkgs.cmake
            pkgs.meson
            pkgs.ninja
            pkgs.pkg-config
            # compilation database + compiler cache
            pkgs.bear
            pkgs.ccache
            # correctness
            pkgs.gdb
            pkgs.valgrind
            # benchmarking and profiling
            pkgs.hyperfine
            pkgs.poop
            pkgs.samply # needs kernel.perf_event_paranoid <= 1; poop works at the default 2
            # Tracy 0.13.1: profiler UI, tracy-capture/-csvexport, *and* the client
            # (include/tracy, libTracyClient — both already on the compiler search
            # paths). Instrumentation stays opt-in: defining TRACY_ENABLE here would
            # instrument the benchmark builds too. By hand it is
            #   cc -DTRACY_ENABLE app.c -lTracyClient   with <tracy/tracy/TracyC.h>
            # and find_package(Tracy) / meson dependency('Tracy', method: 'cmake')
            # — capital T — resolve through the CMAKE_PREFIX_PATH below and define it
            # for you. TRACY_NO_EXIT=1 is a *runtime* variable: it makes a program
            # that finishes in microseconds wait for the profiler to attach.
            # Bumping past tracy 0.14 needs -DTRACY_ENABLE=ON — upstream flipped that
            # CMake default to OFF and nixpkgs does not pass it.
            pkgs.tracy
            # fuzzing: AFL++ 4.35c, the fuzzer for C in active development (libFuzzer
            # has been maintenance-only since 2022; LibAFL is a Rust framework and is
            # not packaged). This pin builds it on its own clang 21.1.8, so
            # afl-clang-lto compiles C23 harnesses regardless of the shell's compiler,
            # with collision-free LTO edge coverage and CmpLog. It also understands
            # libFuzzer-style harnesses (-fsanitize=fuzzer links libAFLDriver), so one
            # LLVMFuzzerTestOneInput serves afl-fuzz here and libFuzzer in the clang
            # shell (compiler-rt 22 ships libclang_rt.fuzzer). Upstream is at 5.03c
            # (AGPL-3.0 since 5.00c); bumping past the pin means a source build.
            # Never set AFL_CC in the environment: afl-cc reads it as its backend.
            pkgs.aflplusplus
            # clangd / clang-format / clang-tidy, kept on the LLVM chosen above.
            # nixpkgs wraps clangd so it turns $NIX_CFLAGS_COMPILE into
            # $C_INCLUDE_PATH: start the editor from inside the shell (direnv) and
            # it resolves glibc headers with no --query-driver.
            llvm.clang-tools
          ];

          mkToolchainShell =
            ccName: stdenv: ldName:
            (pkgs.mkShell.override { inherit stdenv; }) {
              name = "c23-${ccName}${lib.optionalString (ldName != null) "-${ldName}"}";

              packages = tools ++ lib.optional (ldName != null) linkers.${ldName};

              # This is where you build -O0 -g for gdb/valgrind and -O2 for
              # hyperfine/poop. nixpkgs' default hardening injects
              # -D_FORTIFY_SOURCE=3 (which warns on every -O0 compile),
              # -fstack-protector-strong and -fzero-call-used-regs into both.
              # Off, so the compiler does exactly what the command line says and
              # measurements mean something; release builds re-enable hardening in
              # their own derivation. One injection survives hardeningDisable: the
              # cc wrapper appends -fno-omit-frame-pointer -mno-omit-leaf-frame-pointer
              # to every x86-64 compile (cc-cflags-before, no knob), so a build that
              # wants the register back must pass -fomit-frame-pointer itself; a
              # command-line flag wins.
              hardeningDisable = [ "all" ];

              env = {
                # Two spellings of one default, because they cover different holes.
                # CFLAGS is what make/cmake/meson/configure read, so it reaches
                # compile_commands.json — without it clangd parses C17 and reports
                # constexpr/nullptr/auto as errors.
                CFLAGS = "-std=gnu23";
                # ...and cc-wrapper reads this one, which catches build systems
                # that assign over CFLAGS. _BEFORE places it ahead of the caller's
                # own flags, so an explicit -std=c17 still wins.
                # gcc 15 already defaults to gnu23; clang 22 still defaults to gnu17.
                NIX_CFLAGS_COMPILE_BEFORE = "-std=gnu23";

                # clangd needs a compilation database. meson always writes one;
                # this makes cmake do the same; `bear -- make` covers handwritten
                # Makefiles (and records the wrapper-injected flags with it).
                CMAKE_EXPORT_COMPILE_COMMANDS = "1";

                # ccache is otherwise just a binary on PATH. cmake reads these two;
                # meson and make do not look for ccache at all, so they need an
                # explicit CC="ccache $CC" (or a meson native file).
                CMAKE_C_COMPILER_LAUNCHER = "ccache";
                CMAKE_CXX_COMPILER_LAUNCHER = "ccache";
              }
              // lib.optionalAttrs (ldName != null) {
                NIX_CFLAGS_LINK = "-fuse-ld=${ldName}";
              };

              shellHook = ''
                # nixpkgs' cmake hook fills NIXPKGS_CMAKE_PREFIX_PATH but only reads
                # it inside cmakeConfigurePhase. Exporting it is what lets a
                # hand-run `cmake` find the shell's own libraries — find_package(Tracy).
                export CMAKE_PREFIX_PATH="''${NIXPKGS_CMAKE_PREFIX_PATH-}''${CMAKE_PREFIX_PATH:+:''${CMAKE_PREFIX_PATH}}"

                # stderr, so `nix develop -c cmd | ...` keeps a clean stdout.
                echo 'c23 · ${ccName} ${stdenv.cc.cc.version} · ld.${
                  if ldName == null then "bfd" else "${ldName} ${lib.getVersion linkers.${ldName}}"
                } · -std=gnu23 · nixpkgs hardening off' >&2
              '';
            };

          # {gcc,clang} x {stdenv default, mold, lld}. Nix is lazy, so the shells
          # you never enter cost nothing: all six evaluate to ~2.8k values on top
          # of the package set they share.
          shells = lib.concatMapAttrs (
            ccName: stdenv:
            {
              ${ccName} = mkToolchainShell ccName stdenv null;
            }
            // lib.mapAttrs' (
              ldName: _: lib.nameValuePair "${ccName}-${ldName}" (mkToolchainShell ccName stdenv ldName)
            ) linkers
          ) compilers;
        in
        shells // { default = shells.gcc; }
      );

      formatter = forAllSystems (system: pkgsFor.${system}.nixfmt-tree);
    };
}
