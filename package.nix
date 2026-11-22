{
  lib,
  stdenv,
  cmake,
  obs-studio,
}:

stdenv.mkDerivation {
  pname = "obs-mac-voice-processing";
  version = "1.0.0";

  src = lib.fileset.toSource {
    root = ./.;
    fileset = lib.fileset.unions [
      ./CMakeLists.txt
      ./cmake
      ./data
      ./src
    ];
  };

  nativeBuildInputs = [ cmake ];

  buildInputs = [ obs-studio ];

  cmakeFlags = [
    "-Dlibobs_DIR=${obs-studio}/Frameworks/libobs.framework/Resources/cmake"
  ];

  meta = {
    description = "OBS Studio filter for echo cancellation and noise suppression through macOS's voice processing";
    homepage = "https://github.com/moeleak/obs-mac-voice-processing";
    license = lib.licenses.gpl2Plus;
    platforms = lib.platforms.darwin;
  };
}
