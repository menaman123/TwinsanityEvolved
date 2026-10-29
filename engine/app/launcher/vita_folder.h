// app/launcher/vita_folder.h -- the PS Vita folder: what the launcher writes so that a player copies ONE folder to
// the console. The PS Vita install's folder (<game folder>/vita by default) holds:
//   twinsanityevolved.vpk          the package (the install places it: ReleaseInstaller)
//   README-vita.txt                the steps (the manifest's targets.vita.note, the disc's name filled in)
//   ux0/data/twinsanity/           the console's data folder as it sits on the memory card (targets.vita.folder.data_dir):
//       ps2r.ini                   the game folder's settings without the computer's own keys (settings_left_out: its
//                                  folders, its picture size and window, its game rate -- the console has its own)
//       mods/<id>/                 a COPY of the game folder's language packs and of the mods switched on, nothing else
//       disc-image.txt             the disc the console starts with: the file name of the disc the game folder starts with
//                                  when it is a plain name a memory card holds, else game.iso (default_disc)
// The disc image itself is never copied (the player copies their own) and neither are the saves (the console keeps its
// own memory card). Made after the PS Vita install and again on demand (Settings > PS Vita): a file already in the copy
// with the same size and time is kept, a mod no longer copied (switched off, removed) leaves the copy. Plain files.
#pragma once
#include <QString>
#include <QStringList>
namespace ps2r::launcher {
class TitleManifest;

struct VitaFolderReport {
    QString folder;        // the PS Vita folder
    QString dataDir;       // <folder>/<data_dir> ("" when the manifest names no data folder: the package and the README only)
    QString disc;          // the disc's file name on the console (what disc-image.txt names)
    QString discFrom;      // the computer's disc that name was taken from ("" = none known: the default name)
    QStringList mods;      // the folders copied (language packs and the mods switched on), in the overlay order
    QStringList names;     // ... their names (mod.json's), as the screens say them
    qint64 modBytes = 0;   // their size on the memory card
    int copied = 0;        // files written this time
    int kept = 0;          // files already there (same size and time)
    int removed = 0;       // files and folders taken out of the copy (a mod switched off or removed)
    QString error;         // "" = made; else one sentence
};

class VitaFolder {
public:
    // Makes (or brings up to date) the PS Vita folder `folder` from the game folder `gameFolder` (its ps2r.ini, mods folder
    // and disc-image.txt; "" or a folder without them: the console's defaults, no mods, game.iso). `fallbackDisc`: the disc
    // to name when the game folder names none (the first disc on the Games page). False with report->error set.
    static bool make(const TitleManifest* manifest, const QString& gameFolder, const QString& folder, const QString& fallbackDisc,
                     VitaFolderReport* report);
    // The PS Vita folder inside a game folder (<gameFolder>/<targets.vita.folder.name>)
    static QString folderIn(const TitleManifest* manifest, const QString& gameFolder);
    // The game folder a PS Vita folder was made in (its parent when it carries the folder name), else ""
    static QString homeOf(const TitleManifest* manifest, const QString& folder);
    // The disc's name on the console: the computer's disc file name when a memory card holds it as it is (printable ASCII,
    // none of \ / : * ? " < > |, a disc extension), else the default name
    static QString consoleDiscName(const TitleManifest* manifest, const QString& disc);
    // The game folder's ps2r.ini text without the keys a copy leaves out ("" when there is no file)
    static QString settingsText(const TitleManifest* manifest, const QString& settingsFile);
    // The steps (the manifest's note, "{disc}" -> the disc's name, "{data_dir}" -> the data folder's console path)
    static QStringList steps(const TitleManifest* manifest, const QString& disc);
    // The manifest's PS Vita sentences with their names filled in ({vpk} {data} {data_dir} {disc} {disc_file}): the discs on
    // the console (the North American disc beside the European one, disc-image.txt choosing), the Mods page's panel and
    // About's section
    static QString fill(const TitleManifest* manifest, QString line, const QString& disc);
    static QStringList discsNote(const TitleManifest* manifest, const QString& disc);
    static QString modsNote(const TitleManifest* manifest, const QString& disc);
    static QStringList about(const TitleManifest* manifest, const QString& disc);
    static QString readme(const TitleManifest* manifest, const VitaFolderReport& report);
    // One line for the screens: where it is, the disc's name, the mods and their size
    static QString summary(const VitaFolderReport& report);
    // The data folder as the console names it: ux0/data/twinsanity -> ux0:data/twinsanity
    static QString consolePath(const QString& dataDir);
};

}  // namespace ps2r::launcher
