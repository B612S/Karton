// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.delegates as Delegates
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

Kirigami.Dialog {
    id: root

    property Domain domain: null
    property bool saving: false
    property string pendingName: ""
    property var hardware: ({})
    property var disks: []
    property var interfaces: []
    property var usbDevices: []
    property var pciDevices: []

    readonly property var bootDevices: ["hd", "cdrom", "network"]
    readonly property var bootLabels: [i18n("Hard Disk"), i18n("CD-ROM"), i18n("Network")]
    readonly property var videoModels: ["qxl", "virtio", "vga"]
    readonly property var videoLabels: ["QXL", "VirtIO", "VGA"]
    readonly property var soundModels: ["ich9", "ac97", "none"]
    readonly property var soundLabels: ["ich9", "AC97", i18n("None")]

    title: domain ? i18n("Settings — %1", domain.config.name) : i18n("Settings")
    padding: 0
    modal: true
    preferredWidth: Kirigami.Units.gridUnit * 54
    preferredHeight: Kirigami.Units.gridUnit * 34

    function refresh() {
        if (!domain) {
            return;
        }
        hardware = Karton.domainHardwareInfo(domain);
        disks = Karton.domainDisks(domain);
        interfaces = Karton.domainInterfaces(domain);
        usbDevices = Karton.domainUsbDevices(domain);
        pciDevices = Karton.domainPciDevices(domain);

        firmwareCombo.currentIndex = hardware.firmware === "uefi" ? 1 : 0;
        secureBootCheck.checked = hardware.secureBoot === true;
        secureBootCheck.visible = firmwareCombo.currentIndex === 1;
        bootCombo.currentIndex = Math.max(0, bootDevices.indexOf(hardware.bootDev));
        videoCombo.currentIndex = Math.max(0, videoModels.indexOf(hardware.videoModel));
        videoHeadsSpin.value = Math.max(1, hardware.videoHeads || 1);
        accel3dCheck.checked = hardware.accel3d === true;
        soundCombo.currentIndex = Math.max(0, soundModels.indexOf(hardware.soundModel));
        descriptionField.text = hardware.description || "";
    }

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
        refresh();
    }

    function save() {
        if (!domain || nameField.text.trim() === "") {
            return;
        }

        const hw = {
            firmware: firmwareCombo.currentIndex === 1 ? "uefi" : "bios",
            secureBoot: secureBootCheck.checked,
            bootDev: bootDevices[bootCombo.currentIndex],
            videoModel: videoModels[videoCombo.currentIndex],
            videoHeads: videoHeadsSpin.value,
            accel3d: accel3dCheck.checked,
            soundModel: soundModels[soundCombo.currentIndex],
            description: descriptionField.text
        };

        if (!Karton.editDomainHardware(domain, hw)) {
            saving = false;
            return;
        }

        const name = nameField.text.trim();
        const changed = name !== domain.config.name
            || memorySpinBox.value !== domain.config.maxRam
            || cpuSpinBox.value !== domain.config.cpus;

        if (!changed) {
            close();
            applicationWindow().showPassiveNotification(i18n("Settings saved"));
            return;
        }

        pendingName = name;
        saving = true;
        const domainConfig = {
            name: name,
            memoryGB: memorySpinBox.value,
            storageGB: Math.max(1, domain.config.maxDiskStorage),
            cpus: cpuSpinBox.value
        };
        if (!Karton.editDomain(domain, domainConfig)) {
            saving = false;
            pendingName = "";
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: i18n("Add Hardware")
            icon.name: "list-add-symbolic"
            enabled: root.domain !== null
            onTriggered: addHardwareMenu.popup()
        },
        Kirigami.Action {
            text: i18n("Save")
            icon.name: "dialog-ok-symbolic"
            enabled: !root.saving && root.domain !== null
            onTriggered: root.save()
        },
        Kirigami.Action {
            text: i18n("Close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        }
    ]

    Connections {
        target: Karton
        function onDomainEdited(success, name) {
            if (name !== root.pendingName) {
                return;
            }
            root.saving = false;
            root.pendingName = "";
            if (success) {
                root.close();
                applicationWindow().showPassiveNotification(i18n("Settings saved"));
            }
        }
    }

    Controls.Menu {
        id: addHardwareMenu

        Controls.MenuItem {
            text: i18n("Disk image")
            icon.name: "drive-harddisk-symbolic"
            onTriggered: storageDialog.open()
        }
        Controls.MenuItem {
            text: i18n("ISO image")
            icon.name: "media-optical-symbolic"
            onTriggered: storageDialog.open()
        }
        Controls.MenuItem {
            text: i18n("Network interface")
            icon.name: "network-wired-symbolic"
            onTriggered: networksDialog.open()
        }
        Controls.MenuItem {
            text: i18n("USB device")
            icon.name: "drive-removable-media-usb-symbolic"
            onTriggered: usbDialog.open()
        }
        Controls.MenuItem {
            text: i18n("PCI device")
            icon.name: "computer-symbolic"
            onTriggered: pciDialog.open()
        }
    }

    RowLayout {
        spacing: 0

        ListView {
            id: categoryList
            Layout.preferredWidth: Kirigami.Units.gridUnit * 12
            Layout.fillHeight: true
            currentIndex: 0

            model: [
                {label: i18n("Overview"), icon: "computer-symbolic"},
                {label: i18n("CPUs & Memory"), icon: "preferences-system-symbolic"},
                {label: i18n("Boot Options"), icon: "system-reboot-symbolic"},
                {label: i18n("Disks"), icon: "drive-harddisk-symbolic"},
                {label: i18n("Network"), icon: "network-wired-symbolic"},
                {label: i18n("Display"), icon: "video-symbolic"},
                {label: i18n("Sound"), icon: "audio-volume-high-symbolic"},
                {label: i18n("USB & PCI"), icon: "drive-removable-media-usb-symbolic"}
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

            // Overview
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing

                        FormCard.FormTextFieldDelegate {
                            id: nameField
                            label: i18n("Name:")
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
                            onToggled: if (root.domain && checked !== root.domain.autostart) Karton.setAutostart(root.domain, checked)
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextFieldDelegate {
                            id: descriptionField
                            label: i18n("Description:")
                            Layout.fillWidth: true
                        }
                    }
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormTextDelegate {
                            text: i18n("UUID")
                            description: root.domain ? root.domain.config.uuid : ""
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextDelegate {
                            text: i18n("State")
                            description: root.domain ? root.domain.config.state : ""
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // CPUs and memory
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormSpinBoxDelegate {
                            id: memorySpinBox
                            label: i18n("Memory (GB)")
                            from: 1
                            to: Math.max(1024, root.domain ? root.domain.config.maxRam : 64)
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormSpinBoxDelegate {
                            id: cpuSpinBox
                            label: i18n("CPUs")
                            from: 1
                            to: 256
                            Layout.fillWidth: true
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        text: i18n("Changing memory or CPU count applies after the VM is restarted.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Boot options
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormComboBoxDelegate {
                            id: firmwareCombo
                            text: i18n("Firmware:")
                            model: ["BIOS", "UEFI"]
                            Layout.fillWidth: true
                            onCurrentIndexChanged: secureBootCheck.visible = currentIndex === 1
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormCheckDelegate {
                            id: secureBootCheck
                            text: i18n("Secure Boot")
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormComboBoxDelegate {
                            id: bootCombo
                            text: i18n("Boot device:")
                            model: root.bootLabels
                            Layout.fillWidth: true
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        text: i18n("Firmware and boot order are written to the persistent VM configuration.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Disks
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Disks and CD-ROMs")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 12
                        clip: true
                        model: root.disks
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            text: (modelData.device === "cdrom" ? i18n("CD-ROM") : i18n("Disk")) + " · " + modelData.target + " — " + modelData.source
                        }
                    }
                    Controls.Button {
                        Layout.alignment: Qt.AlignRight
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        text: i18n("Manage Disks…")
                        icon.name: "drive-harddisk-symbolic"
                        onClicked: storageDialog.open()
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Network
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Network interfaces")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 12
                        clip: true
                        model: root.interfaces
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            text: (modelData.source.length > 0 ? modelData.source : modelData.type)
                                  + (modelData.mac.length > 0 ? " · " + modelData.mac : "")
                                  + (modelData.model.length > 0 ? " · " + modelData.model : "")
                        }
                    }
                    Controls.Button {
                        Layout.alignment: Qt.AlignRight
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        text: i18n("Manage Networks…")
                        icon.name: "network-wired-symbolic"
                        onClicked: networksDialog.open()
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Display
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormComboBoxDelegate {
                            id: videoCombo
                            text: i18n("Video model:")
                            model: root.videoLabels
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormSpinBoxDelegate {
                            id: videoHeadsSpin
                            label: i18n("Heads")
                            from: 1
                            to: 4
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormCheckDelegate {
                            id: accel3dCheck
                            text: i18n("3D acceleration")
                            Layout.fillWidth: true
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Sound
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormComboBoxDelegate {
                            id: soundCombo
                            text: i18n("Sound model:")
                            model: root.soundLabels
                            Layout.fillWidth: true
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // USB and PCI
            Controls.ScrollView {
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("USB devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                        clip: true
                        model: root.usbDevices
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            text: modelData.vendor + ":" + modelData.product
                        }
                    }
                    Controls.Button {
                        Layout.alignment: Qt.AlignRight
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        text: i18n("Manage USB Devices…")
                        icon.name: "drive-removable-media-usb-symbolic"
                        onClicked: usbDialog.open()
                    }
                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("PCI devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                        clip: true
                        model: root.pciDevices
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            text: modelData.label
                        }
                    }
                    Controls.Button {
                        Layout.alignment: Qt.AlignRight
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        text: i18n("Manage PCI Devices…")
                        icon.name: "computer-symbolic"
                        onClicked: pciDialog.open()
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    StorageDialog {
        id: storageDialog
        domain: root.domain
        onClosed: root.refresh()
    }

    NetworksDialog {
        id: networksDialog
        domain: root.domain
        onClosed: root.refresh()
    }

    UsbDevicesDialog {
        id: usbDialog
        domain: root.domain
        onClosed: root.refresh()
    }

    PciDevicesDialog {
        id: pciDialog
        domain: root.domain
        onClosed: root.refresh()
    }
}
