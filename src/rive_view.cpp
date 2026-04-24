#include "rive_view.h"
#include "rive_painter_renderer.h"

#include <QByteArray>
#include <QFile>
#include <QLoggingCategory>
#include <QQuickWindow>

#include <rive/animation/state_machine_instance.hpp>
#include <rive/artboard.hpp>
#include <rive/file.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>

// Category name chosen so consumers can enable with QT_LOGGING_RULES="rive.*=true".
Q_LOGGING_CATEGORY(lcRiveView, "rive.view")

namespace {

rive::Fit toRiveFit(RiveView::Fit f)
{
    switch (f)
    {
    case RiveView::Fit::Contain:   return rive::Fit::contain;
    case RiveView::Fit::Cover:     return rive::Fit::cover;
    case RiveView::Fit::Fill:      return rive::Fit::fill;
    case RiveView::Fit::None:      return rive::Fit::none;
    case RiveView::Fit::ScaleDown: return rive::Fit::scaleDown;
    }
    return rive::Fit::contain;
}

// Read the .riv bytes. Supports qrc:// paths (embedded resources) and
// file:// paths. Anything else is rejected.
QByteArray readRiveBytes(const QUrl& url, QString* error)
{
    QString path;
    if (url.scheme() == QStringLiteral("qrc"))
        path = QStringLiteral(":") + url.path();
    else if (url.isLocalFile())
        path = url.toLocalFile();
    else if (url.scheme().isEmpty() && url.path().startsWith(QLatin1Char(':')))
        path = url.path(); // Raw ":/..." qrc form.
    else
    {
        *error = QStringLiteral("Unsupported URL scheme: %1").arg(url.scheme());
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        *error = QStringLiteral("Failed to open %1: %2").arg(path, file.errorString());
        return {};
    }
    return file.readAll();
}

} // namespace

RiveView::RiveView(QQuickItem* parent) : QQuickPaintedItem(parent)
{
    // Antialiased output. QQuickPaintedItem renders into a texture owned by
    // the scene graph, so we don't set renderTarget here — the default
    // (FramebufferObject) works on all Qt 6 backends.
    setAntialiasing(true);
    setFlag(ItemHasContents, true);

    // Kick off the first advance as soon as we're shown.
    m_frameTimer.start();
}

RiveView::~RiveView() = default;

void RiveView::setSource(const QUrl& url)
{
    if (m_source == url)
        return;
    m_source = url;
    emit sourceChanged();
    reload();
}

void RiveView::setFit(Fit f)
{
    if (m_fit == f)
        return;
    m_fit = f;
    emit fitChanged();
    update();
}

void RiveView::setPlaying(bool playing)
{
    if (m_playing == playing)
        return;
    m_playing = playing;
    if (m_playing)
    {
        resetAnimationClock();
        m_settled = false;
        update(); // kick a repaint so advanceFrame runs again
    }
    emit playingChanged();
}

void RiveView::reload()
{
    m_stateMachine.reset();
    m_artboard.reset();
    m_file.reset();
    m_settled = false;

    if (m_source.isEmpty())
    {
        update();
        return;
    }

    QString error;
    const QByteArray bytes = readRiveBytes(m_source, &error);
    if (bytes.isEmpty())
    {
        qCWarning(lcRiveView) << "Failed to read source:" << error;
        emit loadFailed(error);
        update();
        return;
    }

    rive::ImportResult result = rive::ImportResult::malformed;
    auto file = rive::File::import(
        rive::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                  static_cast<size_t>(bytes.size())),
        &m_factory,
        &result);

    if (result != rive::ImportResult::success || !file)
    {
        const QString reason = result == rive::ImportResult::unsupportedVersion
                                   ? QStringLiteral("Unsupported .riv version")
                                   : QStringLiteral("Malformed .riv file");
        qCWarning(lcRiveView) << "Import failed:" << reason;
        emit loadFailed(reason);
        update();
        return;
    }

    // rive::File is ref-counted via rcp<>; we're the sole consumer here so
    // we release the rcp into a unique_ptr-shaped member for clearer
    // ownership semantics.
    rive::File* rawFile = file.release();
    m_file.reset(rawFile);

    m_artboard = m_file->artboardDefault();
    if (!m_artboard)
    {
        emit loadFailed(QStringLiteral("No default artboard in .riv file"));
        update();
        return;
    }

    m_stateMachine = m_artboard->defaultStateMachine();
    // .riv files don't have to have a state machine — animating still works
    // via the artboard alone (it will advance its animations on its own).

    resetAnimationClock();
    update();
}

void RiveView::resetAnimationClock()
{
    m_frameTimer.restart();
    m_lastAdvanceNs = 0;
}

void RiveView::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickPaintedItem::itemChange(change, data);
    if (change == ItemSceneChange && data.window)
    {
        // Drive the advance loop off the scene graph's frame clock. Vsync-
        // aligned updates without a QTimer fighting the render thread.
        // DirectConnection so advance runs on the render thread's sync phase
        // — rive's scene-graph mutations need to land before paint().
        connect(data.window, &QQuickWindow::beforeSynchronizing,
                this, &RiveView::advanceFrame, Qt::DirectConnection);
    }
}

void RiveView::advanceFrame()
{
    if (!m_playing || !m_artboard)
        return;

    // Short-circuit when the animation has settled (no state-machine
    // transitions pending, no active animations). Without this the view
    // burns CPU drawing the same frame every vsync — was the "20% idle"
    // behaviour in the spike. The next user interaction / setSource /
    // setPlaying will flip m_settled back to false.
    if (m_settled)
        return;

    const qint64 nowNs = m_frameTimer.nsecsElapsed();
    const qint64 deltaNs = nowNs - m_lastAdvanceNs;
    m_lastAdvanceNs = nowNs;
    const float deltaSec = static_cast<float>(deltaNs) * 1e-9f;

    // Clamp absurd deltas (e.g. after a stall) so one missed frame doesn't
    // fast-forward the animation by seconds.
    const float clamped = std::min(deltaSec, 0.25f);

    // Rive's advance* returns false when nothing further needs to animate
    // — either the state machine hit a stable state or the artboard's
    // animations all finished. That's our cue to stop calling update().
    bool needsMore = true;
    if (m_stateMachine)
        needsMore = m_stateMachine->advanceAndApply(clamped);
    else
        needsMore = m_artboard->advance(clamped);

    if (!needsMore)
        m_settled = true;

    // advance* mutates the rive scene graph but doesn't schedule a repaint
    // — we have to request one explicitly.
    update();
}

void RiveView::paint(QPainter* painter)
{
    if (!m_artboard)
        return;

    painter->setRenderHint(QPainter::Antialiasing, true);
    painter->setRenderHint(QPainter::SmoothPixmapTransform, true);

    RivePainterRenderer renderer(painter);
    renderer.save();

    // Map the artboard's logical coordinate space into the item's pixel
    // rect using rive's built-in alignment helper.
    const rive::AABB frame(0.0f, 0.0f, static_cast<float>(width()),
                           static_cast<float>(height()));
    renderer.align(toRiveFit(m_fit), rive::Alignment::center, frame,
                   m_artboard->bounds());

    m_artboard->draw(&renderer);

    renderer.restore();
}
