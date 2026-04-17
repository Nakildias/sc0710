{
    inputs = {
        nixpkgs.url = "github:nixos/nixpkgs/nixos-unstable";
    };

    outputs = { self, nixpkgs, ... }:
    {
        nixosModules.default = ./default.nix;
    };
}
