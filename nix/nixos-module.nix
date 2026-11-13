{
  config,
  lib,
  pkgs,
  ...
}:
let
  cfg = config.hardware.sc0710;
  minimumKernel = "6.12";
  kernelVersion = lib.versions.majorMinor config.boot.kernelPackages.kernel.version;
in
{
  imports = [
    (lib.mkRemovedOptionModule [
      "hardware"
      "sc0710"
      "kernel"
    ] "Set boot.kernelPackages instead; the driver is built against it automatically.")
  ];

  options.hardware.sc0710 = {
    enable = lib.mkEnableOption "the sc0710 kernel module";

    package = lib.mkOption {
      type = lib.types.package;
      default = config.boot.kernelPackages.callPackage ./packages/driver.nix { };
      defaultText = lib.literalExpression "config.boot.kernelPackages.callPackage ./packages/driver.nix { }";
      description = "The sc0710 driver package, built against the configured kernel.";
    };

    cli = {
      enable = lib.mkEnableOption "the sc0710-cli helper" // {
        default = true;
      };

      package = lib.mkOption {
        type = lib.types.package;
        default = pkgs.callPackage ./packages/cli.nix { };
        defaultText = lib.literalExpression "pkgs.callPackage ./packages/cli.nix { }";
        description = "The sc0710-cli package.";
      };
    };
  };

  config = lib.mkIf cfg.enable {
    assertions = [
      {
        assertion = lib.versionAtLeast kernelVersion minimumKernel;
        message = "sc0710 requires kernel >= ${minimumKernel}, found ${kernelVersion}.";
      }
    ];

    boot.extraModulePackages = [ cfg.package ];
    boot.kernelModules = [ "sc0710" ];
    environment.systemPackages = lib.optional cfg.cli.enable cfg.cli.package;
  };
}
