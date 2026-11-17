// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include <libvirt/libvirt.h>

#include <KAboutData>
#include <KLocalizedQmlContext>
#include <KLocalizedString>
#include <KirigamiAddons/App/KirigamiAppDefaults>
#include <QApplication>
#include <QDir>
#include <QIcon>
#include <QLocalServer>
#include <QLocalSocket>
#include <QLockFile>
#include <QQmlApplicationEngine>
#include <QQuickWindow>
#include <QSGRendererInterface>
#include <QStandardPaths>
#include <QSurfaceFormat>
#include <QThread>
#include <QTimer>
#include <QWindow>
#include <QtQml>
#include <cstdlib>
#include <memory>

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
    if (requested == "software") {
        return QSGRendererInterface::Software;
    }
    if (requested == "vulkan" || requested == "vk") {
        // Qt's Vulkan/Wayland swapchain stops presenting after the first
        // fullscreen transition (Qt 6.12 + NVIDIA), so Vulkan is not supported.
        qCWarning(KARTON_DEBUG) << "Vulkan rendering is not supported anymore; using OpenGL";
    }
    return QSGRendererInterface::OpenGL;
}

static void raiseMainWindow(int attempts = 10)
{
    const QWindowList windows = QGuiApplication::topLevelWindows();
    for (QWindow *window : windows) {
        if (window->objectName() != QStringLiteral("mainWindow")) {
            continue;
        }
        if (window->visibility() == QWindow::Minimized) {
            window->showNormal();
        } else {
            window->show();
        }
        window->raise();
        window->requestActivate();
        return;
    }
    if (attempts > 0) {
        // The second instance can reach the socket while the first one is
        // still constructing its main window.
        QTimer::singleShot(100, [attempts]() {
            raiseMainWindow(attempts - 1);
        });
    }
}

static bool notifyRunningInstance(const QString &socketName)
{
    QLocalSocket socket;
    for (int attempt = 0; attempt < 10; ++attempt) {
        socket.connectToServer(socketName);
        if (socket.waitForConnected(100)) {
            socket.write("raise");
            socket.flush();
            socket.waitForBytesWritten(200);
            socket.disconnectFromServer();
            return true;
        }
        QThread::msleep(50);
    }
    return false;
}

int main(int argc, char *argv[])
{
    QSurfaceFormat format = QSurfaceFormat::defaultFormat();
    // Present on the compositor's frame callbacks / swap interval and avoid
    // tearing. The guest image does not need multisampling.
    format.setSwapBehavior(QSurfaceFormat::DoubleBuffer);
    format.setSwapInterval(1);
    format.setSamples(0);
    QSurfaceFormat::setDefaultFormat(format);

    QApplication app(argc, argv);
    app.setApplicationVersion(QStringLiteral(KARTON_VERSION_STRING));
    KirigamiAppDefaults::apply(&app);

    const QSGRendererInterface::GraphicsApi graphicsApi = preferredGraphicsApi();
    QQuickWindow::setGraphicsApi(graphicsApi);
    qCInfo(KARTON_DEBUG) << "Using graphics API:" << (graphicsApi == QSGRendererInterface::OpenGL ? "OpenGL" : "software");

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

    const bool checkQml = QCoreApplication::arguments().contains(QStringLiteral("--check-qml"));

    // Only one main window may exist. Viewers are separate windows inside the
    // primary process, so a second process is never needed for them.
    std::unique_ptr<QLockFile> instanceLock;
    std::unique_ptr<QLocalServer> instanceServer;
    if (!checkQml) {
        QString runtimeDir = QStandardPaths::writableLocation(QStandardPaths::RuntimeLocation);
        if (runtimeDir.isEmpty()) {
            runtimeDir = QDir::tempPath();
        }
        const QString lockPath = runtimeDir + QStringLiteral("/karton.lock");
        const QString socketName = runtimeDir + QStringLiteral("/karton.socket");

        instanceLock = std::make_unique<QLockFile>(lockPath);
        instanceLock->setStaleLockTime(0);
        if (!instanceLock->tryLock(100)) {
            qCInfo(KARTON_DEBUG) << "Another Karton instance is already running.";
            notifyRunningInstance(socketName);
            return 0;
        }

        instanceServer = std::make_unique<QLocalServer>();
        QLocalServer::removeServer(socketName);
        if (instanceServer->listen(socketName)) {
            QObject::connect(instanceServer.get(), &QLocalServer::newConnection, instanceServer.get(), [server = instanceServer.get()]() {
                while (QLocalSocket *socket = server->nextPendingConnection()) {
                    socket->readAll();
                    raiseMainWindow();
                    socket->disconnectFromServer();
                    socket->deleteLater();
                }
            });
        } else {
            qCWarning(KARTON_DEBUG) << "Failed to start the single-instance server:" << instanceServer->errorString();
        }
    }

    QQmlApplicationEngine engine;

    qCInfo(KARTON_DEBUG) << "Hello! Starting Karton...";

    KLocalization::setupLocalizedContext(&engine);
    QObject::connect(&app, &QCoreApplication::aboutToQuit, [&instanceLock]() {
        if (instanceLock) {
            instanceLock->unlock();
        }
        // Qt Wayland/NVIDIA can abort while destroying platform windows during
        // application teardown. All Karton state has already been persisted by
        // the QML close handlers; exit once the event loop has stopped.
        std::_Exit(0);
    });

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
