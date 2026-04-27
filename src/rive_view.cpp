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
#include <QTouchEvent>

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
    setAcceptTouchEvents(true);
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

QStringList RiveView::stateMachineNames() const
{
    return m_artboard ? m_artboard->stateMachineNames() : QStringList{};
}

QStringList RiveView::viewModelNames() const
{
    return m_file ? m_file->viewModelNames() : QStringList{};
}

void RiveView::setViewModelName(const QString& name)
{
    if (m_viewModelName == name)
        return;
    m_viewModelName = name;
    emit viewModelNameChanged();
    m_loadRequested = true;
    update();
}

void RiveView::setViewModelInstanceName(const QString& name)
{
    if (m_viewModelInstanceName == name)
        return;
    m_viewModelInstanceName = name;
    emit viewModelInstanceNameChanged();
    m_loadRequested = true;
    update();
}

void RiveView::setLayoutSize(const QSizeF& size)
{
    if (m_layoutSize == size)
        return;
    m_layoutSize = size;
    emit layoutSizeChanged();
    if (m_artboard)
    {
        if (size.isValid())
            m_artboard->setSize(size);
        else
            m_artboard->resetSize();
        m_settled = false;
    }
    update();
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
    if (m_viewModel)
    {
        m_viewModel.reset();
        emit viewModelChanged();
    }
    m_artboard.reset();
    m_file.reset();
    m_loadedUrl.clear();
    m_loadedArtboardName.clear();
    m_loadedStateMachineName.clear();
    m_loadedViewModelName.clear();
    m_loadedViewModelInstanceName.clear();
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
        emit stateMachineNamesChanged();
        return;
    }
    if (m_layoutSize.isValid())
        m_artboard->setSize(m_layoutSize);
    emit stateMachineNamesChanged();
    rebuildStateMachine();
    rebuildViewModel();
}

void RiveView::rebuildViewModel()
{
    if (m_viewModel)
    {
        m_viewModel.reset();
        emit viewModelChanged();
    }
    if (!m_file || !m_artboard || !m_artboard->raw())
        return;

    qCInfo(lcRiveView) << "rebuildViewModel: file VMs =" << m_file->viewModelNames()
                       << "requested vm =" << m_viewModelName
                       << "instance =" << m_viewModelInstanceName;

    rive::rcp<rive::ViewModelInstance> instance = m_file->createViewModelInstance(
        m_artboard->raw(), m_viewModelName, m_viewModelInstanceName);
    if (!instance)
    {
        qCInfo(lcRiveView) << "rebuildViewModel: createViewModelInstance returned null";
        return;
    }

    // Bind to both artboard and SM so transitions and visual bindings
    // see the same instance.
    m_artboard->bindViewModelInstance(instance);
    if (m_stateMachine)
        m_stateMachine->bindViewModelInstance(instance);

    // Construct the wrapper with factory + file so image / artboard-
    // ref typed properties can decode and resolve. m_file is our
    // shared_ptr<RiveFile>; the underlying rive::File pointer is
    // accessed via the RiveFile member.
    m_viewModel = std::make_unique<RiveViewModelInstance>(
        std::move(instance), m_backend->factory(), m_file ? m_file->raw() : nullptr);
    // We construct on the render thread (during updatePaintNode), but
    // QML talks to the VM from the GUI thread — typed property
    // accessors create child QObjects, which Qt forbids across
    // threads. Move thread affinity to RiveView's (GUI) thread before
    // anyone observes us. moveToThread on a freshly-constructed
    // QObject with no pending events is the supported pattern.
    m_viewModel->moveToThread(this->thread());

    // Wake the advance loop whenever a VM property is mutated. A
    // settled state machine otherwise won't re-process data bindings,
    // so a slider tweak from QML wouldn't update the artboard.
    // QueuedConnection because the signal originates on the GUI thread
    // (QML setter) but `m_settled` / update() must be marshaled into
    // the next render-thread tick safely.
    connect(m_viewModel.get(), &RiveViewModelInstance::propertyMutated,
            this, [this]() {
                m_settled = false;
                update();
            });

    qCInfo(lcRiveView) << "rebuildViewModel: bound VM with"
                       << m_viewModel->propertyNames().size() << "properties:"
                       << m_viewModel->propertyNames();
    emit viewModelChanged();
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
            m_loadedViewModelName.clear();
            m_loadedViewModelInstanceName.clear();
            m_stateMachine = nullptr;
            if (m_viewModel)
            {
                m_viewModel.reset();
                emit viewModelChanged();
            }
            m_artboard.reset();
            emit artboardNamesChanged();
            emit viewModelNamesChanged();
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
        emit viewModelNamesChanged();
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
        // SM swap re-binds the VM instance through the new SM so its
        // transition guards see the same data.
        if (m_viewModel && m_stateMachine)
            m_stateMachine->bindViewModelInstance(m_viewModel->sharedRaw());
        m_settled = false;
    }

    // View-model name / preset change: rebuild only the VM, leave
    // artboard + SM alone.
    if (m_loadedViewModelName != m_viewModelName ||
        m_loadedViewModelInstanceName != m_viewModelInstanceName)
    {
        m_loadedViewModelName = m_viewModelName;
        m_loadedViewModelInstanceName = m_viewModelInstanceName;
        rebuildViewModel();
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

void RiveView::dispatchPointer(QEvent::Type type, const QPointF& localPos, int pointerId)
{
    if (!m_stateMachine)
        return;
    const QPointF ab = mapToArtboard(localPos);
    switch (type)
    {
    case QEvent::MouseButtonPress:
    case QEvent::TouchBegin:         m_stateMachine->pointerDown(ab, pointerId); break;
    case QEvent::MouseMove:
    case QEvent::HoverMove:
    case QEvent::TouchUpdate:        m_stateMachine->pointerMove(ab, pointerId); break;
    case QEvent::MouseButtonRelease:
    case QEvent::TouchEnd:           m_stateMachine->pointerUp(ab, pointerId); break;
    case QEvent::HoverLeave:
    case QEvent::TouchCancel:        m_stateMachine->pointerExit(ab, pointerId); break;
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
        dispatchPointer(QEvent::MouseButtonPress, event->position(),
                        static_cast<int>(event->pointingDevice() ? event->points().at(0).id() : 0));
        event->accept();
        return;
    }
    QQuickItem::mousePressEvent(event);
}

void RiveView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        dispatchPointer(QEvent::MouseMove, event->position(),
                        static_cast<int>(event->pointingDevice() ? event->points().at(0).id() : 0));
        event->accept();
        return;
    }
    QQuickItem::mouseMoveEvent(event);
}

void RiveView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        dispatchPointer(QEvent::MouseButtonRelease, event->position(),
                        static_cast<int>(event->pointingDevice() ? event->points().at(0).id() : 0));
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

void RiveView::touchEvent(QTouchEvent* event)
{
    if (!m_inputForwarding)
    {
        QQuickItem::touchEvent(event);
        return;
    }
    // One state-machine pointer per touch point. Each QEventPoint
    // carries a stable id() that persists across the touch's
    // lifetime, which is exactly what rive expects in pointerId.
    bool consumed = false;
    for (const QEventPoint& p : event->points())
    {
        QEvent::Type t = QEvent::None;
        switch (p.state())
        {
        case QEventPoint::Pressed:    t = QEvent::TouchBegin; break;
        case QEventPoint::Updated:    t = QEvent::TouchUpdate; break;
        case QEventPoint::Released:   t = QEvent::TouchEnd; break;
        case QEventPoint::Stationary: continue; // nothing to forward
        case QEventPoint::Unknown:    continue;
        }
        if (t == QEvent::TouchBegin)
            forceActiveFocus();
        dispatchPointer(t, p.position(), p.id());
        consumed = true;
    }
    if (consumed)
        event->accept();
    else
        QQuickItem::touchEvent(event);
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
        // After the SM has stepped (and possibly mutated the VM via
        // data-bind), tick the VM so its delegates fire and our typed
        // property wrappers can poll for changes.
        if (m_viewModel)
            m_viewModel->advance();
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
