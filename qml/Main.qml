// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as QQC2
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    objectName: "mainWindow"

    // The largest settings/add-hardware dialogs need roughly this much room
    // once their own 48 px margins are subtracted; anything smaller clips
    // controls or the dialog footer.
    readonly property int minimumWindowWidth: Kirigami.Units.gridUnit * 44
    readonly property int minimumWindowHeight: Kirigami.Units.gridUnit * 33
    width: Math.max(Kirigami.Units.gridUnit * 50, minimumWindowWidth)
    height: Math.max(Kirigami.Units.gridUnit * 35, minimumWindowHeight)
    minimumWidth: minimumWindowWidth
    minimumHeight: minimumWindowHeight

    property bool sizeRestored: false




    // Viewer windows outlive the VM page that opens them. Creating them from
    // a page's QML context leaves that context invalid as soon as the page is
    // destroyed (switching to another VM), after which calls such as
    // showViewer()/requestMinimize() fail with "invalid context". The main
    // window lives for the whole application, so its context is safe.
    property var viewerWindowComponent: null

    function createViewerWindow(domain, previewMode) {
        if (!domain) {
            return null;
        }
        if (!viewerWindowComponent) {
            viewerWindowComponent = Qt.createComponent("VMViewerWindow.qml", Component.PreferSynchronous);
        }
        if (!viewerWindowComponent || viewerWindowComponent.status !== Component.Ready) {
            console.error("Failed to create VM viewer:",
                          viewerWindowComponent ? viewerWindowComponent.errorString() : "no component");
            return null;
        }
        const window = viewerWindowComponent.createObject(null, {
            domain: domain,
            previewMode: previewMode === true,
            // The viewer maps itself once its geometry and fullscreen mode are
            // known, so its first frame is already in the final state.
            visible: false
        });
        if (!window) {
            console.error("Failed to create VM viewer window");
        }
        return window;
    }

    // Karton always uses the two-column desktop layout: the VM list is the
    // fixed left column and the details page the right one. Keeping the mode
    // fixed prevents the list from disappearing when a VM is opened.
    pageStack.columnView.columnResizeMode: Kirigami.ColumnView.FixedColumns
    pageStack.columnView.columnWidth: Kirigami.Units.gridUnit * 16

    // Keep the VM list column visible when a details page is pushed or the
    // window is resized. Kirigami's ColumnView may scroll horizontally to the
    // last column; that left the list off-screen (the reported "panel
    // disappeared" regression). Re-assert the fixed mode and reset the
    // horizontal scroll on every navigation/resize.
    Connections {
        target: pageStack
        function onCurrentItemChanged() {
            pageStack.columnView.columnResizeMode = Kirigami.ColumnView.FixedColumns;
            pageStack.columnView.contentX = 0;
        }
        function onDepthChanged() {
            pageStack.columnView.columnResizeMode = Kirigami.ColumnView.FixedColumns;
            pageStack.columnView.contentX = 0;
        }
    }

    title: qsTr("karton_virtual_machine_manager")

    pageStack.initialPage: VMListPage { }

    ErrorLogDialog {
        id: errorLogDialog
    }

    BusyOperationDialog {
        id: busyOperationDialog
        windowActive: root.active
        domain: pageStack.lastItem && pageStack.lastItem.domain !== undefined ? pageStack.lastItem.domain : null
    }

    Connections {
        target: Karton
        function onErrorOccurred(message) {
            errorLogDialog.recordError(message);
        }
    }

    Shortcut {
        sequence: "Ctrl+Shift+E"
        onActivated: errorLogDialog.open()
    }

    Component.onCompleted: {
        const saved = Karton.mainWindowSize();
        if (saved.width > 0 && saved.height > 0) {
            width = Math.max(minimumWindowWidth, Math.min(saved.width, Screen.width * 0.95));
            height = Math.max(minimumWindowHeight, Math.min(saved.height, Screen.height * 0.95));
        }
        sizeRestored = true;
        // Compile the viewer component once the window is up instead of
        // blocking the first "open VM" click.
        Qt.callLater(function() {
            if (!viewerWindowComponent) {
                viewerWindowComponent = Qt.createComponent("VMViewerWindow.qml", Component.PreferSynchronous);
                if (viewerWindowComponent && viewerWindowComponent.status === Component.Error) {
                    // Do not cache a failed preload: createViewerWindow() will
                    // retry and report the real error when the user opens a VM.
                    viewerWindowComponent = null;
                }
            }
        });
    }

    onWidthChanged: {
        if (sizeRestored) {
            mainSizeTimer.restart();
        }
        pageStack.columnView.contentX = 0;
    }
    onHeightChanged: if (sizeRestored) mainSizeTimer.restart()
    onClosing: (close) => {
        // Destroying the platform window from inside the close event deadlocks
        // in QWaylandWindow::reset() (Qt Wayland) and aborts Karton. Ignore the
        // event and hide the window instead.
        close.accepted = false;
        Karton.saveMainWindowSize(width, height);
        if (Karton.viewerCount > 0) {
            // Keep the VM viewer windows (and the VM displays) alive. The
            // application quits when the last viewer is closed.
            root.hide();
            return;
        }
        // aboutToQuit exits the process before Qt tears its windows down.
        root.hide();
        Qt.quit();
    }

    Connections {
        target: Karton
        function onViewerCountChanged() {
            // The main window is hidden and the last VM viewer was closed;
            // there is no window left to interact with.
            if (Karton.viewerCount === 0 && !root.visible) {
                Qt.quit();
            }
        }
    }

    Timer {
        id: mainSizeTimer
        interval: 500
        repeat: false
        onTriggered: Karton.saveMainWindowSize(root.width, root.height)
    }

    // A small always-available indicator for the error journal. It only shows
    // up once something has actually failed.
    QQC2.ToolButton {
        id: errorBadge
        z: 800
        anchors.right: parent.right
        anchors.bottom: parent.bottom
        anchors.margins: Kirigami.Units.largeSpacing
        visible: errorLogDialog.errorCount > 0
        icon.name: "data-error-symbolic"
        text: errorLogDialog.errorCount
        display: QQC2.AbstractButton.TextBesideIcon
        onClicked: errorLogDialog.open()

        background: Rectangle {
            radius: height / 2
            color: Kirigami.Theme.backgroundColor
            border.width: 1
            border.color: Kirigami.Theme.negativeTextColor
        }

        QQC2.ToolTip.visible: hovered
        QQC2.ToolTip.text: qsTr("error_log")
    }

    // Dropping an ISO or disk image anywhere on a VM page inserts/attaches it.
    // The page, not the window, decides what the file becomes.
    DropArea {
        id: mediaDropArea
        anchors.fill: parent
        z: 900
        // Only a VM page knows what to do with media; disable the area
        // elsewhere so drops still reach text fields and other targets.
        enabled: pageStack.lastItem && typeof pageStack.lastItem.insertDroppedMedia === "function"
        onDropped: drop => {
            if (!drop.hasUrls) {
                return;
            }
            const page = pageStack.lastItem;
            if (!page || typeof page.insertDroppedMedia !== "function") {
                return;
            }
            for (let i = 0; i < drop.urls.length; ++i) {
                page.insertDroppedMedia(page.localPath(drop.urls[i]));
            }
            drop.acceptProposedAction();
        }
    }

    Rectangle {
        anchors.fill: parent
        z: 901
        visible: mediaDropArea.containsDrag && pageStack.lastItem
            && typeof pageStack.lastItem.insertDroppedMedia === "function"
        color: Qt.rgba(0, 0, 0, 0.45)

        Rectangle {
            anchors.centerIn: parent
            width: dropHint.implicitWidth + Kirigami.Units.gridUnit * 3
            height: dropHint.implicitHeight + Kirigami.Units.gridUnit * 2
            radius: Kirigami.Units.largeSpacing
            color: Kirigami.Theme.backgroundColor
            border.width: 2
            border.color: Kirigami.Theme.highlightColor

            Kirigami.Heading {
                id: dropHint
                anchors.centerIn: parent
                level: 3
                text: qsTr("drop_media_hint")
            }
        }
    }
}
