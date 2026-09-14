#include <QDir>
#include <QFile>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QSignalSpy>
#include <QTest>
#include <QTransform>
#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
#include <QQuickRenderControl>
#include <QQuickRenderTarget>
#include <QScopeGuard>
#include <rhi/qrhi.h>
#endif

#include "rive_artboard.h"
#include "rive_file.h"
#include "rive_state_machine.h"
#include "rive_view.h"
#include "test_helpers.h"

#include <rive/text/text.hpp>
#include <rive/text/text_value_run.hpp>
#include <rive/viewmodel/viewmodel_instance_list.hpp>
#include <rive/viewmodel/viewmodel_instance_string.hpp>
#include <utils/no_op_factory.hpp>

namespace {
QString samplePath(const QString& name)
{
    return QDir(QStringLiteral(RIVE_QT_VIEWER_SAMPLES_DIR)).filePath(name);
}

void sampleRows()
{
    QTest::addColumn<QJsonObject>("sample");
    QFile file(QStringLiteral(RIVE_QT_UPGRADE_CATALOG));
    QVERIFY(file.open(QIODevice::ReadOnly));
    const auto samples = QJsonDocument::fromJson(file.readAll()).object()["samples"].toArray();
    QVERIFY(!samples.isEmpty());
    for (const auto& entry : samples) {
        const auto sample = entry.toObject();
        QTest::newRow(qPrintable(sample["id"].toString())) << sample;
    }
}

bool scriptingAvailable()
{
#if defined(WITH_RIVE_SCRIPTING_LUAU) && defined(WITH_RIVE_TOOLS)
    return true;
#else
    return false;
#endif
}
} // namespace

class TstRuntimeUpgrade : public QObject
{
    Q_OBJECT

private slots:
    void initTestCase()
    {
#ifdef Q_OS_MACOS
        QQuickWindow::setGraphicsApi(QSGRendererInterface::Metal);
#endif
    }

    void importsAndAdvances_data() { sampleRows(); }

    void importsAndAdvances()
    {
        QFETCH(QJsonObject, sample);
        if (sample["requiresScripting"].toBool() && !scriptingAvailable())
            QSKIP("This fixture requires scripting and Rive tools");
        rive::NoOpFactory factory;
        QString error;
        auto file = RiveFile::fromUrl(QUrl::fromLocalFile(samplePath(sample["file"].toString())),
                                      &factory, &error);
        QVERIFY2(file != nullptr, qPrintable(error));
        QVERIFY(!file->artboardNames().isEmpty());
        // Exercise every artboard offered in the viewer, including all six
        // grid/stack cases and the nested/global scripting artboards.
        for (const QString& name : file->artboardNames()) {
            auto artboard = file->createArtboard(name);
            QVERIFY2(artboard != nullptr, qPrintable(name));
            QVERIFY(artboard->width() > 0 && artboard->height() > 0);
            std::unique_ptr<RiveStateMachine> sm(artboard->createStateMachine({}));
            QVERIFY2(sm != nullptr, qPrintable(name));
            auto vm = file->createViewModelInstance(artboard->raw(),
                sample["viewModel"].toString(), sample["instance"].toString());
            if (vm) {
                artboard->bindViewModelInstance(vm);
                sm->bindViewModelInstance(vm);
            }
            for (int frame = 0; frame < 120; ++frame)
                sm->advance(1.0f / 60.0f);
            if (sample["id"].toString() == "global-scripting" && name == "Parent") {
                QVERIFY(vm);
                auto* names = vm->propertyValue("globalNames");
                QVERIFY(names && names->is<rive::ViewModelInstanceList>());
                QStringList actualNames;
                for (const auto& item : names->as<rive::ViewModelInstanceList>()->listItems()) {
                    QVERIFY(item->viewModelInstance());
                    auto* value = item->viewModelInstance()->propertyValue("label");
                    QVERIFY(value && value->is<rive::ViewModelInstanceString>());
                    actualNames.append(QString::fromStdString(
                        value->as<rive::ViewModelInstanceString>()->propertyValue()));
                }
                // The embedded script enumerates the two globals and appends
                // an intentionally named "Wrong" entry as a mutation check.
                QCOMPARE(actualNames, QStringList({"Sizes", "Colors", "Wrong"}));
            }
        }
    }

    void fittedTextUsesFittedHeight()
    {
        rive::NoOpFactory factory;
        QString error;
        auto file = RiveFile::fromUrl(
            QUrl::fromLocalFile(samplePath("upgrade-fitted-text.riv")), &factory, &error);
        QVERIFY2(file != nullptr, qPrintable(error));
        auto artboard = file->createArtboard({});
        QVERIFY(artboard);
        std::unique_ptr<RiveStateMachine> sm(artboard->createStateMachine({}));
        QVERIFY(sm);
        auto vm = file->createViewModelInstance(artboard->raw(), "MainVM", "Instance");
        QVERIFY(vm);
        sm->bindViewModelInstance(vm);
        artboard->bindViewModelInstance(vm);
        sm->advance(0);

        // Upstream's 7.4 fixture fits its 175pt title down to 121pt. The hug
        // slot must track the fitted height (~293), not the old ~423px box.
        rive::Text* title = nullptr;
        for (auto* text : artboard->raw()->find<rive::Text>()) {
            if (!text->runs().empty() && !text->runs()[0]->text().empty()) {
                title = text;
                break;
            }
        }
        QVERIFY(title);
        QVERIFY(title->parent()->is<rive::LayoutComponent>());
        const float height = title->parent()->as<rive::LayoutComponent>()->layoutHeight();
        QVERIFY2(qAbs(height - 292.81f) < 0.5f, qPrintable(QString::number(height)));
    }

#if defined(Q_OS_MACOS) && QT_VERSION >= QT_VERSION_CHECK(6, 6, 0)
    void rendersSample_data() { sampleRows(); }

    void rendersSample()
    {
        QFETCH(QJsonObject, sample);
        if (sample["requiresScripting"].toBool() && !scriptingAvailable())
            QSKIP("This fixture requires scripting and Rive tools");
        // Hidden QQuickWindow::grabWindow() can recreate the scene graph on
        // every capture, resetting the animation. RenderControl keeps one
        // GPU context alive while we explicitly render and read back frames.
        QQuickRenderControl control;
        QQuickWindow window(&control);
        window.resize(640, 480);
        window.setColor(QColor("#0a0a0a"));
        auto* view = new RiveView(window.contentItem());
        view->setWidth(640);
        view->setHeight(480);
        view->setFit(RiveView::Fit::Contain);
        view->setViewModelName(sample["viewModel"].toString());
        view->setViewModelInstanceName(sample["instance"].toString());
        QSignalSpy failed(view, &RiveView::loadFailed);
        view->setSource(QUrl::fromLocalFile(samplePath(sample["file"].toString())));
        if (!control.initialize())
            QSKIP("Offscreen Metal rendering unavailable on this host");
        auto* rhi = control.rhi();
        QVERIFY(rhi);
        const QSize size(640, 480);
        std::unique_ptr<QRhiTexture> texture(rhi->newTexture(QRhiTexture::RGBA8, size, 1,
            QRhiTexture::RenderTarget | QRhiTexture::UsedAsTransferSource));
        QVERIFY(texture->create());
        std::unique_ptr<QRhiRenderBuffer> depth(rhi->newRenderBuffer(
            QRhiRenderBuffer::DepthStencil, size, 1));
        QVERIFY(depth->create());
        QRhiTextureRenderTargetDescription description{QRhiColorAttachment(texture.get())};
        description.setDepthStencilBuffer(depth.get());
        std::unique_ptr<QRhiTextureRenderTarget> target(rhi->newTextureRenderTarget(description));
        std::unique_ptr<QRhiRenderPassDescriptor> pass(target->newCompatibleRenderPassDescriptor());
        target->setRenderPassDescriptor(pass.get());
        QVERIFY(target->create());
        window.setRenderTarget(QQuickRenderTarget::fromRhiRenderTarget(target.get()));
        const auto cleanup = qScopeGuard([&] {
            window.setRenderTarget({});
            control.invalidate();
        });
        const auto capture = [&]() {
            control.polishItems();
            control.beginFrame();
            control.sync();
            control.render();
            QRhiReadbackResult result;
            auto* batch = rhi->nextResourceUpdateBatch();
            batch->readBackTexture(texture.get(), &result);
            control.commandBuffer()->resourceUpdate(batch);
            control.endFrame(); // Offscreen readbacks complete before returning.
            if (result.data.isEmpty())
                return QImage();
            QImage image(reinterpret_cast<const uchar*>(result.data.constData()),
                         size.width(), size.height(), QImage::Format_RGBA8888_Premultiplied);
            return rhi->isYUpInFramebuffer()
                ? image.transformed(QTransform::fromScale(1, -1)) : image.copy();
        };

        QImage first;
        for (int frame = 0; frame < 150; ++frame) {
            QTest::qWait(16);
            first = capture();
            QVERIFY2(failed.isEmpty(), failed.isEmpty() ? "" : qPrintable(failed[0][0].toString()));
            if (!first.isNull() && riveqt_test::nonUniformFraction(first) > 0.02)
                break;
        }
        QVERIFY(!first.isNull());
        QVERIFY2(riveqt_test::nonUniformFraction(first) > 0.02,
                 "Sample rendered a blank frame");

        QImage last;
        double maxDifference = 0;
        for (int frame = 0; frame < 60; ++frame) {
            QTest::qWait(16);
            last = capture();
            QVERIFY(!last.isNull());
            maxDifference = qMax(maxDifference, riveqt_test::meanChannelDifference(first, last));
        }
        QVERIFY(failed.isEmpty());
        if (sample["checkAnimation"].toBool())
            QVERIFY2(maxDifference > 0.1, "Scripted clip froze instead of advancing");

        const QString snapshots = qEnvironmentVariable("RIVE_QT_SAMPLE_SNAPSHOT_DIR");
        if (!snapshots.isEmpty()) {
            QVERIFY(QDir().mkpath(snapshots));
            QVERIFY(last.save(QDir(snapshots).filePath(sample["id"].toString() + ".png")));
        }
    }
#endif
};

QTEST_MAIN(TstRuntimeUpgrade)
#include "tst_runtime_upgrade.moc"
