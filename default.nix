{ config, pkgs, lib, ... }:
let
    package-version = builtins.readFile ./version;

    cli = pkgs.writeShellScriptBin "sc0710-cli" (builtins.readFile ./scripts/sc0710-cli.sh);
    fw  = pkgs.writeShellScript "sc0710-firmware" (builtins.readFile ./scripts/sc0710-firmware.sh);

    driver = pkgs.stdenv.mkDerivation rec {
        name = "sc0710-${package-version}-${config.hardware.sc0710.kernel.version}";
        version = package-version;

        enableParallelBuilding = true;

        src = ./.;
        nativeBuildInputs = [ config.hardware.sc0710.kernel.moduleBuildDependencies ];

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
            enableFirmware = lib.mkEnableOption "Enable automatic firmware installation";
            kernel = lib.mkOption {
                default = config.boot.kernelPackages.kernel;
            };
        };
    };
    config = (lib.mkIf (config.hardware.sc0710.enable) {
        boot.extraModulePackages   = [ driver   ];
        boot.kernelModules         = [ "sc0710" ];
        environment.systemPackages = [ cli      ];

        systemd.services.sc0710-firmware = lib.mkIf config.hardware.sc0710.enableFirmware {
            unitConfig = {
                Description = "Install SC0710 FPGA Firmware";
                After = [ "network-online.target" ];
                Wants = [ "network-online.target" ];
            };
            serviceConfig = {
                Type = "oneshot";
                ExecStart = "${fw}";
                RemainAfterExit = true;
            };
            wantedBy = [ "multi-user.target" ];
        };
    });
}
