#include "mainwindow.h"
#include <QApplication>
#include <QFileInfo>
#include <QIcon>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("AdaEdit");
    QApplication::setApplicationName("AdaEdit");
    QApplication::setApplicationVersion("0.1.0");

    // Window icon (title bar + the running window's taskbar entry / Alt-Tab), with
    // every size so each context renders crisply.  setDesktopFileName ties the
    // window to adaedit.desktop so Wayland taskbars use the same icon/grouping.
    QIcon icon;
    for (int s : {16, 22, 24, 32, 36, 48, 64, 128, 256, 512})
        icon.addFile(QStringLiteral(":/icons/adaedit-%1.png").arg(s));
    QApplication::setWindowIcon(icon);
    // On X11 the title bar uses the window's _NET_WM_ICON (set above); setting a
    // desktop-file name makes KWin's decoration switch to the THEMED icon instead
    // (and a missing decoration size = blank title bar).  The taskbar maps the
    // window via WM_CLASS -> the .desktop's StartupWMClass without it.  Wayland has
    // no _NET_WM_ICON, so there it's required for any icon at all.
    if (QGuiApplication::platformName().contains(QLatin1String("wayland")))
        QGuiApplication::setDesktopFileName(QStringLiteral("adaedit"));

    MainWindow w;
    // A directory argument opens the project folder; files open in tabs.
    bool opened = false;
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (QFileInfo(arg).isDir())
            w.openFolderPath(arg);
        else
            w.openPath(arg);
        opened = true;
    }
    // With no path arguments, restore the previous session (folder + open files
    // + active tab); window size and dock layout are restored in the ctor.
    if (!opened)
        w.restoreSession();
    // No forced "untitled" tab -- an empty editor area is allowed (use File ▸ New).
    w.show();
    return app.exec();
}
