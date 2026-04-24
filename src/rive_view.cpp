#include "rive_view.h"
#include "rive_metal_renderer.h"

#include <QFile>
#include <QLoggingCategory>
#include <QQuickWindow>
#include <QSGSimpleTextureNode>

Q_LOGGING_CATEGORY(lcRiveView, "rive.view")

namespace {

// Fit enum is duplicated between RiveView and RiveMetalRenderer to
// keep the renderer header free of QObject machinery.
RiveMetalRenderer::FitMode toRendererFit(RiveView::Fit f)
{
    switch (f)
    {
    case RiveView::Fit::Contain:   return RiveMetalRenderer::FitMode::Contain;
    case RiveView::Fit::Cover:     return RiveMetalRenderer::FitMode::Cover;
    case RiveView::Fit::Fill:      return RiveMetalRenderer::FitMode::Fill;
    case RiveView::Fit::None:      return RiveMetalRenderer::FitMode::None;
    case RiveView::Fit::ScaleDown: return RiveMetalRenderer::FitMode::ScaleDown;
    }
    return RiveMetalRenderer::FitMode::Contain;
}

// Both qrc:// and file:// .riv files are supported. Anything else is
// rejected — a network fetch would need an async path.
QByteArray readRiveBytes(const QUrl& url, QString* error)
{
    QString path;
    if (url.scheme() == QStringLiteral("qrc"))
        path = QStringLiteral(":") + url.path();
    else if (url.isLocalFile())
        path = url.toLocalFile();
    else if (url.scheme().isEmpty() && url.path().startsWith(QLatin1Char(':')))
        path = url.path();
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

RiveView::RiveView(QQuickItem* parent)
    : QQuickItem(parent), m_renderer(std::make_unique<RiveMetalRenderer>())
{
    setFlag(ItemHasContents, true);
    m_frameTimer.start();
}

RiveView::~RiveView() = default;

void RiveView::setSource(const QUrl& url)
{
    if (m_source == url)
        return;
    m_source = url;
    emit sourceChanged();
    readSourceBytes();
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

void RiveView::readSourceBytes()
{
    m_pendingBytes.clear();
    m_loadPending = false;
    m_settled = false;

    if (m_source.isEmpty())
        return;

    QString error;
    QByteArray bytes = readRiveBytes(m_source, &error);
    if (bytes.isEmpty())
    {
        qCWarning(lcRiveView) << "Failed to read source:" << error;
        emit loadFailed(error);
        return;
    }
    m_pendingBytes = std::move(bytes);
    m_loadPending = true;
    m_frameTimer.restart();
    m_lastAdvanceNs = 0;
}

void RiveView::itemChange(ItemChange change, const ItemChangeData& data)
{
    QQuickItem::itemChange(change, data);
    if (change == ItemSceneChange)
    {
        if (data.window)
        {
            // Drive advance off the scene graph's frame clock so the
            // animation steps in lockstep with vsync. Direct connection
            // — onBeforeSynchronizing runs on the render thread before
            // updatePaintNode is invoked.
            connect(data.window, &QQuickWindow::beforeSynchronizing, this,
                    &RiveView::onBeforeSynchronizing, Qt::DirectConnection);
            connect(data.window, &QQuickWindow::sceneGraphInvalidated, this,
                    &RiveView::onSceneGraphInvalidated, Qt::DirectConnection);
        }
        else
        {
            // Item removed from the window. The QSG-side resources will
            // be torn down via sceneGraphInvalidated; nothing to do here.
        }
    }
}

void RiveView::onBeforeSynchronizing()
{
    if (!m_playing || m_settled)
        return;
    // Just request a polish/update — actual advance happens in
    // updatePaintNode where we have access to the renderer.
    update();
}

void RiveView::onSceneGraphInvalidated()
{
    // Renderer holds QRhi-bound resources that go invalid here. Drop
    // and rebuild on next updatePaintNode.
    m_renderer = std::make_unique<RiveMetalRenderer>();
    m_rendererInitialized = false;
    m_loadPending = !m_pendingBytes.isEmpty();
    m_settled = false;
}

QSGNode* RiveView::updatePaintNode(QSGNode* oldNode, UpdatePaintNodeData*)
{
    auto* node = static_cast<QSGSimpleTextureNode*>(oldNode);
    const QSizeF itemSize(width(), height());
    const QQuickWindow* win = window();

    if (!win || itemSize.isEmpty())
    {
        delete node;
        return nullptr;
    }

    if (!m_rendererInitialized)
    {
        QString error;
        if (!m_renderer->initialize(window(), &error))
        {
            // RHI mismatch / no Metal device. Surface to QML once, then
            // stop trying — repeated emissions on every frame would be
            // a footgun. The plugin's docs call out the requirement.
            qCWarning(lcRiveView) << "RiveMetalRenderer init failed:" << error;
            // Still return null so the scene graph composites nothing.
            // Caller can react to loadFailed if they want a placeholder.
            static thread_local bool warnedOnce = false;
            if (!warnedOnce)
            {
                warnedOnce = true;
                emit loadFailed(error);
            }
            delete node;
            return nullptr;
        }
        m_rendererInitialized = true;
    }

    if (m_loadPending)
    {
        m_loadPending = false;
        QString error;
        if (!m_renderer->loadFile(m_pendingBytes, &error))
        {
            qCWarning(lcRiveView) << "Failed to load .riv:" << error;
            emit loadFailed(error);
            delete node;
            return nullptr;
        }
    }

    if (!m_renderer->hasArtboard())
    {
        delete node;
        return nullptr;
    }

    // Advance the artboard for this frame. The first updatePaintNode
    // after a load won't have a "previous" timestamp — fall through
    // with delta = 0 to lay down the initial frame.
    if (m_playing && !m_settled)
    {
        const qint64 nowNs = m_frameTimer.nsecsElapsed();
        const qint64 deltaNs = nowNs - m_lastAdvanceNs;
        m_lastAdvanceNs = nowNs;
        const float delta = std::min(static_cast<float>(deltaNs) * 1e-9f, 0.25f);
        const bool needsMore = m_renderer->advance(delta);
        if (!needsMore)
            m_settled = true;
    }

    const qreal dpr = win->effectiveDevicePixelRatio();
    const QSize pixelSize(static_cast<int>(std::ceil(itemSize.width() * dpr)),
                          static_cast<int>(std::ceil(itemSize.height() * dpr)));

    QSGTexture* tex = m_renderer->ensureTexture(pixelSize);
    if (!tex)
    {
        delete node;
        return nullptr;
    }

    if (!node)
    {
        node = new QSGSimpleTextureNode();
        node->setOwnsTexture(false);
        // Linear filtering for crisp upscale and clean downscale —
        // PLS already AAs internally so we don't need MSAA on top.
        node->setFiltering(QSGTexture::Linear);
    }

    if (node->texture() != tex)
        node->setTexture(tex);
    node->setRect(QRectF(0, 0, itemSize.width(), itemSize.height()));

    m_renderer->renderFrame(toRendererFit(m_fit));

    // Schedule the next frame if still animating — beforeSynchronizing
    // alone won't fire if no item requests an update.
    if (m_playing && !m_settled)
        update();

    return node;
}
