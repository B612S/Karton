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

    readonly property real wideScreenThreshold: Kirigami.Units.gridUnit * 40
    readonly property bool isWidescreen: (root.width >= wideScreenThreshold) && root.wideScreen // prevent being widescreen at first launch

    pageStack.columnView.columnResizeMode: isWidescreen ? Kirigami.ColumnView.FixedColumns : Kirigami.ColumnView.SingleColumn
    pageStack.columnView.columnWidth: Kirigami.Units.gridUnit * 16

    title: qsTr("karton_virtual_machine_manager")

    pageStack.initialPage: VMListPage { }

    Component.onCompleted: {
        const saved = Karton.mainWindowSize();
        if (saved.width > 0 && saved.height > 0) {
            width = Math.max(minimumWindowWidth, Math.min(saved.width, Screen.width * 0.95));
            height = Math.max(minimumWindowHeight, Math.min(saved.height, Screen.height * 0.95));
        }
        sizeRestored = true;
    }

    onWidthChanged: if (sizeRestored) mainSizeTimer.restart()
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
}
