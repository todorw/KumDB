#include <QApplication>
#include <QIcon>
#include <QFile>
#include <QTextStream>

#include "MainWindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("KumDB Studio");
    QApplication::setOrganizationName("KumDB");

    // Dark theme, loaded once here rather than baked into any one widget --
    // every window/dialog the app creates picks it up automatically.
    QFile styleFile(":/theme/style.qss");
    if (styleFile.open(QFile::ReadOnly | QFile::Text)) {
        QApplication::setStyle("Fusion"); // a11y-safe base the QSS rules build on consistently across platforms
        app.setStyleSheet(QTextStream(&styleFile).readAll());
    }

    QIcon appIcon;
    appIcon.addFile(":/icons/icon_16.png");
    appIcon.addFile(":/icons/icon_32.png");
    appIcon.addFile(":/icons/icon_48.png");
    appIcon.addFile(":/icons/icon_64.png");
    appIcon.addFile(":/icons/icon_128.png");
    appIcon.addFile(":/icons/icon_256.png");
    QApplication::setWindowIcon(appIcon);

    MainWindow window;
    window.show();
    if (argc > 1) window.openInitialDatabase(QString::fromLocal8Bit(argv[1]));

    return QApplication::exec();
}
