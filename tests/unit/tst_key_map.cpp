// Layer 1 — pure unit test for the Qt↔rive input conversions in
// src/rive/rive_key_map.cpp. No rive runtime instance, no GPU. Guards the
// keyboard-routing contract: a wrong mapping silently sends the wrong key
// to a focused Rive artboard.

#include <QtCore/Qt>
#include <QTest>

#include "rive_key_map.h"

#include <rive/input/focusable.hpp>

using rive_key_map::fromQtKey;
using rive_key_map::fromQtModifiers;

class TstKeyMap : public QObject
{
    Q_OBJECT

private slots:
    void asciiLettersMapOneToOne()
    {
        const auto a = fromQtKey(Qt::Key_A);
        QVERIFY(a.valid);
        QCOMPARE(a.key, rive::Key::a);

        const auto z = fromQtKey(Qt::Key_Z);
        QVERIFY(z.valid);
        QCOMPARE(z.key, rive::Key::z);
    }

    void digitsMapOneToOne()
    {
        const auto k0 = fromQtKey(Qt::Key_0);
        QVERIFY(k0.valid);
        QCOMPARE(k0.key, rive::Key::key0);

        const auto k9 = fromQtKey(Qt::Key_9);
        QVERIFY(k9.valid);
        QCOMPARE(k9.key, rive::Key::key9);
    }

    void functionKeysMap()
    {
        const auto f1 = fromQtKey(Qt::Key_F1);
        QVERIFY(f1.valid);
        QCOMPARE(f1.key, rive::Key::f1);
    }

    void namedAndNavigationKeysMap()
    {
        QCOMPARE(fromQtKey(Qt::Key_Space).key, rive::Key::space);
        QCOMPARE(fromQtKey(Qt::Key_Escape).key, rive::Key::escape);
        QCOMPARE(fromQtKey(Qt::Key_Left).key, rive::Key::left);
        QCOMPARE(fromQtKey(Qt::Key_Right).key, rive::Key::right);
        QCOMPARE(fromQtKey(Qt::Key_Up).key, rive::Key::up);
        QCOMPARE(fromQtKey(Qt::Key_Down).key, rive::Key::down);
        // Return and Enter both fold to rive::Key::enter.
        QCOMPARE(fromQtKey(Qt::Key_Return).key, rive::Key::enter);
        QCOMPARE(fromQtKey(Qt::Key_Enter).key, rive::Key::enter);
        for (int k : {Qt::Key_Space, Qt::Key_Escape, Qt::Key_Left, Qt::Key_Return})
            QVERIFY(fromQtKey(k).valid);
    }

    void unmappedKeysReportInvalid()
    {
        // Media / browser keys have no rive equivalent → valid == false so
        // the caller falls through to normal Qt handling.
        QVERIFY(!fromQtKey(Qt::Key_MediaPlay).valid);
        QVERIFY(!fromQtKey(Qt::Key_VolumeUp).valid);
        QVERIFY(!fromQtKey(Qt::Key_Back).valid);
    }

    void modifiersAreBitwiseOred()
    {
        QCOMPARE(fromQtModifiers(Qt::NoModifier), rive::KeyModifiers::none);
        QCOMPARE(fromQtModifiers(Qt::ShiftModifier), rive::KeyModifiers::shift);

        const auto combo = fromQtModifiers(Qt::ShiftModifier | Qt::ControlModifier |
                                           Qt::AltModifier | Qt::MetaModifier);
        const auto expected = rive::KeyModifiers::shift | rive::KeyModifiers::ctrl |
                              rive::KeyModifiers::alt | rive::KeyModifiers::meta;
        QCOMPARE(combo, expected);

        // Control alone must not leak shift/alt/meta bits.
        const auto ctrl = fromQtModifiers(Qt::ControlModifier);
        QCOMPARE(ctrl, rive::KeyModifiers::ctrl);
        QVERIFY((ctrl & rive::KeyModifiers::shift) == rive::KeyModifiers::none);
    }
};

QTEST_APPLESS_MAIN(TstKeyMap)
#include "tst_key_map.moc"
