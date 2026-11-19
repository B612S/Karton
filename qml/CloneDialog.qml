// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.Dialog {
    id: root
    property Domain domain: null

    title: domain ? qsTr("clone_1").arg(domain.config.name) : qsTr("clone_vm")

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.95)
        radius: 8
        border.color: "transparent"
    }

    property string errorText: ""
    property bool cloning: false
    property string destinationDirectory: ""
    preferredWidth: applicationWindow() ? Math.max(480, Math.min(640, applicationWindow().width - 48)) : 640

    function defaultName() {
        const base = (domain ? domain.config.name : "vm").replace(/[^A-Za-z0-9._-]/g, "-");
        return base + "-clone";
    }

    onOpened: {
        errorText = "";
        cloning = false;
        nameField.text = defaultName();
        destinationDirectory = Karton.imagesDirectory();
    }

    Connections {
        target: Karton
        function onDomainCloned(success, sourceName, newName) {
            if (!root.cloning) {
                return;
            }
            if (success) {
                root.cloning = false;
                root.close();
                if (applicationWindow()) {
                    applicationWindow().showPassiveNotification(qsTr("cloned_vm_1").arg(newName));
                }
            } else {
                root.cloning = false;
                root.errorText = qsTr("cloning_failed_see_the_notification_for_details");
            }
        }
        function onErrorOccurred(errorMessage) {
            root.errorText = errorMessage;
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            text: qsTr("the_configuration_and_all_local_disk_images_will_be_copied_the_clone_is_created_stopped")
            wrapMode: Text.WordWrap
            opacity: 0.8
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

        Controls.TextField {
            id: nameField
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            placeholderText: qsTr("new_vm_name")
            enabled: !root.cloning
        }

        RowLayout {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            spacing: 8

            Controls.Label {
                text: qsTr("destination_folder")
                Layout.fillWidth: true
            }
            Controls.TextField {
                id: destinationField
                Layout.preferredWidth: 240
                text: root.destinationDirectory
                readOnly: true
                enabled: !root.cloning
                placeholderText: Karton.imagesDirectory()
            }
            Controls.Button {
                text: qsTr("choose")
                icon.name: "folder-open-symbolic"
                enabled: !root.cloning
                onClicked: destinationFolderDialog.open()
            }
        }

        Controls.Label {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.65
            font.pixelSize: 12
            text: qsTr("the_copied_disk_images_will_be_placed_in_this_folder")
        }

        RowLayout {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Controls.BusyIndicator {
                visible: root.cloning
                running: root.cloning
            }

            Item {
                Layout.fillWidth: true
            }

            Controls.Button {
                text: qsTr("close")
                enabled: !root.cloning
                onClicked: root.close()
            }

            Controls.Button {
                text: qsTr("clone")
                icon.name: "edit-copy-symbolic"
                enabled: !root.cloning && nameField.text.trim().length > 0 && root.domain !== null
                onClicked: {
                    root.errorText = "";
                    if (root.destinationDirectory.trim().length > 0) {
                        const accessError = Karton.checkDiskDirectoryAccess(root.destinationDirectory);
                        if (accessError.length > 0) {
                            root.errorText = accessError;
                            return;
                        }
                    }
                    root.cloning = true;
                    if (!Karton.cloneDomain(root.domain, nameField.text)) {
                        root.cloning = false;
                    }
                }
            }
        }
    }

    FolderDialog {
        id: destinationFolderDialog
        title: qsTr("choose_clone_destination_folder")
        onAccepted: {
            root.destinationDirectory = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""));
            destinationField.text = root.destinationDirectory;
        }
    }
}
