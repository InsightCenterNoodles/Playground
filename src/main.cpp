#include "playground.h"

#include <QCoreApplication>

#include <QCommandLineParser>
#include <QLoggingCategory>

int main(int argc, char* argv[]) {
    auto app = QCoreApplication(argc, argv);

    QCoreApplication::setApplicationName("Playground");
    QCoreApplication::setApplicationVersion("0.1");

    QCommandLineParser parser;
    parser.setApplicationDescription("Geometry export tool for NOODLES");
    parser.addHelpOption();
    parser.addVersionOption();

    QCommandLineOption debug_option(
        "d", QCoreApplication::translate("main", "Enable debug output."));

    QCommandLineOption port_option(
        "p",
        QCoreApplication::translate("main", "Port number to use."),
        "port",
        "50000");

    parser.addOption(debug_option);
    parser.addOption(port_option);

    parser.addPositionalArgument("files", "Geometry files to import");

    parser.process(app);

    bool use_debug = parser.isSet(debug_option);

    if (!use_debug) { QLoggingCategory::setFilterRules("*.debug=false"); }

    quint16 port = 50000;

    if (parser.isSet(port_option)) {
        bool ok;
        auto new_port = parser.value(port_option).toInt(&ok);
        if (ok and new_port > 0 and
            new_port < std::numeric_limits<uint16_t>::max()) {
            port = new_port;
        }
    }

    Playground battc(port, parser.positionalArguments());

    return app.exec();
}
