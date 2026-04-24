// Standalone Rive viewer. Loads Main.qml which lists every sample .riv
// from the disk samples/ directory and lets the user switch between them.

#include <QGuiApplication>
#include <QQmlApplicationEngine>
#include <QQmlContext>
#include <QQuickStyle>
#include <QString>
#include <QUrl>

#ifndef RIVE_VIEWER_SAMPLES_DIR
// Fallback in case the define wasn't injected — looks in the cwd. Not
// useful in a deployed build but keeps the binary runnable for debugging.
#define RIVE_VIEWER_SAMPLES_DIR "samples"
#endif

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);

    // Basic style — no dependency on platform-specific Controls.
    QQuickStyle::setStyle(QStringLiteral("Basic"));

    QQmlApplicationEngine engine;

    // Expose the disk samples path to QML. Using a file URL so
    // FolderListModel can consume it directly.
    const QUrl samplesDir = QUrl::fromLocalFile(QStringLiteral(RIVE_VIEWER_SAMPLES_DIR));
    engine.rootContext()->setContextProperty(QStringLiteral("viewerSamplesDir"), samplesDir);

    engine.loadFromModule("RiveViewer", "Main");

    if (engine.rootObjects().isEmpty())
        return -1;

    return app.exec();
}
