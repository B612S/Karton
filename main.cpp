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
#include <QVulkanInstance>
#include <QtQml>
#include <cstdlib>

#include "karton_debug.h"
#include "karton_version.h"

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
    const bool checkQml = QCoreApplication::arguments().contains(QStringLiteral("--check-qml"));
    engine.loadFromModule("org.kde.karton", "Main");

    if (engine.rootObjects().isEmpty()) {
        return -1;
    }

    // Used by the opt-in test suite to catch QML component/import errors
    // without entering the event loop or requiring a graphical session.
    if (checkQml) {
        // QML starts helper threads and Qt's normal teardown can wait on them
        // in a headless test run. The smoke test only needs the load result.
        std::_Exit(0);
    }

    return app.exec();
}
