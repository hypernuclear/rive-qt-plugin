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
#include <QVariantMap>

#include <memory>

#include <rive/refcnt.hpp>

namespace rive {
class ArtboardInstance;
class LinearAnimationInstance;
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

    // Linear animations defined on this artboard, in declaration order.
    // Used as a playback fallback when the artboard has no state machine
    // (rive's web player and other runtimes do the same: SM → first
    // animation → static).
    QStringList animationNames() const;
    /// Duration of the named animation in SECONDS, 0 when there's no such
    /// animation. Lets a host weight a multi-clip sequence by real length
    /// without having to load each clip to read its frame count.
    qreal animationDuration(const QString& name) const;

    /// Whether the named animation repeats (loop or ping-pong in the .riv).
    /// Readable WITHOUT loading the clip, same cost as animationDuration.
    /// False on a miss — a one-shot answer would be a lie either way, and
    /// "doesn't repeat" is the safe default for sequencing.
    bool animationLoops(const QString& name) const;

    /// Static structure of a state machine's graph, read WITHOUT
    /// instantiating it — so a host can plan a whole sequence up front.
    /// Returns a map:
    ///   entry:       QString  — timeline name of the state the entry node
    ///                points at ("" when it isn't a single AnimationState)
    ///   states:      QStringList — timeline names of every AnimationState,
    ///                in declaration order ("" for entry/exit/any/blend)
    ///   transitions: QVariantList of {from, to} QString pairs — one entry
    ///                per authored transition, resolved to timeline names
    /// Both names in a transition may be "" (entry / exit / any states).
    /// Empty map when the named machine doesn't exist. Only the FIRST
    /// layer is reported — hosts driving a sequence use one layer, and a
    /// flat map has nowhere to disambiguate a second.
    QVariantMap stateMachineGraph(const QString& name) const;

    // Instantiate a linear animation. Empty name = first animation.
    // Returns null if the artboard has no animations or the name doesn't
    // match. Caller owns the returned instance.
    std::unique_ptr<rive::LinearAnimationInstance> createAnimation(
        const QString& name) const;

    // Text run mutation. LEGACY: prefer binding a view-model string
    // property to the run in the editor and writing through
    // RiveViewModelInstance::props — Rive itself is steering away from
    // runtime text-run mutation. These remain for parity with rive-wasm
    // and direct designer-handoff workflows. Returns true if the named
    // run was found and the text was set; false if not.
    Q_INVOKABLE bool setTextRun(const QString& name, const QString& value);
    // Path is slash-delimited and resolves through nested artboards
    // (e.g. "Card/Header"). Empty path is equivalent to setTextRun.
    Q_INVOKABLE bool setTextRunAtPath(const QString& name,
                                      const QString& value,
                                      const QString& path);

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
