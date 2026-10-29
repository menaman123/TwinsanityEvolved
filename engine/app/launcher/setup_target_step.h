// app/launcher/setup_target_step.h -- step 3 of the setup, "Where will you play?": This computer (chosen), PS Vita (the
// three-line hand-off note below), or Another computer (which one: Windows, macOS, Linux).
#pragma once
#include <QWidget>
class QLabel;
namespace ps2r::launcher {
class ChoiceCard;
class LauncherApp;
class SegmentedControl;

class SetupTargetStep : public QWidget {
    Q_OBJECT
public:
    SetupTargetStep(LauncherApp* app, QWidget* parent = nullptr);
    // the chosen target: this computer's id, "vita", or another computer's id
    QString target() const;
    void setTarget(const QString& target);
    ChoiceCard* thisComputer() const { return here_; }
    ChoiceCard* vita() const { return vita_; }
    ChoiceCard* another() const { return other_; }
    SegmentedControl* otherPlatforms() const { return platforms_; }
    QString vitaNote() const;

signals:
    void targetChanged(const QString& target);

private:
    void pickCard(ChoiceCard* card);
    LauncherApp* app_;
    ChoiceCard* here_;
    ChoiceCard* vita_;
    ChoiceCard* other_;
    SegmentedControl* platforms_;
    QLabel* note_;
};

}  // namespace ps2r::launcher
