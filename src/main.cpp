#include "mainwindow.h"
#include <QApplication>
#include <QFileInfo>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setOrganizationName("AdaEdit");
    QApplication::setApplicationName("AdaEdit");
    QApplication::setApplicationVersion("0.1.0");

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
    w.ensureOpenTab();          // fall back to a blank untitled tab if nothing opened
    w.show();
    return app.exec();
}
