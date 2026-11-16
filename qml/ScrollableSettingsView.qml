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
            const flickable = root.contentItem;
            if (!flickable || flickable.contentY === undefined || flickable.contentHeight === undefined) {
                // Let the default ScrollView implementation handle it.
                return;
            }
            const maximum = Math.max(0, flickable.contentHeight - flickable.height);
            if (maximum <= 0) {
                return;
            }
            const step = Math.max(40, flickable.height * 0.15);
            flickable.contentY = Math.min(maximum, Math.max(0, flickable.contentY - event.angleDelta.y / 120 * step));
            event.accepted = true;
        }
    }
}
