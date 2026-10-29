// app/launcher/mod_row.h -- one mod on the Mods page: the drag handle (a folder mod),
// its name and region badge, "by <author> · <version>", the summary of what it changes, the code-changes warning, and at
// the right the switch that writes mods.<id>.enabled (none for a mod the game has no switch for); under the lines Move up /
// Move down (the keyboard's way to reorder), Open folder and Remove… for a folder mod. A mod whose description cannot be
// read shows why instead, without a switch.
#pragma once
#include <QPoint>
#include <QWidget>
class QHBoxLayout;
class QLabel;
namespace ps2r::launcher {
class Badge;
class FlatButton;
class SwitchControl;

class ModRow : public QWidget {
    Q_OBJECT
public:
    ModRow(const QString& id, const QString& name, QWidget* parent = nullptr);
    QString id() const { return id_; }
    void setByline(const QString& text);
    void setSummary(const QString& text);
    void setWarning(const QString& text);        // the warning colour lines (left out on this disc, code changes not supported,
                                                 // settings it asks for)
    void setNotes(const QString& text);          // the author's notes (a crate's '!' lines), one a line
    void setRegion(const QString& label);        // the badge; "" = none
    void setSwitchable(bool switchable);         // false: no switch (always available in the game)
    void setFolderMod(bool folderMod);           // the handle, Move up / Move down, Open folder, Remove…
    void setMovable(bool up, bool down);
    void setOn(bool on);   // without emitting
    bool on() const;
    void setBroken(const QString& why);
    bool broken() const { return broken_; }

    SwitchControl* toggle() const { return switch_; }
    FlatButton* upButton() const { return up_; }
    FlatButton* downButton() const { return down_; }
    FlatButton* openButton() const { return open_; }
    FlatButton* removeButton() const { return remove_; }
    QString nameText() const;
    QString bylineText() const;
    QString summaryText() const;
    QString warningText() const;
    QString notesText() const;
    QString regionText() const;
    bool switchable() const { return switchable_; }
    bool folderMod() const { return folderMod_; }
    QRect handleRect() const;

signals:
    void switched(const QString& id, bool on);
    void moveRequested(const QString& id, int delta);
    void openRequested(const QString& id);
    void removeRequested(const QString& id);
    void dragRequested(const QString& id);   // the handle was pressed and moved: the page starts the drag

protected:
    void paintEvent(QPaintEvent* event) override;
    void mousePressEvent(QMouseEvent* event) override;
    void mouseMoveEvent(QMouseEvent* event) override;
    void mouseReleaseEvent(QMouseEvent* event) override;

private:
    void restyle();
    QString id_;
    QLabel* name_;
    Badge* region_;
    QLabel* byline_;
    QLabel* summary_;
    QLabel* warning_;
    QLabel* notes_;
    QWidget* actions_;
    FlatButton* up_;
    FlatButton* down_;
    FlatButton* open_;
    FlatButton* remove_;
    SwitchControl* switch_;
    bool switchable_ = true, folderMod_ = false, broken_ = false, pressed_ = false;
    QPoint pressAt_;
};

}  // namespace ps2r::launcher
