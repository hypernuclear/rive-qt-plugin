// Layer 2 — headless contract test for RiveFile (src/rive/rive_file.cpp).
// Drives the real wrapper against fixtures using rive::NoOpFactory, so no
// GPU/renderer is involved. Guards: file import, artboard/view-model
// enumeration, instance minting (default / named / miss), error reporting,
// and the process-wide URL cache.

#include <QTest>

#include "rive_file.h"
#include "rive_artboard.h"

#include <utils/no_op_factory.hpp>

#include "test_helpers.h"

using riveqt_test::fixtureUrl;
using riveqt_test::loadFixtureBytes;

namespace {
constexpr auto kFixture = "data_binding_test.riv";
}

class TstRiveFile : public QObject
{
    Q_OBJECT

private:
    rive::NoOpFactory m_factory;

    std::shared_ptr<RiveFile> load(const char* name = kFixture, QString* err = nullptr)
    {
        QString local;
        return RiveFile::fromBytes(fixtureUrl(name), loadFixtureBytes(name),
                                   &m_factory, err ? err : &local);
    }

private slots:
    void importsValidFile()
    {
        QString err;
        auto file = load(kFixture, &err);
        QVERIFY2(file != nullptr, qPrintable(err));
        QVERIFY(err.isEmpty());
        QVERIFY(file->artboardCount() >= 1);
        QVERIFY(file->artboardNames().contains(QStringLiteral("Artboard")));
    }

    void enumeratesViewModels()
    {
        auto file = load();
        QVERIFY(file);
        QVERIFY(file->viewModelCount() >= 2);
        const QStringList vms = file->viewModelNames();
        QVERIFY(vms.contains(QStringLiteral("PersonViewModel")));
        QVERIFY(vms.contains(QStringLiteral("DrinkViewModel")));
    }

    void createsDefaultArtboard()
    {
        auto file = load();
        QVERIFY(file);
        auto ab = file->createArtboard(QString());
        QVERIFY(ab != nullptr);
        QVERIFY(ab->width() > 0.0);
        QVERIFY(ab->height() > 0.0);
    }

    void createsNamedArtboard()
    {
        auto file = load();
        QVERIFY(file);
        auto ab = file->createArtboard(QStringLiteral("Artboard"));
        QVERIFY(ab != nullptr);
        QCOMPARE(ab->name(), QStringLiteral("Artboard"));
    }

    void missingArtboardReturnsNull()
    {
        auto file = load();
        QVERIFY(file);
        QVERIFY(file->createArtboard(QStringLiteral("NoSuchArtboard")) == nullptr);
    }

    void malformedBytesFailWithError()
    {
        QString err;
        const QUrl bogusUrl = QUrl(QStringLiteral("file:///tmp/riveqt-bogus.riv"));
        auto file = RiveFile::fromBytes(bogusUrl, QByteArray("not a riv file"),
                                        &m_factory, &err);
        QVERIFY(file == nullptr);
        QVERIFY(!err.isEmpty());
    }

    void sameUrlIsCachedSameInstance()
    {
        // fromUrl reads file:// bytes itself; two calls for the same URL must
        // return the very same RiveFile (decoded once), and the entry must
        // expire once all shared_ptrs drop.
        const QUrl url = fixtureUrl(kFixture);
        QString err;
        auto a = RiveFile::fromUrl(url, &m_factory, &err);
        QVERIFY2(a != nullptr, qPrintable(err));
        auto b = RiveFile::fromUrl(url, &m_factory, &err);
        QVERIFY(b != nullptr);
        QCOMPARE(a.get(), b.get());

        RiveFile* firstRaw = a.get();
        a.reset();
        b.reset();
        // After the last ref drops the cache weak_ptr expires; a fresh load
        // re-decodes (pointer equality with the freed one is not required —
        // we only assert we still get a valid file).
        auto c = RiveFile::fromUrl(url, &m_factory, &err);
        QVERIFY(c != nullptr);
        Q_UNUSED(firstRaw);
    }
};

QTEST_MAIN(TstRiveFile)
#include "tst_rive_file.moc"
