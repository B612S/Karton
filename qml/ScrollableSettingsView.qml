// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls

// Flickable-based scrolling, matching Luma's settings pages. The wrapper
// leaves room for the vertical scrollbar so controls never slide underneath it.
Flickable {
    id: root

    default property alias contentData: contentWrapper.data
    // Allows a modal dialog to hide the page scrollbar behind it.
    property int scrollBarPolicy: ScrollBar.AsNeeded
    // Optional equal margins around the content and an overlay scrollbar that
    // sits at the very right edge instead of reserving layout width.
    property int contentMargins: 0
    property bool overlayScrollBar: false

    clip: true
    contentWidth: width
    contentHeight: contentWrapper.height + root.contentMargins * 2
    boundsBehavior: Flickable.StopAtBounds

    ScrollBar.vertical: ScrollBar {
        policy: root.scrollBarPolicy
        width: 10
    }

    Item {
        id: contentWrapper
        x: root.contentMargins
        y: root.contentMargins
        width: Math.max(0, root.width - (root.overlayScrollBar ? 0 : 16) - root.contentMargins * 2)
        height: childrenRect.height
    }
}
