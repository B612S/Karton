// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.Dialog {
    id: root
    property Domain domain: null

    title: domain ? qsTr("snapshots_1").arg(domain.config.name) : qsTr("snapshots")

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.95)
        radius: 8
        border.color: "transparent"
    }


    property var snapshotList: []
    property string errorText: ""
    preferredWidth: applicationWindow() ? Math.max(520, Math.min(700, applicationWindow().width - 48)) : 700

    function refresh() {
        snapshotList = domain ? Karton.domainSnapshots(domain) : [];
    }

    function defaultName() {
        return "snapshot-" + Qt.formatDateTime(new Date(), "yyyyMMdd-HHmmss");
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
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            level: 3
            text: qsTr("snapshots")
        }

        Controls.Label {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        Controls.Label {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            visible: root.snapshotList.length === 0
            text: qsTr("no_snapshots_yet")
            opacity: 0.7
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 10
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            clip: true
            model: root.snapshotList
            spacing: Kirigami.Units.smallSpacing

            delegate: Controls.ItemDelegate {
                width: ListView.view.width

                contentItem: RowLayout {
                    spacing: Kirigami.Units.largeSpacing

                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0

                        Controls.Label {
                            Layout.fillWidth: true
                            text: modelData.name + (modelData.current ? " (" + qsTr("current") + ")" : "")
                            elide: Text.ElideRight
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: modelData.creationTime + " · " + modelData.state
                                  + (modelData.description.length > 0 ? " · " + modelData.description : "")
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
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
        }

        Controls.TextField {
            id: nameField
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            placeholderText: qsTr("snapshot_name")
        }

        Controls.TextField {
            id: descriptionField
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            placeholderText: qsTr("description_optional")
        }

        Controls.CheckBox {
            id: includeMemoryCheck
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            text: qsTr("include_memory_internal_snapshot")
            enabled: root.domain !== null && root.domain.isActive
        }

        Controls.Label {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            visible: includeMemoryCheck.enabled && !includeMemoryCheck.checked
            text: qsTr("without_memory_a_running_vm_gets_an_external_disk_only_snapshot")
            opacity: 0.7
            wrapMode: Text.WordWrap
        }

        Controls.Button {
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            text: qsTr("create_snapshot")
            icon.name: "camera-photo-symbolic"
            enabled: nameField.text.trim().length > 0 && root.domain !== null
            onClicked: {
                if (Karton.createSnapshot(root.domain, nameField.text, descriptionField.text, includeMemoryCheck.checked)) {
                    root.refresh();
                    nameField.text = root.defaultName();
                    descriptionField.text = "";
                }
            }
        }

        Controls.Button {
            Layout.alignment: Qt.AlignRight
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            text: qsTr("close")
            onClicked: root.close()
        }
    }
}
