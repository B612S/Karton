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

    clip: true
    contentWidth: width
    contentHeight: contentWrapper.height
    boundsBehavior: Flickable.StopAtBounds

    ScrollBar.vertical: ScrollBar {
        policy: root.scrollBarPolicy
        width: 10
    }

    Item {
        id: contentWrapper
        width: Math.max(0, root.width - 16)
        height: childrenRect.height
    }
}
