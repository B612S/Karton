// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
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
    property var networks: []
    property var pciHostDevices: []

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
        networks = Karton.networks();
        pciHostDevices = Karton.pciDevices();

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

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
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
            onTriggered: categoryList.currentIndex = 3
        }
        Controls.MenuItem {
            text: i18n("ISO image")
            icon.name: "media-optical-symbolic"
            onTriggered: categoryList.currentIndex = 3
        }
        Controls.MenuItem {
            text: i18n("Network interface")
            icon.name: "network-wired-symbolic"
            onTriggered: categoryList.currentIndex = 4
        }
        Controls.MenuItem {
            text: i18n("USB device")
            icon.name: "drive-removable-media-usb-symbolic"
            onTriggered: usbDialog.open()
        }
        Controls.MenuItem {
            text: i18n("PCI device")
            icon.name: "computer-symbolic"
            onTriggered: categoryList.currentIndex = 7
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
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 10
                        clip: true
                        model: root.disks
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
                                        text: (modelData.device === "cdrom" ? i18n("CD-ROM") : i18n("Disk")) + " · " + modelData.target
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.source + " · " + modelData.bus
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachDisk(root.domain, modelData.target, false)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing

                        FormCard.FormSpinBoxDelegate {
                            id: newDiskSize
                            label: i18n("New disk size (GB)")
                            from: 1
                            to: 2048
                            value: 8
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormComboBoxDelegate {
                            id: newDiskFormat
                            text: i18n("Format:")
                            model: ["qcow2", "raw", "vmdk"]
                            Layout.fillWidth: true
                        }
                        Controls.Button {
                            Layout.alignment: Qt.AlignRight
                            text: i18n("Create disk")
                            icon.name: "list-add-symbolic"
                            onClicked: {
                                if (Karton.addDisk(root.domain, newDiskSize.value, newDiskFormat.currentText)) {
                                    root.refresh();
                                }
                            }
                        }
                    }

                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing

                        FormCard.FormTextFieldDelegate {
                            id: imagePathField
                            label: i18n("Disk image path:")
                            Layout.fillWidth: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.largeSpacing
                            Layout.rightMargin: Kirigami.Units.largeSpacing
                            Controls.Button {
                                text: i18n("Browse…")
                                icon.name: "document-open-symbolic"
                                onClicked: imageFileDialog.open()
                            }
                            Controls.Button {
                                Layout.fillWidth: true
                                text: i18n("Attach image")
                                icon.name: "list-add-symbolic"
                                enabled: imagePathField.text.trim().length > 0
                                onClicked: {
                                    if (Karton.attachDiskImage(root.domain, imagePathField.text.trim(), "qcow2")) {
                                        imagePathField.text = "";
                                        root.refresh();
                                    }
                                }
                            }
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextFieldDelegate {
                            id: isoPathField
                            label: i18n("ISO image path:")
                            Layout.fillWidth: true
                        }
                        RowLayout {
                            Layout.fillWidth: true
                            Layout.leftMargin: Kirigami.Units.largeSpacing
                            Layout.rightMargin: Kirigami.Units.largeSpacing
                            Controls.Button {
                                text: i18n("Browse…")
                                icon.name: "document-open-symbolic"
                                onClicked: isoFileDialog.open()
                            }
                            Controls.Button {
                                Layout.fillWidth: true
                                text: i18n("Attach ISO")
                                icon.name: "media-optical-symbolic"
                                enabled: isoPathField.text.trim().length > 0
                                onClicked: {
                                    if (Karton.attachIso(root.domain, isoPathField.text.trim())) {
                                        isoPathField.text = "";
                                        root.refresh();
                                    }
                                }
                            }
                        }
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
                        text: i18n("Libvirt networks")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 9
                        clip: true
                        model: root.networks
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
                                        text: modelData.name
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: (modelData.active ? i18n("active") : i18n("inactive"))
                                              + " · " + modelData.forward
                                              + (modelData.bridge.length > 0 ? " · " + modelData.bridge : "")
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: modelData.active ? i18n("Stop") : i18n("Start")
                                    icon.name: modelData.active ? "media-playback-stop-symbolic" : "media-playback-start-symbolic"
                                    onClicked: {
                                        if (modelData.active) {
                                            Karton.stopNetwork(modelData.name);
                                        } else {
                                            Karton.startNetwork(modelData.name);
                                        }
                                        root.refresh();
                                    }
                                }
                                Controls.CheckBox {
                                    text: i18n("Autostart")
                                    checked: modelData.autostart
                                    onToggled: {
                                        Karton.setNetworkAutostart(modelData.name, checked);
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Controls.TextField {
                            id: newNetworkField
                            Layout.fillWidth: true
                            placeholderText: i18n("New NAT network name")
                        }
                        Controls.Button {
                            text: i18n("Create NAT")
                            icon.name: "list-add-symbolic"
                            enabled: newNetworkField.text.trim().length > 0
                            onClicked: {
                                if (Karton.createNatNetwork(newNetworkField.text.trim())) {
                                    newNetworkField.text = "";
                                    root.refresh();
                                }
                            }
                        }
                    }

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("VM interfaces")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 8
                        clip: true
                        model: root.interfaces
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
                                        text: (modelData.source.length > 0 ? modelData.source : modelData.type)
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.mac + (modelData.model.length > 0 ? " · " + modelData.model : "")
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Detach")
                                    icon.name: "list-remove-symbolic"
                                    enabled: modelData.mac.length > 0
                                    onClicked: {
                                        if (Karton.detachNetwork(root.domain, modelData.mac)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Controls.ComboBox {
                            id: attachNetworkCombo
                            Layout.fillWidth: true
                            model: root.networks.map(function(network) { return network.name; })
                        }
                        Controls.ComboBox {
                            id: attachModelCombo
                            Layout.preferredWidth: Kirigami.Units.gridUnit * 9
                            model: ["virtio", "e1000"]
                        }
                        Controls.Button {
                            text: i18n("Attach")
                            icon.name: "list-add-symbolic"
                            enabled: attachNetworkCombo.count > 0
                            onClicked: {
                                if (Karton.attachNetwork(root.domain, attachNetworkCombo.currentText, attachModelCombo.currentText)) {
                                    root.refresh();
                                }
                            }
                        }
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
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData.vendor + ":" + modelData.product
                                    elide: Text.ElideRight
                                }
                                Controls.Button {
                                    text: i18n("Detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachUsbDevice(root.domain, modelData.vendor, modelData.product, modelData.bus, modelData.device)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Controls.Label {
                            Layout.fillWidth: true
                            text: i18n("USB passthrough is a runtime operation.")
                            opacity: 0.7
                            wrapMode: Text.WordWrap
                        }
                        Controls.Button {
                            text: i18n("Manage USB Devices…")
                            icon.name: "drive-removable-media-usb-symbolic"
                            onClicked: usbDialog.open()
                        }
                    }

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Attached PCI devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                        clip: true
                        model: root.pciDevices
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData.address
                                    elide: Text.ElideRight
                                }
                                Controls.Button {
                                    text: i18n("Detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachPciDevice(root.domain, modelData.pciDomain, modelData.bus, modelData.slot, modelData.function)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Available PCI devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 9
                        clip: true
                        model: root.pciHostDevices
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
                                        text: modelData.label
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.address
                                              + (modelData.iommuGroup.length > 0 ? " · IOMMU " + modelData.iommuGroup : " · " + i18n("no IOMMU group"))
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Attach")
                                    icon.name: "list-add-symbolic"
                                    enabled: modelData.iommuGroup.length > 0
                                    onClicked: {
                                        if (Karton.attachPciDevice(root.domain, modelData.pciDomain, modelData.bus, modelData.slot, modelData.function)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.fillWidth: true
                        text: i18n("Attaching a PCI device detaches it from the host. Devices without an IOMMU group cannot be attached.")
                        opacity: 0.7
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                }
            }
        }
    }

    UsbDevicesDialog {
        id: usbDialog
        domain: root.domain
        onClosed: root.refresh()
    }

    FileDialog {
        id: imageFileDialog
        title: i18n("Select a disk image")
        nameFilters: [i18n("Disk images (*.qcow2 *.img *.raw *.vmdk)"), i18n("All files (*)")]
        onAccepted: imagePathField.text = root.localPath(selectedFile)
    }

    FileDialog {
        id: isoFileDialog
        title: i18n("Select an ISO image")
        nameFilters: [i18n("ISO images (*.iso)"), i18n("All files (*)")]
        onAccepted: isoPathField.text = root.localPath(selectedFile)
    }

}
