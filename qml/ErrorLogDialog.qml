// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

// Session-wide record of libvirt/SPICE errors. Short-lived passive
// notifications disappear before the user can read a long libvirt message;
// this keeps the last messages available for inspection.
KartonDialog {
    id: root

    title: qsTr("error_log")
    preferredDialogWidth: Kirigami.Units.gridUnit * 42
    preferredHeight: Kirigami.Units.gridUnit * 26
    modal: true
    showCloseButton: true
    standardButtons: Kirigami.Dialog.NoButton

    readonly property int errorCount: errorModel.count

    function recordError(message) {
        if (!message) {
            return;
        }
        errorModel.insert(0, {
            message: message,
            time: new Date().toLocaleTimeString(Qt.locale())
        });
        while (errorModel.count > 100) {
            errorModel.remove(errorModel.count - 1);
        }
    }

    ListModel {
        id: errorModel
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.smallSpacing

        Controls.Label {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            visible: errorModel.count === 0
            text: qsTr("no_errors_recorded")
            opacity: 0.7
            horizontalAlignment: Text.AlignHCenter
            wrapMode: Text.WordWrap
        }

        Controls.ScrollView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 17
            visible: errorModel.count > 0
            clip: true

            ListView {
                model: errorModel
                spacing: Kirigami.Units.smallSpacing

                delegate: ColumnLayout {
                    width: ListView.view.width
                    spacing: 2

                    Controls.Label {
                        text: model.time
                        font.pixelSize: 11
                        opacity: 0.6
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        text: model.message
                        textFormat: Text.PlainText
                        wrapMode: Text.WordWrap
                    }
                    Kirigami.Separator {
                        Layout.fillWidth: true
                        Layout.topMargin: Kirigami.Units.smallSpacing
                    }
                }
            }
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("clear")
            icon.name: "edit-clear-all-symbolic"
            enabled: errorModel.count > 0
            onTriggered: errorModel.clear()
        },
        Kirigami.Action {
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        }
    ]
}
