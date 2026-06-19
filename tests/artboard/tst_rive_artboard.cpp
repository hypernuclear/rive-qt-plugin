// Layer 2 — headless contract test for RiveArtboard
// (src/rive/rive_artboard.cpp). Guards size introspection / layout driving,
// state-machine + animation enumeration, instance minting, and text-run
// mutation — all without a renderer.

#include <QTest>

#include "rive_file.h"
#include "rive_artboard.h"
#include "rive_state_machine.h"

#include <utils/no_op_factory.hpp>
// createAnimation() returns unique_ptr<LinearAnimationInstance>; the temporary's
// destructor at the call site needs the complete type.
#include <rive/animation/linear_animation_instance.hpp>

#include "test_helpers.h"

using riveqt_test::fixtureUrl;
using riveqt_test::loadFixtureBytes;

namespace {
constexpr auto kFixture = "data_binding_test.riv";
constexpr auto kArtboard = "Artboard";
constexpr auto kStateMachine = "State Machine 1";
}

class TstRiveArtboard : public QObject
{
    Q_OBJECT

private:
    rive::NoOpFactory m_factory;
    std::shared_ptr<RiveFile> m_file;

    std::unique_ptr<RiveArtboard> makeArtboard(const char* name = kArtboard)
    {
        return m_file->createArtboard(QString::fromUtf8(name));
    }

private slots:
    void initTestCase()
    {
        QString err;
        m_file = RiveFile::fromBytes(fixtureUrl(kFixture), loadFixtureBytes(kFixture),
                                     &m_factory, &err);
        QVERIFY2(m_file != nullptr, qPrintable(err));
    }

    void enumeratesStateMachines()
    {
        auto ab = makeArtboard();
        QVERIFY(ab);
        QVERIFY(ab->stateMachineNames().contains(QString::fromUtf8(kStateMachine)));
    }

    void animationEnumerationIsStable()
    {
        auto ab = makeArtboard();
        QVERIFY(ab);
        // Whatever the fixture has, the call must be deterministic and a miss
        // must yield nullptr (not a default).
        const QStringList names = ab->animationNames();
        QCOMPARE(ab->animationNames(), names);
        QVERIFY(ab->createAnimation(QStringLiteral("definitely-not-an-animation")) == nullptr);
    }

    void sizeReflectsDesignAndDrivesLayout()
    {
        auto ab = makeArtboard();
        QVERIFY(ab);
        const qreal ow = ab->originalWidth();
        const qreal oh = ab->originalHeight();
        QVERIFY(ow > 0.0);
        QVERIFY(oh > 0.0);

        ab->setSize(QSizeF(123.0, 456.0));
        QCOMPARE(ab->width(), 123.0);
        QCOMPARE(ab->height(), 456.0);

        ab->resetSize();
        QCOMPARE(ab->width(), ow);
        QCOMPARE(ab->height(), oh);
    }

    void missingTextRunReturnsFalse()
    {
        auto ab = makeArtboard();
        QVERIFY(ab);
        QVERIFY(!ab->setTextRun(QStringLiteral("no-such-run"),
                                QStringLiteral("value")));
    }

    void createsStateMachineByNameDefaultAndMiss()
    {
        auto ab = makeArtboard();
        QVERIFY(ab);

        RiveStateMachine* named = ab->createStateMachine(QString::fromUtf8(kStateMachine));
        QVERIFY(named != nullptr);
        QCOMPARE(named->name(), QString::fromUtf8(kStateMachine));

        auto ab2 = makeArtboard();
        RiveStateMachine* def = ab2->createStateMachine(QString());
        QVERIFY(def != nullptr);

        auto ab3 = makeArtboard();
        QVERIFY(ab3->createStateMachine(QStringLiteral("no-such-sm")) == nullptr);
    }
};

QTEST_MAIN(TstRiveArtboard)
#include "tst_rive_artboard.moc"
