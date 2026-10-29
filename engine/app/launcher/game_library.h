// app/launcher/game_library.h -- the disc images the user chose (each a game card): the identity the scanner gave
// (region, retail / modded, the program), last played and playtime. Persisted in launcher.json through AppData.
#pragma once
#include <QDateTime>
#include <QObject>
#include <QStringList>
#include <QVector>
#include "disc_scanner.h"
namespace ps2r::launcher {
class AppData;

struct GameEntry {
    DiscFound disc;
    QDateTime lastPlayed;
    qint64 playtimeSeconds = 0;
    bool missing = false;   // the file is gone since it was chosen
};

class GameLibrary : public QObject {
    Q_OBJECT
public:
    explicit GameLibrary(AppData* appData, QObject* parent = nullptr);

    const QVector<GameEntry>& games() const { return games_; }
    int indexOf(const QString& path) const;
    const GameEntry* game(const QString& path) const;
    // Adds or refreshes an entry; returns true when it is new. A disc the user removed from the list is not added back
    // by a search (disc.fromSearch); choosing or dropping it again does.
    bool add(const DiscFound& disc);
    void remove(const QString& path, bool byUser = true);   // byUser: a search does not bring it back
    bool removedByUser(const QString& path) const { return removed_.contains(path); }
    // The disc moved: the entry follows it (Locate…), keeping its facts.
    bool relocate(const QString& oldPath, const DiscFound& disc);
    // A newer identity of a disc on the list (the whole-image check finished): false when the disc is not on the list.
    bool updateIdentity(const DiscFound& disc);
    // The discs on the list whose identity an older launcher wrote (no whole-image verdict): identified again at start.
    QStringList staleIdentities() const;
    void recordPlayStart(const QString& path);
    void recordPlayEnd(const QString& path, qint64 seconds);
    void refreshMissing();
    void save();

    static QString relativeTime(const QDateTime& when, const QDateTime& now = QDateTime::currentDateTime());
    static QString playtimeText(qint64 seconds);

signals:
    void changed();

private:
    void load();
    AppData* appData_;
    QVector<GameEntry> games_;
    QStringList removed_;
};

}  // namespace ps2r::launcher
