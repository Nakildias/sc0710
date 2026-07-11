{ config, pkgs, lib, ... }:
let
    compatibleKernels = [ "6.12" "6.13" "6.14" "6.15" "6.16" "6.17" "6.18" "6.19" "7.0" ];
    currentKernelMajorMinor = lib.versions.majorMinor config.hardware.sc0710.kernel.version;

    package-version = lib.fileContents ./version;

    cli = pkgs.writeShellScriptBin "sc0710-cli" (builtins.readFile ./scripts/sc0710-cli.sh);

    driver = pkgs.stdenv.mkDerivation rec {
        name = "sc0710-${package-version}-${config.hardware.sc0710.kernel.version}";
        version = package-version;

        enableParallelBuilding = true;

        src = ./.;
        nativeBuildInputs = config.hardware.sc0710.kernel.moduleBuildDependencies;

        makeFlags = [
            "KBUILD_DIR=${config.hardware.sc0710.kernel.dev}/lib/modules/${config.hardware.sc0710.kernel.modDirVersion}/build"
        ];

        installPhase = ''
            mkdir -p $out/lib/modules/${config.hardware.sc0710.kernel.modDirVersion}/kernel/drivers/media/pci/
            install -D build/sc0710.ko $out/lib/modules/${config.hardware.sc0710.kernel.modDirVersion}/kernel/drivers/media/pci/
        '';
    };
in
{
    options = {
        hardware.sc0710 = {
            enable = lib.mkEnableOption "Enable the sc0710 kernel module";
            kernel = lib.mkOption {
                default = config.boot.kernelPackages.kernel;
            };
        };
    };
    config = (lib.mkIf (config.hardware.sc0710.enable) {
        assertions = [
            {
                assertion = lib.elem currentKernelMajorMinor compatibleKernels;
                message = "sc0710 driver is not compatible with kernel ${currentKernelMajorMinor}. Compatible versions: ${lib.concatStringsSep ", " compatibleKernels}";
            }
        ];

        boot.extraModulePackages   = [ driver   ];
        boot.kernelModules         = [ "sc0710" ];
        environment.systemPackages = [ cli      ];

        # 4K Pro only: the driver programs the ECP5 FPGA during probe and needs
        # SC0710.FWI.HEX on disk; provision it once with scripts/extract-firmware.sh
        # (installs to /lib/firmware/sc0710, which the kernel loader reads directly).
    });
}
