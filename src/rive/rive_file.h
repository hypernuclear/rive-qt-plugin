#ifndef RIVE_FILE_H
#define RIVE_FILE_H

// Shared parsed .riv data, cached weakly by URL and graphics factory on the
// importing render thread. Files contain GPU assets: independent renderers
// must import against their own factory. Each view owns its artboard/VM;
// the parsed file expires when its final consumer releases it.
//
// raw() is borrowed and must only be used on the owning render thread.

#include <QByteArray>
#include <QMutex>
#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

#include <rive/file.hpp>

namespace rive {
class ArtboardInstance;
class FileAssetLoader;
class ViewModel;
class ViewModelInstance;
}

class RiveArtboard;

class RiveFile
{
public:
    ~RiveFile();

    RiveFile(const RiveFile&) = delete;
    RiveFile& operator=(const RiveFile&) = delete;

    // Load (or retrieve cached). errorOut populated on failure. Reads the
    // bytes itself via readBytes() — which BLOCKS for http(s) sources, so
    // do not call this from the render thread for a remote URL. Render-path
    // callers fetch bytes off-thread and use fromBytes() instead.
    static std::shared_ptr<RiveFile> fromUrl(const QUrl& url,
                                             rive::Factory* factory,
                                             QString* errorOut);

    // Import (or retrieve cached) from already-fetched bytes. No IO — the
    // caller is responsible for acquiring `bytes` (off the render thread
    // for remote sources). `url` is still required: it keys the cache and
    // anchors relative asset resolution. errorOut populated on failure.
    static std::shared_ptr<RiveFile> fromBytes(const QUrl& url,
                                               const QByteArray& bytes,
                                               rive::Factory* factory,
                                               QString* errorOut);

    // Read .riv bytes from a qrc:// or file:// URL (and, blocking, http(s)).
    // Exposed publicly so callers (e.g. tests, or RiveView's GUI-thread
    // fetch) can preload without going through the cache.
    static QByteArray readBytes(const QUrl& url, QString* errorOut);

    QStringList artboardNames() const;
    int artboardCount() const;

    // Instantiate an artboard. Empty name = default. Returns nullptr
    // if no artboard matches.
    std::unique_ptr<RiveArtboard> createArtboard(const QString& name) const;

    // Access to the underlying rive::File (borrowed). Used by VM
    // properties that need to resolve assets (e.g. artboard refs).
    rive::File* raw() const;

    // View model definitions baked into the file, in declaration order.
    QStringList viewModelNames() const;
    int viewModelCount() const;

    // Build a fresh view-model instance:
    //   - If `viewModelName` is empty: pick the VM attached to the
    //     given artboard's editor binding (typical use — instance
    //     follows the artboard).
    //   - If `viewModelName` is set and `instanceName` empty: create a
    //     fresh instance from the named VM definition.
    //   - If both set: instantiate the editor-authored preset by name.
    //
    // Returns null rcp on miss.
    rive::rcp<rive::ViewModelInstance> createViewModelInstance(
        rive::ArtboardInstance* artboard,
        const QString& viewModelName,
        const QString& instanceName) const;

private:
    RiveFile();

    rive::rcp<rive::File> m_file;
    // Asset loader — lifetime tied to RiveFile. The runtime keeps a
    // borrowed FileAssetLoader* internally; we hold the owning rcp so
    // the loader outlives any in-flight asset decode work.
    rive::rcp<rive::FileAssetLoader> m_assetLoader;
    // Serializes instancing/enumeration against m_file so two render
    // threads (distinct windows) sharing this file can't race on rive's
    // non-atomic refcounts. See the threading note at the top of the file.
    mutable QMutex m_instanceMutex;
};

#endif // RIVE_FILE_H
