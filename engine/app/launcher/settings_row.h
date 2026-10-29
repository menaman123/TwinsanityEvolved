// app/launcher/settings_row.h -- one settings row: the label, the control (a segmented choice, a popup for more than
// six values, or a switch) and the help line; it reads one key of the game's options file and writes it on change.
#pragma once
#include <QWidget>
#include "title_manifest.h"
class QLabel;
namespace ps2r::launcher {
class FlatButton;
class SegmentedControl;
class SwitchControl;

class SettingsRow : public QWidget {
    Q_OBJECT
public:
    SettingsRow(const SettingsRowSpec& spec, QWidget* parent = nullptr);
    const SettingsRowSpec& spec() const { return spec_; }
    QString value() const;
    void setValue(const QString& value);   // without emitting
    void setHelpOverride(const QString& help);   // a row's state explained (e.g. "off (uncapped)")
    void setLocked(bool locked);
    // A derived row's choices (the disc's languages): the control is rebuilt when they differ; the value is kept when it is
    // still offered (else the first choice is shown). Without emitting.
    void setChoices(const QVector<SettingsChoice>& choices);
    const QVector<SettingsChoice>& choices() const { return spec_.choices; }
    SegmentedControl* segmented() const { return segmented_; }
    SwitchControl* toggle() const { return switch_; }
    FlatButton* popup() const { return popup_; }

signals:
    void valueChanged(const QString& key, const QString& value);

protected:
    void paintEvent(QPaintEvent* event) override;

private:
    void restyle();
    void showPopup();
    QWidget* buildChoiceControl();   // the segmented choice or the popup for spec_.choices
    SettingsRowSpec spec_;
    QLabel* label_;
    QLabel* help_;
    SegmentedControl* segmented_ = nullptr;
    SwitchControl* switch_ = nullptr;
    FlatButton* popup_ = nullptr;
    QString popupValue_;
    QWidget* control_ = nullptr;
    bool locked_ = false;
};

}  // namespace ps2r::launcher
