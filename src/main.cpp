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
    for (int i = 1; i < argc; ++i) {
        const QString arg = QString::fromLocal8Bit(argv[i]);
        if (QFileInfo(arg).isDir())
            w.openFolderPath(arg);
        else
            w.openPath(arg);
    }
    // With no path arguments, reopen the last folder from the previous session.
    if (argc <= 1)
        w.restoreLastFolder();
    w.show();
    return app.exec();
}
