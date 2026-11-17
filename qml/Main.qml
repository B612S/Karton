// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2024 Aaron Rainbolt <arraybolt3@gmail.com>
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import org.kde.kirigami as Kirigami

Kirigami.ApplicationWindow {
    id: root
    objectName: "mainWindow"

    width: Kirigami.Units.gridUnit * 50
    height: Kirigami.Units.gridUnit * 35

    property bool sizeRestored: false

    readonly property real wideScreenThreshold: Kirigami.Units.gridUnit * 40
    readonly property bool isWidescreen: (root.width >= wideScreenThreshold) && root.wideScreen // prevent being widescreen at first launch

    pageStack.columnView.columnResizeMode: isWidescreen ? Kirigami.ColumnView.FixedColumns : Kirigami.ColumnView.SingleColumn
    pageStack.columnView.columnWidth: Kirigami.Units.gridUnit * 16

    title: i18nc("@title:window", "Karton Virtual Machine Manager")

    pageStack.initialPage: VMListPage { }

    Component.onCompleted: {
        const saved = Karton.mainWindowSize();
        if (saved.width > 0 && saved.height > 0) {
            width = Math.max(Kirigami.Units.gridUnit * 30, Math.min(saved.width, Screen.width * 0.95));
            height = Math.max(Kirigami.Units.gridUnit * 20, Math.min(saved.height, Screen.height * 0.95));
        }
        sizeRestored = true;
    }

    onWidthChanged: if (sizeRestored) mainSizeTimer.restart()
    onHeightChanged: if (sizeRestored) mainSizeTimer.restart()
    onClosing: (close) => {
        // Destroying the platform window from inside the close event deadlocks
        // in QWaylandWindow::reset() (Qt Wayland) and aborts Karton. Ignore the
        // event, hide the window, close the viewers and leave the event loop;
        // aboutToQuit exits the process before Qt tears its windows down.
        close.accepted = false;
        Karton.saveMainWindowSize(width, height);
        Karton.closeAllViewers();
        root.hide();
        Qt.quit();
    }

    Timer {
        id: mainSizeTimer
        interval: 500
        repeat: false
        onTriggered: Karton.saveMainWindowSize(root.width, root.height)
    }
}
