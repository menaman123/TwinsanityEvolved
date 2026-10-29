// app/launcher/launcher_version.h -- the version this launcher was made as: PS2R_LAUNCHER_VERSION, the tree's
// `git describe --tags --always` at packaging time (tools/ci/release_launcher*.sh / .ps1 pass it; a tagged release is
// exactly its tag), "0.2" for a plain developer configure. One source file reads the definition, so a new version
// rebuilds one object (the self-update test makes two launchers from one build directory). The self-update compares a
// release with this by the release list's order and dates, never as text.
#pragma once
#include <QString>
namespace ps2r::launcher {

QString launcherVersion();

}  // namespace ps2r::launcher
