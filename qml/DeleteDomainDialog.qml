// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

// Shared delete confirmation. The VM list context menu and the VM page toolbar
// must ask the same question in the same way, so both instantiate this dialog
// instead of keeping their own copy.
KartonDialog {
    id: root

    // The VM to remove. Callers assign it right before open().
    property Domain domain: null

    title: qsTr("delete_virtual_machine")
    preferredDialogWidth: Kirigami.Units.gridUnit * 26
    modal: true

    // Opening the dialog must never inherit the previous answer.
    onOpened: deleteDiskCheck.checked = false
    // Do not keep a Domain pointer that may be destroyed by the deletion.
    onClosed: root.domain = null

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: root.domain ? qsTr("delete_vm_1").arg(root.domain.config.name) : ""
        }
        Controls.CheckBox {
            id: deleteDiskCheck
            text: qsTr("delete_disk")
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("delete")
            icon.name: "delete-symbolic"
            onTriggered: {
                const domainToDelete = root.domain;
                if (!domainToDelete) {
                    return;
                }
                const domainName = domainToDelete.config.name;
                const window = applicationWindow();
                // Drop the reference before libvirt removes the domain; the
                // page or model may destroy the wrapper while this call runs.
                root.domain = null;
                root.close();
                if (Karton.deleteDomain(domainToDelete, deleteDiskCheck.checked) && window) {
                    window.showPassiveNotification(qsTr("deleted_vm_1").arg(domainName));
                }
            }
        }
    ]
}
