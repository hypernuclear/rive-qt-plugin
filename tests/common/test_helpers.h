#ifndef RIVE_QT_TEST_HELPERS_H
#define RIVE_QT_TEST_HELPERS_H

// Shared test utilities for the rive-qt wrapper suite.
//
//  - Fixture resolution: turn a bare ".riv" name into an absolute path /
//    file:// URL / byte buffer, using the RIVE_QT_TEST_FIXTURES_DIR compile
//    definition so tests don't depend on the working directory.
//  - Image snapshots: compare a rendered QImage against a stored baseline
//    with a loose mean-channel-difference tolerance (render output varies
//    slightly across GPUs/drivers). Regenerate baselines by running with the
//    UPDATE_SNAPSHOTS environment variable set (mirrors hypershot's pattern).

#include <QByteArray>
#include <QImage>
#include <QString>
#include <QUrl>

namespace riveqt_test {

// Absolute path to a fixture file, e.g. fixturePath("clean-knob.riv").
QString fixturePath(const QString& name);

// file:// URL for a fixture — what RiveFile::fromUrl / fromBytes expects.
QUrl fixtureUrl(const QString& name);

// Read a fixture's raw bytes. Empty QByteArray (and a qWarning) on failure.
QByteArray loadFixtureBytes(const QString& name);

// True when UPDATE_SNAPSHOTS is set in the environment — baselines are
// (re)written instead of compared.
bool isSnapshotUpdateMode();

// Compare `actual` against the baseline PNG at
// RIVE_QT_TEST_SNAPSHOTS_DIR/<relativePath>.
//
//  - Update mode: writes `actual` as the baseline, returns true.
//  - Missing baseline (compare mode): writes it, returns false with a
//    message in *diffOut telling the caller to re-run / commit it.
//  - Otherwise: returns true iff dimensions match and the mean per-channel
//    absolute difference is <= maxMeanDiff (0..255). *diffOut gets the
//    measured difference on mismatch.
bool compareImageSnapshot(const QImage& actual,
                          const QString& relativePath,
                          double maxMeanDiff = 4.0,
                          QString* diffOut = nullptr);

// Mean per-channel absolute difference (0..255) between two images, or a
// large sentinel (255.0) if their sizes differ. Exposed for tests that want
// to assert directly without a stored baseline.
double meanChannelDifference(const QImage& a, const QImage& b);

// Fraction of pixels (0..1) that differ from the image's top-left pixel by
// more than `threshold` per channel. ~0 means a flat/solid fill — the
// signature of a failed texture upload or a blank frame.
double nonUniformFraction(const QImage& image, int threshold = 8);

} // namespace riveqt_test

#endif // RIVE_QT_TEST_HELPERS_H
