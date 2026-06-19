// Layer 2 — headless contract test for RiveQtAssetLoader
// (src/rive/rive_qt_asset_loader.cpp). The hosted/referenced fetch paths
// need network / sibling files, so here we pin the deterministic
// host-facing surface: the static font-fallback configuration that callers
// use to keep missing-font text rendering instead of going blank. The
// loadContents() path itself is exercised indirectly by every file load in
// the other suites.

#include <QTest>

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
};

QTEST_MAIN(TstAssetLoader)
#include "tst_asset_loader.moc"
