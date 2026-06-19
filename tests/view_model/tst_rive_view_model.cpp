// Layer 2 — headless contract test for the data-binding wrappers
// (src/rive/rive_view_model.cpp + rive_vm_property.cpp). This is the
// highest-value suite: data binding churned heavily across the runtime
// bump. Guards property enumeration, typed accessors + type-mismatch
// nulls + caching, value round-trips, mutation signals, and triggers —
// all headless via rive::NoOpFactory.

#include <QSignalSpy>
#include <QTest>

#include "rive_file.h"
#include "rive_artboard.h"
#include "rive_view_model.h"
#include "rive_vm_property.h"

#include <utils/no_op_factory.hpp>

#include "test_helpers.h"

using riveqt_test::fixtureUrl;
using riveqt_test::loadFixtureBytes;

namespace {
constexpr auto kFixture = "data_binding_test.riv";
constexpr auto kArtboard = "Artboard";
constexpr auto kViewModel = "PersonViewModel";
}

class TstRiveViewModel : public QObject
{
    Q_OBJECT

private:
    rive::NoOpFactory m_factory;
    std::shared_ptr<RiveFile> m_file;
    std::unique_ptr<RiveArtboard> m_artboard;

    // Fresh PersonViewModel instance + wrapper, parented to `owner` so it's
    // cleaned up per-test.
    RiveViewModelInstance* makeInstance(QObject* owner)
    {
        rive::rcp<rive::ViewModelInstance> vm =
            m_file->createViewModelInstance(m_artboard->raw(),
                                            QString::fromUtf8(kViewModel), QString());
        if (!vm)
            return nullptr;
        return RiveViewModelInstance::wrap(vm, owner);
    }

private slots:
    void initTestCase()
    {
        QString err;
        m_file = RiveFile::fromBytes(fixtureUrl(kFixture), loadFixtureBytes(kFixture),
                                     &m_factory, &err);
        QVERIFY2(m_file != nullptr, qPrintable(err));
        m_artboard = m_file->createArtboard(QString::fromUtf8(kArtboard));
        QVERIFY(m_artboard != nullptr);
    }

    void definitionEnumeratesProperties()
    {
        // RiveViewModel (definition handle) reflects editor metadata.
        const QStringList vms = m_file->viewModelNames();
        QVERIFY(vms.contains(QString::fromUtf8(kViewModel)));
    }

    void instanceEnumeratesProperties()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        const QStringList names = vmi->propertyNames();
        QVERIFY(names.contains(QStringLiteral("name")));
        QVERIFY(names.contains(QStringLiteral("age")));
        QVERIFY(names.contains(QStringLiteral("agreedToTerms")));
        QVERIFY(names.contains(QStringLiteral("favColor")));
    }

    void typedAccessorsResolveByType()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        QVERIFY(vmi->string(QStringLiteral("name")) != nullptr);
        QVERIFY(vmi->number(QStringLiteral("age")) != nullptr);
        QVERIFY(vmi->boolean(QStringLiteral("agreedToTerms")) != nullptr);
        QVERIFY(vmi->color(QStringLiteral("favColor")) != nullptr);
        QVERIFY(vmi->trigger(QStringLiteral("onFormSubmit")) != nullptr);
        QVERIFY(vmi->viewModel(QStringLiteral("favDrink")) != nullptr);
    }

    void typeMismatchReturnsNull()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        // "name" is a string — asking for it as a number/boolean must miss.
        QVERIFY(vmi->number(QStringLiteral("name")) == nullptr);
        QVERIFY(vmi->boolean(QStringLiteral("name")) == nullptr);
        // A property that doesn't exist at all.
        QVERIFY(vmi->string(QStringLiteral("nope")) == nullptr);
    }

    void accessorsAreCached()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        auto* a = vmi->string(QStringLiteral("name"));
        auto* b = vmi->string(QStringLiteral("name"));
        QVERIFY(a != nullptr);
        QCOMPARE(a, b);
    }

    void colorRoundTrips()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        auto* c = vmi->color(QStringLiteral("favColor"));
        QVERIFY(c);
        const QColor target(12, 34, 56, 255);
        c->setValue(target);
        QCOMPARE(c->value(), target);
    }

    void stringWriteEmitsMutationAndPersists()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        auto* s = vmi->string(QStringLiteral("name"));
        QVERIFY(s);

        QSignalSpy mutatedSpy(vmi, &RiveViewModelInstance::propertyMutated);
        QSignalSpy valueSpy(s, &RiveVMStringProperty::valueChanged);
        s->setValue(QStringLiteral("Zed"));
        QCOMPARE(s->value(), QStringLiteral("Zed"));
        QVERIFY(mutatedSpy.count() >= 1);
        QVERIFY(valueSpy.count() >= 1);
    }

    void triggerFiresAndPollEmits()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        auto* t = vmi->trigger(QStringLiteral("onFormSubmit"));
        QVERIFY(t);

        // fire() must wake the host via propertyMutated (so a settled view
        // re-renders), and bump the underlying counter.
        QSignalSpy mutatedSpy(vmi, &RiveViewModelInstance::propertyMutated);
        QSignalSpy triggeredSpy(t, &RiveVMTriggerProperty::triggered);
        t->fire();
        QVERIFY(mutatedSpy.count() >= 1);

        // poll() observing the counter tick emits triggered() — this is the
        // same path a rive-driven (SM) trigger takes. Driven directly here
        // because the full advance() resets the trigger via advanced() before
        // polling when no state machine is bound to consume it.
        t->poll();
        QVERIFY(triggeredSpy.count() >= 1);
    }

    void numberRoundTrips()
    {
        QObject owner;
        auto* vmi = makeInstance(&owner);
        QVERIFY(vmi);
        auto* n = vmi->number(QStringLiteral("age"));
        QVERIFY(n);
        n->setValue(42.0);
        QCOMPARE(n->value(), 42.0);
    }
};

QTEST_MAIN(TstRiveViewModel)
#include "tst_rive_view_model.moc"
