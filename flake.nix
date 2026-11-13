{
  inputs = {
    nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
  };

  outputs =
    { nixpkgs, ... }:
    let
      systems = nixpkgs.lib.systems.flakeExposed;

      eachSystem = f: nixpkgs.lib.genAttrs systems (system: f system nixpkgs.legacyPackages.${system});
    in
    {
      nixosModules.default = ./nix/nixos-module.nix;
      packages = eachSystem (
        _: pkgs: {
          cli = pkgs.callPackage ./nix/packages/cli.nix { };
          driver = pkgs.linuxPackages.callPackage ./nix/packages/driver.nix { };
        }
      );
    };
}
