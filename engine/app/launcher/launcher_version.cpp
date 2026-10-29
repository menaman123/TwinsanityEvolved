// app/launcher/launcher_version.cpp -- see launcher_version.h (the only file compiled with PS2R_LAUNCHER_VERSION).
#include "launcher_version.h"
namespace ps2r::launcher {

QString launcherVersion() {
#ifdef PS2R_LAUNCHER_VERSION
    return QStringLiteral(PS2R_LAUNCHER_VERSION);
#else
    return QStringLiteral("0.2");
#endif
}

}  // namespace ps2r::launcher
