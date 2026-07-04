#include <QApplication>
#include <QDir>
#include "loginwindow.h"

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);

    QString appDir = QCoreApplication::applicationDirPath();
    QDir::setCurrent(appDir);

    LoginWindow loginWindow;
    loginWindow.showMaximized();

    return app.exec();
}
