{ writeShellScriptBin }:
writeShellScriptBin "sc0710-cli" (builtins.readFile ../../scripts/sc0710-cli.sh)
