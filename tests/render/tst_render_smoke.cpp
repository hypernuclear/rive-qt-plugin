// Layer 3 — offscreen render smoke test (Metal / macOS). Exercises the GPU
// path the headless suite can't:
//
//   - decodeImageUploadsTexture: calls the real backend factory's
//     decodeImage() (QImage decode -> RiveQtFactory makeImageTexture, the
//     exact call the v0.1.x runtime bump broke) on a synthetic PNG and
//     asserts a texture-backed RenderImage of the right size comes back. A
//     broken format/arg would yield a nil Metal texture -> null image.
//
//   - rendersNonBlankFrame: drives a real RiveView through the Metal backend
//     and grabs a frame, asserting it isn't blank/uniform (a dead render
//     path renders nothing).
//
// Robustness: if no offscreen Metal context is available (GPU-less CI), the
// affected test SKIPs rather than fails. The build already guards the
// compile-time signature of makeImageTexture; this guards the runtime.

#include <QBuffer>
#include <QColor>
#include <QFileInfo>
#include <QImage>
#include <QQuickItem>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>

#include "rive_view.h"
#include "backends/rive_render_backend.h"

#include <rive/factory.hpp>
#include <rive/renderer.hpp> // rive::RenderImage
#include <rive/span.hpp>

#include "test_helpers.h"

namespace {
constexpr int kSize = 256;

// A small non-uniform PNG in memory — feeds the image-decode path.
QByteArray makeTestPng(int w, int h)
{
    QImage img(w, h, QImage::Format_RGBA8888);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            img.setPixelColor(x, y, QColor((x * 7) % 256, (y * 5) % 256, 128, 255));
    QByteArray bytes;
    QBuffer buf(&bytes);
    buf.open(QIODevice::WriteOnly);
    img.save(&buf, "PNG");
    return bytes;
}
} // namespace

class TstRenderSmoke : public QObject
{
    Q_OBJECT

private:
    QImage renderFixture(const char* fixture, int* loadFailures = nullptr)
    {
        QQuickWindow window;
        window.resize(kSize, kSize);

        auto* rv = new RiveView(window.contentItem());
        rv->setWidth(kSize);
        rv->setHeight(kSize);
        rv->setFit(RiveView::Fit::Contain);
        QSignalSpy failSpy(rv, &RiveView::loadFailed);
        rv->setSource(riveqt_test::fixtureUrl(QString::fromUtf8(fixture)));
        rv->setPlaying(true);

        // grabWindow() renders to an offscreen target without ever showing a
        // window — works headlessly and avoids a window flash.
        window.create();

        QImage img;
        for (int i = 0; i < 120; ++i)
        {
            QTest::qWait(16);
            const QImage grabbed = window.grabWindow();
            if (!grabbed.isNull())
            {
                img = grabbed;
                if (riveqt_test::nonUniformFraction(img) > 0.02)
                    break;
            }
        }
        if (loadFailures)
            *loadFailures = failSpy.count();
        return img;
    }

private slots:
    void initTestCase()
    {
        // Pick Metal explicitly — the only backend the plugin builds on Apple.
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
    }

    void decodeImageUploadsTexture()
    {
        // Stand up the real Metal backend so we get a RiveQtFactory bound to a
        // live GPU device, then run a PNG through decodeImage -> makeImageTexture.
        QQuickWindow window;
        window.resize(64, 64);
        window.create();
        window.grabWindow(); // force scene-graph / RHI initialization

        QString err;
        auto backend = RiveRenderBackend::create(&window, &err);
        if (!backend)
            QSKIP(qPrintable(QStringLiteral("no Metal backend: %1").arg(err)));
        if (!backend->initialize(&window, &err))
            QSKIP(qPrintable(QStringLiteral("backend init failed (no GPU?): %1").arg(err)));

        rive::Factory* factory = backend->factory();
        QVERIFY(factory != nullptr);

        const QByteArray png = makeTestPng(48, 24);
        const auto* data = reinterpret_cast<const uint8_t*>(png.constData());
        rive::rcp<rive::RenderImage> image =
            factory->decodeImage(rive::Span<const uint8_t>(data, png.size()));

        // The crux: a wrong makeImageTexture format/arg set yields a nil Metal
        // texture and a null RenderImage here.
        QVERIFY2(image != nullptr,
                 "decodeImage returned null — makeImageTexture path is broken");
        QCOMPARE(image->width(), 48);
        QCOMPARE(image->height(), 24);
    }

    void rendersNonBlankFrame()
    {
        int loadFailures = 0;
        const QImage img = renderFixture("clean-knob.riv", &loadFailures);

        if (img.isNull())
            QSKIP("offscreen Metal grab unavailable on this host");
        if (loadFailures > 0)
            QSKIP("RiveView reported loadFailed offscreen (no usable GPU context)");

        // Retina back-buffers are devicePixelRatio-scaled (e.g. 512 for a
        // 256-logical window); just require at least the logical size.
        QVERIFY(img.width() >= kSize && img.height() >= kSize);

        const double nonUniform = riveqt_test::nonUniformFraction(img);
        QVERIFY2(nonUniform > 0.02,
                 qPrintable(QStringLiteral("frame is blank/uniform (non-uniform "
                                           "fraction %1) — render path may be broken")
                                .arg(nonUniform)));
    }

    // RiveView's half of the duration accessor. Only the no-source contract
    // is checked here: a second QQuickWindow in this process can't get a Metal
    // backend (see decodeImageUploadsTexture's skip), so a load-dependent
    // assertion would skip forever rather than guard anything. The substantive
    // lookup is covered headlessly in tst_rive_artboard.
    void animationDurationWithoutSourceIsZero()
    {
        RiveView rv;
        QCOMPARE(rv.animationDuration(QStringLiteral("anything")), 0.0);
        QCOMPARE(rv.animationDuration(QString()), 0.0);
    }
    void matchesGoldenSnapshot()
    {
        // Opt-in golden: skipped unless a baseline has been committed (run with
        // UPDATE_SNAPSHOTS=1 to create one). Kept out of the default gate
        // because GPU/driver differences make a strict pixel golden flaky; the
        // non-blank check above is the portable signal.
        if (!riveqt_test::isSnapshotUpdateMode() &&
            !QFileInfo::exists(riveqt_test::fixturePath(
                QStringLiteral("snapshots/clean-knob.png"))))
        {
            QSKIP("no committed golden (run UPDATE_SNAPSHOTS=1 to create one)");
        }

        int loadFailures = 0;
        const QImage img = renderFixture("clean-knob.riv", &loadFailures);
        if (img.isNull() || loadFailures > 0)
            QSKIP("offscreen Metal render unavailable on this host");

        QString diff;
        const bool ok = riveqt_test::compareImageSnapshot(
            img, QStringLiteral("clean-knob.png"), /*maxMeanDiff=*/12.0, &diff);
        QVERIFY2(ok, qPrintable(diff));
    }
};

QTEST_MAIN(TstRenderSmoke)
#include "tst_render_smoke.moc"
