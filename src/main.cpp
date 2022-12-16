#include "playground.h"

#include <QCoreApplication>


#include <QLoggingCategory>

int main(int argc, char* argv[]) {
    auto app = QCoreApplication(argc, argv);

    QCoreApplication::setApplicationName("Playground");
    QCoreApplication::setApplicationVersion("0.2");

    Playground playground;

    return app.exec();
}
