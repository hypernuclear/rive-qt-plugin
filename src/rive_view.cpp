#include "rive_view.h"

#include "backends/rive_render_backend.h"
#include "rive/rive_artboard.h"
#include "rive/rive_file.h"
#include "rive/rive_state_machine.h"

#include <QHoverEvent>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

#include <cmath>

#include <rive/artboard.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
#include <rive/math/mat2d.hpp>
#include <rive/math/vec2d.hpp>
#include <rive/renderer.hpp>

Q_LOGGING_CATEGORY(lcRiveView, "rive.view")

namespace {

RiveRenderBackend::FitMode toBackendFit(RiveView::Fit f)
{
    switch (f)
    {
    case RiveView::Fit::Contain:   return RiveRenderBackend::FitMode::Contain;
    case RiveView::Fit::Cover:     return RiveRenderBackend::FitMode::Cover;
    case RiveView::Fit::Fill:      return RiveRenderBackend::FitMode::Fill;
    case RiveView::Fit::None:      return RiveRenderBackend::FitMode::None;
    case RiveView::Fit::ScaleDown: return RiveRenderBackend::FitMode::ScaleDown;
    }
    return RiveRenderBackend::FitMode::Contain;
}

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

} // namespace

RiveView::RiveView(QQuickItem* parent) : QQuickItem(parent)
{
    setFlag(ItemHasContents, true);
    setAcceptedMouseButtons(Qt::LeftButton | Qt::RightButton | Qt::MiddleButton);
    setAcceptHoverEvents(true);
    // Accept keyboard focus so rive's FocusManager receives key events
    // when the user clicks / tabs into the view.
    setFlag(ItemAcceptsInputMethod, true);
    setActiveFocusOnTab(true);
    m_frameTimer.start();
}

RiveView::~RiveView() = default;

void RiveView::setSource(const QUrl& url)
{
    if (m_source == url)
        return;
    m_source = url;
    emit sourceChanged();
    requestLoad();
    update();
}

void RiveView::setArtboard(const QString& name)
{
    if (m_artboardName == name)
        return;
    m_artboardName = name;
    emit artboardChanged();
    // Defer the rebuild to the render thread — we can't touch rive
    // state safely from the GUI thread.
    m_loadRequested = true;
    update();
}

void RiveView::setStateMachineName(const QString& name)
{
    if (m_stateMachineName == name)
        return;
    m_stateMachineName = name;
    emit stateMachineNameChanged();
    m_loadRequested = true;
    update();
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
        m_frameTimer.restart();
        m_lastAdvanceNs = 0;
        m_settled = false;
        update();
    }
    emit playingChanged();
}

void RiveView::setInputForwarding(bool b)
{
    if (m_inputForwarding == b)
        return;
    m_inputForwarding = b;
    emit inputForwardingChanged();
}

QStringList RiveView::artboardNames() const
{
    return m_file ? m_file->artboardNames() : QStringList{};
}

void RiveView::requestLoad()
{
    m_loadRequested = true;
    m_settled = false;
}

void RiveView::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange && data.window)
    {
        connect(data.window, &QQuickWindow::beforeSynchronizing, this,
                &RiveView::onBeforeSynchronizing, Qt::DirectConnection);
        connect(data.window, &QQuickWindow::sceneGraphInvalidated, this,
                &RiveView::onSceneGraphInvalidated, Qt::DirectConnection);
    }
}

void RiveView::onBeforeSynchronizing()
{
    // Keep requesting updates as long as the animation is live.
    if (m_playing && !m_settled)
        update();
}

void RiveView::onSceneGraphInvalidated()
{
    if (m_backend)
        m_backend->abandonGraphicsResources();
    m_backend.reset();
    m_backendReady = false;
    // Domain state that was loaded against the torn-down factory is
    // no longer valid — drop and re-create on the next paint. SM goes
    // with the artboard (Qt parent chain).
    if (m_stateMachine)
        m_stateMachine->disconnect(this);
    m_stateMachine = nullptr;
    m_artboard.reset();
    m_file.reset();
    m_loadRequested = !m_source.isEmpty();
    m_settled = false;
}

void RiveView::rebuildArtboard()
{
    // Drop SM + artboard (SM is a Qt child of artboard).
    if (m_stateMachine)
        m_stateMachine->disconnect(this);
    m_stateMachine = nullptr;
    m_artboard.reset();

    if (!m_file)
        return;

    m_artboard = m_file->createArtboard(m_artboardName);
    if (!m_artboard)
    {
        emit loadFailed(m_artboardName.isEmpty()
                             ? QStringLiteral("No default artboard in .riv file")
                             : QStringLiteral("Artboard not found: %1").arg(m_artboardName));
        return;
    }
    rebuildStateMachine();
}

void RiveView::rebuildStateMachine()
{
    if (m_stateMachine)
    {
        m_stateMachine->disconnect(this);
        m_stateMachine = nullptr;
    }
    if (!m_artboard)
        return;

    RiveStateMachine* sm = m_artboard->createStateMachine(m_stateMachineName);
    if (!sm)
    {
        // Not fatal — an artboard may legitimately have no state machine,
        // or the user typed a name that doesn't exist. Animations
        // continue to play (default timeline).
        return;
    }
    m_stateMachine = sm;
    connect(sm, &RiveStateMachine::eventReported, this, &RiveView::eventReported);
    connect(sm, &RiveStateMachine::stateChanged, this,
            &RiveView::stateMachineStateChanged);
}

void RiveView::tryLoad()
{
    if (!m_backend || !m_backend->isInitialized())
        return;

    // Source change: reload the file (and cascade — new artboard + SM).
    if (m_loadedUrl != m_source)
    {
        m_loadedUrl = m_source;
        if (m_source.isEmpty())
        {
            m_file.reset();
            m_loadedArtboardName.clear();
            m_loadedStateMachineName.clear();
            m_stateMachine = nullptr;
            m_artboard.reset();
            emit artboardNamesChanged();
            return;
        }

        QString err;
        auto file = RiveFile::fromUrl(m_source, m_backend->factory(), &err);
        if (!file)
        {
            qCWarning(lcRiveView) << "Load failed:" << err;
            emit loadFailed(err);
            m_file.reset();
            m_artboard.reset();
            m_stateMachine = nullptr;
            emit artboardNamesChanged();
            return;
        }
        m_file = std::move(file);
        emit artboardNamesChanged();
        // Force artboard + SM rebuild below by mismatching the names.
        m_loadedArtboardName = QStringLiteral("\x01__force_rebuild__");
    }

    // Artboard change: rebuild the artboard (cascades to SM).
    if (m_loadedArtboardName != m_artboardName)
    {
        m_loadedArtboardName = m_artboardName;
        rebuildArtboard();
        // rebuildArtboard recreates the SM with the current
        // m_stateMachineName, so mark it as already-applied.
        m_loadedStateMachineName = m_stateMachineName;
        m_frameTimer.restart();
        m_lastAdvanceNs = 0;
        m_settled = false;
        return;
    }

    // SM-only change: keep the artboard, swap the SM.
    if (m_loadedStateMachineName != m_stateMachineName)
    {
        m_loadedStateMachineName = m_stateMachineName;
        rebuildStateMachine();
        m_settled = false;
    }
}

QPointF RiveView::mapToArtboard(const QPointF& localPos) const
{
    if (!m_artboard || !m_artboard->raw())
        return localPos;
    const rive::AABB frame(0.0f, 0.0f, static_cast<float>(width()),
                           static_cast<float>(height()));
    const rive::Mat2D forward = rive::computeAlignment(
        toRiveFit(m_fit), rive::Alignment::center, frame,
        m_artboard->raw()->bounds());
    const rive::Mat2D inverse = forward.invertOrIdentity();
    const rive::Vec2D out = inverse * rive::Vec2D(static_cast<float>(localPos.x()),
                                                  static_cast<float>(localPos.y()));
    return QPointF(out.x, out.y);
}

void RiveView::dispatchPointer(QEvent::Type type, const QPointF& localPos)
{
    if (!m_stateMachine)
        return;
    const QPointF ab = mapToArtboard(localPos);
    switch (type)
    {
    case QEvent::MouseButtonPress:   m_stateMachine->pointerDown(ab); break;
    case QEvent::MouseMove:
    case QEvent::HoverMove:          m_stateMachine->pointerMove(ab); break;
    case QEvent::MouseButtonRelease: m_stateMachine->pointerUp(ab); break;
    case QEvent::HoverLeave:         m_stateMachine->pointerExit(ab); break;
    default: break;
    }
}

void RiveView::mousePressEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        // Click grabs keyboard focus so subsequent key events reach
        // rive's FocusManager via our keyPressEvent override.
        forceActiveFocus();
        dispatchPointer(QEvent::MouseButtonPress, event->position());
        event->accept();
        return;
    }
    QQuickItem::mousePressEvent(event);
}

void RiveView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        dispatchPointer(QEvent::MouseMove, event->position());
        event->accept();
        return;
    }
    QQuickItem::mouseMoveEvent(event);
}

void RiveView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        dispatchPointer(QEvent::MouseButtonRelease, event->position());
        event->accept();
        return;
    }
    QQuickItem::mouseReleaseEvent(event);
}

void RiveView::hoverMoveEvent(QHoverEvent* event)
{
    if (m_inputForwarding)
        dispatchPointer(QEvent::HoverMove, event->position());
    QQuickItem::hoverMoveEvent(event);
}

void RiveView::hoverLeaveEvent(QHoverEvent* event)
{
    if (m_inputForwarding)
        dispatchPointer(QEvent::HoverLeave, event->position());
    QQuickItem::hoverLeaveEvent(event);
}

void RiveView::keyPressEvent(QKeyEvent* event)
{
    if (!m_inputForwarding || !m_artboard)
    {
        QQuickItem::keyPressEvent(event);
        return;
    }

    // Focus navigation takes precedence over raw keyInput so Tab/arrows
    // move focus inside the artboard (which Qt would otherwise steal
    // for its own focus system). rive returns false when it has
    // nothing focusable, in which case we fall through to the raw
    // keyInput path below.
    bool navigated = false;
    switch (event->key())
    {
    case Qt::Key_Tab:
        navigated = (event->modifiers() & Qt::ShiftModifier)
                        ? m_artboard->focusPrevious()
                        : m_artboard->focusNext();
        break;
    case Qt::Key_Backtab:
        navigated = m_artboard->focusPrevious();
        break;
    case Qt::Key_Left:  navigated = m_artboard->focusLeft(); break;
    case Qt::Key_Right: navigated = m_artboard->focusRight(); break;
    case Qt::Key_Up:    navigated = m_artboard->focusUp(); break;
    case Qt::Key_Down:  navigated = m_artboard->focusDown(); break;
    default: break;
    }
    if (navigated)
    {
        event->accept();
        update();
        return;
    }

    const bool consumed = m_artboard->keyInput(
        event->key(), event->modifiers(), /*pressed=*/true, event->isAutoRepeat());

    // Text input alongside keyInput: some rive listeners use textInput
    // for character entry (e.g. form fields). Only deliver when the
    // event carries actual text, no modifier keys eating it, and
    // keyInput didn't already claim the event.
    bool textConsumed = false;
    if (!consumed && !event->text().isEmpty())
        textConsumed = m_artboard->textInput(event->text());

    if (consumed || textConsumed)
    {
        event->accept();
        update();
    }
    else
    {
        QQuickItem::keyPressEvent(event);
    }
}

void RiveView::keyReleaseEvent(QKeyEvent* event)
{
    if (!m_inputForwarding || !m_artboard)
    {
        QQuickItem::keyReleaseEvent(event);
        return;
    }
    const bool consumed = m_artboard->keyInput(
        event->key(), event->modifiers(), /*pressed=*/false, event->isAutoRepeat());
    if (consumed)
    {
        event->accept();
        update();
    }
    else
    {
        QQuickItem::keyReleaseEvent(event);
    }
}

QSGNode* RiveView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    const QSizeF itemSize(width(), height());
    QQuickWindow* win = window();

    if (!win || itemSize.isEmpty())
    {
        delete node;
        return nullptr;
    }

    if (!m_backend)
    {
        QString err;
        m_backend = RiveRenderBackend::create(win, &err);
        if (!m_backend)
        {
            qCWarning(lcRiveView) << "No render backend:" << err;
            emit loadFailed(err);
            delete node;
            return nullptr;
        }
    }
    if (!m_backendReady)
    {
        QString err;
        if (!m_backend->initialize(win, &err))
        {
            qCWarning(lcRiveView) << "Backend init failed:" << err;
            emit loadFailed(err);
            delete node;
            return nullptr;
        }
        m_backendReady = true;
    }

    if (m_loadRequested)
    {
        m_loadRequested = false;
        tryLoad();
    }

    if (!m_artboard)
    {
        delete node;
        return nullptr;
    }

    // Advance the SM (if any) or the raw artboard.
    if (m_playing && !m_settled)
    {
        const qint64 nowNs = m_frameTimer.nsecsElapsed();
        const qint64 deltaNs = nowNs - m_lastAdvanceNs;
        m_lastAdvanceNs = nowNs;
        const float delta = std::min(static_cast<float>(deltaNs) * 1e-9f, 0.25f);
        bool needsMore = true;
        if (m_stateMachine)
            needsMore = m_stateMachine->advance(delta);
        else if (m_artboard->raw())
            needsMore = m_artboard->raw()->advance(delta);
        if (!needsMore)
            m_settled = true;
    }

    const qreal dpr = win->effectiveDevicePixelRatio();
    const QSize pixelSize(static_cast<int>(std::ceil(itemSize.width() * dpr)),
                          static_cast<int>(std::ceil(itemSize.height() * dpr)));

    QSGTexture* tex = m_backend->ensureTexture(pixelSize);
    if (!tex)
    {
        delete node;
        return nullptr;
    }

    if (!node)
    {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(false);
        node->setFiltering(QSGTexture::Linear);
    }
    if (node->texture() != tex)
        node->setTexture(tex);
    node->setRect(QRectF(0, 0, itemSize.width(), itemSize.height()));

    m_backend->renderFrame(m_artboard->raw(), toBackendFit(m_fit));

    if (m_playing && !m_settled)
        update();

    return node;
}
