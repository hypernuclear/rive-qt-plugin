#include "rive_qt_asset_loader.h"

#include <QByteArray>
#include <QEventLoop>
#include <QFile>
#include <QLoggingCategory>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>

#include <rive/assets/file_asset.hpp>
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

} // namespace

RiveQtAssetLoader::RiveQtAssetLoader(QUrl baseUrl) : m_baseUrl(std::move(baseUrl)) {}

bool RiveQtAssetLoader::loadContents(rive::FileAsset& asset,
                                     rive::Span<const uint8_t> /*inBandBytes*/,
                                     rive::Factory* factory)
{
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

    qCInfo(lcRiveAssetLoader)
        << "no resolution for asset:" << QString::fromStdString(asset.name());
    return false;
}
