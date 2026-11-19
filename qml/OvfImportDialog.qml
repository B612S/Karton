// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

// Import an OVF descriptor or an OVA archive. The VM is created stopped with
// its disks converted to qcow2 in the chosen folder.
Kirigami.Dialog {
    id: root

    property string sourcePath: ""
    property bool importing: false
    property string errorText: ""
    property var activeNetworks: []

    title: qsTr("import_ovf_ova")
    preferredWidth: Kirigami.Units.gridUnit * 40
    modal: true

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function selectedNetwork() {
        const index = networkCombo.currentIndex - 1;
        return index >= 0 && index < root.activeNetworks.length ? root.activeNetworks[index].name : "";
    }

    onOpened: {
        activeNetworks = Karton.networks().filter(network => network.active);
        sourcePath = "";
        nameField.text = "";
        destinationField.text = Karton.imagesDirectory();
        networkCombo.currentIndex = 0;
        errorText = "";
        importing = false;
    }

    Connections {
        target: Karton
        function onOvfImportStarted(name) {
            root.importing = true;
            root.errorText = "";
        }
        function onOvfImportFinished(success, name, message) {
            root.importing = false;
            if (success) {
                root.close();
            } else if (message.length > 0) {
                root.errorText = message;
            }
        }
        function onErrorOccurred(message) {
            if (root.opened) {
                root.errorText = message;
            }
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                text: qsTr("source")
            }
            Controls.TextField {
                Layout.fillWidth: true
                readOnly: true
                text: root.sourcePath
                placeholderText: qsTr("choose_an_ovf_or_ova_file")
            }
            Controls.Button {
                text: qsTr("choose")
                enabled: !root.importing
                onClicked: ovfFileDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                text: qsTr("name")
            }
            Controls.TextField {
                id: nameField
                Layout.fillWidth: true
                enabled: !root.importing
                placeholderText: qsTr("name_from_the_ovf")
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                text: qsTr("destination")
            }
            Controls.TextField {
                id: destinationField
                Layout.fillWidth: true
                readOnly: true
            }
            Controls.Button {
                text: qsTr("choose")
                enabled: !root.importing
                onClicked: destinationDialog.open()
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.Label {
                text: qsTr("network")
            }
            Controls.ComboBox {
                id: networkCombo
                Layout.fillWidth: true
                enabled: !root.importing
                model: [qsTr("no_network")].concat(root.activeNetworks.map(network => network.name))
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing
            visible: root.importing

            Controls.BusyIndicator {
                running: root.importing
                Layout.preferredWidth: Kirigami.Units.iconSizes.small
                Layout.preferredHeight: Kirigami.Units.iconSizes.small
            }
            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("importing_the_ovf_disks_are_converted_to_qcow2")
                wrapMode: Text.WordWrap
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            text: qsTr("the_vm_is_created_stopped_and_can_be_started_from_the_list")
            opacity: 0.65
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            enabled: !root.importing
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("import")
            icon.name: "document-import-symbolic"
            enabled: root.sourcePath.length > 0 && !root.importing
            onTriggered: {
                root.importing = true;
                root.errorText = "";
                if (!Karton.importOvf(root.sourcePath,
                                       nameField.text.trim(),
                                       destinationField.text.trim(),
                                       root.selectedNetwork())) {
                    root.importing = false;
                }
            }
        }
    ]

    FileDialog {
        id: ovfFileDialog
        title: qsTr("import_ovf_ova")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("ovf_ova_packages"), qsTr("all_files")]
        onAccepted: root.sourcePath = root.localPath(selectedFile)
    }

    FolderDialog {
        id: destinationDialog
        title: qsTr("choose_destination_folder")
        onAccepted: destinationField.text = root.localPath(selectedFolder)
    }
}
