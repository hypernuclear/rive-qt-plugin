#include "rive_qt_asset_loader.h"

#include <QByteArray>
#include <QEventLoop>
#include <QFile>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <rive/assets/file_asset.hpp>
#include <rive/assets/font_asset.hpp>
#include <rive/simple_array.hpp>

Q_LOGGING_CATEGORY(lcRiveAssetLoader, "rive.assetloader")

namespace {

// Synchronous fetch via QNetworkAccessManager + a local QEventLoop.
// Same pattern as RiveFile::readBytes for http URLs.
QByteArray fetchHttp(const QUrl& url)
{
    QNetworkAccessManager nam;
    QNetworkRequest req(url);
    req.setAttribute(QNetworkRequest::RedirectPolicyAttribute,
                     QNetworkRequest::NoLessSafeRedirectPolicy);
    QNetworkReply* reply = nam.get(req);
    QEventLoop loop;
    QObject::connect(reply, &QNetworkReply::finished, &loop, &QEventLoop::quit);
    loop.exec();
    QByteArray data;
    if (reply->error() == QNetworkReply::NoError)
        data = reply->readAll();
    else
        qCInfo(lcRiveAssetLoader)
            << "fetch failed:" << url.toString() << reply->errorString();
    reply->deleteLater();
    return data;
}

QByteArray fetchLocal(const QUrl& url)
{
    QString path;
    if (url.scheme() == QStringLiteral("qrc"))
        path = QStringLiteral(":") + url.path();
    else if (url.isLocalFile())
        path = url.toLocalFile();
    else
        return {};
    QFile f(path);
    if (!f.open(QIODevice::ReadOnly))
    {
        qCInfo(lcRiveAssetLoader)
            << "local fetch failed:" << path << f.errorString();
        return {};
    }
    return f.readAll();
}

QByteArray fetch(const QUrl& url)
{
    if (url.scheme() == QStringLiteral("http") ||
        url.scheme() == QStringLiteral("https"))
        return fetchHttp(url);
    return fetchLocal(url);
}

// Platform-specific best-effort default fallback font. Picked for wide
// unicode coverage and ubiquity on the target OS. The host can override
// via RiveQtAssetLoader::setFallbackFontPath() — typically pointing at
// a bundled qrc:/ font that ships with their app.
QString platformDefaultFallbackFontPath()
{
#if defined(Q_OS_WIN)
    return QStringLiteral("C:/Windows/Fonts/segoeui.ttf");
#elif defined(Q_OS_MACOS)
    // Supplemental/* contains real .ttf files; /System/Library/Fonts/
    // root is mostly .ttc collections, which HarfBuzz can read but we
    // prefer the simpler single-face .ttf when one's available.
    static const QStringList candidates = {
        QStringLiteral("/System/Library/Fonts/Supplemental/Arial Unicode.ttf"),
        QStringLiteral("/Library/Fonts/Arial Unicode.ttf"),
        QStringLiteral("/System/Library/Fonts/Helvetica.ttc"),
    };
    for (const QString& p : candidates)
        if (QFile::exists(p))
            return p;
    return {};
#elif defined(Q_OS_LINUX)
    static const QStringList candidates = {
        QStringLiteral("/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf"),
        QStringLiteral("/usr/share/fonts/dejavu/DejaVuSans.ttf"),
        QStringLiteral("/usr/share/fonts/dejavu-sans-fonts/DejaVuSans.ttf"),
        QStringLiteral("/usr/share/fonts/TTF/DejaVuSans.ttf"),
    };
    for (const QString& p : candidates)
        if (QFile::exists(p))
            return p;
    return {};
#else
    return {};
#endif
}

// User override + platform-default cache. The override is a single
// QString shared across all loaders, set via the static API. Mutex
// guards setter↔getter races (set on GUI thread, read on render
// thread during File::import).
struct FallbackFontConfig
{
    QMutex mutex;
    QString overridePath;
    bool overrideSet = false;
};

FallbackFontConfig& fallbackFontConfig()
{
    static FallbackFontConfig c;
    return c;
}

} // namespace

void RiveQtAssetLoader::setFallbackFontPath(const QString& path)
{
    auto& c = fallbackFontConfig();
    QMutexLocker lock(&c.mutex);
    c.overridePath = path;
    c.overrideSet = true;
}

QString RiveQtAssetLoader::fallbackFontPath()
{
    auto& c = fallbackFontConfig();
    QMutexLocker lock(&c.mutex);
    if (c.overrideSet)
        return c.overridePath;
    return platformDefaultFallbackFontPath();
}

RiveQtAssetLoader::RiveQtAssetLoader(QUrl baseUrl) : m_baseUrl(std::move(baseUrl)) {}

bool RiveQtAssetLoader::loadContents(rive::FileAsset& asset,
                                     rive::Span<const uint8_t> inBandBytes,
                                     rive::Factory* factory)
{
    // In-band: the asset's bytes are embedded in the .riv itself. Decline so
    // the runtime decodes the embedded copy (FileAssetImporter::resolve does
    // exactly that when the loader returns false) — the embedded bytes must
    // always beat a referenced fetch or the font fallback below, otherwise an
    // embedded font renders in the fallback face.
    if (inBandBytes.size() > 0)
        return false;

    // Hosted: asset has a cdn uuid — fetch from <cdnBaseUrl>/<uuid>.
    const std::string uuid = asset.cdnUuidStr();
    if (!uuid.empty())
    {
        const QUrl cdn = QUrl(QString::fromStdString(asset.cdnBaseUrl()) +
                              QLatin1Char('/') +
                              QString::fromStdString(uuid));
        qCInfo(lcRiveAssetLoader)
            << "fetch hosted:" << QString::fromStdString(asset.name())
            << "->" << cdn.toString();
        QByteArray bytes = fetch(cdn);
        if (!bytes.isEmpty())
        {
            rive::SimpleArray<uint8_t> arr(
                reinterpret_cast<const uint8_t*>(bytes.constData()),
                static_cast<std::size_t>(bytes.size()));
            return asset.decode(arr, factory);
        }
    }

    // Referenced: try `<baseUrl>/<name>.<ext>`.
    if (m_baseUrl.isValid())
    {
        const QString fname = QString::fromStdString(asset.name()) +
                              QLatin1Char('.') +
                              QString::fromStdString(asset.fileExtension());
        const QUrl candidate = m_baseUrl.resolved(QUrl(fname));
        qCInfo(lcRiveAssetLoader)
            << "fetch referenced:" << fname << "->" << candidate.toString();
        QByteArray bytes = fetch(candidate);
        if (!bytes.isEmpty())
        {
            rive::SimpleArray<uint8_t> arr(
                reinterpret_cast<const uint8_t*>(bytes.constData()),
                static_cast<std::size_t>(bytes.size()));
            return asset.decode(arr, factory);
        }
    }

    // Font fallback. .riv files routinely reference fonts that aren't
    // embedded and that we can't fetch (the rive CDN doesn't serve
    // font sources by name; Inter.ttf etc. just 404). Without this,
    // every text run in the artboard renders blank. With it, we hand
    // rive a system or user-configured font as a substitute — text
    // metrics will differ from the original but the content reads.
    if (asset.is<rive::FontAsset>())
    {
        QString path = fallbackFontPath();
        if (!path.isEmpty())
        {
            // Accept qrc:/foo as shorthand for :/foo so hosts can pass
            // either form when bundling the fallback in their app's
            // resources.
            if (path.startsWith(QStringLiteral("qrc:/")))
                path = QStringLiteral(":") + path.mid(4);
            QFile f(path);
            if (f.open(QIODevice::ReadOnly))
            {
                const QByteArray bytes = f.readAll();
                if (!bytes.isEmpty())
                {
                    qCInfo(lcRiveAssetLoader)
                        << "font fallback:"
                        << QString::fromStdString(asset.name())
                        << "->" << path;
                    rive::SimpleArray<uint8_t> arr(
                        reinterpret_cast<const uint8_t*>(bytes.constData()),
                        static_cast<std::size_t>(bytes.size()));
                    return asset.decode(arr, factory);
                }
            }
            else
            {
                qCInfo(lcRiveAssetLoader)
                    << "font fallback open failed:" << path
                    << f.errorString();
            }
        }
    }

    qCInfo(lcRiveAssetLoader)
        << "no resolution for asset:" << QString::fromStdString(asset.name());
    return false;
}
