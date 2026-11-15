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

    width: Kirigami.Units.gridUnit * 53
    height: Kirigami.Units.gridUnit * 36

    onClosing: (close) => {
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

        actions: [
            Kirigami.Action {
                text: i18n("Actual Size")
                icon.name: "zoom-original"
                onTriggered: {
                    viewerWindow.width = Math.max(320, domainViewer.implicitWidth / domainViewer.dprHelper.devicePixelRatio);
                    viewerWindow.height = Math.max(240, domainViewer.implicitHeight / domainViewer.dprHelper.devicePixelRatio + pageStack.globalToolBar.height);
                }
            },
            Kirigami.Action {
                text: i18n("Fit to Window")
                icon.name: "zoom-fit-best"
                onTriggered: viewerWindow.showMaximized()
            },
            Kirigami.Action {
                text: i18n("Release Pointer")
                icon.name: "transform-browse"
                visible: domainViewer.pointerGrabbed
                onTriggered: domainViewer.releasePointerGrab()
            },
            Kirigami.Action {
                icon.name: "view-fullscreen"
                onTriggered: {
                    if (viewerWindow.visibility === Window.FullScreen) {
                        viewerWindow.showNormal();
                    } else {
                        viewerWindow.showFullScreen();
                    }
                }
            }
        ]

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
    }
}
