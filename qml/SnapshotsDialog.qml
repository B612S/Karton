// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

KartonDialog {
    id: root
    property Domain domain: null

    title: domain ? qsTr("snapshots_1").arg(domain.config.name) : qsTr("snapshots")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36

    property var snapshotList: []
    property string errorText: ""

    function refresh() {
        snapshotList = domain ? Karton.domainSnapshots(domain) : [];
    }

    function defaultName() {
        return "snapshot-" + Qt.formatDateTime(new Date(), "yyyyMMdd-HHmmss");
    }

    function formatSize(bytes) {
        if (!bytes || bytes <= 0) {
            return "";
        }
        const gib = bytes / (1024 * 1024 * 1024);
        if (gib >= 1) {
            return gib.toFixed(gib >= 10 ? 0 : 1) + " GiB";
        }
        return (bytes / (1024 * 1024)).toFixed(1) + " MiB";
    }

    function createSnapshot() {
        if (Karton.createSnapshot(root.domain, nameField.text, descriptionField.text, includeMemoryCheck.checked)) {
            root.refresh();
            nameField.text = root.defaultName();
            descriptionField.text = "";
        }
    }

    onOpened: {
        errorText = "";
        nameField.text = defaultName();
        descriptionField.text = "";
        includeMemoryCheck.checked = false;
        refresh();
    }

    Component.onCompleted: refresh()

    Connections {
        target: Karton
        function onErrorOccurred(errorMessage) {
            root.errorText = errorMessage;
        }
    }

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.snapshotList.length === 0
            text: qsTr("no_snapshots_yet")
            opacity: 0.7
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 10
            visible: root.snapshotList.length > 0
            clip: true
            model: root.snapshotList
            spacing: Kirigami.Units.smallSpacing

            delegate: Controls.ItemDelegate {
                width: ListView.view.width

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    Rectangle {
                        Layout.alignment: Qt.AlignTop
                        Layout.topMargin: 6
                        width: 8
                        height: 8
                        radius: 4
                        color: modelData.current ? Kirigami.Theme.highlightColor : Kirigami.Theme.disabledTextColor
                    }

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 2

                        RowLayout {
                            Layout.fillWidth: true
                            spacing: Kirigami.Units.smallSpacing

                            Controls.Label {
                                Layout.fillWidth: true
                                text: modelData.name + (modelData.current ? " (" + qsTr("current") + ")" : "")
                                font.weight: modelData.current ? Font.DemiBold : Font.Normal
                                elide: Text.ElideRight
                            }
                            Kirigami.Badge {
                                text: modelData.type === "external" ? qsTr("external_snapshot") : qsTr("internal_snapshot")
                                type: modelData.type === "external" ? Kirigami.Badge.Type.Warning : Kirigami.Badge.Type.Information
                            }
                        }

                        Controls.Label {
                            Layout.fillWidth: true
                            text: {
                                const parts = [modelData.creationTime, modelData.state];
                                if (modelData.hasMemory) {
                                    parts.push(qsTr("with_memory"));
                                }
                                if (modelData.sizeBytes > 0) {
                                    parts.push(root.formatSize(modelData.sizeBytes));
                                }
                                if (modelData.parent.length > 0) {
                                    parts.push(qsTr("from_snapshot_1").arg(modelData.parent));
                                }
                                if (modelData.description.length > 0) {
                                    parts.push(modelData.description);
                                }
                                return parts.join(" · ");
                            }
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }

                    Controls.Button {
                        text: qsTr("revert")
                        icon.name: "edit-undo-symbolic"
                        onClicked: {
                            if (Karton.revertSnapshot(root.domain, modelData.name)) {
                                root.refresh();
                            }
                        }
                    }
                    Controls.Button {
                        text: qsTr("delete")
                        icon.name: "edit-delete-symbolic"
                        onClicked: {
                            if (Karton.deleteSnapshot(root.domain, modelData.name)) {
                                root.refresh();
                            }
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: root.snapshotList.length > 0
        }

        Controls.Label {
            text: qsTr("create_snapshot")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }

        GridLayout {
            Layout.fillWidth: true
            columns: 2
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: 12

            Controls.Label {
                text: qsTr("snapshot_name")
                Layout.fillWidth: true
            }
            Controls.TextField {
                id: nameField
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
            }

            Controls.Label {
                text: qsTr("description_optional")
                Layout.fillWidth: true
            }
            Controls.TextField {
                id: descriptionField
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
            }
        }

        Controls.CheckBox {
            id: includeMemoryCheck
            text: qsTr("include_memory_internal_snapshot")
            enabled: root.domain !== null && root.domain.isActive
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: includeMemoryCheck.enabled && !includeMemoryCheck.checked
            text: qsTr("without_memory_a_running_vm_gets_an_external_disk_only_snapshot")
            opacity: 0.7
            wrapMode: Text.WordWrap
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("create_snapshot")
            icon.name: "camera-photo-symbolic"
            enabled: nameField.text.trim().length > 0 && root.domain !== null
            onTriggered: root.createSnapshot()
        }
    ]
}
