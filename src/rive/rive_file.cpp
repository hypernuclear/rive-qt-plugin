#include "rive_file.h"
#include "rive_artboard.h"

#include <QByteArray>
#include <QFile>
#include <QHash>
#include <QLoggingCategory>
#include <QMutex>
#include <QMutexLocker>

#include <rive/artboard.hpp>
#include <rive/assets/file_asset.hpp>
#include <rive/viewmodel/viewmodel.hpp>
#include <rive/viewmodel/viewmodel_instance.hpp>

#ifdef WITH_RIVE_SCRIPTING
#include <rive/assets/script_asset.hpp>
#endif

Q_LOGGING_CATEGORY(lcRiveFile, "rive.file")

namespace {

// Process-wide cache of loaded files. Weak refs so entries self-expire
// when the last consumer drops its shared_ptr.
struct FileCache
{
    QMutex mutex;
    QHash<QUrl, std::weak_ptr<RiveFile>> entries;
};

FileCache& cache()
{
    static FileCache c;
    return c;
}

} // namespace

RiveFile::RiveFile() = default;
RiveFile::~RiveFile() = default;

QByteArray RiveFile::readBytes(const QUrl& url, QString* errorOut)
{
    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
    };

    QString path;
    if (url.scheme() == QStringLiteral("qrc"))
        path = QStringLiteral(":") + url.path();
    else if (url.isLocalFile())
        path = url.toLocalFile();
    else if (url.scheme().isEmpty() && url.path().startsWith(QLatin1Char(':')))
        path = url.path();
    else
    {
        setError(QStringLiteral("Unsupported URL scheme: %1").arg(url.scheme()));
        return {};
    }

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
    {
        setError(QStringLiteral("Failed to open %1: %2").arg(path, file.errorString()));
        return {};
    }
    return file.readAll();
}

std::shared_ptr<RiveFile> RiveFile::fromUrl(const QUrl& url,
                                            rive::Factory* factory,
                                            QString* errorOut)
{
    auto setError = [&](const QString& msg) {
        if (errorOut)
            *errorOut = msg;
    };

    if (url.isEmpty())
    {
        setError(QStringLiteral("RiveFile::fromUrl: empty URL"));
        return nullptr;
    }
    if (!factory)
    {
        setError(QStringLiteral("RiveFile::fromUrl: null factory"));
        return nullptr;
    }

    auto& c = cache();
    QMutexLocker lock(&c.mutex);

    // Cache hit (still alive).
    if (auto it = c.entries.find(url); it != c.entries.end())
    {
        if (auto existing = it->lock())
            return existing;
        c.entries.erase(it);
    }

    QString readErr;
    const QByteArray bytes = readBytes(url, &readErr);
    if (bytes.isEmpty())
    {
        setError(readErr);
        return nullptr;
    }

    rive::ImportResult result = rive::ImportResult::malformed;
    auto imported = rive::File::import(
        rive::Span<const uint8_t>(reinterpret_cast<const uint8_t*>(bytes.constData()),
                                  static_cast<std::size_t>(bytes.size())),
        factory,
        &result);
    if (result != rive::ImportResult::success || !imported)
    {
        setError(result == rive::ImportResult::unsupportedVersion
                     ? QStringLiteral("Unsupported .riv version")
                     : QStringLiteral("Malformed .riv file"));
        return nullptr;
    }

    // std::shared_ptr with a custom deleter so RiveFile's dtor stays
    // private — we want RiveFile instances to only come from fromUrl().
    std::shared_ptr<RiveFile> wrapped(new RiveFile(), [](RiveFile* p) { delete p; });
    wrapped->m_file = imported;
    c.entries.insert(url, wrapped);

#ifdef WITH_RIVE_SCRIPTING
    // Diagnostic: enumerate script assets so we can see what got
    // registered and what didn't. Walks the file's assets looking for
    // ScriptAsset entries; for each, logs name + isModule + verified
    // + bytecode size. Useful for diagnosing "require could not find
    // a script named X" errors.
    {
        int scriptCount = 0;
        for (const rive::rcp<rive::FileAsset>& asset : imported->assets())
        {
            if (!asset || !asset->is<rive::ScriptAsset>())
                continue;
            auto* sa = asset->as<rive::ScriptAsset>();
            ++scriptCount;
            qCInfo(lcRiveFile)
                << "script asset:"
                << "name=" << QString::fromStdString(sa->name())
                << "folder=" << QString::fromStdString(sa->folderPath())
                << "isModule=" << sa->isModule()
                << "verified=" << sa->verified()
                << "bytecodeBytes=" << static_cast<int>(sa->moduleBytecode().size());
        }
        if (scriptCount > 0)
            qCInfo(lcRiveFile) << "loaded" << scriptCount << "script asset(s)";
    }
#endif

    return wrapped;
}

QStringList RiveFile::artboardNames() const
{
    QStringList out;
    if (!m_file)
        return out;
    const std::size_t n = m_file->artboardCount();
    out.reserve(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i)
        out.append(QString::fromStdString(m_file->artboardNameAt(i)));
    return out;
}

int RiveFile::artboardCount() const
{
    return m_file ? static_cast<int>(m_file->artboardCount()) : 0;
}

std::unique_ptr<RiveArtboard> RiveFile::createArtboard(const QString& name) const
{
    if (!m_file)
        return nullptr;
    std::unique_ptr<rive::ArtboardInstance> instance;
    QString resolvedName = name;
    if (name.isEmpty())
    {
        instance = m_file->artboardDefault();
        if (instance && m_file->artboardCount() > 0)
            resolvedName = QString::fromStdString(m_file->artboardNameAt(0));
    }
    else
    {
        instance = m_file->artboardNamed(name.toStdString());
    }
    if (!instance)
        return nullptr;
    return std::make_unique<RiveArtboard>(std::move(instance), std::move(resolvedName));
}

rive::File* RiveFile::raw() const
{
    return m_file.get();
}

QStringList RiveFile::viewModelNames() const
{
    QStringList out;
    if (!m_file)
        return out;
    const std::size_t n = m_file->viewModelCount();
    out.reserve(static_cast<int>(n));
    for (std::size_t i = 0; i < n; ++i)
    {
        if (rive::ViewModel* vm = m_file->viewModel(i))
            out.append(QString::fromStdString(vm->name()));
    }
    return out;
}

int RiveFile::viewModelCount() const
{
    return m_file ? static_cast<int>(m_file->viewModelCount()) : 0;
}

rive::rcp<rive::ViewModelInstance>
RiveFile::createViewModelInstance(rive::ArtboardInstance* artboard,
                                  const QString& viewModelName,
                                  const QString& instanceName) const
{
    if (!m_file)
        return nullptr;

    if (viewModelName.isEmpty())
    {
        // Default path: instance follows the artboard's editor binding.
        if (!artboard)
            return nullptr;
        return m_file->createDefaultViewModelInstance(artboard);
    }

    if (instanceName.isEmpty())
        return m_file->createViewModelInstance(viewModelName.toStdString());

    return m_file->createViewModelInstance(viewModelName.toStdString(),
                                           instanceName.toStdString());
}
