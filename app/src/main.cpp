#include <QApplication>

#include "MainWindow.h"

int main(int argc, char **argv) {
    QApplication app(argc, argv);
    QApplication::setApplicationName("KumDB Studio");
    QApplication::setOrganizationName("KumDB");

    MainWindow window;
    window.show();

    return QApplication::exec();
}
