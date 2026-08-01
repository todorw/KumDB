#include <QApplication>
#include <QIcon>

#include "MainWindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("KumDB Studio");
    QApplication::setOrganizationName("KumDB");

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

    return QApplication::exec();
}
