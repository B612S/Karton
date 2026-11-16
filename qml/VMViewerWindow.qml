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
    // fixed window; Actual Size/Fit to Window remain available in the menu.
    width: screenInfo ? Math.min(screenInfo.width, Math.max(Kirigami.Units.gridUnit * 60, Math.min(screenInfo.width * 0.85, Kirigami.Units.gridUnit * 120))) : Kirigami.Units.gridUnit * 70
    height: screenInfo ? Math.min(screenInfo.height, Math.max(Kirigami.Units.gridUnit * 40, Math.min(screenInfo.height * 0.85, Kirigami.Units.gridUnit * 72))) : Kirigami.Units.gridUnit * 45

    property bool viewerRegistered: false

    function registerViewer() {
        if (!viewerRegistered) {
            viewerRegistered = true;
            Karton.viewerOpened();
        }
    }

    Component.onCompleted: {
        registerViewer();
        Qt.callLater(function() {
            domainViewer.forceActiveFocus();
        });
    }

    Component.onDestruction: {
        if (viewerRegistered) {
            viewerRegistered = false;
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

    onClosing: (close) => {
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.viewerClosed();
        }
        domainViewer.saveFrameToDomain();
        domainViewer.disconnectFromSpice();
        close.accepted = true;
        // The window is created dynamically from VMPage and has no parent, so
        // it has to be destroyed explicitly.
        Qt.callLater(function() {
            viewerWindow.destroy();
        });
    }

    Connections {
        target: Karton
        function onDomainRemoved(removedDomain) {
            if (removedDomain === viewerWindow.domain) {
                viewerWindow.close();
            }
        }
    }

    pageStack.initialPage: Kirigami.Page {
        title: viewerWindow.title
        padding: 0

        onWidthChanged: if (domainViewer) domainViewer.updateDisplayResolution()
        onHeightChanged: if (domainViewer) domainViewer.updateDisplayResolution()

        actions: [
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
                icon.name: "view-more-symbolic"
                onTriggered: {
                    domainViewer.releasePointerGrab();
                    viewerMenu.popup();
                }
            }
        ]

        Controls.Menu {
            id: viewerMenu

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
                text: i18n("Fit to Window")
                icon.name: "zoom-fit-best-symbolic"
                onTriggered: viewerWindow.showMaximized()
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
            Controls.MenuItem {
                text: i18n("Storage")
                icon.name: "drive-harddisk-symbolic"
                onTriggered: storageDialog.open()
            }
            Controls.MenuItem {
                text: i18n("Networks")
                icon.name: "network-wired-symbolic"
                onTriggered: networksDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: i18n("Machine")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("Settings")
                icon.name: "settings-configure-symbolic"
                onTriggered: configurationDialog.open()
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

            focus: true
            activeFocusOnTab: true
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: domainViewer.pointerGrabbed && domainViewer.errorMessage.length === 0
            text: i18n("Pointer captured. Press Esc to release it.")
            type: Kirigami.MessageType.Information
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: domainViewer.errorMessage.length > 0
            text: domainViewer.errorMessage
            type: Kirigami.MessageType.Error
        }

        UsbDevicesDialog {
            id: usbDialog
            domain: viewerWindow.domain
        }

        ConfigurationDialog {
            id: configurationDialog
            domain: viewerWindow.domain
        }

        SnapshotsDialog {
            id: snapshotsDialog
            domain: viewerWindow.domain
        }

        StorageDialog {
            id: storageDialog
            domain: viewerWindow.domain
        }

        NetworksDialog {
            id: networksDialog
            domain: viewerWindow.domain
        }
    }
}
