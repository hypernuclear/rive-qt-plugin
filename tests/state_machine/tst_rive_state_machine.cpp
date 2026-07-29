// Layer 2 — headless contract test for RiveStateMachine
// (src/rive/rive_state_machine.cpp). Guards pointer hit-testing, frame
// stepping (advanceAndApply), state-change draining, and view-model
// binding — all CPU-side, no renderer.

#include <QSignalSpy>
#include <QTest>

#include "rive_file.h"
#include "rive_artboard.h"
#include "rive_state_machine.h"
#include "rive_view_model.h"

#include <utils/no_op_factory.hpp>

#include "test_helpers.h"

using riveqt_test::fixtureUrl;
using riveqt_test::loadFixtureBytes;

namespace {
constexpr auto kFixture = "data_binding_test.riv";
constexpr auto kArtboard = "Artboard";
constexpr float kFrame = 1.0f / 60.0f;
}

class TstRiveStateMachine : public QObject
{
    Q_OBJECT

private:
    rive::NoOpFactory m_factory;
    std::shared_ptr<RiveFile> m_file;

private slots:
    void initTestCase()
    {
        QString err;
        m_file = RiveFile::fromBytes(fixtureUrl(kFixture), loadFixtureBytes(kFixture),
                                     &m_factory, &err);
        QVERIFY2(m_file != nullptr, qPrintable(err));
    }

    void pointerEventsReturnHitResults()
    {
        auto ab = m_file->createArtboard(QString::fromUtf8(kArtboard));
        QVERIFY(ab);
        RiveStateMachine* sm = ab->createStateMachine(QString());
        QVERIFY(sm);
        sm->advance(0.0f); // pose

        // Any in-bounds point yields a defined HitResult; the call must not
        // crash and must return one of the enum values.
        const auto r = sm->pointerDown(QPointF(ab->width() / 2, ab->height() / 2));
        QVERIFY(r == RiveStateMachine::HitResult::None ||
                r == RiveStateMachine::HitResult::Hit ||
                r == RiveStateMachine::HitResult::HitOpaque);
        sm->pointerMove(QPointF(1, 1));
        sm->pointerUp(QPointF(1, 1));
    }

    void advanceRunsAndSettles()
    {
        auto ab = m_file->createArtboard(QString::fromUtf8(kArtboard));
        QVERIFY(ab);
        RiveStateMachine* sm = ab->createStateMachine(QString());
        QVERIFY(sm);

        // Step a few seconds of frames. A finite form-style machine settles
        // (advance returns false) within that budget; assert we reach it.
        bool settled = false;
        for (int i = 0; i < 600 && !settled; ++i)
            settled = !sm->advance(kFrame);
        QVERIFY2(settled, "state machine never settled within 10s of frames");
    }

    void currentStatePlayheadTracksAdvance()
    {
        auto ab = m_file->createArtboard(QString::fromUtf8(kArtboard));
        QVERIFY(ab);
        RiveStateMachine* sm = ab->createStateMachine(QString());
        QVERIFY(sm);

        // Before the first advance the machine hasn't entered anything.
        QCOMPARE(sm->currentStateTimeline(), QString());
        QCOMPARE(sm->currentStateTime(), 0.0);

        // Once advancing, the reported timeline (when named) must be one
        // the artboard enumerates, and the playhead must live inside the
        // clip's real duration. The fixture's entry state is an
        // AnimationState, so after the pose advance both are populated.
        sm->advance(0.0f);
        const QStringList animations = ab->animationNames();
        const QString timeline = sm->currentStateTimeline();
        QVERIFY2(animations.contains(timeline), qPrintable(timeline));
        QVERIFY(sm->currentStateTime() >= 0.0);
        QVERIFY(sm->currentStateTime() <= ab->animationDuration(timeline));

        // Advancing moves the playhead forward (the state may loop, so
        // assert against the cumulative direction over a few frames rather
        // than a single strict increase).
        const qreal before = sm->currentStateTime();
        for (int i = 0; i < 10; ++i)
            sm->advance(kFrame);
        QVERIFY(sm->currentStateTime() != before
                || sm->currentStateTimeline() != timeline);
    }

    void bindViewModelThenAdvance()
    {
        auto ab = m_file->createArtboard(QString::fromUtf8(kArtboard));
        QVERIFY(ab);
        RiveStateMachine* sm = ab->createStateMachine(QString());
        QVERIFY(sm);

        rive::rcp<rive::ViewModelInstance> vm =
            m_file->createViewModelInstance(ab->raw(), QStringLiteral("PersonViewModel"),
                                            QString());
        QVERIFY(vm != nullptr);
        sm->bindViewModelInstance(vm);
        ab->bindViewModelInstance(vm);

        // Drive a transition: fire the submit trigger on the bound VM and
        // advance. We assert it doesn't crash and keeps stepping — the
        // stateChanged signal is exercised via its spy below.
        QSignalSpy spy(sm, &RiveStateMachine::stateChanged);
        auto* vmi = RiveViewModelInstance::wrap(vm, this);
        QVERIFY(vmi);
        if (auto* t = vmi->trigger(QStringLiteral("onFormSubmit")))
            t->fire();
        for (int i = 0; i < 120; ++i)
            sm->advance(kFrame);
        // At least the initial entry transition should have raised a state
        // change over this many frames.
        QVERIFY(spy.count() >= 1);
        // Every emission carries the entered state's TIMELINE name where one
        // exists (an AnimationState), and an empty string otherwise (entry /
        // exit / any / blend states have no single timeline). Whichever this
        // fixture produces, the name must be one the artboard actually
        // enumerates — never a stray or truncated string.
        const QStringList animations = ab->animationNames();
        int named = 0;
        for (const QList<QVariant>& emission : spy) {
            const QString stateName = emission.at(1).toString();
            if (!stateName.isEmpty()) {
                ++named;
                QVERIFY2(animations.contains(stateName), qPrintable(stateName));
            }
        }
        // This fixture's transitions land on AnimationStates, so at least one
        // emission must be named. Guards the regression where every state
        // reported an empty string.
        QVERIFY(named >= 1);
    }
};

QTEST_MAIN(TstRiveStateMachine)
#include "tst_rive_state_machine.moc"
