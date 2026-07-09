// Layer 2 — headless contract test for RiveQtAssetLoader
// (src/rive/rive_qt_asset_loader.cpp). The hosted/referenced fetch paths
// need network / sibling files, so here we pin the deterministic
// host-facing surface: the static font-fallback configuration that callers
// use to keep missing-font text rendering instead of going blank. The
// loadContents() path itself is exercised indirectly by every file load in
// the other suites.

#include <QTest>

#include <rive/assets/font_asset.hpp>
#include <rive/span.hpp>

#include "rive_qt_asset_loader.h"

#include "test_helpers.h"

class TstAssetLoader : public QObject
{
    Q_OBJECT

private slots:
    void hasPlatformDefaultFallbackFont()
    {
        // A sensible non-empty default ships per platform so unresolved
        // fonts don't render blank out of the box.
        QVERIFY(!RiveQtAssetLoader::fallbackFontPath().isEmpty());
    }

    void fallbackFontPathRoundTrips()
    {
        const QString original = RiveQtAssetLoader::fallbackFontPath();

        const QString custom = riveqt_test::fixturePath(QStringLiteral("some-font.ttf"));
        RiveQtAssetLoader::setFallbackFontPath(custom);
        QCOMPARE(RiveQtAssetLoader::fallbackFontPath(), custom);

        // Empty disables the fallback.
        RiveQtAssetLoader::setFallbackFontPath(QString());
        QVERIFY(RiveQtAssetLoader::fallbackFontPath().isEmpty());

        // Restore so test ordering can't leak state.
        RiveQtAssetLoader::setFallbackFontPath(original);
        QCOMPARE(RiveQtAssetLoader::fallbackFontPath(), original);
    }

    void constructsWithBaseUrl()
    {
        // The loader is a rive::FileAssetLoader; constructing it with a base
        // URL must not throw or crash (it's handed to File::import).
        RiveQtAssetLoader loader(riveqt_test::fixtureUrl(QStringLiteral("data_binding_test.riv")));
        Q_UNUSED(loader);
    }

    void inBandBytesAreDeclined()
    {
        // Regression: an asset whose bytes are embedded in the .riv must be
        // DECLINED (return false) so FileAssetImporter::resolve() decodes the
        // embedded copy. The loader used to ignore in-band bytes entirely and
        // claim font assets with the fallback font, so a .riv with an
        // embedded font rendered its text in the wrong face. The early
        // decline happens before any factory use, so nullptr is safe here.
        rive::FontAsset asset;
        const uint8_t bytes[] = {0x00, 0x01, 0x02, 0x03};
        RiveQtAssetLoader loader(riveqt_test::fixtureUrl(QStringLiteral("data_binding_test.riv")));
        QVERIFY(!loader.loadContents(
            asset, rive::Span<const uint8_t>(bytes, sizeof(bytes)), nullptr));
    }
};

QTEST_MAIN(TstAssetLoader)
#include "tst_asset_loader.moc"
