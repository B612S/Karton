// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Controls
import QtQuick.Controls as Controls

// Flickable-based scrolling, matching Luma's settings pages. A Flickable
// handles mouse-wheel and touchpad scrolling natively and does not depend on
// ScrollView's internal content-size bookkeeping.
Flickable {
    id: root

    clip: true
    contentWidth: width
    contentHeight: Math.max(height, contentItem.childrenRect.height)
    boundsBehavior: Flickable.StopAtBounds

    ScrollBar.vertical: ScrollBar {
        policy: ScrollBar.AsNeeded
        width: 10
    }

    // Luma reserves room for the scrollbar; otherwise form controls extend
    // under it and become hard to click at the right edge.
    Component.onCompleted: contentItem.width = Qt.binding(() => Math.max(0, root.width - 16))
}
