// app/launcher/game_library.cpp -- the chosen discs and their facts.
#include "game_library.h"
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonObject>
#include "app_data.h"
namespace ps2r::launcher {

GameLibrary::GameLibrary(AppData* appData, QObject* parent) : QObject(parent), appData_(appData) { load(); }

void GameLibrary::load() {
    games_.clear();
    removed_.clear();
    for (const QJsonValue& v : appData_->value("removed_discs").toArray()) removed_ << v.toString();
    for (const QJsonValue& v : appData_->value("games").toArray()) {
        const QJsonObject o = v.toObject();
        GameEntry e;
        e.disc = DiscScanner::fromJson(o.value("identity").toObject());
        e.disc.path = o.value("path").toString();
        e.lastPlayed = QDateTime::fromString(o.value("last_played").toString(), Qt::ISODate);
        e.playtimeSeconds = qint64(o.value("playtime_seconds").toDouble());
        if (!e.disc.path.isEmpty()) games_ << e;
    }
    refreshMissing();
}

void GameLibrary::save() {
    QJsonArray arr;
    for (const GameEntry& e : games_) {
        QJsonObject o;
        o["path"] = e.disc.path;
        o["identity"] = DiscScanner::toJson(e.disc);
        if (e.lastPlayed.isValid()) o["last_played"] = e.lastPlayed.toString(Qt::ISODate);
        o["playtime_seconds"] = double(e.playtimeSeconds);
        arr << o;
    }
    appData_->setValue("games", arr);
    appData_->setValue("removed_discs", QJsonArray::fromStringList(removed_));
    appData_->save();
}

int GameLibrary::indexOf(const QString& path) const {
    for (int i = 0; i < games_.size(); ++i) if (games_[i].disc.path == path) return i;
    return -1;
}

const GameEntry* GameLibrary::game(const QString& path) const {
    const int i = indexOf(path);
    return i < 0 ? nullptr : &games_[i];
}

bool GameLibrary::add(const DiscFound& disc) {
    if (disc.fromSearch && removed_.contains(disc.path)) return false;   // the user took it off the list
    removed_.removeAll(disc.path);
    const int i = indexOf(disc.path);
    if (i >= 0) {
        games_[i].disc = disc;
        games_[i].missing = false;
        save();
        emit changed();
        return false;
    }
    GameEntry e;
    e.disc = disc;
    games_ << e;
    save();
    emit changed();
    return true;
}

bool GameLibrary::relocate(const QString& oldPath, const DiscFound& disc) {
    const int i = indexOf(oldPath);
    if (i < 0) return add(disc);
    if (indexOf(disc.path) >= 0 && disc.path != oldPath) { games_.remove(i); save(); emit changed(); return false; }
    games_[i].disc = disc;
    games_[i].missing = false;
    save();
    emit changed();
    return true;
}

bool GameLibrary::updateIdentity(const DiscFound& disc) {
    const int i = indexOf(disc.path);
    if (i < 0) return false;
    DiscFound next = disc;
    next.fromSearch = games_[i].disc.fromSearch;
    games_[i].disc = next;
    games_[i].missing = false;
    save();
    emit changed();
    return true;
}

QStringList GameLibrary::staleIdentities() const {
    QStringList out;
    for (const GameEntry& e : games_)
        if (!e.missing && (e.disc.verdict.isEmpty() || !e.disc.checked)) out << e.disc.path;
    return out;
}

void GameLibrary::remove(const QString& path, bool byUser) {
    const int i = indexOf(path);
    if (i < 0) return;
    games_.remove(i);
    if (byUser && !removed_.contains(path)) removed_ << path;
    save();
    emit changed();
}

void GameLibrary::recordPlayStart(const QString& path) {
    const int i = indexOf(path);
    if (i < 0) return;
    games_[i].lastPlayed = QDateTime::currentDateTime();
    save();
    emit changed();
}

void GameLibrary::recordPlayEnd(const QString& path, qint64 seconds) {
    const int i = indexOf(path);
    if (i < 0) return;
    games_[i].playtimeSeconds += qMax<qint64>(0, seconds);
    save();
    emit changed();
}

void GameLibrary::refreshMissing() {
    for (GameEntry& e : games_) e.missing = !QFileInfo::exists(e.disc.path);
}

QString GameLibrary::relativeTime(const QDateTime& when, const QDateTime& now) {
    if (!when.isValid()) return QStringLiteral("Never played");
    const qint64 secs = when.secsTo(now);
    if (secs < 60) return QStringLiteral("Last played just now");
    if (secs < 3600) return QStringLiteral("Last played %1 min ago").arg(secs / 60);
    if (when.date() == now.date()) return QStringLiteral("Last played today %1").arg(when.toString("HH:mm"));
    if (when.date().daysTo(now.date()) == 1) return QStringLiteral("Last played yesterday %1").arg(when.toString("HH:mm"));
    if (when.date().daysTo(now.date()) < 7) return QStringLiteral("Last played %1").arg(when.toString("dddd HH:mm"));
    return QStringLiteral("Last played %1").arg(when.toString("yyyy-MM-dd"));
}

QString GameLibrary::playtimeText(qint64 seconds) {
    if (seconds < 60) return QStringLiteral("under a minute");
    const qint64 h = seconds / 3600, m = (seconds % 3600) / 60;
    if (h == 0) return QStringLiteral("%1 min").arg(m);
    return QStringLiteral("%1 h %2 min").arg(h).arg(m);
}

}  // namespace ps2r::launcher
