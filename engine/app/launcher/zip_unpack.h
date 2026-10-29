// app/launcher/zip_unpack.h -- a release zip unpacked in-process with Qt's zip reader (QZipReader, Qt's private core
// module): every entry must stay inside the folder it is unpacked into (no absolute path, no drive letter, no ".."),
// directories are made, symbolic links are made as links (a macOS bundle's frameworks are links: Versions/Current ->
// A; its code signature seals them), files keep their permissions (the owner can always read and write them). The
// game's install (ReleaseInstaller) and the launcher's own update (SelfUpdate) use it.
#pragma once
#include <QString>
namespace ps2r::launcher {

// true when `path` (a zip entry's name, '/' separated) stays inside the folder it is unpacked into
bool zipEntryIsSafe(const QString& path);
// Unpacks `zipPath` into `into` (made when missing). False with *error on the first problem; what was written stays.
bool unpackZip(const QString& zipPath, const QString& into, QString* error);

}  // namespace ps2r::launcher
