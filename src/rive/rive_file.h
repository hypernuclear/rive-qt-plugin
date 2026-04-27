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
// Threading: fromUrl takes a lock. Artboard creation doesn't — rive's
// File is safe to read concurrently for artboard instantiation (each
// instance gets its own copy of the artboard tree).
//
// Factory caveat (phase 1): the rive::Factory passed in is typically
// the backend-specific RenderContext. If two views share a URL but use
// different factories, the second would pick up artboards decoded
// against the first factory — wrong texture/buffer backends. On macOS
// + Metal only (today) there's always one factory so this is fine.
// If we add a second backend we'll need to key the cache on (url,
// factory-identity).

#include <QString>
#include <QStringList>
#include <QUrl>

#include <memory>

#include <rive/file.hpp>

namespace rive {
class ArtboardInstance;
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

    // Load (or retrieve cached). errorOut populated on failure.
    static std::shared_ptr<RiveFile> fromUrl(const QUrl& url,
                                             rive::Factory* factory,
                                             QString* errorOut);

    // Read .riv bytes from a qrc:// or file:// URL. Other schemes are
    // rejected. Exposed publicly so callers (e.g. tests) can preload
    // without going through the cache.
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
};

#endif // RIVE_FILE_H
