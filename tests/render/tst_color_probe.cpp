// TEMPORARY diagnostic probe — renders an arbitrary .riv (RIVE_PROBE_FILE env
// var) through the real Metal backend over black and over white backgrounds,
// then prints the dominant rendered colors. Used to determine whether a
// perceived color shift ("mustard yellow") is baked into the render output
// (value shift or alpha bleed) or happens display-side. Not part of the gate.

#include <QGuiApplication>
#include <QImage>
#include <QMap>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>

#include "rive_view.h"

namespace {
constexpr int kW = 780;  // 3.25:1, the hero's authored ratio
constexpr int kH = 240;

QImage renderOver(const QColor& bg, const QString& rivPath, int* loadFailures)
{
    QQuickWindow window;
    window.resize(kW, kH);
    window.setColor(bg);

    auto* rv = new RiveView(window.contentItem());
    rv->setWidth(kW);
    rv->setHeight(kH);
    rv->setFit(RiveView::Fit::Cover);
    QSignalSpy failSpy(rv, &RiveView::loadFailed);
    rv->setSource(QUrl::fromLocalFile(rivPath));
    rv->setPlaying(true);

    window.create();

    QImage img;
    for (int i = 0; i < 120; ++i)
    {
        QTest::qWait(16);
        const QImage grabbed = window.grabWindow();
        if (!grabbed.isNull())
        {
            img = grabbed;
            // Wait until the riv actually painted (more than bg present).
            QMap<QRgb, int> colors;
            for (int y = 0; y < img.height(); y += 8)
                for (int x = 0; x < img.width(); x += 8)
                    colors[img.pixel(x, y)]++;
            if (colors.size() > 3)
                break;
        }
    }
    if (loadFailures)
        *loadFailures = failSpy.count();
    return img;
}

void printTopColors(const char* label, const QImage& img)
{
    QMap<QRgb, int> counts;
    for (int y = 0; y < img.height(); y += 4)
        for (int x = 0; x < img.width(); x += 4)
            counts[img.pixel(x, y)]++;
    // Sort descending by count.
    QList<QPair<int, QRgb>> sorted;
    for (auto it = counts.constBegin(); it != counts.constEnd(); ++it)
        sorted.append({it.value(), it.key()});
    std::sort(sorted.begin(), sorted.end(),
              [](const auto& a, const auto& b) { return a.first > b.first; });
    qInfo("=== %s (%dx%d) top colors ===", label, img.width(), img.height());
    for (int i = 0; i < qMin(6, static_cast<int>(sorted.size())); ++i)
    {
        const QRgb c = sorted[i].second;
        qInfo("  #%02X%02X%02X  x%d", qRed(c), qGreen(c), qBlue(c), sorted[i].first);
    }
}
} // namespace

class TstColorProbe : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    }

    void probeColors()
    {
        const QString rivPath = qEnvironmentVariable("RIVE_PROBE_FILE");
        if (rivPath.isEmpty())
            QSKIP("set RIVE_PROBE_FILE to a .riv path");

        int fail = 0;
        const QImage overBlack = renderOver(Qt::black, rivPath, &fail);
        if (overBlack.isNull())
            QSKIP("offscreen Metal grab unavailable");
        if (fail > 0)
            QSKIP("riv failed to load");
        printTopColors("over BLACK", overBlack);

        const QImage overWhite = renderOver(Qt::white, rivPath, &fail);
        printTopColors("over WHITE", overWhite);

        qInfo("If the same content color differs between the two runs, the riv");
        qInfo("has sub-100%% opacity and the window background bleeds through.");
    }
};

QTEST_MAIN(TstColorProbe)
#include "tst_color_probe.moc"
