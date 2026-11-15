// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import QtQuick.Dialogs as Dialogs
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

Kirigami.Dialog {
    id: addDomainDialog

    title: i18n("Add New Virtual Machine")
    padding: Kirigami.Units.largeSpacing
    property bool showError: false
    property bool creating: false
    property string pendingName: ""
    property int step: 0
    readonly property int lastStep: 1
    modal: true

    preferredWidth: Kirigami.Units.gridUnit * 40
    preferredHeight: Kirigami.Units.gridUnit * 27

    function getShortOsId(path) {
        return OsinfoConfig.getShortIdFromId(OsinfoConfig.getOsIdFromDisk(path));
    }

    function validateSource() {
        const shortOsId = osTextField.text.trim();
        const valid = nameField.text.trim() !== ""
            && diskImageField.text.trim() !== ""
            && OsinfoConfig.osList.includes(shortOsId);
        showError = !valid;
        return valid;
    }

    function createVm() {
        if (!validateSource()) {
            step = 0;
            return;
        }

        const name = nameField.text.trim();
        const domainConfig = {
            name: name,
            shortOsId: osTextField.text.trim(),
            isoDiskPath: diskImageField.text,
            memoryGB: memorySpinBox.value,
            storageGB: storageSpinBox.value,
            diskFormat: diskFormatCombo.currentText,
            cpus: cpuSpinBox.value,
            enableAccel3d: enableAccel3dCheckBox.checked
        };

        pendingName = name;
        creating = true;
        if (!Karton.createDomain(domainConfig)) {
            creating = false;
            pendingName = "";
            return;
        }
        applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Creating VM: %1", name));
    }

    onOpened: {
        step = 0;
        showError = false;
        nameField.forceActiveFocus();
    }

    customFooterActions: [
        Kirigami.Action {
            text: i18nc("verb, return to the previous wizard step", "Back")
            icon.name: "go-previous"
            visible: addDomainDialog.step > 0
            enabled: !addDomainDialog.creating
            onTriggered: addDomainDialog.step = Math.max(0, addDomainDialog.step - 1)
        },
        Kirigami.Action {
            text: i18nc("verb, continue to the next wizard step", "Next")
            icon.name: "go-next"
            visible: addDomainDialog.step < addDomainDialog.lastStep
            enabled: !addDomainDialog.creating
            onTriggered: {
                if (addDomainDialog.step === 0 && !addDomainDialog.validateSource()) {
                    return;
                }
                addDomainDialog.step = Math.min(addDomainDialog.lastStep, addDomainDialog.step + 1);
            }
        },
        Kirigami.Action {
            text: i18nc("verb, creation button for a new VM", "Create")
            icon.name: "dialog-ok"
            visible: addDomainDialog.step === addDomainDialog.lastStep
            enabled: !addDomainDialog.creating
            onTriggered: addDomainDialog.createVm()
        }
    ]

    Connections {
        target: Karton
        function onDomainCreated(success, name) {
            if (name !== addDomainDialog.pendingName) {
                return;
            }

            addDomainDialog.creating = false;
            addDomainDialog.pendingName = "";
            if (success) {
                addDomainDialog.close();
                applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Created VM: %1", name));
            }
        }
    }

    StackLayout {
        currentIndex: addDomainDialog.step

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                Layout.leftMargin: Kirigami.Units.gridUnit * 2
                Layout.rightMargin: Kirigami.Units.gridUnit * 2
                Layout.topMargin: Kirigami.Units.largeSpacing
                level: 3
                text: i18n("Installation source")
            }

            Kirigami.InlineMessage {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.gridUnit * 2
                Layout.rightMargin: Kirigami.Units.gridUnit * 2
                text: i18nc("Error message", "Please fill in all required fields")
                type: Kirigami.MessageType.Error
                visible: addDomainDialog.showError
            }

            FormCard.FormCard {
                Layout.fillWidth: true

                FormCard.FormTextFieldDelegate {
                    id: nameField
                    label: i18nc("@label:textbox", "VM Name:")
                    placeholderText: i18n("Enter VM Name")
                    Layout.fillWidth: true
                    validator: RegularExpressionValidator {
                        regularExpression: /^[A-Za-z0-9._-]+$/
                    }
                }

                Dialogs.FileDialog {
                    id: fileDialog
                    title: i18nc("@label:filedialog", "Choose a disk image")
                    nameFilters: [i18nc("name filter for the file dialog",
                                        "Disk images (*.qcow2 *.raw *.img *.iso *.vdi *.vmdk)")]
                    onAccepted: {
                        diskImageField.text = decodeURIComponent(fileDialog.selectedFile.toString().replace(/^file:\/\//, ""));
                        let shortOsId = getShortOsId(diskImageField.text);
                        if (shortOsId === "") {
                            osTextField.placeholderText = i18n("Could not identify the OS. Please enter an OS Variant.");
                            osField.editText = "";
                            osTextField.text = osField.editText;
                        } else {
                            osField.editText = shortOsId;
                            osTextField.text = osField.editText;
                        }
                    }
                }

                FormCard.FormDelegateSeparator {}
                FormCard.AbstractFormDelegate {
                    background: null
                    contentItem: RowLayout {
                        Layout.fillWidth: true

                        Controls.TextField {
                            id: diskImageField
                            Layout.fillWidth: true
                            placeholderText: i18n("Select a disk image")
                            readOnly: true
                        }

                        Controls.Button {
                            text: i18nc("verb, look for a file in explorer", "Browse")
                            onClicked: {
                                fileDialog.open();
                            }
                        }
                    }
                }
                FormCard.AbstractFormDelegate {
                    background: null // removes hover selection
                    hoverEnabled: false
                    contentItem: ColumnLayout {
                        spacing: Kirigami.Units.smallSpacing

                        Controls.Label {
                            text: i18n("OS Variant:")
                        }

                        Controls.ComboBox {
                            id: osField
                            Layout.fillWidth: true
                            editable: true
                            currentIndex: -1

                            contentItem: Controls.TextField {
                                id: osTextField
                                placeholderText: i18n("Select or enter an OS variant")
                                background: Item {}
                                onTextEdited: {
                                    osField.editText = text;
                                    suggestionsPopup.updateSuggestions();
                                }
                                onActiveFocusChanged: {
                                    if (activeFocus) {
                                        suggestionsPopup.updateSuggestions();
                                    } else {
                                        suggestionsPopup.close();
                                    }
                                }
                            }

                            // triggers on text edits in OS variant selection
                            Controls.Popup {
                                id: suggestionsPopup
                                parent: osField
                                y: osField.height
                                width: osField.width
                                padding: 0
                                implicitHeight: Math.min(listView.contentHeight, Kirigami.Units.gridUnit * 10)

                                function updateSuggestions() {
                                    const filterText = osTextField.text.toLowerCase();
                                    listView.model = OsinfoConfig.osList.filter((osVariant) => osVariant.toLowerCase().includes(filterText));
                                    listView.model.length > 0 ? open() : close();
                                }

                                contentItem: ListView {
                                    id: listView
                                    implicitHeight: contentHeight
                                    clip: true
                                    delegate: Controls.ItemDelegate {
                                        width: ListView.view.width
                                        text: modelData
                                        onClicked: {
                                            osTextField.text = modelData;
                                            osField.editText = modelData;
                                            suggestionsPopup.close();
                                            osTextField.forceActiveFocus();
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
            }
        }

        ColumnLayout {
            spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                Layout.leftMargin: Kirigami.Units.gridUnit * 2
                Layout.rightMargin: Kirigami.Units.gridUnit * 2
                Layout.topMargin: Kirigami.Units.largeSpacing
                level: 3
                text: i18n("Hardware")
            }

            FormCard.FormCard {
                Layout.fillWidth: true
                FormCard.FormSpinBoxDelegate {
                    id: memorySpinBox
                    label: i18nc("@label:spinbox, RAM", "Memory (GB)")
                    value: 4
                    from: 1
                    to: 64
                    Layout.fillWidth: true
                }

                FormCard.FormDelegateSeparator {}

                FormCard.FormSpinBoxDelegate {
                    id: storageSpinBox
                    label: i18nc("@label:spinbox", "Disk Storage (GB)")
                    from: 1
                    to: 2048
                    value: 4
                    Layout.fillWidth: true
                }

                FormCard.FormDelegateSeparator {}

                FormCard.FormComboBoxDelegate {
                    id: diskFormatCombo
                    text: i18n("Disk format:")
                    model: ["qcow2", "raw", "vmdk"]
                    currentIndex: 0
                    Layout.fillWidth: true
                }

                FormCard.FormDelegateSeparator {}

                FormCard.FormSpinBoxDelegate {
                    id: cpuSpinBox
                    label: i18nc("@label:spinbox, number of cpus", "CPUs")
                    from: 1
                    to: 16
                    value: 2
                    Layout.fillWidth: true
                }

                FormCard.FormCheckDelegate {
                    id: enableAccel3dCheckBox
                    text: Karton.vulkanRendering ? i18nc("@option:check", "Hardware acceleration (requires OpenGL rendering)") : i18nc("@option:check", "Enable hardware acceleration")
                    checked: !Karton.vulkanRendering
                    enabled: !Karton.vulkanRendering
                    Layout.fillWidth: true
                }
            }
        }
    }
}
