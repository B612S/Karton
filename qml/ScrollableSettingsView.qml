// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Controls as Controls

Controls.ScrollView {
    id: root

    clip: true

    WheelHandler {
        acceptedDevices: PointerDevice.Mouse | PointerDevice.TouchPad
        onWheel: function(event) {
            if (event.angleDelta.y === 0) {
                return;
            }
            const viewport = root.height;
            const maximum = Math.max(0, root.contentHeight - viewport);
            const step = Math.max(40, viewport * 0.15);
            root.contentY = Math.min(maximum, Math.max(0, root.contentY - event.angleDelta.y / 120 * step));
            event.accepted = true;
        }
    }
}
