#include "serialmonitor.h"

#include <QtSerialPort/QSerialPort>
#include <QPlainTextEdit>
#include <QLineEdit>
#include <QComboBox>
#include <QToolButton>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QLabel>
#include <QScrollBar>
#include <QSettings>
#include <QTextCursor>

SerialMonitor::SerialMonitor(QWidget *parent) : QWidget(parent)
{
    m_port = new QSerialPort(this);

    // --- controls row: port, baud, connect ---
    m_portEdit = new QLineEdit(this);
    m_portEdit->setPlaceholderText(QStringLiteral("/dev/ttyACM0"));
    m_portEdit->setText(QSettings().value("serial/port", "/dev/ttyACM0").toString());
    m_portEdit->setMaximumWidth(170);

    m_baudCombo = new QComboBox(this);
    for (int b : {9600, 74880, 115200, 230400, 460800, 921600})
        m_baudCombo->addItem(QString::number(b), b);
    m_baudCombo->setCurrentText(QSettings().value("serial/baud", "115200").toString());

    m_connectBtn = new QToolButton(this);
    m_connectBtn->setText(tr("Connect"));
    connect(m_connectBtn, &QToolButton::clicked, this, &SerialMonitor::toggle);

    auto *clearBtn = new QToolButton(this);
    clearBtn->setText(tr("Clear"));
    connect(clearBtn, &QToolButton::clicked, this, &SerialMonitor::clearOutput);

    auto *top = new QHBoxLayout;
    top->setContentsMargins(2, 2, 2, 2);
    top->addWidget(new QLabel(tr("Port:"), this));
    top->addWidget(m_portEdit);
    top->addWidget(new QLabel(tr("Baud:"), this));
    top->addWidget(m_baudCombo);
    top->addWidget(m_connectBtn);
    top->addWidget(clearBtn);
    top->addStretch(1);

    // --- output ---
    m_view = new QPlainTextEdit(this);
    m_view->setReadOnly(true);
    m_view->setMaximumBlockCount(10000);
    QFont mono("monospace");
    mono.setStyleHint(QFont::TypeWriter);
    m_view->setFont(mono);

    // --- send row ---
    m_sendEdit = new QLineEdit(this);
    m_sendEdit->setPlaceholderText(tr("type to send to the device, Enter to send…"));
    m_sendEdit->setEnabled(false);
    connect(m_sendEdit, &QLineEdit::returnPressed, this, &SerialMonitor::send);

    m_eolCombo = new QComboBox(this);
    m_eolCombo->addItem(tr("CR+LF"), "\r\n");
    m_eolCombo->addItem(tr("LF"),    "\n");
    m_eolCombo->addItem(tr("CR"),    "\r");
    m_eolCombo->addItem(tr("None"),  "");

    auto *bottom = new QHBoxLayout;
    bottom->setContentsMargins(2, 2, 2, 2);
    bottom->addWidget(m_sendEdit, 1);
    bottom->addWidget(new QLabel(tr("EOL:"), this));
    bottom->addWidget(m_eolCombo);

    auto *lay = new QVBoxLayout(this);
    lay->setContentsMargins(0, 0, 0, 0);
    lay->addLayout(top);
    lay->addWidget(m_view, 1);
    lay->addLayout(bottom);

    connect(m_port, &QSerialPort::readyRead, this, [this] { append(m_port->readAll()); });
    connect(m_port, &QSerialPort::errorOccurred, this, [this](QSerialPort::SerialPortError e) {
        if (e == QSerialPort::NoError) return;
        // A device unplugged / read error: report and close so the UI is consistent.
        if (e == QSerialPort::ResourceError || e == QSerialPort::PermissionError ||
            e == QSerialPort::DeviceNotFoundError) {
            emit status(tr("Serial: %1").arg(m_port->errorString()));
            if (m_port->isOpen()) close();
        }
    });

    refreshConnectButton();
}

void SerialMonitor::setPortName(const QString &name)
{
    if (!name.isEmpty()) m_portEdit->setText(name);
}

QString SerialMonitor::portName() const { return m_portEdit->text().trimmed(); }
bool SerialMonitor::isOpen() const { return m_port->isOpen(); }

void SerialMonitor::open()
{
    if (m_port->isOpen()) return;
    const QString name = portName();
    if (name.isEmpty()) { emit status(tr("Serial: no port set")); return; }
    const int baud = m_baudCombo->currentData().toInt();

    m_port->setPortName(name);
    m_port->setBaudRate(baud);
    m_port->setDataBits(QSerialPort::Data8);
    m_port->setParity(QSerialPort::NoParity);
    m_port->setStopBits(QSerialPort::OneStop);
    m_port->setFlowControl(QSerialPort::NoFlowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        QString hint = m_port->errorString();
        if (m_port->error() == QSerialPort::PermissionError)
            hint += tr("  (run Build ▸ Set up device access…)");
        emit status(tr("Serial: can't open %1 — %2").arg(name, hint));
        append(("\n--- can't open " + name + ": " + hint + " ---\n").toUtf8());
        return;
    }
    m_port->setDataTerminalReady(true);   // some boards gate output on DTR
    QSettings().setValue("serial/port", name);
    QSettings().setValue("serial/baud", QString::number(baud));
    append(("--- connected " + name + " @ " + QString::number(baud) + " ---\n").toUtf8());
    emit status(tr("Serial: connected %1 @ %2").arg(name).arg(baud));
    refreshConnectButton();
}

void SerialMonitor::close()
{
    if (m_port->isOpen()) {
        m_port->close();
        append("--- disconnected ---\n");
    }
    refreshConnectButton();
    emit status(tr("Serial: disconnected"));
}

void SerialMonitor::toggle() { m_port->isOpen() ? close() : open(); }

void SerialMonitor::clearOutput() { m_view->clear(); }

void SerialMonitor::send()
{
    if (!m_port->isOpen()) return;
    const QByteArray eol = m_eolCombo->currentData().toString().toUtf8();
    m_port->write(m_sendEdit->text().toUtf8() + eol);
    m_sendEdit->clear();
}

void SerialMonitor::append(const QByteArray &bytes)
{
    // Show device output verbatim apart from stripping CR (so CRLF doesn't double
    // space).  Keep the view pinned to the bottom unless the user scrolled up.
    QString text = QString::fromUtf8(bytes);
    text.remove(QLatin1Char('\r'));
    auto *bar = m_view->verticalScrollBar();
    const bool atBottom = bar->value() >= bar->maximum() - 4;
    QTextCursor c = m_view->textCursor();
    c.movePosition(QTextCursor::End);
    c.insertText(text);
    if (atBottom) bar->setValue(bar->maximum());
}

void SerialMonitor::refreshConnectButton()
{
    const bool open = m_port->isOpen();
    m_connectBtn->setText(open ? tr("Disconnect") : tr("Connect"));
    m_sendEdit->setEnabled(open);
    m_portEdit->setEnabled(!open);
    m_baudCombo->setEnabled(!open);
}
