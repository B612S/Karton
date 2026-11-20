// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

// Progress and Cancel for a long libvirt operation that runs off the GUI
// thread (currently managed save). One instance lives in the main window and
// one in each viewer; only the active window shows it, so the same operation
// is never displayed twice.
KartonDialog {
    id: root

    property Domain domain: null
    property bool windowActive: true
    property bool cancelRequested: false

    readonly property bool active: Karton.busy && windowActive && domain !== null
        && Karton.busyUuid === domain.config.uuid
    readonly property string operation: Karton.busyOperation

    title: operation === "snapshot-create" ? qsTr("creating_snapshot")
        : (operation === "snapshot-revert" ? qsTr("reverting_to_snapshot")
        : (operation === "snapshot-delete" ? qsTr("deleting_snapshot") : qsTr("hibernating_the_vm")))
    preferredDialogWidth: Kirigami.Units.gridUnit * 30
    modal: true
    closePolicy: Controls.Popup.NoAutoClose

    onActiveChanged: {
        if (active) {
            cancelRequested = false;
            open();
        } else {
            close();
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            text: root.operation.startsWith("snapshot-")
                ? qsTr("snapshot_operation_in_progress")
                : (root.domain ? qsTr("saving_the_memory_of_1_to_disk").arg(root.domain.config.name)
                               : qsTr("saving_the_vm_memory_to_disk"))
            wrapMode: Text.WordWrap
        }

        Controls.ProgressBar {
            Layout.fillWidth: true
            from: 0
            to: 100
            value: Karton.busyProgress > 0 ? Karton.busyProgress : 0
            indeterminate: Karton.busyProgress < 0
        }

        Controls.Label {
            Layout.fillWidth: true
            text: Karton.busyProgress < 0 ? qsTr("this_can_take_a_few_minutes")
                                          : (Math.round(Karton.busyProgress) + " %")
            opacity: 0.7
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            enabled: Karton.busyCancellable && !root.cancelRequested
            onTriggered: {
                root.cancelRequested = true;
                Karton.cancelBusyOperation();
            }
        }
    ]
}
