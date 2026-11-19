// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import org.kde.kirigami as Kirigami

// Shared shell for every Karton dialog. It keeps the background, corner
// radius, padding and width policy identical across dialogs so a new screen
// cannot look different from the rest. Dialogs override preferredDialogWidth
// (or width) only when their content genuinely needs a different size.
//
// Long values (paths, libvirt messages) must never change the window width:
// give the content an explicit width (root.availableWidth) and let labels
// wrap or elide inside it. preferredWidth is intentionally not used here
// because Kirigami.Dialog would grow to the content's unwrapped width.
Kirigami.Dialog {
    id: root

    // Comfortable default: a two-column row with a 260 px control and a picker
    // button still fits next to its label.
    property real preferredDialogWidth: Kirigami.Units.gridUnit * 36

    readonly property real dialogWidthLimit: parent && parent.width > 0
        ? Math.max(Kirigami.Units.gridUnit * 24, parent.width - Kirigami.Units.gridUnit * 2)
        : Kirigami.Units.gridUnit * 60

    width: Math.min(preferredDialogWidth, dialogWidthLimit)
    padding: Kirigami.Units.largeSpacing

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.98)
        radius: 8
        border.color: "transparent"
    }
}
