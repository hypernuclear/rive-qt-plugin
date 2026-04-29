#ifndef RIVE_QT_ASSET_LOADER_H
#define RIVE_QT_ASSET_LOADER_H

// RiveQtAssetLoader — resolves "hosted" / "referenced" assets in .riv
// files via Qt I/O (QFile / QNetworkAccessManager).
//
// .riv files can carry asset references (image / font / audio) in three
// flavors:
//   - "Embedded": bytes inlined in the .riv. Always works without a
//     loader.
//   - "Hosted": only the cdnUuid is in the .riv; bytes live at
//     `<cdnBaseUrl>/<uuid>`. The runtime calls FileAssetLoader for these.
//   - "Referenced": only a name + extension; expected to live next to
//     the .riv (or wherever the host configures).
//
// Without a loader, hosted/referenced assets render blank. We synthesize
// a loader that tries — in order:
//   1. Hosted CDN (if asset has a cdnUuid). URL = <cdnBaseUrl>/<uuid>.
//   2. Referenced via a base URL (typically the directory of the .riv).
//      URL = <baseUrl>/<name>.<ext>.
//
// Sync I/O via QEventLoop on the calling thread — same trade-off as
// RiveFile::readBytes for http URLs. Acceptable because asset loading
// only happens during File::import (one-time, on the render thread),
// not per-frame.

#include <QString>
#include <QUrl>

#include <rive/file_asset_loader.hpp>

class RiveQtAssetLoader : public rive::FileAssetLoader
{
public:
    // baseUrl is where "referenced" (non-CDN) assets live — typically
    // the directory of the originating .riv file.
    explicit RiveQtAssetLoader(QUrl baseUrl);
    ~RiveQtAssetLoader() override = default;

    bool loadContents(rive::FileAsset& asset,
                      rive::Span<const uint8_t> inBandBytes,
                      rive::Factory* factory) override;

private:
    QUrl m_baseUrl;
};

#endif // RIVE_QT_ASSET_LOADER_H
