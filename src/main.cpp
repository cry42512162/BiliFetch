#include "MainWindow.h"

#include <QApplication>

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("BiliFetch"));
    QApplication::setOrganizationName(QStringLiteral("BiliFetch"));

    MainWindow w;
    w.show();

    // 命令行直抓：BiliFetch.exe <BV号/链接/关键词…>（首个非选项参数）
    const QStringList args = app.arguments().mid(1);
    for (const QString &a : args) {
        if (!a.startsWith(QLatin1Char('-')) && !a.trimmed().isEmpty()) {
            w.openTarget(a.trimmed());
            break;
        }
    }

    return app.exec();
}
