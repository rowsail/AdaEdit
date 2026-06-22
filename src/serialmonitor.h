#pragma once

#include <QWidget>

class QSerialPort;
class QPlainTextEdit;
class QLineEdit;
class QComboBox;
class QToolButton;

// A self-contained serial console backed by Qt's QSerialPort -- no external
// miniterm/picocom/screen, no python, works inside the GUI (the old `./x monitor`
// needed an interactive TTY, which a piped QProcess never provides).
class SerialMonitor : public QWidget
{
    Q_OBJECT
public:
    explicit SerialMonitor(QWidget *parent = nullptr);

    void    setPortName(const QString &name);   // e.g. /dev/ttyACM0
    QString portName() const;
    bool    isOpen() const;

    void open();          // open the named port at the selected baud
    void close();         // close it
    void toggle();        // open if closed, close if open
    void clearOutput();

signals:
    void status(const QString &msg);            // surfaced on the status bar

private:
    void append(const QByteArray &bytes);
    void send();
    void refreshConnectButton();

    QSerialPort    *m_port       = nullptr;
    QPlainTextEdit *m_view       = nullptr;
    QLineEdit      *m_portEdit   = nullptr;
    QComboBox      *m_baudCombo  = nullptr;
    QToolButton    *m_connectBtn = nullptr;
    QLineEdit      *m_sendEdit   = nullptr;
    QComboBox      *m_eolCombo   = nullptr;
};
