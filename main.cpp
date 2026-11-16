// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#include <libvirt/libvirt.h>

#include <KAboutData>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KirigamiAddons/App/KirigamiAppDefaults>
#include <QApplication>
#include <QIcon>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QTimer>
#include <QVulkanInstance>
#include <QtQml>
#include <cstdlib>

#include "karton_debug.h"
#include "karton_version.h"

static bool qmlCheckFailed = false;

static void qmlCheckMessageHandler(QtMsgType type, const QMessageLogContext &context, const QString &message)
{
    Q_UNUSED(context);
    if (type == QtWarningMsg || type == QtCriticalMsg || type == QtFatalMsg) {
        if (message.contains(QStringLiteral("qrc:/qt/qml/org/kde/karton/"))
            && (message.contains(QStringLiteral("Cannot")) || message.contains(QStringLiteral("non-existent"))
                || message.contains(QStringLiteral("unavailable")) || message.contains(QStringLiteral("failed to load"))
                || message.contains(QStringLiteral("as it is null")))) {
            qmlCheckFailed = true;
        }
    }
    fprintf(stderr, "%s\n", qPrintable(message));
}

static QSGRendererInterface::GraphicsApi preferredGraphicsApi()
{
    const QByteArray requested = qgetenv("KARTON_GRAPHICS_API").trimmed().toLower();
    if (requested == "opengl" || requested == "gl") {
        return QSGRendererInterface::OpenGL;
    }
    if (requested == "vulkan" || requested == "vk") {
        return QSGRendererInterface::Vulkan;
    }
    if (requested == "software") {
        return QSGRendererInterface::Software;
    }

    // Prefer Vulkan, but fall back to OpenGL on systems where it is not
    // available (e.g. no Vulkan driver or a headless environment).
    QVulkanInstance probe;
    if (probe.create()) {
        return QSGRendererInterface::Vulkan;
    }

    qCWarning(KARTON_DEBUG) << "Vulkan is not available, falling back to OpenGL";
    return QSGRendererInterface::OpenGL;
}

int main(int argc, char *argv[])
{
    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(KARTON_VERSION_STRING));
    KirigamiAppDefaults::apply(&app);

    const QSGRendererInterface::GraphicsApi graphicsApi = preferredGraphicsApi();
    if (graphicsApi == QSGRendererInterface::Vulkan) {
        // Needed by VulkanScanoutRenderer to import SPICE DMA-BUF scanouts.
        qputenv("QT_VULKAN_DEVICE_EXTENSIONS", "VK_KHR_external_memory_fd;VK_EXT_external_memory_dma_buf;VK_EXT_image_drm_format_modifier");
    }
    QQuickWindow::setGraphicsApi(graphicsApi);
    qCInfo(KARTON_DEBUG) << "Using graphics API:"
                         << (graphicsApi == QSGRendererInterface::Vulkan       ? "Vulkan"
                                 : graphicsApi == QSGRendererInterface::OpenGL ? "OpenGL"
                                                                               : "software");

    KLocalizedString::setApplicationDomain("karton");

    KAboutData about(QStringLiteral("karton"),
                     i18nc("Application name", "Karton"),
                     QStringLiteral(KARTON_VERSION_STRING),
                     i18nc("Application description", "Manage and run virtual machines with ease"),
                     KAboutLicense::GPL_V3,
                     i18nc("Copyright statement", "(C) KDE"));
    about.setDesktopFileName(QStringLiteral("org.kde.karton"));

    about.addAuthor(i18nc("author name", "Aaron Rainbolt"), i18nc("author role", "Initial developer"), QStringLiteral("arraybolt3@gmail.com"));
    about.addAuthor(i18nc("author name", "Derek Lin"), i18nc("author role", "Developer"), QStringLiteral("derekhongdalin@gmail.com"));
    about.setTranslator(i18nc("NAME OF TRANSLATORS", "Your names"), i18nc("EMAIL OF TRANSLATORS", "Your emails"));

    KAboutData::setApplicationData(about);
    QGuiApplication::setWindowIcon(QIcon::fromTheme(QStringLiteral("org.kde.karton")));

    QQmlApplicationEngine engine;

    qCInfo(KARTON_DEBUG) << "Hello! Starting Karton...";

    KLocalization::setupLocalizedContext(&engine);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, []() {
        // Qt Wayland/NVIDIA can abort while destroying platform windows during
        // application teardown. All Karton state has already been persisted by
        // the QML close handlers; exit once the event loop has stopped.
        std::_Exit(0);
    });

    const bool checkQml = QCoreApplication::arguments().contains(QStringLiteral("--check-qml"));
    if (checkQml) {
        qInstallMessageHandler(qmlCheckMessageHandler);
    }
    engine.loadFromModule("org.kde.karton", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // Used by the opt-in test suite to catch QML component/import errors
    // without entering the event loop or requiring a graphical session.
    if (checkQml) {
        // Main.qml alone does not instantiate VMViewerWindow and the dialogs
        // that are created only on user action. Load the smoke component too,
        // so invalid property assignments in those components fail this test.
        engine.loadFromModule("org.kde.karton", "QmlSmokeTest");
        if (engine.rootObjects().isEmpty()) {
            return -1;
        }
        // Let the event loop deliver delayed component creation and bindings;
        // otherwise a null visual item or a late binding error can escape the
        // smoke test. QML starts helper threads, so exit without Qt teardown.
        QTimer::singleShot(500, &app, []() {
            std::_Exit(qmlCheckFailed ? 1 : 0);
        });
        return app.exec();
    }

    return app.exec();
}
