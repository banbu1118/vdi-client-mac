#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QMessageLogContext>
#include <cstdio>
#include "loginwindow.h"

/**
 * 运行时日志策略：只保留 Warning/Error/Fatal，屏蔽全部 QtDebugMsg
 * （qDebug 调试日志不再打印，避免终端刷屏）。
 */
static void qf_message_handler(QtMsgType type, const QMessageLogContext& context,
                               const QString& msg)
{
    (void)context;
    switch (type)
    {
        case QtWarningMsg:
            std::fprintf(stderr, "[qf.warn] %s\n", msg.toUtf8().constData());
            break;
        case QtCriticalMsg:
        case QtFatalMsg:
            std::fprintf(stderr, "[qf.error] %s\n", msg.toUtf8().constData());
            break;
        default:
            /* QtDebugMsg 及以下静默 */
            break;
    }
    std::fflush(stderr);
}

int main(int argc, char *argv[])
{
    qInstallMessageHandler(qf_message_handler);

    QApplication app(argc, argv);

    /* 运行时应用图标（Qt 标准：QApplication::setWindowIcon，资源来自 resources.qrc）。
     * Finder/启动时图标由 .app bundle 的 logo.icns 提供（CMake MACOSX_BUNDLE）。 */
    app.setWindowIcon(QIcon(QStringLiteral(":/logo.png")));

    QString appDir = QCoreApplication::applicationDirPath();
    QDir::setCurrent(appDir);

    LoginWindow loginWindow;
    loginWindow.showMaximized();

    return app.exec();
}