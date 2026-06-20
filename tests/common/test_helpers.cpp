#include "test_helpers.h"

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QtGlobal>

#include <cmath>
#include <cstdlib>

#ifndef RIVE_QT_TEST_FIXTURES_DIR
#error "RIVE_QT_TEST_FIXTURES_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif
#ifndef RIVE_QT_TEST_SNAPSHOTS_DIR
#error "RIVE_QT_TEST_SNAPSHOTS_DIR must be defined by the build (see tests/CMakeLists.txt)"
#endif

namespace riveqt_test {

QString fixturePath(const QString& name)
{
    return QDir(QStringLiteral(RIVE_QT_TEST_FIXTURES_DIR)).filePath(name);
}

QUrl fixtureUrl(const QString& name)
{
    return QUrl::fromLocalFile(fixturePath(name));
}

QByteArray loadFixtureBytes(const QString& name)
{
    QFile f(fixturePath(name));
    if (!f.open(QIODevice::ReadOnly))
    {
        qWarning("riveqt_test: cannot open fixture '%s': %s",
                 qPrintable(name), qPrintable(f.errorString()));
        return {};
    }
    return f.readAll();
}

bool isSnapshotUpdateMode()
{
    return qEnvironmentVariableIsSet("UPDATE_SNAPSHOTS");
}

static QString snapshotPath(const QString& relativePath)
{
    return QDir(QStringLiteral(RIVE_QT_TEST_SNAPSHOTS_DIR)).filePath(relativePath);
}

double meanChannelDifference(const QImage& a, const QImage& b)
{
    if (a.size() != b.size() || a.isNull() || b.isNull())
        return 255.0;

    const QImage ia = a.convertToFormat(QImage::Format_RGBA8888);
    const QImage ib = b.convertToFormat(QImage::Format_RGBA8888);

    quint64 total = 0;
    const int w = ia.width();
    const int h = ia.height();
    for (int y = 0; y < h; ++y)
    {
        const uchar* ra = ia.constScanLine(y);
        const uchar* rb = ib.constScanLine(y);
        for (int x = 0; x < w * 4; ++x)
            total += static_cast<quint64>(std::abs(int(ra[x]) - int(rb[x])));
    }
    const double count = static_cast<double>(w) * h * 4.0;
    return count > 0.0 ? static_cast<double>(total) / count : 0.0;
}

double nonUniformFraction(const QImage& image, int threshold)
{
    if (image.isNull())
        return 0.0;
    const QImage img = image.convertToFormat(QImage::Format_RGBA8888);
    const int w = img.width();
    const int h = img.height();
    if (w == 0 || h == 0)
        return 0.0;

    const uchar* first = img.constScanLine(0); // top-left reference pixel
    const int r0 = first[0], g0 = first[1], b0 = first[2], a0 = first[3];

    quint64 differing = 0;
    for (int y = 0; y < h; ++y)
    {
        const uchar* row = img.constScanLine(y);
        for (int x = 0; x < w; ++x)
        {
            const uchar* p = row + x * 4;
            if (std::abs(int(p[0]) - r0) > threshold ||
                std::abs(int(p[1]) - g0) > threshold ||
                std::abs(int(p[2]) - b0) > threshold ||
                std::abs(int(p[3]) - a0) > threshold)
                ++differing;
        }
    }
    return static_cast<double>(differing) / (static_cast<double>(w) * h);
}

bool compareImageSnapshot(const QImage& actual,
                          const QString& relativePath,
                          double maxMeanDiff,
                          QString* diffOut)
{
    const QString path = snapshotPath(relativePath);

    const auto writeBaseline = [&](const char* why) {
        QDir().mkpath(QFileInfo(path).absolutePath());
        if (!actual.save(path, "PNG"))
        {
            if (diffOut)
                *diffOut = QStringLiteral("failed to write baseline %1").arg(path);
            return false;
        }
        qInfo("riveqt_test: %s baseline %s", why, qPrintable(path));
        return true;
    };

    if (isSnapshotUpdateMode())
        return writeBaseline("updated");

    if (!QFileInfo::exists(path))
    {
        writeBaseline("created missing");
        if (diffOut)
            *diffOut = QStringLiteral(
                           "baseline %1 did not exist; wrote it — re-run to "
                           "compare, and commit the snapshot")
                           .arg(path);
        return false;
    }

    QImage baseline(path);
    if (baseline.isNull())
    {
        if (diffOut)
            *diffOut = QStringLiteral("could not load baseline %1").arg(path);
        return false;
    }

    if (baseline.size() != actual.size())
    {
        if (diffOut)
            *diffOut = QStringLiteral("size mismatch: baseline %1x%2 vs actual %3x%4")
                           .arg(baseline.width()).arg(baseline.height())
                           .arg(actual.width()).arg(actual.height());
        return false;
    }

    const double diff = meanChannelDifference(actual, baseline);
    if (diff > maxMeanDiff)
    {
        if (diffOut)
            *diffOut = QStringLiteral("mean channel diff %1 exceeds tolerance %2")
                           .arg(diff).arg(maxMeanDiff);
        return false;
    }
    return true;
}

} // namespace riveqt_test
