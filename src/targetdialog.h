#pragma once
#include <QDialog>
#include "project.h"

class QListWidget;
class QLineEdit;
class QCheckBox;
class QFormLayout;

// Master-detail editor for the project's target profiles: a list of targets on
// the left, a form of all command/debug fields on the right.
class TargetDialog : public QDialog
{
    Q_OBJECT
public:
    explicit TargetDialog(const QVector<TargetProfile> &targets, QWidget *parent = nullptr);
    QVector<TargetProfile> result() const { return m_targets; }

private slots:
    void selectRow(int row);
    void addTarget();
    void removeTarget();

private:
    void commitForm();           // form -> m_targets[m_current]
    void loadForm(int row);      // m_targets[row] -> form
    QLineEdit *field(const QString &label, QFormLayout *form);

    QListWidget *m_list = nullptr;
    QVector<TargetProfile> m_targets;
    int m_current = -1;

    QLineEdit *m_name = nullptr;
    QLineEdit *m_desc = nullptr;
    QLineEdit *m_build = nullptr;
    QLineEdit *m_flash = nullptr;
    QLineEdit *m_run = nullptr;
    QLineEdit *m_monitor = nullptr;
    QLineEdit *m_dbgServer = nullptr;
    QLineEdit *m_gdb = nullptr;
    QLineEdit *m_gdbProg = nullptr;
    QLineEdit *m_gdbRemote = nullptr;
    QLineEdit *m_initBp = nullptr;
    QCheckBox *m_smp = nullptr;
};
