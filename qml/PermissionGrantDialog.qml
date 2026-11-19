// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

KartonDialog {
    id: root

    property string targetPath: ""

    signal confirmed()
    signal cancelled()

    property bool granted: false

    title: qsTr("grant_access")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36

    function openFor(path) {
        targetPath = path;
        granted = false;
        open();
    }

    onClosed: {
        if (!granted) {
            cancelled();
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: 12

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: qsTr("give_the_libvirt_qemu_user_access_to_this_folder")
        }
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            font.family: "monospace"
            font.pixelSize: 12
            opacity: 0.8
            text: root.targetPath
        }
        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
            font.pixelSize: 12
            text: qsTr("karton_will_add_acl_entries_only_for_the_libvirt_qemu_user_execute_permission_on_parent_fo")
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("grant_access")
            icon.name: "lock-open-symbolic"
            enabled: root.targetPath.length > 0
            onTriggered: {
                root.granted = true;
                root.confirmed();
                root.close();
            }
        }
    ]
}
