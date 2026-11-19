// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import org.kde.kirigami as Kirigami

// Small canvas graph used for the live CPU/RAM history on the VM page. A
// Canvas keeps the component dependency-free and repaints only when a new
// sample arrives.
Item {
    id: root

    property var values: []
    property real maximum: 100
    property color lineColor: Kirigami.Theme.highlightColor
    property color fillColor: Qt.rgba(lineColor.r, lineColor.g, lineColor.b, 0.18)

    implicitHeight: Kirigami.Units.gridUnit * 5

    Canvas {
        id: canvas
        anchors.fill: parent

        onPaint: {
            const ctx = getContext("2d");
            const w = width;
            const h = height;
            ctx.reset();
            ctx.clearRect(0, 0, w, h);
            if (w <= 0 || h <= 0) {
                return;
            }

            const gridColor = Qt.rgba(Kirigami.Theme.textColor.r,
                                      Kirigami.Theme.textColor.g,
                                      Kirigami.Theme.textColor.b, 0.12);
            ctx.strokeStyle = gridColor;
            ctx.lineWidth = 1;
            for (let i = 1; i < 4; ++i) {
                const y = Math.round(h * i / 4) + 0.5;
                ctx.beginPath();
                ctx.moveTo(0, y);
                ctx.lineTo(w, y);
                ctx.stroke();
            }

            if (root.values.length < 2) {
                return;
            }

            const max = Math.max(1, root.maximum);
            const step = w / (root.values.length - 1);
            const pointY = value => h - Math.max(0, Math.min(max, value)) / max * h;

            ctx.beginPath();
            ctx.moveTo(0, h);
            for (let i = 0; i < root.values.length; ++i) {
                ctx.lineTo(i * step, pointY(root.values[i]));
            }
            ctx.lineTo(w, h);
            ctx.closePath();
            ctx.fillStyle = root.fillColor;
            ctx.fill();

            ctx.beginPath();
            for (let i = 0; i < root.values.length; ++i) {
                const x = i * step;
                const y = pointY(root.values[i]);
                if (i === 0) {
                    ctx.moveTo(x, y);
                } else {
                    ctx.lineTo(x, y);
                }
            }
            ctx.strokeStyle = root.lineColor;
            ctx.lineWidth = 2;
            ctx.lineJoin = "round";
            ctx.lineCap = "round";
            ctx.stroke();

            const lastX = w;
            const lastY = pointY(root.values[root.values.length - 1]);
            ctx.beginPath();
            ctx.arc(lastX - 2, lastY, 3, 0, Math.PI * 2);
            ctx.fillStyle = root.lineColor;
            ctx.fill();
        }
    }

    onValuesChanged: canvas.requestPaint()
    onWidthChanged: canvas.requestPaint()
    onHeightChanged: canvas.requestPaint()
    onMaximumChanged: canvas.requestPaint()
    onLineColorChanged: canvas.requestPaint()
}
