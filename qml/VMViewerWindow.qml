// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

import QtQuick
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.ApplicationWindow {
    id: viewerWindow
    required property Domain domain

    title: domain ? i18nc("%1 is the name of the virtual machine", "VM Viewer - %1", domain.config.name) : i18n("VM Viewer")

    readonly property var screenInfo: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null

    // Open large enough for a modern guest resolution instead of a small
    // fixed window; Actual Size and Fit Guest to Window remain available.
    width: screenInfo ? Math.min(screenInfo.width, Math.max(Kirigami.Units.gridUnit * 60, Math.min(screenInfo.width * 0.85, Kirigami.Units.gridUnit * 120))) : Kirigami.Units.gridUnit * 70
    height: screenInfo ? Math.min(screenInfo.height, Math.max(Kirigami.Units.gridUnit * 40, Math.min(screenInfo.height * 0.85, Kirigami.Units.gridUnit * 72))) : Kirigami.Units.gridUnit * 45

    property bool viewerRegistered: false
    readonly property bool isRunning: domain ? domain.isActive : false
    property bool wasRunning: false
    property bool sizeRestored: false
    property alias domainViewerItem: domainViewer
    property int fixedContentWidth: 0
    property int fixedContentHeight: 0

    function registerViewer() {
        if (!viewerRegistered) {
            viewerRegistered = true;
            Karton.viewerOpened();
            Karton.registerViewer(viewerWindow.domain, viewerWindow);
        }
    }

    function fitWindowToGuest() {
        if (fixedContentWidth > 0 || domainViewer.agentConnected || domainViewer.implicitWidth <= 0 || domainViewer.implicitHeight <= 0 || !viewerWindow.visible) {
            return;
        }
        const aspect = domainViewer.implicitWidth / domainViewer.implicitHeight;
        const chrome = Math.max(0, viewerWindow.height - domainViewer.height);
        const maxContentHeight = Screen.height * 0.85 - chrome;
        let contentWidth = viewerWindow.width;
        let contentHeight = contentWidth / aspect;
        if (contentHeight > maxContentHeight) {
            contentHeight = maxContentHeight;
            contentWidth = contentHeight * aspect;
        }
        viewerWindow.width = Math.round(contentWidth);
        viewerWindow.height = Math.round(contentHeight + chrome);
    }

    function applyFixedWindowSize() {
        if (fixedContentWidth <= 0 || fixedContentHeight <= 0) {
            return;
        }
        const scale = Math.min(1.0, Screen.width * 0.9 / fixedContentWidth, Screen.height * 0.85 / fixedContentHeight);
        const contentWidth = Math.round(fixedContentWidth * scale);
        const contentHeight = Math.round(fixedContentHeight * scale);
        const chromeHeight = Math.max(48, viewerWindow.height - domainViewer.height);
        const windowWidth = Math.max(320, contentWidth);
        const windowHeight = Math.max(240, contentHeight + chromeHeight);
        viewerWindow.minimumWidth = windowWidth;
        viewerWindow.maximumWidth = windowWidth;
        viewerWindow.minimumHeight = windowHeight;
        viewerWindow.maximumHeight = windowHeight;
        viewerWindow.width = windowWidth;
        viewerWindow.height = windowHeight;
    }

    Component.onCompleted: {
        registerViewer();
        const resolution = Karton.domainDisplayResolution(domain);
        if (resolution.width > 0 && resolution.height > 0) {
            fixedContentWidth = resolution.width;
            fixedContentHeight = resolution.height;
            domainViewer.setFixedDisplayResolution(resolution.width, resolution.height);
        }
        domainViewer.audioPlaybackEnabled = Karton.audioPlaybackEnabled(domain);
        domainViewer.microphoneEnabled = Karton.microphoneEnabled(domain);
        const savedSize = Karton.viewerWindowSize(domain);
        if (savedSize.width > 0 && savedSize.height > 0) {
            width = Math.max(320, Math.min(savedSize.width, Screen.width * 0.95));
            height = Math.max(240, Math.min(savedSize.height, Screen.height * 0.95));
        }
        sizeRestored = true;
        if (fixedContentWidth > 0) {
            Qt.callLater(applyFixedWindowSize);
        }
        wasRunning = isRunning;
        Qt.callLater(function() {
            domainViewer.forceActiveFocus();
        });
    }

    onWidthChanged: if (sizeRestored) viewerSizeTimer.restart()
    onHeightChanged: if (sizeRestored) viewerSizeTimer.restart()

    Component.onDestruction: {
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
    }

    onActiveChanged: {
        if (active) {
            Qt.callLater(function() {
                domainViewer.forceActiveFocus();
            });
        }
    }

    onVisibleChanged: {
        if (visible) {
            Qt.callLater(fitWindowToGuest);
        }
    }

    onClosing: (close) => {
        Karton.saveViewerWindowSize(viewerWindow.domain, viewerWindow.width, viewerWindow.height);
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
        if (viewerWindow.isRunning) {
            domainViewer.saveFrameToDomain();
        }
        domainViewer.disconnectFromSpice();
        close.accepted = true;
        // The window is created dynamically from VMPage and has no parent, so
        // it has to be destroyed explicitly.
        Qt.callLater(function() {
            viewerWindow.destroy();
        });
    }

    Timer {
        id: viewerSizeTimer
        interval: 500
        repeat: false
        onTriggered: Karton.saveViewerWindowSize(viewerWindow.domain, viewerWindow.width, viewerWindow.height)
    }

    Connections {
        target: Karton
        function onDomainRemoved(removedDomain) {
            if (removedDomain === viewerWindow.domain) {
                viewerWindow.close();
            }
        }
    }

    Connections {
        target: Karton
        function onClipboardSettingsChanged() {
            domainViewer.clipboardTextMode = Karton.clipboardTextMode();
            domainViewer.clipboardFilesMode = Karton.clipboardFilesMode();
        }
    }

    Connections {
        target: viewerWindow.domain
        function onIsActiveChanged(active) {
            if (active) {
                viewerWindow.wasRunning = true;
                domainViewer.setupSpiceSession();
            } else if (viewerWindow.wasRunning) {
                viewerWindow.close();
            }
        }
    }

    pageStack.initialPage: Kirigami.Page {
        title: viewerWindow.title
        padding: 0

        onWidthChanged: if (domainViewer) domainViewer.updateDisplayResolution()
        onHeightChanged: if (domainViewer) domainViewer.updateDisplayResolution()

        // If the guest agent is absent, the guest cannot resize itself. Match
        // the window to the guest frame once so the display is not letterboxed
        // by black bars just because the window happened to open at 16:9.
        Connections {
            target: domainViewer
            function onImplicitWidthChanged() {
                viewerWindow.fitWindowToGuest();
            }
            function onImplicitHeightChanged() {
                viewerWindow.fitWindowToGuest();
            }
        }

        // Keep the main page preview in sync with what is currently on screen
        // without taking a QEMU screenshot (which would pause the guest).
        Timer {
            id: viewerPreviewTimer
            interval: 1000
            repeat: true
            running: viewerWindow.visible && viewerWindow.isRunning
            triggeredOnStart: true
            onTriggered: domainViewer.updateDomainPreview()
        }

        actions: [
            Kirigami.Action {
                text: viewerWindow.isRunning ? i18n("Shut Down") : i18n("Start")
                icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                onTriggered: {
                    if (viewerWindow.isRunning) {
                        Karton.stopDomain(viewerWindow.domain);
                    } else if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Starting VM: %1!", viewerWindow.domain.config.name));
                    }
                }
            },
            Kirigami.Action {
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: powerMenu.popup()
            },
            Kirigami.Action {
                text: viewerWindow.domain && viewerWindow.domain.isPaused ? i18n("Resume") : i18n("Pause")
                icon.name: viewerWindow.domain && viewerWindow.domain.isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    if (!viewerWindow.domain) {
                        return;
                    }
                    if (viewerWindow.domain.isPaused) {
                        Karton.resumeDomain(viewerWindow.domain);
                    } else {
                        Karton.pauseDomain(viewerWindow.domain);
                    }
                }
            },
            Kirigami.Action {
                text: i18n("Reboot")
                icon.name: "system-reboot-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: Karton.rebootDomain(viewerWindow.domain)
            },
            Kirigami.Action {
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: rebootMenu.popup()
            },
            Kirigami.Action {
                icon.name: "view-fullscreen-symbolic"
                onTriggered: {
                    if (viewerWindow.visibility === Window.FullScreen) {
                        viewerWindow.showNormal();
                    } else {
                        viewerWindow.showFullScreen();
                    }
                }
            },
            Kirigami.Action {
                text: i18n("More")
                icon.name: "view-more-symbolic"
                onTriggered: {
                    domainViewer.releasePointerGrab();
                    viewerMenu.popup();
                }
            }
        ]

        Connections {
            target: Karton
            function onClosePopupsRequested() {
                powerMenu.close();
                rebootMenu.close();
                viewerMenu.close();
            }
        }

        Controls.Menu {
            id: powerMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

            Controls.MenuItem {
                text: i18n("Power Off")
                icon.name: "process-stop-symbolic"
                onTriggered: Karton.forceStopDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: rebootMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

            Controls.MenuItem {
                text: i18n("Reset")
                icon.name: "system-reboot-symbolic"
                onTriggered: Karton.resetDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: viewerMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

            Controls.MenuItem {
                text: i18n("Display")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("Actual Size")
                icon.name: "zoom-original-symbolic"
                onTriggered: {
                    viewerWindow.width = Math.max(320, domainViewer.implicitWidth / domainViewer.dprHelper.devicePixelRatio);
                    viewerWindow.height = Math.max(240, domainViewer.implicitHeight / domainViewer.dprHelper.devicePixelRatio + pageStack.globalToolBar.height);
                }
            }
            Controls.MenuItem {
                text: i18n("Fit Guest to Window")
                icon.name: "transform-scale-symbolic"
                onTriggered: domainViewer.fitGuestToWindow()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: i18n("Devices")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("USB Devices")
                icon.name: "drive-removable-media-usb-symbolic"
                onTriggered: usbDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: i18n("Machine")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("Hibernate")
                icon.name: "document-save-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    if (Karton.saveDomainState(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Hibernating VM: %1", viewerWindow.domain.config.name));
                    }
                }
            }
            Controls.MenuItem {
                text: i18n("Settings")
                icon.name: "settings-configure-symbolic"
                onTriggered: settingsDialog.open()
            }
            Controls.MenuItem {
                text: i18n("Snapshots")
                icon.name: "camera-photo-symbolic"
                onTriggered: snapshotsDialog.open()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "black"
        }

        DomainViewer {
            id: domainViewer

            property DevicePixelRatioHelper dprHelper: DevicePixelRatioHelper {
                window: domainViewer.Window.window
            }

            readonly property real frameAspect: (implicitWidth > 0 && implicitHeight > 0) ? implicitWidth / implicitHeight : (16.0 / 9.0)

            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height * frameAspect)
            height: width / frameAspect

            domain: viewerWindow.domain
            spiceTlsEnabled: Karton.spiceTlsEnabled()
            spiceCaCertificate: Karton.spiceCaCertificate()
            clipboardTextMode: Karton.clipboardTextMode()
            clipboardFilesMode: Karton.clipboardFilesMode()
            visible: viewerWindow.isRunning
            enabled: viewerWindow.isRunning

            focus: true
            activeFocusOnTab: true
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.pointerGrabbed && domainViewer.errorMessage.length === 0
            text: i18n("Pointer captured. Press Esc to release it.")
            type: Kirigami.MessageType.Information
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.errorMessage.length > 0
            text: domainViewer.errorMessage
            type: Kirigami.MessageType.Error
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && !domainViewer.agentConnected
                && !Karton.guestAgentWarningDismissed && domainViewer.errorMessage.length === 0
            text: i18n("Guest agent is not connected. Automatic resize requires spice-vdagent in the guest.")
            type: Kirigami.MessageType.Warning
            showCloseButton: true
            onVisibleChanged: {
                if (!visible && viewerWindow.isRunning && !domainViewer.agentConnected) {
                    Karton.guestAgentWarningDismissed = true;
                }
            }
        }

        Kirigami.Icon {
            id: startIcon
            anchors.centerIn: parent
            visible: !viewerWindow.isRunning
            source: "media-playback-start-symbolic"
            width: Kirigami.Units.gridUnit * 8
            height: width
            opacity: startMouseArea.containsMouse ? 0.75 : 1.0

            MouseArea {
                id: startMouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Starting VM: %1!", viewerWindow.domain.config.name));
                    }
                }
            }
        }

        UsbDevicesDialog {
            id: usbDialog
            domain: viewerWindow.domain
        }

        SettingsDialog {
            id: settingsDialog
            domain: viewerWindow.domain
            parentWindow: viewerWindow
        }

        SnapshotsDialog {
            id: snapshotsDialog
            domain: viewerWindow.domain
        }

    }
}
