#ifndef RIVE_ARTBOARD_H
#define RIVE_ARTBOARD_H

// RiveArtboard — QObject wrapping rive::ArtboardInstance.
//
// Produced by RiveFile::createArtboard(). Owns the underlying rive
// artboard and can create at most one state machine at a time (phase 1:
// single-SM). Size + bounds are read-only reflections of the artboard's
// design-time values.
//
// We don't own the originating RiveFile here; RiveView keeps that alive
// for as long as any artboard from it is in use. Destroying the file
// while this artboard exists is a lifetime bug.

#include <QObject>
#include <QSizeF>
#include <QString>
#include <QStringList>

#include <memory>

#include <rive/refcnt.hpp>

namespace rive {
class ArtboardInstance;
class ViewModelInstance;
}

class RiveStateMachine;

class RiveArtboard : public QObject
{
    Q_OBJECT
    Q_PROPERTY(QString name READ name CONSTANT)
    Q_PROPERTY(qreal width READ width CONSTANT)
    Q_PROPERTY(qreal height READ height CONSTANT)
    Q_PROPERTY(QStringList stateMachineNames READ stateMachineNames CONSTANT)

public:
    RiveArtboard(std::unique_ptr<rive::ArtboardInstance> instance,
                 QString name,
                 QObject* parent = nullptr);
    ~RiveArtboard() override;

    QString name() const { return m_name; }
    qreal width() const;
    qreal height() const;
    QSizeF size() const { return QSizeF(width(), height()); }

    qreal originalWidth() const;
    qreal originalHeight() const;

    // Drive the artboard's layout system at runtime. Use for
    // responsive layouts authored with rive's layout features —
    // setting the artboard size triggers a re-layout.
    void setSize(const QSizeF& size);
    void resetSize();

    // State machines defined on this artboard, in declaration order.
    QStringList stateMachineNames() const;

    // Raw access for the render backend.
    rive::ArtboardInstance* raw() const;

    // Instantiate a state machine. Empty name = default. Returns nullptr
    // if the SM doesn't exist. Caller gets ownership — this artboard
    // holds a parent reference for QObject cleanup, but the typical
    // owner is RiveView.
    RiveStateMachine* createStateMachine(const QString& name);

    // Keyboard + focus forwarding. Return true iff rive consumed the
    // event — caller uses that to decide whether Qt should continue
    // propagating. No-op (returns false) if the artboard has no focus
    // manager or no focused node.
    bool keyInput(int qtKey, int qtModifiers, bool pressed, bool isAutoRepeat);
    bool textInput(const QString& text);
    bool focusNext();
    bool focusPrevious();
    bool focusLeft();
    bool focusRight();
    bool focusUp();
    bool focusDown();

    // Bind a view-model instance — drives any data-bound properties
    // authored in the editor against this artboard.
    void bindViewModelInstance(rive::rcp<rive::ViewModelInstance> instance);

private:
    std::unique_ptr<rive::ArtboardInstance> m_artboard;
    QString m_name;
};

#endif // RIVE_ARTBOARD_H
