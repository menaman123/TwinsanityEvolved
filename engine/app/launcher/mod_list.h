// app/launcher/mod_list.h -- the mods folder as the Mods page shows it: one entry per
// <mods>/<id>/mod.json (schema 1 or 2, the stage's mod folder format), checked the way the game checks it when it scans the
// folder (the folder named by its id, lower-case letters, digits and _; the manifest's id, schema and kind), in the overlay
// order (mods.order first, then the rest by id -- the game's rule), with the lines a row shows: the region badge, the
// summary of what the mod changes, the line for a mod the disc the game starts with leaves out, and the code-changes
// warning. Pure reading; nothing here writes. The launcher is where a folder mod is switched: the game's Options > MODS page
// lists only the mods that come with it.
#pragma once
#include <QPair>
#include <QString>
#include <QStringList>
#include <QVector>
namespace ps2r::launcher {

struct ModEntry {
    QString id;                 // the folder's name
    QString dir;                // <mods>/<id>
    QString name, author, version, description;
    QString kind;               // "disc" | "language" | "builtin" (a schema 1 manifest without a kind)
    int schema = 1;
    QString region;             // base.region: "PAL" | "NTSC-U" | "NTSC-J" | "any" | ""
    QString executable;         // base.executable
    QString sourceKind;         // source.kind
    QStringList files;          // files[].path
    // A recipe of files[] records the file it was made over (form "ranges" with base_bytes: what the importer writes): the
    // game composes the mod only over that very file -- its base check, platform/nativeee/host/mods_disc.cpp
    bool baseChecked = false;
    QVector<QPair<QString, qint64>> dataChanges;   // executable.data_changes: section -> bytes
    QStringList codeChanges;    // executable.code_changes: "FUN_... name"
    QString languageId;         // language.id (a language pack's)
    QString languageLabel;      // language.label
    QString languageBank;       // language.text_bank (relative to the folder)
    QStringList notes;          // notes: the author's lines (a Mod Crate's '!' lines of ModCrateInfo.txt)
    QStringList gameCodeSettings;   // import.game_code_settings: what the mod asks Crate Mod Loader to apply to the game's code
    bool readable = false;      // the game lists it; otherwise `problem` says why, in a sentence
    QString problem;
};

class ModList {
public:
    // Every sub-folder of `modsDir` holding a mod.json, by id. Hidden folders (the import's .import-tmp) are skipped.
    static QVector<ModEntry> read(const QString& modsDir);
    // The MODS of the folder: read() without the language packs. A language pack (kind "language") is not a mod: the
    // Mods page never lists it -- it is a choice of the Settings page's Text language row (app/host/disc_languages.h)
    static QVector<ModEntry> mods(const QString& modsDir);
    // The language packs of the folder the game would read (kind "language", readable), by id
    static QVector<ModEntry> languagePacks(const QString& modsDir);
    static bool isLanguagePack(const ModEntry& entry) { return entry.kind == QLatin1String("language"); }
    static ModEntry readOne(const QString& dir);
    // The entries in the overlay order: the ids `order` names (mods.order: comma or space separated) first, then the rest
    // by id; entries the game would not read last.
    static QVector<ModEntry> ordered(const QVector<ModEntry>& entries, const QString& order);
    static QStringList parseOrder(const QString& order);
    static QString orderValue(const QStringList& ids);   // "a,b,c"
    // A mod's id as the game takes it: [a-z0-9_]+, at most 64 characters.
    static bool validId(const QString& id);

    // The row's lines
    static QString regionLabel(const QString& region);   // "PAL", "NTSC-U", "NTSC-J", "Any region"; "" for none
    static QString byline(const QString& author, const QString& version);   // "by <author> · <version>"
    // what it changes, one sentence ("" when nothing is known); for a mod made for another region than `discRegion` (the
    // disc the game starts with: "PAL", "NTSC-U", "NTSC-J"; "" = unknown) it says so: "..., made for the North American disc."
    // (whether the game leaves it out there is leftOutLine's)
    static QString summary(const ModEntry& entry, const QString& discRegion = QString());
    // A mod the game leaves out at its start on the disc of `discRegion`, as far as the launcher can tell: made over another
    // region's disc, with recipes that record their base -- the game's base check refuses them over this disc's files:
    // "Left out on this disc: made for the North American disc." ("" otherwise). The game's other refusals (a recipe over
    // another dump or a modified image of the same region) are in the game's log only.
    static QString leftOutLine(const ModEntry& entry, const QString& discRegion);
    static QString codeLine(const ModEntry& entry);       // "Code changes not supported: ..." or ""
    // the warning lines: the left-out line (with `discRegion`), the code changes, and the Crate Mod Loader settings the mod
    // asks for ("" when none)
    static QString warningLines(const ModEntry& entry, const QString& discRegion = QString());
    static QString notesText(const ModEntry& entry);      // the author's notes, one a line ("" when none)
    // "the European disc", "the North American disc", "the Japanese disc"; "" for "any" or an unknown region
    static QString regionDisc(const QString& region);
};

}  // namespace ps2r::launcher
