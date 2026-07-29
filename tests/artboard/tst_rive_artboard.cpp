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
#include <rive/animation/linear_animation.hpp>
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

    void animationDurationMatchesEnumeration()
    {
        auto ab = makeArtboard();
        QVERIFY(ab);
        // Every enumerated animation reports its REAL length, and it is
        // readable WITHOUT instantiating the clip for playback — that's the
        // point of the accessor: a host sequencing several timelines can weight
        // them by length before any of them has played.
        //
        // Ground truth comes the other way round: mint the clip BY NAME and
        // compute frames/fps off it. That catches a lookup returning the wrong
        // animation (say, always the first) and a unit slip (frames reported as
        // seconds) — neither of which a "> 0" check would notice.
        const QStringList names = ab->animationNames();
        QVERIFY(!names.isEmpty());
        for (const QString& name : names)
        {
            std::unique_ptr<rive::LinearAnimationInstance> inst = ab->createAnimation(name);
            QVERIFY2(inst != nullptr, qPrintable(name));
            const rive::LinearAnimation* clip = inst->animation();
            QVERIFY2(clip != nullptr, qPrintable(name));
            QVERIFY2(clip->fps() > 0, qPrintable(name));
            const qreal expected =
                static_cast<qreal>(clip->duration()) / static_cast<qreal>(clip->fps());
            QVERIFY2(expected > 0.0, qPrintable(name));
            const qreal actual = ab->animationDuration(name);
            QVERIFY2(qFuzzyCompare(actual, expected),
                     qPrintable(QStringLiteral("%1: got %2s, expected %3s")
                                    .arg(name).arg(actual).arg(expected)));
        }
        // Misses and empty names are 0, never a default or a crash.
        QCOMPARE(ab->animationDuration(QStringLiteral("definitely-not-an-animation")), 0.0);
        QCOMPARE(ab->animationDuration(QString()), 0.0);
        // Reading a duration spins up a throwaway animation INSTANCE to get at
        // the clip behind it. It must stay throwaway: repeated reads agree, and
        // the artboard is left able to hand out a fresh playable instance —
        // i.e. the probe never becomes, or consumes, the real one.
        if (!names.isEmpty())
        {
            const QString first = names.first();
            const qreal once = ab->animationDuration(first);
            QCOMPARE(ab->animationDuration(first), once);
            QVERIFY(ab->createAnimation(first) != nullptr);
            QCOMPARE(ab->animationDuration(first), once);
            QCOMPARE(ab->animationNames(), names);
        }
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
