#include "targetdialog.h"

#include <QtWidgets>

QLineEdit *TargetDialog::field(const QString &label, QFormLayout *form)
{
    auto *e = new QLineEdit(this);
    form->addRow(label, e);
    return e;
}

TargetDialog::TargetDialog(const QVector<TargetProfile> &targets, QWidget *parent)
    : QDialog(parent), m_targets(targets)
{
    setWindowTitle(tr("Manage targets"));
    resize(820, 460);

    m_list = new QListWidget(this);
    m_list->setMaximumWidth(200);
    connect(m_list, &QListWidget::currentRowChanged, this, &TargetDialog::selectRow);

    auto *add = new QPushButton(tr("&Add"), this);
    auto *rem = new QPushButton(tr("&Remove"), this);
    connect(add, &QPushButton::clicked, this, &TargetDialog::addTarget);
    connect(rem, &QPushButton::clicked, this, &TargetDialog::removeTarget);
    auto *listSide = new QVBoxLayout;
    listSide->addWidget(m_list);
    auto *listBtns = new QHBoxLayout;
    listBtns->addWidget(add);
    listBtns->addWidget(rem);
    listSide->addLayout(listBtns);

    auto *form = new QFormLayout;
    m_name      = field(tr("Name"), form);
    m_desc      = field(tr("Description"), form);
    m_build     = field(tr("Build command"), form);
    m_flash     = field(tr("Flash command"), form);
    m_run       = field(tr("Run command"), form);
    m_monitor   = field(tr("Monitor command"), form);
    m_dbgServer = field(tr("Debug server command"), form);
    m_gdb       = field(tr("GDB command"), form);
    m_gdbProg   = field(tr("GDB program (.elf)"), form);
    m_gdbRemote = field(tr("GDB remote"), form);
    m_initBp    = field(tr("Init breakpoint"), form);
    m_smp       = new QCheckBox(tr("Dual-core debug (SMP / ESP_RTOS=hwthread)"), this);
    form->addRow(QString(), m_smp);

    auto *hint = new QLabel(
        tr("Tokens: {root} opened folder, {repo} launcher/tools root above it, "
           "{example} examples/<name>, {file} source, {dir}, {base}, {exe}. "
           "Commands run at the opened folder."), this);
    hint->setWordWrap(true);
    hint->setStyleSheet("color: gray;");

    auto *formSide = new QVBoxLayout;
    formSide->addLayout(form);
    formSide->addWidget(hint);
    formSide->addStretch();

    auto *top = new QHBoxLayout;
    top->addLayout(listSide);
    top->addLayout(formSide, 1);

    auto *buttons = new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this);
    connect(buttons, &QDialogButtonBox::accepted, this, [this, buttons] {
        commitForm();
        QDialog::accept();
        Q_UNUSED(buttons);
    });
    connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);

    auto *root = new QVBoxLayout(this);
    root->addLayout(top, 1);
    root->addWidget(buttons);

    for (const auto &t : m_targets)
        m_list->addItem(t.name);
    if (!m_targets.isEmpty())
        m_list->setCurrentRow(0);
    else
        loadForm(-1);
}

void TargetDialog::selectRow(int row)
{
    if (m_current >= 0)
        commitForm();
    m_current = row;
    loadForm(row);
}

void TargetDialog::loadForm(int row)
{
    const bool ok = row >= 0 && row < m_targets.size();
    for (QLineEdit *e : {m_name, m_desc, m_build, m_flash, m_run, m_monitor,
                         m_dbgServer, m_gdb, m_gdbProg, m_gdbRemote, m_initBp})
        e->setEnabled(ok);
    m_smp->setEnabled(ok);
    if (!ok) {
        for (QLineEdit *e : {m_name, m_desc, m_build, m_flash, m_run, m_monitor,
                             m_dbgServer, m_gdb, m_gdbProg, m_gdbRemote, m_initBp})
            e->clear();
        m_smp->setChecked(false);
        return;
    }
    const TargetProfile &t = m_targets[row];
    m_name->setText(t.name);
    m_desc->setText(t.description);
    m_build->setText(t.buildCommand);
    m_flash->setText(t.flashCommand);
    m_run->setText(t.runCommand);
    m_monitor->setText(t.monitorCommand);
    m_dbgServer->setText(t.debugServerCommand);
    m_gdb->setText(t.gdbCommand);
    m_gdbProg->setText(t.gdbProgram);
    m_gdbRemote->setText(t.gdbRemote);
    m_initBp->setText(t.initBreakpoint);
    m_smp->setChecked(t.debugSmp);
}

void TargetDialog::commitForm()
{
    if (m_current < 0 || m_current >= m_targets.size()) return;
    TargetProfile &t = m_targets[m_current];
    t.name               = m_name->text();
    t.description        = m_desc->text();
    t.buildCommand       = m_build->text();
    t.flashCommand       = m_flash->text();
    t.runCommand         = m_run->text();
    t.monitorCommand     = m_monitor->text();
    t.debugServerCommand = m_dbgServer->text();
    t.gdbCommand         = m_gdb->text();
    t.gdbProgram         = m_gdbProg->text();
    t.gdbRemote          = m_gdbRemote->text();
    t.initBreakpoint     = m_initBp->text();
    t.debugSmp           = m_smp->isChecked();
    if (auto *item = m_list->item(m_current))
        item->setText(t.name.isEmpty() ? tr("(unnamed)") : t.name);
}

void TargetDialog::addTarget()
{
    if (m_current >= 0) commitForm();
    TargetProfile t;
    t.name = tr("New target");
    t.buildCommand = "./x build";
    t.gdbRemote = "localhost:3333";
    t.initBreakpoint = "app_main";
    m_targets.append(t);
    m_list->addItem(t.name);
    m_list->setCurrentRow(m_targets.size() - 1);
}

void TargetDialog::removeTarget()
{
    const int row = m_list->currentRow();
    if (row < 0 || row >= m_targets.size()) return;
    m_current = -1;                       // avoid committing into a stale row
    m_targets.remove(row);
    delete m_list->takeItem(row);
    if (m_targets.isEmpty()) loadForm(-1);
}
