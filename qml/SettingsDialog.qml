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

Kirigami.ApplicationWindow {
    id: root

    visible: false
    modality: Qt.ApplicationModal
    flags: Qt.Dialog

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
    property var usbHostDevices: []
    property var performance: ({})

    readonly property var bootDevices: ["hd", "cdrom", "network"]
    readonly property var bootLabels: [i18n("Hard Disk"), i18n("CD-ROM"), i18n("Network")]
    readonly property var videoModels: ["qxl", "virtio", "vga"]
    readonly property var videoLabels: ["QXL", "VirtIO", "VGA"]
    readonly property var soundModels: ["ich9", "ac97", "none"]
    readonly property var soundLabels: ["ich9", "AC97", i18n("None")]

    title: domain ? i18n("Settings — %1", domain.config.name) : i18n("Settings")

    readonly property var screenInfo: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null
    readonly property real adaptiveWidth: screenInfo ? Math.min(1000, screenInfo.width * 0.72) : 1000
    readonly property real adaptiveHeight: screenInfo ? Math.min(680, screenInfo.height * 0.72) : 680
    width: adaptiveWidth
    height: adaptiveHeight
    minimumWidth: Math.min(720, width)
    minimumHeight: Math.min(480, height)
    x: screenInfo ? Math.round((screenInfo.width - width) / 2) : 0
    y: screenInfo ? Math.round((screenInfo.height - height) / 2) : 0

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
        usbHostDevices = Karton.usbDevices();
        performance = Karton.domainStats(domain);

        firmwareCombo.currentIndex = hardware.firmware === "uefi" ? 1 : 0;
        secureBootCheck.checked = hardware.secureBoot === true;
        secureBootCheck.visible = firmwareCombo.currentIndex === 1;
        bootCombo.currentIndex = Math.max(0, bootDevices.indexOf(hardware.bootDev));
        videoCombo.currentIndex = Math.max(0, videoModels.indexOf(hardware.videoModel));
        videoHeadsSpin.value = Math.max(1, hardware.videoHeads || 1);
        accel3dCheck.checked = hardware.accel3d === true;
        soundCombo.currentIndex = Math.max(0, soundModels.indexOf(hardware.soundModel));
        descriptionField.text = hardware.description || "";
        if (disks.length > 0) {
            if (diskTargetCombo.currentIndex < 0) {
                diskTargetCombo.currentIndex = 0;
            }
            Qt.callLater(function() { root.refreshDiskAdvanced(); });
        }
        tpmEnabledCheck.checked = hardware.tpmEnabled === true;
        tpmModelCombo.currentIndex = Math.max(0, ["tpm-crb", "tpm-tis"].indexOf(hardware.tpmModel));
        tpmVersionCombo.currentIndex = Math.max(0, ["2.0", "1.2"].indexOf(hardware.tpmVersion));
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function refreshDiskAdvanced() {
        if (!root.domain || !diskTargetCombo.currentText) {
            return;
        }
        const advanced = Karton.domainDiskAdvanced(root.domain, diskTargetCombo.currentText);
        diskReadonlyCheck.checked = advanced.readonly === true;
        diskShareableCheck.checked = advanced.shareable === true;
        diskSerialField.text = advanced.serial || "";
        diskCacheCombo.currentIndex = Math.max(0, ["", "none", "writethrough", "writeback", "directsync", "unsafe"].indexOf(advanced.cache || ""));
        diskDiscardCombo.currentIndex = Math.max(0, ["", "unmap", "ignore"].indexOf(advanced.discard || ""));
    }

    onVisibleChanged: {
        if (!visible || !domain) {
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
            tpmEnabled: tpmEnabledCheck.checked,
            tpmModel: ["tpm-crb", "tpm-tis"][tpmModelCombo.currentIndex],
            tpmVersion: ["2.0", "1.2"][tpmVersionCombo.currentIndex],
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
            onTriggered: categoryList.currentIndex = 7
        }
        Controls.MenuItem {
            text: i18n("PCI device")
            icon.name: "computer-symbolic"
            onTriggered: categoryList.currentIndex = 8
        }
        Controls.MenuItem {
            text: i18n("TPM device")
            icon.name: "security-high-symbolic"
            onTriggered: categoryList.currentIndex = 9
        }
    }

    ColumnLayout {
        anchors.fill: parent
        spacing: 0

    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: 0

        ListView {
            id: categoryList
            Layout.preferredWidth: Kirigami.Units.gridUnit * 10
            Layout.fillHeight: true
            currentIndex: 0

            model: [
                {label: i18n("Overview"), icon: "computer-symbolic"},
                {label: i18n("CPUs & Memory"), icon: "preferences-system-symbolic"},
                {label: i18n("Boot Options"), icon: "system-reboot-symbolic"},
                {label: i18n("Disks"), icon: "drive-harddisk-symbolic"},
                {label: i18n("Network"), icon: "network-wired-symbolic"},
                {label: i18n("Display"), icon: "view-fullscreen-symbolic"},
                {label: i18n("Sound"), icon: "audio-volume-high-symbolic"},
                {label: i18n("USB"), icon: "drive-removable-media-usb-symbolic"},
                {label: i18n("PCI"), icon: "computer-symbolic"},
                {label: i18n("TPM"), icon: "security-high-symbolic"},
                {label: i18n("OS Information"), icon: "document-properties-symbolic"},
                {label: i18n("Performance"), icon: "speedometer-symbolic"}
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

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Kirigami.Heading {
                Layout.fillWidth: true
                Layout.margins: Kirigami.Units.largeSpacing
                text: categoryList.currentIndex >= 0 && categoryList.currentIndex < categoryList.model.length
                    ? categoryList.model[categoryList.currentIndex].label
                    : ""
                level: 3
            }
            Kirigami.Separator {
                Layout.fillWidth: true
                opacity: 0.5
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                clip: true
                currentIndex: categoryList.currentIndex

            // Overview
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
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
                Layout.fillHeight: true
                implicitHeight: 0
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
                Layout.fillHeight: true
                implicitHeight: 0
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
                Layout.fillHeight: true
                implicitHeight: 0
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
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 6
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

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Create a new disk")
                        font.bold: true
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
                            currentIndex: 0
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

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Attach existing media")
                        font.bold: true
                    }
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing

                        Controls.Button {
                            Layout.fillWidth: true
                            Layout.margins: Kirigami.Units.largeSpacing
                            text: i18n("Attach disk image…")
                            icon.name: "document-open-symbolic"
                            onClicked: imageFileDialog.open()
                        }
                        FormCard.FormDelegateSeparator {}
                        Controls.Button {
                            Layout.fillWidth: true
                            Layout.margins: Kirigami.Units.largeSpacing
                            text: i18n("Attach ISO image…")
                            icon.name: "media-optical-symbolic"
                            onClicked: isoFileDialog.open()
                        }
                    }

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Advanced disk options")
                        font.bold: true
                    }
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing

                        FormCard.FormComboBoxDelegate {
                            id: diskTargetCombo
                            text: i18n("Disk:")
                            model: root.disks.filter(function(disk) { return disk.device === "disk"; }).map(function(disk) { return disk.target; })
                            Layout.fillWidth: true
                            onCurrentTextChanged: root.refreshDiskAdvanced()
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormCheckDelegate {
                            id: diskReadonlyCheck
                            text: i18n("Read-only")
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormCheckDelegate {
                            id: diskShareableCheck
                            text: i18n("Shareable")
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextFieldDelegate {
                            id: diskSerialField
                            label: i18n("Serial:")
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormComboBoxDelegate {
                            id: diskCacheCombo
                            text: i18n("Cache mode:")
                            model: [i18n("Hypervisor default"), "none", "writethrough", "writeback", "directsync", "unsafe"]
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormComboBoxDelegate {
                            id: diskDiscardCombo
                            text: i18n("Discard mode:")
                            model: [i18n("Hypervisor default"), "unmap", "ignore"]
                            Layout.fillWidth: true
                        }
                    }
                    Controls.Button {
                        Layout.alignment: Qt.AlignRight
                        Layout.rightMargin: Kirigami.Units.largeSpacing
                        text: i18n("Apply disk options")
                        icon.name: "dialog-ok-symbolic"
                        enabled: diskTargetCombo.currentText.length > 0
                        onClicked: {
                            const advanced = {
                                readonly: diskReadonlyCheck.checked,
                                shareable: diskShareableCheck.checked,
                                serial: diskSerialField.text,
                                cache: diskCacheCombo.currentIndex === 0 ? "" : diskCacheCombo.currentText,
                                discard: diskDiscardCombo.currentIndex === 0 ? "" : diskDiscardCombo.currentText
                            };
                            if (Karton.editDomainDiskAdvanced(root.domain, diskTargetCombo.currentText, advanced)) {
                                root.refresh();
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Network
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
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
                Layout.fillHeight: true
                implicitHeight: 0
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
                Layout.fillHeight: true
                implicitHeight: 0
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

            // USB
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Persistent USB devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 5
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
                                        if (Karton.detachUsbDevicePersistent(root.domain, modelData.vendor, modelData.product, modelData.bus, modelData.device)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }

                    Controls.Label {
                        Layout.margins: Kirigami.Units.largeSpacing
                        text: i18n("Available host USB devices")
                        font.bold: true
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        Layout.preferredHeight: Kirigami.Units.gridUnit * 9
                        clip: true
                        model: root.usbHostDevices
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
                                        text: modelData.vendor + ":" + modelData.product + " · bus " + modelData.bus + ", device " + modelData.device
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Attach")
                                    icon.name: "list-add-symbolic"
                                    enabled: !modelData.hub
                                    onClicked: {
                                        if (Karton.attachUsbDevicePersistent(root.domain, modelData.vendor, modelData.product, modelData.bus, modelData.device)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        text: i18n("Devices attached here are stored in the persistent VM configuration. For live passthrough while the VM is running, use USB Devices in the viewer menu.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // PCI
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width

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

            // TPM
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing

                        FormCard.FormCheckDelegate {
                            id: tpmEnabledCheck
                            text: i18n("Enable TPM")
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormComboBoxDelegate {
                            id: tpmModelCombo
                            text: i18n("TPM model:")
                            model: ["CRB", "TIS"]
                            Layout.fillWidth: true
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormComboBoxDelegate {
                            id: tpmVersionCombo
                            text: i18n("TPM version:")
                            model: ["2.0", "1.2"]
                            Layout.fillWidth: true
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        wrapMode: Text.WordWrap
                        opacity: 0.7
                        text: i18n("TPM emulation requires swtpm in the host and is applied to the persistent VM configuration.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // OS information
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormTextDelegate {
                            text: i18n("Name")
                            description: root.domain ? root.domain.config.name : ""
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextDelegate {
                            text: i18n("UUID")
                            description: root.domain ? root.domain.config.uuid : ""
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextDelegate {
                            text: i18n("OS ID")
                            description: root.domain ? root.domain.config.osId : ""
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextDelegate {
                            text: i18n("Short OS ID")
                            description: root.domain ? root.domain.config.shortOsId : ""
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

            // Performance
            Controls.ScrollView {
                Layout.fillHeight: true
                implicitHeight: 0
                contentWidth: availableWidth
                ColumnLayout {
                    width: parent.width
                    FormCard.FormCard {
                        Layout.fillWidth: true
                        Layout.margins: Kirigami.Units.largeSpacing
                        FormCard.FormTextDelegate {
                            text: i18n("CPU Usage")
                            description: root.performance.cpuPercent !== undefined ? i18n("%1 %", root.performance.cpuPercent.toFixed(1)) : "—"
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextDelegate {
                            text: i18n("Memory Usage")
                            description: root.performance.memoryKiB !== undefined
                                ? i18n("%1 MiB of %2 MiB", (root.performance.memoryKiB / 1024).toFixed(0), (root.performance.maxMemoryKiB / 1024).toFixed(0))
                                : "—"
                        }
                        FormCard.FormDelegateSeparator {}
                        FormCard.FormTextDelegate {
                            text: i18n("vCPUs")
                            description: root.performance.vcpus !== undefined ? root.performance.vcpus : ""
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }
            }
        }
    }

    Controls.ToolBar {
        Layout.fillWidth: true
        contentItem: RowLayout {
            Item { Layout.fillWidth: true }
            Controls.Button {
                text: i18n("Add Hardware")
                icon.name: "list-add-symbolic"
                enabled: root.domain !== null
                onClicked: addHardwareMenu.popup()
            }
            Controls.Button {
                text: i18n("Save")
                icon.name: "dialog-ok-symbolic"
                enabled: !root.saving && root.domain !== null
                onClicked: root.save()
            }
            Controls.Button {
                text: i18n("Close")
                icon.name: "dialog-cancel-symbolic"
                onClicked: root.close()
            }
        }
    }
    }

    FileDialog {
        id: imageFileDialog
        title: i18n("Select a disk image")
        nameFilters: [i18n("Disk images (*.qcow2 *.img *.raw *.vmdk)"), i18n("All files (*)")]
        onAccepted: {
            if (Karton.attachDiskImage(root.domain, root.localPath(selectedFile), "qcow2")) {
                root.refresh();
            }
        }
    }

    FileDialog {
        id: isoFileDialog
        title: i18n("Select an ISO image")
        nameFilters: [i18n("ISO images (*.iso)"), i18n("All files (*)")]
        onAccepted: {
            if (Karton.attachIso(root.domain, root.localPath(selectedFile))) {
                root.refresh();
            }
        }
    }

}
