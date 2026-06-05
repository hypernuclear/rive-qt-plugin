#ifndef RIVE_FILE_H
#define RIVE_FILE_H

// RiveFile — shared handle to a parsed .riv file.
//
// Not a QObject. Kept as a plain class so we can manage it with
// std::shared_ptr + a process-wide std::weak_ptr cache keyed by URL.
// The pattern:
//
//   auto file = RiveFile::fromUrl(url, factory, &err);
//   auto artboard = file->createArtboard("main");
//
// Multiple RiveView instances pointing at the same URL share a single
// RiveFile — decoded exactly once. When the last view releases its
// shared_ptr, the cache entry's weak_ptr expires and the next fromUrl()
// call re-decodes fresh.
//
// Threading: the process-wide cache is mutex-guarded, and every method
// that mints instances or enumerates the file (createArtboard,
// createViewModelInstance, artboardNames, viewModelNames, ...) takes a
// per-file lock. This matters because RiveFile is shared across views by
// URL: two RiveViews in *different* windows run on *different* render
// threads, and rive's File mints instances by bumping non-atomic rcp
// refcounts on shared assets — concurrent instancing without the lock
// would corrupt those counts. The lock serializes that cross-window
// minting (within one window the scene-graph sync barrier already
// serializes access).
//
// `raw()` hands out the bare rive::File* and is NOT covered by the lock —
// callers (the VM property wrappers resolving assets) must only touch it
// from their own window's render thread. Cross-window concurrent use of
// raw() is the one remaining unguarded path; revisit if a view-model
// genuinely needs to share a file across windows.
//
// Factory caveat (phase 1): the rive::Factory passed in is typically
// the backend-specific RenderContext. If two views share a URL but use
// different factories, the second would pick up artboards decoded
// against the first factory — wrong texture/buffer backends. On macOS
// + Metal only (today) there's always one factory so this is fine.
// If we add a second backend we'll need to key the cache on (url,
// factory-identity).

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
