// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

Kirigami.Dialog {
    title: domain ? i18nc("%1 is the name of the virtual machine", "Configure '%1'", domain.config.name) : ""
    id: configurationDialog
    padding: 0
    modal: true

    property Domain domain: null
    property bool saving: false
    property string pendingName: ""

    preferredWidth: Kirigami.Units.gridUnit * 32
    preferredHeight: Kirigami.Units.gridUnit * 22

    onOpened: {
        if (!domain) {
            return;
        }
        categoryList.currentIndex = 0;
        saving = false;
        pendingName = "";
        nameField.text = domain.config.name;
        autostartCheck.checked = domain.autostart;
        memorySpinBox.value = domain.config.maxRam;
        cpuSpinBox.value = domain.config.cpus;
        storageSpinBox.value = domain.config.maxDiskStorage;
        nameField.forceActiveFocus();
    }

    customFooterActions: [
        Kirigami.Action {
            text: i18nc("verb, save VM configuration changes", "Save")
            icon.name: "dialog-ok"
            enabled: !configurationDialog.saving
            onTriggered: {
                if (!configurationDialog.domain || nameField.text.trim() === "") {
                    return;
                }
                const domainConfig = {
                    name: nameField.text.trim(),
                    memoryGB: memorySpinBox.value,
                    storageGB: storageSpinBox.value,
                    cpus: cpuSpinBox.value
                };

                configurationDialog.pendingName = domainConfig.name;
                configurationDialog.saving = true;
                if (!Karton.editDomain(configurationDialog.domain, domainConfig)) {
                    configurationDialog.saving = false;
                    configurationDialog.pendingName = "";
                }
            }
        }
    ]

    Connections {
        target: Karton
        function onDomainEdited(success, name) {
            if (name !== configurationDialog.pendingName) {
                return;
            }

            configurationDialog.saving = false;
            configurationDialog.pendingName = "";
            if (success) {
                configurationDialog.close();
                applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Updated configuration for VM: %1", name));
            }
        }
    }

    RowLayout {
        spacing: 0

        ListView {
            id: categoryList
            Layout.preferredWidth: Kirigami.Units.gridUnit * 8
            Layout.fillHeight: true
            currentIndex: 0

            model: [
                {label: i18nc("@title:group configuration category", "General"), icon: "settings-configure"},
                {label: i18nc("@title:group configuration category", "Hardware"), icon: "cpu"},
                {label: i18nc("@title:group configuration category", "Storage"), icon: "drive-harddisk-symbolic"}
            ]

            delegate: Delegates.RoundedItemDelegate {
                width: ListView.view.width - ListView.view.leftMargin - ListView.view.rightMargin
                text: modelData.label
                icon.name: modelData.icon
                highlighted: ListView.isCurrentItem
                onClicked: categoryList.currentIndex = index
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        StackLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            currentIndex: categoryList.currentIndex

            ColumnLayout {
                spacing: 0

                FormCard.FormCard {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing

                    FormCard.FormTextFieldDelegate {
                        id: nameField
                        label: i18nc("@label:textbox", "VM Name:")
                        Layout.fillWidth: true
                        validator: RegularExpressionValidator {
                            regularExpression: /^[A-Za-z0-9._-]+$/
                        }
                    }

                    FormCard.FormDelegateSeparator {}

                    FormCard.FormCheckDelegate {
                        id: autostartCheck
                        text: i18n("Start on boot")
                        Layout.fillWidth: true
                        onToggled: {
                            if (configurationDialog.domain && checked !== configurationDialog.domain.autostart) {
                                Karton.setAutostart(configurationDialog.domain, checked);
                            }
                        }
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }

            ColumnLayout {
                spacing: 0

                FormCard.FormCard {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing

                    FormCard.FormSpinBoxDelegate {
                        id: memorySpinBox
                        label: i18nc("@label:spinbox, RAM", "Memory (GB)")
                        from: 1
                        to: Math.max(64, configurationDialog.domain ? configurationDialog.domain.config.maxRam : 64)
                        Layout.fillWidth: true
                    }

                    FormCard.FormDelegateSeparator {}

                    FormCard.FormSpinBoxDelegate {
                        id: cpuSpinBox
                        label: i18nc("@label:spinbox, number of cpus", "CPUs")
                        from: 1
                        to: Math.max(16, configurationDialog.domain ? configurationDialog.domain.config.cpus : 16)
                        Layout.fillWidth: true
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }

            ColumnLayout {
                spacing: 0

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing
                    wrapMode: Text.WordWrap
                    visible: configurationDialog.domain && configurationDialog.domain.config.maxDiskStorage <= 0
                    text: i18n("This virtual machine was not created by Karton, so its disk size is unknown and cannot be changed here.")
                }

                FormCard.FormCard {
                    Layout.fillWidth: true
                    Layout.margins: Kirigami.Units.largeSpacing

                    FormCard.FormSpinBoxDelegate {
                        id: storageSpinBox
                        enabled: configurationDialog.domain ? configurationDialog.domain.config.maxDiskStorage > 0 : false
                        label: i18nc("@label:spinbox", "Disk Storage (GB)")
                        from: configurationDialog.domain ? Math.min(2048, Math.max(1, configurationDialog.domain.config.maxDiskStorage)) : 1
                        to: Math.max(2048, configurationDialog.domain ? configurationDialog.domain.config.maxDiskStorage : 2048)
                        Layout.fillWidth: true
                    }
                }

                Item {
                    Layout.fillHeight: true
                }
            }
        }
    }
}
