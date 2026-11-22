{
  description = "OBS Studio filter for echo cancellation and noise suppression through macOS's voice processing";

  # nixpkgs' obs-studio builds on aarch64-darwin there, with libobs to build
  # plugins against (NixOS/nixpkgs#591713).
  inputs.nixpkgs.url = "github:moeleak/nixpkgs/nixos-unstable";

  outputs =
    { self, nixpkgs }:
    let
      forAllSystems =
        f: nixpkgs.lib.genAttrs [ "aarch64-darwin" ] (system: f nixpkgs.legacyPackages.${system});
    in
    {
      packages = forAllSystems (pkgs: {
        default = self.packages.${pkgs.stdenv.hostPlatform.system}.obs-mac-voice-processing;
        obs-mac-voice-processing = pkgs.callPackage ./package.nix { };
        # OBS with the plugin, as Home Manager's programs.obs-studio wraps it.
        obs = pkgs.wrapOBS {
          plugins = [ self.packages.${pkgs.stdenv.hostPlatform.system}.obs-mac-voice-processing ];
        };
      });

      overlays.default = final: prev: {
        obs-studio-plugins = prev.obs-studio-plugins // {
          obs-mac-voice-processing = final.callPackage ./package.nix { };
        };
      };

      formatter = forAllSystems (pkgs: pkgs.nixfmt);
    };
}
