#include "rive_view.h"

#include "backends/rive_render_backend.h"
#include "rive/rive_artboard.h"
#include "rive/rive_file.h"
#include "rive/rive_qt_asset_loader.h"
#include "rive/rive_state_machine.h"

#include <QGuiApplication>
#include <QHoverEvent>
#include <QKeyEvent>
#include <QLoggingCategory>
#include <QMouseEvent>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>
#include <QScreen>
#include <QStyleHints>
#include <QTouchEvent>

#include <cmath>

#include <rive/animation/linear_animation_instance.hpp>
#include <rive/animation/state_machine_instance.hpp>
#include <rive/artboard.hpp>
#include <rive/file.hpp>
#include <rive/layout.hpp>
#include <rive/math/aabb.hpp>
#include <rive/math/mat2d.hpp>
#include <rive/math/vec2d.hpp>
#include <rive/renderer.hpp>
#include <rive/viewmodel/runtime/viewmodel_runtime.hpp>

Q_LOGGING_CATEGORY(lcRiveView, "rive.view")

namespace {

RiveRenderBackend::FitMode toBackendFit(RiveView::Fit f)
{
    switch (f)
    {
    case RiveView::Fit::Fill:      return RiveRenderBackend::FitMode::Fill;
    case RiveView::Fit::Contain:   return RiveRenderBackend::FitMode::Contain;
    case RiveView::Fit::Cover:     return RiveRenderBackend::FitMode::Cover;
    case RiveView::Fit::FitWidth:  return RiveRenderBackend::FitMode::FitWidth;
    case RiveView::Fit::FitHeight: return RiveRenderBackend::FitMode::FitHeight;
    case RiveView::Fit::None:      return RiveRenderBackend::FitMode::None;
    case RiveView::Fit::ScaleDown: return RiveRenderBackend::FitMode::ScaleDown;
    case RiveView::Fit::Layout:    return RiveRenderBackend::FitMode::Layout;
    }
    return RiveRenderBackend::FitMode::Contain;
}

RiveRenderBackend::AlignmentMode toBackendAlignment(RiveView::Alignment a)
{
    switch (a)
    {
    case RiveView::Alignment::TopLeft:      return RiveRenderBackend::AlignmentMode::TopLeft;
    case RiveView::Alignment::TopCenter:    return RiveRenderBackend::AlignmentMode::TopCenter;
    case RiveView::Alignment::TopRight:     return RiveRenderBackend::AlignmentMode::TopRight;
    case RiveView::Alignment::CenterLeft:   return RiveRenderBackend::AlignmentMode::CenterLeft;
    case RiveView::Alignment::Center:       return RiveRenderBackend::AlignmentMode::Center;
    case RiveView::Alignment::CenterRight:  return RiveRenderBackend::AlignmentMode::CenterRight;
    case RiveView::Alignment::BottomLeft:   return RiveRenderBackend::AlignmentMode::BottomLeft;
    case RiveView::Alignment::BottomCenter: return RiveRenderBackend::AlignmentMode::BottomCenter;
    case RiveView::Alignment::BottomRight:  return RiveRenderBackend::AlignmentMode::BottomRight;
    }
    return RiveRenderBackend::AlignmentMode::Center;
}

rive::Fit toRiveFit(RiveView::Fit f)
{
    switch (f)
    {
    case RiveView::Fit::Fill:      return rive::Fit::fill;
    case RiveView::Fit::Contain:   return rive::Fit::contain;
    case RiveView::Fit::Cover:     return rive::Fit::cover;
    case RiveView::Fit::FitWidth:  return rive::Fit::fitWidth;
    case RiveView::Fit::FitHeight: return rive::Fit::fitHeight;
    case RiveView::Fit::None:      return rive::Fit::none;
    case RiveView::Fit::ScaleDown: return rive::Fit::scaleDown;
    case RiveView::Fit::Layout:    return rive::Fit::layout;
    }
    return rive::Fit::contain;
}

// Mirror of toBackendAlignment for the GUI-thread mapToArtboard path.
// Kept separate from the backend mapping so this TU doesn't need to
// include rive_render_backend_helpers.h.
rive::Alignment toRiveAlignment(RiveView::Alignment a)
{
    switch (a)
    {
    case RiveView::Alignment::TopLeft:      return rive::Alignment::topLeft;
    case RiveView::Alignment::TopCenter:    return rive::Alignment::topCenter;
    case RiveView::Alignment::TopRight:     return rive::Alignment::topRight;
    case RiveView::Alignment::CenterLeft:   return rive::Alignment::centerLeft;
    case RiveView::Alignment::Center:       return rive::Alignment::center;
    case RiveView::Alignment::CenterRight:  return rive::Alignment::centerRight;
    case RiveView::Alignment::BottomLeft:   return rive::Alignment::bottomLeft;
    case RiveView::Alignment::BottomCenter: return rive::Alignment::bottomCenter;
    case RiveView::Alignment::BottomRight:  return rive::Alignment::bottomRight;
    }
    return rive::Alignment::center;
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
    // Kick off byte acquisition on the GUI thread *now*, before the next
    // paint. For http(s) this returns immediately (async); for qrc/file
    // the bytes are ready synchronously by the time tryLoad() runs.
    beginSourceFetch();
    requestLoad();
    update();
}

void RiveView::beginSourceFetch()
{
    m_sourceBytesReady = false;
    m_sourceFetchError.clear();
    m_sourceBytes.clear();

    // Cancel any in-flight fetch from a previous source. Detach our slot
    // first so the abort()-driven finished() doesn't re-enter us.
    if (m_sourceReply)
    {
        QNetworkReply* old = m_sourceReply;
        m_sourceReply = nullptr;
        old->disconnect(this);
        old->abort();
        old->deleteLater();
    }

    if (m_source.isEmpty())
        return;

    const QString scheme = m_source.scheme();
    if (scheme == QStringLiteral("http") || scheme == QStringLiteral("https"))
    {
        if (!m_nam)
            m_nam = new QNetworkAccessManager(this);
        QNetworkRequest req(m_source);
        // Follow redirects so .riv URLs that 301 to a CDN work.
        req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                         QNetworkRequest::NoLessSafeRedirectPolicy);
        QNetworkReply* reply = m_nam->get(req);
        m_sourceReply = reply;
        const QUrl fetchUrl = m_source;
        connect(reply, &QNetworkReply::finished, this, [this, reply, fetchUrl]() {
            reply->deleteLater();
            // Superseded by a newer setSource()? Ignore this stale reply.
            if (m_sourceReply != reply)
                return;
            m_sourceReply = nullptr;
            if (reply->error() != QNetworkReply::NoError)
            {
                m_sourceFetchError = QStringLiteral("Network fetch %1 failed: %2")
                                         .arg(fetchUrl.toString(), reply->errorString());
                qCWarning(lcRiveView) << m_sourceFetchError;
            }
            else
            {
                m_sourceBytes = reply->readAll();
                m_sourceBytesReady = !m_sourceBytes.isEmpty();
                if (!m_sourceBytesReady)
                    m_sourceFetchError =
                        QStringLiteral("Network fetch %1 returned no data")
                            .arg(fetchUrl.toString());
            }
            // Re-drive the load now that bytes (or an error) are in hand.
            requestLoad();
            update();
        });
        return;
    }

    // qrc / file / resource-path: cheap, bounded read — do it inline on the
    // GUI thread. Never reaches the render thread.
    QString err;
    m_sourceBytes = RiveFile::readBytes(m_source, &err);
    if (m_sourceBytes.isEmpty())
    {
        m_sourceFetchError = err.isEmpty()
                                 ? QStringLiteral("Empty .riv: %1").arg(m_source.toString())
                                 : err;
        qCWarning(lcRiveView) << m_sourceFetchError;
        return;
    }
    m_sourceBytesReady = true;
}

void RiveView::clearLoadedContent()
{
    m_stateMachine.reset(); // before m_artboard (SM touches the artboard on dtor)
    m_animation.reset();
    if (m_viewModel)
    {
        m_viewModel.reset();
        emit viewModelChanged();
    }
    m_artboard.reset();
    m_file.reset();
    emit artboardNamesChanged();
    refreshTimelineMeta();
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

void RiveView::setPlaybackMode(PlaybackMode mode)
{
    if (m_playbackMode == mode)
        return;
    m_playbackMode = mode;
    emit playbackModeChanged();
    m_loadRequested = true;
    update();
}

void RiveView::setAnimationName(const QString& name)
{
    if (m_animationName == name)
        return;
    m_animationName = name;
    emit animationNameChanged();
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

void RiveView::setAlignment(Alignment a)
{
    if (m_alignment == a)
        return;
    m_alignment = a;
    emit alignmentChanged();
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

void RiveView::setSpeed(qreal s)
{
    const qreal clamped = s < 0.0 ? 0.0 : s;
    if (qFuzzyCompare(m_speed, clamped))
        return;
    m_speed = clamped;
    emit speedChanged();
    // Wake the loop in case the SM had settled — a non-zero speed at
    // an interesting state may want to keep ticking.
    m_settled = false;
    update();
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

QStringList RiveView::animationNames() const
{
    return m_artboard ? m_artboard->animationNames() : QStringList{};
}

QStringList RiveView::viewModelNames() const
{
    if (!m_file)
        return {};
    // Once an artboard is loaded, only expose the VM the editor wired up
    // for it (plus an empty-string entry to mean "default"). Other VMs in
    // the file are sub-VMs referenced by typed properties — picking one
    // of those at the top level binds incompatible data shapes to the
    // artboard's data binds, which produces bad property reads inside
    // any attached scripts. Pre-artboard, fall back to the full list so
    // QML can populate dropdowns before the file finishes loading.
    if (m_artboard && m_artboard->raw() && m_file->raw())
    {
        rive::ViewModelRuntime* vm =
            m_file->raw()->defaultArtboardViewModel(m_artboard->raw());
        if (vm)
            return { QString::fromStdString(vm->name()) };
        return {};
    }
    return m_file->viewModelNames();
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

void RiveView::setAutoBindViewModel(bool b)
{
    if (m_autoBindViewModel == b)
        return;
    m_autoBindViewModel = b;
    emit autoBindViewModelChanged();
    // Toggling forces a VM rebuild on the next paint — either to drop
    // the existing wrapper (off) or instantiate one (on).
    m_loadRequested = true;
    m_settled = false;
    update();
}

bool RiveView::bindViewModelInstance(const QString& name)
{
    if (!m_autoBindViewModel)
    {
        m_autoBindViewModel = true;
        emit autoBindViewModelChanged();
    }
    setViewModelInstanceName(name);
    return m_file != nullptr;
}

void RiveView::setFallbackFontPath(const QString& path)
{
    RiveQtAssetLoader::setFallbackFontPath(path);
}

QString RiveView::fallbackFontPath()
{
    return RiveQtAssetLoader::fallbackFontPath();
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
    //
    // The update() must run on the GUI thread: beforeSynchronizing fires on
    // the render thread, and a render-thread update() only works for items
    // rendered directly by the window pass. An item grabbed by a layer
    // (Texture.sourceItem / ShaderEffectSource) — possibly culled from the
    // window pass entirely — relies on the layer's live-update tracking,
    // which only observes GUI-thread dirtying. Queued invoke costs one frame
    // of latency and makes both paths correct.
    if (m_playing && !m_settled)
        QMetaObject::invokeMethod(this, &QQuickItem::update, Qt::QueuedConnection);
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
    {
        m_stateMachine->disconnect(this);
        m_stateMachine.reset(); // Must die before the artboard.
        emit stateMachineChanged();
    }
    m_animation.reset(); // Must die before the artboard (refs it).
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
    m_loadedAnimationName.clear();
    m_loadedViewModelName.clear();
    m_loadedViewModelInstanceName.clear();
    m_loadRequested = !m_source.isEmpty();
    m_settled = false;
    refreshTimelineMeta(); // animation gone — zero the timeline until reload
}

void RiveView::teardownStateMachine()
{
    if (m_stateMachine)
    {
        m_stateMachine->disconnect(this);
        m_stateMachine.reset();
        emit stateMachineChanged();
    }
}

void RiveView::rebuildArtboard()
{
    // SM must die before the artboard — it holds a rive::ArtboardInstance
    // ref via its rive::StateMachineInstance and touches it during
    // destruction (cleanupFocusTree).
    teardownStateMachine();
    m_animation.reset();
    m_artboard.reset();

    if (!m_file)
    {
        refreshTimelineMeta();
        return;
    }

    m_artboard = m_file->createArtboard(m_artboardName);
    if (!m_artboard)
    {
        emit loadFailed(m_artboardName.isEmpty()
                             ? QStringLiteral("No default artboard in .riv file")
                             : QStringLiteral("Artboard not found: %1").arg(m_artboardName));
        emit stateMachineNamesChanged();
        emit animationNamesChanged();
        emit viewModelNamesChanged();
        refreshTimelineMeta();
        return;
    }
    if (m_layoutSize.isValid())
        m_artboard->setSize(m_layoutSize);
    emit stateMachineNamesChanged();
    emit animationNamesChanged();
    // Filter VM list now that we know the artboard's default VM.
    emit viewModelNamesChanged();
    // Honor the explicit playback-mode override. Auto reproduces the
    // historic SM-first-then-animation-fallback behavior; Animation forces
    // the linear timeline (scrubbable); StateMachine never falls back to an
    // animation. refreshTimelineMeta() runs on every branch.
    switch (m_playbackMode)
    {
    case PlaybackMode::Animation:
        // The animation owns the artboard — ensure no SM so rebuildAnimation's
        // !m_stateMachine guard holds (artboard was just rebuilt SM-less, so
        // this is a no-op, but it documents the invariant).
        teardownStateMachine();
        rebuildAnimation();     // refreshes timeline meta
        break;
    case PlaybackMode::StateMachine:
        // SM only — no animation fallback even if the SM is absent.
        rebuildStateMachine();
        m_animation.reset();
        refreshTimelineMeta();  // SM or static artboard — no scrubbable timeline
        break;
    case PlaybackMode::Auto:
        rebuildStateMachine();
        // Animation fallback only kicks in when there's no SM — the most
        // common case in older / simpler rivs (e.g. a marketing animation
        // with a single timeline). With an SM, the SM owns playback.
        if (!m_stateMachine)
            rebuildAnimation();     // refreshes timeline meta
        else
            refreshTimelineMeta();  // SM owns playback — no scrubbable timeline
        break;
    }
    rebuildViewModel();
}

void RiveView::rebuildViewModel()
{
    if (m_viewModel)
    {
        m_viewModel.reset();
        emit viewModelChanged();
    }
    if (!m_autoBindViewModel)
        return; // Caller suppressed binding — leave m_viewModel null.
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
    RiveStateMachine* prev = m_stateMachine.get();

    if (m_stateMachine)
    {
        m_stateMachine->disconnect(this);
        m_stateMachine.reset();
    }
    if (!m_artboard)
    {
        if (prev != m_stateMachine.get())
            emit stateMachineChanged();
        return;
    }

    RiveStateMachine* sm = m_artboard->createStateMachine(m_stateMachineName);
    if (!sm)
    {
        // Not fatal — an artboard may legitimately have no state machine,
        // or the user typed a name that doesn't exist. Animations
        // continue to play (default timeline).
        if (prev != m_stateMachine.get())
            emit stateMachineChanged();
        return;
    }
    // The SM is constructed parent-less on the render thread (see
    // RiveArtboard::createStateMachine). Move it to the GUI thread so
    // its child QQmlPropertyMap (the inputs map) is reachable from QML
    // without "different thread than the application engine" warnings.
    sm->moveToThread(this->thread());
    m_stateMachine.reset(sm);
    connect(sm, &RiveStateMachine::stateChanged, this,
            &RiveView::stateMachineStateChanged);

    if (prev != m_stateMachine.get())
        emit stateMachineChanged();
}

void RiveView::rebuildAnimation()
{
    m_animation.reset();
    if (m_artboard && !m_stateMachine)
    {
        m_animation = m_artboard->createAnimation(m_animationName);
        // Pose the first frame so a view that loads while paused renders
        // frame 0 instead of the artboard's (often blank) rest pose.
        if (m_animation)
            m_animation->advanceAndApply(0.0f);
    }
    refreshTimelineMeta();
}

void RiveView::refreshTimelineMeta()
{
    int fps = 0;
    int frameCount = 0;
    if (m_animation)
    {
        fps = static_cast<int>(m_animation->fps());
        if (fps > 0)
            frameCount = static_cast<int>(
                std::lround(m_animation->durationSeconds() * static_cast<float>(fps)));
    }
    if (fps != m_fps)
    {
        m_fps = fps;
        emit fpsChanged();
    }
    if (frameCount != m_frameCount)
    {
        m_frameCount = frameCount;
        emit frameCountChanged();
    }
    // Any seek queued against a previous animation is moot now.
    m_pendingSeekFrame = -1;
    if (m_animation)
    {
        publishCurrentFrame();
    }
    else if (m_currentFrame != 0)
    {
        m_currentFrame = 0;
        emit currentFrameChanged();
    }
}

void RiveView::publishCurrentFrame()
{
    if (!m_animation || m_fps <= 0)
        return;
    // m_time runs over [startTime, startTime + durationSeconds]; speed is
    // clamped >= 0 so startTime() == the work-area start. Frame is the
    // offset from there scaled by fps.
    const int frame = std::clamp(
        static_cast<int>(std::lround((m_animation->time() - m_animation->startTime()) *
                                     static_cast<float>(m_fps))),
        0, m_frameCount);
    if (frame != m_currentFrame)
    {
        m_currentFrame = frame;
        emit currentFrameChanged();
    }
}

void RiveView::setCurrentFrame(int frame)
{
    // No scrubbable timeline active (no animation, or an SM owns playback).
    if (m_fps <= 0 || m_frameCount <= 0)
        return;
    frame = std::clamp(frame, 0, m_frameCount);
    // The actual seek (m_animation->time()/apply()) must run on the render
    // thread — stash it and force a repaint. Optimistically publish the
    // value so a bound Slider tracks immediately; the render path will
    // re-publish the applied frame (a no-op when equal).
    m_pendingSeekFrame = frame;
    if (m_currentFrame != frame)
    {
        m_currentFrame = frame;
        emit currentFrameChanged();
    }
    m_settled = false; // force a repaint so the seek lands even when paused
    update();
}

void RiveView::tryLoad()
{
    if (!m_backend || !m_backend->isInitialized())
        return;

    // Source change: reload the file (and cascade — new artboard + SM).
    if (m_loadedUrl != m_source)
    {
        // Empty source: clear everything and commit.
        if (m_source.isEmpty())
        {
            m_loadedUrl = m_source;
            m_loadedArtboardName.clear();
            m_loadedStateMachineName.clear();
            m_loadedAnimationName.clear();
            m_loadedViewModelName.clear();
            m_loadedViewModelInstanceName.clear();
            clearLoadedContent();
            emit stateMachineNamesChanged();
            emit animationNamesChanged();
            emit viewModelNamesChanged();
            return;
        }

        // The GUI-thread fetch (beginSourceFetch) errored: report + blank,
        // and commit m_loadedUrl so we don't re-enter every frame.
        if (!m_sourceFetchError.isEmpty())
        {
            m_loadedUrl = m_source;
            qCWarning(lcRiveView) << "Load failed:" << m_sourceFetchError;
            emit loadFailed(m_sourceFetchError);
            clearLoadedContent();
            return;
        }

        // Bytes not ready yet (async http still in flight). Do NOT commit
        // m_loadedUrl — the fetch's finished() callback requestLoad()s us
        // again once the bytes (or an error) arrive, and we retry then.
        if (!m_sourceBytesReady)
            return;

        m_loadedUrl = m_source;
        QString err;
        // Import from the already-fetched bytes — no IO on the render thread.
        auto file =
            RiveFile::fromBytes(m_source, m_sourceBytes, m_backend->factory(), &err);
        if (!file)
        {
            qCWarning(lcRiveView) << "Load failed:" << err;
            emit loadFailed(err);
            clearLoadedContent();
            return;
        }
        m_file = std::move(file);
        emit artboardNamesChanged();
        emit viewModelNamesChanged();
        // Force artboard + SM rebuild below by mismatching the names.
        m_loadedArtboardName = QStringLiteral("\x01__force_rebuild__");
    }

    // Artboard change: rebuild the artboard (cascades to SM + animation).
    if (m_loadedArtboardName != m_artboardName)
    {
        m_loadedArtboardName = m_artboardName;
        rebuildArtboard();
        // rebuildArtboard recreates the SM/animation per m_playbackMode with
        // the current names, so mark them all as already-applied.
        m_loadedStateMachineName = m_stateMachineName;
        m_loadedAnimationName = m_animationName;
        m_loadedPlaybackMode = m_playbackMode;
        m_frameTimer.restart();
        m_lastAdvanceNs = 0;
        m_settled = false;
        return;
    }

    // Playback-mode change (no artboard rebuild): switch which layer drives
    // the artboard. Tears down the inactive layer, builds the active one, and
    // commits both name shadow vars so a later name edit or switch-back works.
    if (m_loadedPlaybackMode != m_playbackMode)
    {
        m_loadedPlaybackMode = m_playbackMode;
        switch (m_playbackMode)
        {
        case PlaybackMode::Animation:
            teardownStateMachine();
            rebuildAnimation();
            break;
        case PlaybackMode::StateMachine:
            m_animation.reset();
            rebuildStateMachine();
            if (m_viewModel && m_stateMachine)
                m_stateMachine->bindViewModelInstance(m_viewModel->sharedRaw());
            refreshTimelineMeta();
            break;
        case PlaybackMode::Auto:
            rebuildStateMachine();
            if (m_viewModel && m_stateMachine)
                m_stateMachine->bindViewModelInstance(m_viewModel->sharedRaw());
            if (!m_stateMachine)
                rebuildAnimation();
            else
            {
                m_animation.reset();
                refreshTimelineMeta();
            }
            break;
        }
        m_loadedStateMachineName = m_stateMachineName;
        m_loadedAnimationName = m_animationName;
        m_settled = false;
        // No early return: the name shadow vars are committed, so the SM /
        // animation blocks below are guarded no-ops, and falling through lets
        // a simultaneous VM-name change still apply via the VM block.
    }

    // SM-only change: keep the artboard, swap the SM. Inert in Animation mode
    // (no SM is built there); the shadow var still advances so a later switch
    // to Auto/StateMachine rebuilds against the latest name.
    if (m_loadedStateMachineName != m_stateMachineName)
    {
        m_loadedStateMachineName = m_stateMachineName;
        if (m_playbackMode != PlaybackMode::Animation)
        {
            rebuildStateMachine();
            // SM swap re-binds the VM instance through the new SM so its
            // transition guards see the same data.
            if (m_viewModel && m_stateMachine)
                m_stateMachine->bindViewModelInstance(m_viewModel->sharedRaw());
            // Re-evaluate animation fallback in case the new SM name was
            // empty/unmatched — but only in Auto. In StateMachine mode an
            // unmatched name leaves a static artboard (no fallback).
            if (!m_stateMachine && m_playbackMode == PlaybackMode::Auto)
                rebuildAnimation();
            else
            {
                m_animation.reset();
                refreshTimelineMeta();
            }
            m_settled = false;
        }
    }

    // Animation-only change (user picked a different timeline). Active when the
    // animation is the playback layer: Animation mode always, or Auto mode with
    // no SM. Under a live SM (Auto/StateMachine) it's inert.
    if (m_loadedAnimationName != m_animationName)
    {
        m_loadedAnimationName = m_animationName;
        if (m_playbackMode == PlaybackMode::Animation || !m_stateMachine)
        {
            rebuildAnimation();
            m_settled = false;
        }
    }

    // View-model name / preset change: rebind the VM and rebuild the SM
    // so its scripted objects re-bind against the new VM. Without the SM
    // rebuild, lua scripts attached to the old SM still hold registry
    // refs into the previous VM's data, and the next advance() hits a
    // lua_getfield on a nil self — abort inside Luau (luaG_indexerror →
    // luaD_throw). Tearing the SM down releases those refs cleanly.
    if (m_loadedViewModelName != m_viewModelName ||
        m_loadedViewModelInstanceName != m_viewModelInstanceName)
    {
        m_loadedViewModelName = m_viewModelName;
        m_loadedViewModelInstanceName = m_viewModelInstanceName;
        const bool hadSm = m_stateMachine != nullptr;
        rebuildViewModel();
        if (hadSm)
        {
            rebuildStateMachine();
            if (m_viewModel && m_stateMachine)
                m_stateMachine->bindViewModelInstance(m_viewModel->sharedRaw());
        }
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
        toRiveFit(m_fit), toRiveAlignment(m_alignment), frame,
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
    // Wake the frame loop. Pointer events queue listener firings inside
    // rive's SM, which only run on the next advance() call. If the SM
    // had settled (e.g. an audio sample finished playing and there's no
    // more animation in flight), onBeforeSynchronizing has stopped
    // calling update() — so without this nudge, the listener queued by
    // pointerDown never fires and clicks appear to do nothing.
    m_settled = false;
    update();
}

void RiveView::mousePressEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        // Click grabs keyboard focus so subsequent key events reach
        // rive's FocusManager via our keyPressEvent override.
        forceActiveFocus();
        // Pointer id 0 — must match the id used for hover dispatches
        // below. Rive's state machine tracks listener engagement per
        // pointer id; using event->points().at(0).id() (which Qt 6
        // assigns from the mouse's pointing device, often 1) would
        // make hover and click look like two unrelated pointers and
        // break click-on-hovered-target. Touch events use real ids.
        dispatchPointer(QEvent::MouseButtonPress, event->position(), 0);
        // Begin drag tracking. We don't fire dragStart yet — that
        // waits until cursor movement exceeds Qt's startDragDistance
        // threshold (matches Qt's standard drag-detection convention).
        m_dragStartPos = event->position();
        m_dragPending = true;
        m_dragStarted = false;
        event->accept();
        return;
    }
    QQuickItem::mousePressEvent(event);
}

void RiveView::mouseMoveEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        // If a mouse-down is in flight and the cursor's traveled past
        // Qt's drag threshold, promote to a drag and notify rive.
        if (m_dragPending && !m_dragStarted && m_stateMachine &&
            m_stateMachine->raw())
        {
            const qreal threshold = QGuiApplication::styleHints()->startDragDistance();
            const QPointF delta = event->position() - m_dragStartPos;
            if (std::hypot(delta.x(), delta.y()) >= threshold)
            {
                const QPointF ab = mapToArtboard(m_dragStartPos);
                m_stateMachine->raw()->dragStart(
                    rive::Vec2D(static_cast<float>(ab.x()),
                                static_cast<float>(ab.y())),
                    /*timeStamp=*/0.0f,
                    // false: keep pointer events flowing to the SM during
                    // the drag. With true (the rive default), the SM
                    // suppresses pointerMove for this pointer once
                    // dragStart fires, on the assumption that an external
                    // drag system (e.g. an OS-level QDrag) has taken over
                    // the cursor. We're driving the drag from the same
                    // pointer stream Qt is delivering, so we need those
                    // moves to keep arriving — otherwise the dragged
                    // element freezes mid-drag and only "catches up" once
                    // the button is released and hoverMove starts firing.
                    /*disablePointer=*/false,
                    /*pointerId=*/0);
                m_dragStarted = true;
                m_dragPending = false;
            }
        }
        dispatchPointer(QEvent::MouseMove, event->position(), 0);
        event->accept();
        return;
    }
    QQuickItem::mouseMoveEvent(event);
}

void RiveView::mouseReleaseEvent(QMouseEvent* event)
{
    if (m_inputForwarding)
    {
        // Close out the drag (if one was in progress) before the
        // pointerUp so rive sees dragEnd → pointerUp ordering.
        if (m_dragStarted && m_stateMachine && m_stateMachine->raw())
        {
            const QPointF ab = mapToArtboard(event->position());
            m_stateMachine->raw()->dragEnd(
                rive::Vec2D(static_cast<float>(ab.x()),
                            static_cast<float>(ab.y())),
                /*timeStamp=*/0.0f,
                /*pointerId=*/0);
        }
        m_dragPending = false;
        m_dragStarted = false;
        dispatchPointer(QEvent::MouseButtonRelease, event->position(), 0);
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

    // Frame driving normally hooks up in itemChange(ItemSceneChange), but an
    // item used as a Texture.sourceItem / ShaderEffectSource has no visual
    // parent, so that change never fires — the animation froze on its first
    // frame when rendered into a Quick3D material. By the time we're painting
    // a layer, window() is valid: connect lazily here (UniqueConnection makes
    // the tree-parented path a no-op).
    if (connect(win, &QQuickWindow::beforeSynchronizing, this,
                &RiveView::onBeforeSynchronizing,
                static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection)))
    {
        qCDebug(lcRiveView) << "frame driver connected at paint time (layer/sourceItem path)";
    }
    connect(win, &QQuickWindow::sceneGraphInvalidated, this,
            &RiveView::onSceneGraphInvalidated,
            static_cast<Qt::ConnectionType>(Qt::DirectConnection | Qt::UniqueConnection));

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

    // Apply a QML-requested timeline seek. The render thread is the only
    // place we may touch m_animation, so setCurrentFrame() just queues the
    // frame and we land it here — before the advance, so a paused scrub
    // still takes effect this frame.
    bool didSeek = false;
    if (m_animation && m_pendingSeekFrame >= 0 && m_fps > 0)
    {
        const float t = m_animation->startTime() +
                        static_cast<float>(m_pendingSeekFrame) / static_cast<float>(m_fps);
        m_animation->time(t);
        m_animation->advanceAndApply(0.0f); // push onto the artboard + run layout/databind
        if (m_viewModel)
            m_viewModel->advance();
        m_pendingSeekFrame = -1;
        m_settled = false;
        // Keep the frame clock anchored to now so resuming playback doesn't
        // jump by the time spent scrubbing.
        m_lastAdvanceNs = m_frameTimer.nsecsElapsed();
        publishCurrentFrame();
        didSeek = true;
    }

    // One-shot per instance: what drives this view's animation. Names the
    // failure mode when a layered instance paints but never moves.
    if (!m_advanceStateLogged)
    {
        m_advanceStateLogged = true;
        qCDebug(lcRiveView) << "advance state:" << m_source.toString()
                            << "sm=" << (m_stateMachine != nullptr)
                            << "anim=" << (m_animation != nullptr)
                            << "playing=" << m_playing << "settled=" << m_settled;
    }

    // Advance the SM (if any) or the raw artboard. Skipped on a frame where
    // we just seeked, so the scrubbed position isn't immediately stepped past.
    if (!didSeek && m_playing && !m_settled)
    {
        const qint64 nowNs = m_frameTimer.nsecsElapsed();
        const qint64 deltaNs = nowNs - m_lastAdvanceNs;
        m_lastAdvanceNs = nowNs;

        // Frame-pacing spike detector: while continuously playing, the
        // interval between frames should sit at the display period. Log
        // intervals 1.6x-5x the running average (above 5x is a hide/show or
        // pause gap, not jank) with timestamps so hitches can be correlated
        // against host-app log activity. Debug level — can fire 1-2x/sec on
        // displays whose pacing occasionally slips a vsync; enable with
        // QT_LOGGING_RULES="rive.view.debug=true" when investigating jank.
        if (m_paceLastNs > 0)
        {
            const qreal paceDelta = static_cast<qreal>(nowNs - m_paceLastNs);
            if (m_paceEmaNs > 0)
            {
                if (paceDelta > m_paceEmaNs * 1.6 && paceDelta < m_paceEmaNs * 5)
                    qCDebug(lcRiveView).nospace()
                        << "frame pacing spike: " << paceDelta / 1e6
                        << " ms (typical " << m_paceEmaNs / 1e6 << " ms)";
                m_paceEmaNs = m_paceEmaNs * 0.9 + paceDelta * 0.1;
            }
            else
            {
                m_paceEmaNs = paceDelta;
            }
        }
        m_paceLastNs = nowNs;
        const float delta = std::min(static_cast<float>(deltaNs) * 1e-9f, 0.25f) *
                            static_cast<float>(m_speed);
        bool needsMore = true;
        if (m_stateMachine)
            needsMore = m_stateMachine->advance(delta);
        else if (m_animation)
        {
            // advanceAndApply ticks the timeline AND mutates the artboard.
            // The artboard's own advance() walks layout / data binds;
            // advanceAndApply does both for us.
            needsMore = m_animation->advanceAndApply(delta);
        }
        else if (m_artboard->raw())
            needsMore = m_artboard->raw()->advance(delta);
        // After the SM has stepped (and possibly mutated the VM via
        // data-bind), tick the VM so its delegates fire and our typed
        // property wrappers can poll for changes.
        if (m_viewModel)
            m_viewModel->advance();
        // Track the playhead so a bound scrubber follows along during playback.
        publishCurrentFrame();
        if (!needsMore)
        {
            m_settled = true;
            if (!m_settleLogged)
            {
                m_settleLogged = true;
                qCDebug(lcRiveView) << "settled:" << m_source.toString()
                                    << "(advance reported no more work)";
            }
        }
    }

    const qreal dpr = win->effectiveDevicePixelRatio();
    const QSize pixelSize(static_cast<int>(std::ceil(itemSize.width() * dpr)),
                          static_cast<int>(std::ceil(itemSize.height() * dpr)));

    // Perf diagnostics, logged once per target size: the render-target pixel
    // count and refresh rate are the environment multipliers behind
    // platform CPU differences (Retina 2x dpr = 4x pixels; ProMotion 120Hz =
    // 2x frames), so surface them where a report can quote them.
    if (pixelSize != m_lastLoggedPixelSize)
    {
        m_lastLoggedPixelSize = pixelSize;
        const QScreen* screen = win->screen();
        qCInfo(lcRiveView) << "render target" << pixelSize << "dpr" << dpr
                           << "refresh" << (screen ? screen->refreshRate() : 0.0) << "Hz";
    }

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
    // GL writes textures bottom-up; Qt samples top-down. Backends that
    // produce bottom-left-origin textures (currently just GL) ask us to
    // mirror the V axis so the displayed image is right-side-up.
    node->setTextureCoordinatesTransform(
        m_backend->textureOriginIsBottomLeft()
            ? QSGSimpleTextureNode::MirrorVertically
            : QSGSimpleTextureNode::NoTransform);

    m_backend->renderFrame(m_artboard->raw(), toBackendFit(m_fit),
                           toBackendAlignment(m_alignment));

    if (m_playing && !m_settled)
        update();

    return node;
}
