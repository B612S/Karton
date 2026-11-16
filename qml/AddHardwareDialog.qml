// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.Dialog {
    id: root

    property Domain domain: null
    property int categoryIndex: 0
    property var networks: []
    property var usbHostDevices: []
    property var pciHostDevices: []
    property string message: ""

    signal deviceAdded()

    modal: true
    padding: 0
    width: applicationWindow() ? Math.min(760, applicationWindow().width - 48) : 760
    height: applicationWindow() ? Math.min(600, applicationWindow().height - 48) : 600
    header: Item {}

    readonly property var categories: [
        { label: i18n("Storage") },
        { label: i18n("Network") },
        { label: i18n("Input") },
        { label: i18n("Controller") },
        { label: i18n("Serial") },
        { label: i18n("Console") },
        { label: i18n("Channel") },
        { label: i18n("USB Host Device") },
        { label: i18n("PCI Host Device") },
        { label: i18n("Redirection") },
        { label: i18n("Watchdog") },
        { label: i18n("RNG") },
        { label: i18n("TPM") }
    ]

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.95)
        radius: 8
        border.color: "transparent"
    }

    onOpened: {
        refresh();
        categoryIndex = 0;
        message = "";
    }

    function refresh() {
        networks = Karton.networks();
        usbHostDevices = Karton.usbDevices();
        pciHostDevices = Karton.pciDevices();
    }

    function added() {
        refresh();
        message = i18n("Device added");
        deviceAdded();
    }

    function addXml(xml) {
        if (Karton.attachXmlDevice(domain, xml)) {
            added();
        }
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: root.height

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                Layout.leftMargin: 24
                Layout.rightMargin: 12
                Controls.Label {
                    text: i18n("Add New Virtual Hardware")
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                    Layout.fillWidth: true
                }
                Controls.ToolButton {
                    icon.name: "dialog-close"
                    display: Controls.AbstractButton.IconOnly
                    onClicked: root.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.preferredWidth: 190
                    Layout.fillHeight: true
                    radius: 8
                    color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                   Kirigami.Theme.backgroundColor.g,
                                   Kirigami.Theme.backgroundColor.b, 0.5)

                    ListView {
                        id: categoryList
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 2
                        model: root.categories
                        currentIndex: 0
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            height: 32
                            focusPolicy: Qt.NoFocus
                            highlighted: ListView.isCurrentItem
                            onClicked: root.categoryIndex = index
                            contentItem: Controls.Label {
                                text: modelData.label
                                elide: Text.ElideRight
                                font.pixelSize: 13
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 20
                    spacing: 12

                    Controls.Label {
                        text: root.categories[root.categoryIndex].label
                        font.pixelSize: 18
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }

                    Controls.ScrollView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        contentWidth: availableWidth
                        clip: true

                        ColumnLayout {
                            width: parent.width
                            spacing: 12

                            // Storage
                            ColumnLayout {
                                visible: root.categoryIndex === 0
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Add a disk image or an ISO image"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: i18n("Choose disk image…")
                                    icon.name: "media-optical-symbolic"
                                    Layout.fillWidth: true
                                    onClicked: diskFileDialog.open()
                                }
                                Controls.Button {
                                    text: i18n("Choose ISO image…")
                                    icon.name: "media-optical-symbolic"
                                    Layout.fillWidth: true
                                    onClicked: isoFileDialog.open()
                                }
                                Controls.Label { text: i18n("Or create a new empty disk"); Layout.fillWidth: true; Layout.topMargin: 8 }
                                RowLayout {
                                    Layout.fillWidth: true
                                    spacing: 8
                                    Controls.SpinBox {
                                        id: addDiskSize
                                        from: 1
                                        to: 2048
                                        value: 8
                                        Layout.fillWidth: true
                                    }
                                    Controls.ComboBox {
                                        id: addDiskFormat
                                        model: ["qcow2", "raw", "vmdk"]
                                        Layout.preferredWidth: 120
                                    }
                                    Controls.Button {
                                        text: i18n("Create disk")
                                        icon.name: "list-add-symbolic"
                                        onClicked: {
                                            if (Karton.addDisk(root.domain, addDiskSize.value, addDiskFormat.currentText)) {
                                                root.added();
                                            }
                                        }
                                    }
                                }
                            }

                            // Network
                            ColumnLayout {
                                visible: root.categoryIndex === 1
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Network"); Layout.fillWidth: true }
                                Controls.ComboBox {
                                    id: networkCombo
                                    Layout.fillWidth: true
                                    model: root.networks.map(function(network) { return network.name; })
                                }
                                Controls.ComboBox {
                                    id: networkModelCombo
                                    Layout.fillWidth: true
                                    model: ["virtio", "e1000"]
                                }
                                Controls.Button {
                                    text: i18n("Add network interface")
                                    icon.name: "list-add-symbolic"
                                    enabled: networkCombo.count > 0
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.attachNetwork(root.domain, networkCombo.currentText, networkModelCombo.currentText)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Input
                            ColumnLayout {
                                visible: root.categoryIndex === 2
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Input device"); Layout.fillWidth: true }
                                Controls.ComboBox {
                                    id: inputTypeCombo
                                    Layout.fillWidth: true
                                    model: ["tablet", "mouse", "keyboard"]
                                }
                                Controls.ComboBox {
                                    id: inputBusCombo
                                    Layout.fillWidth: true
                                    model: ["usb", "ps2", "virtio"]
                                }
                                Controls.Button {
                                    text: i18n("Add input device")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.attachInputDevice(root.domain, inputTypeCombo.currentText, inputBusCombo.currentText)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Controller
                            ColumnLayout {
                                visible: root.categoryIndex === 3
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Controller type"); Layout.fillWidth: true }
                                Controls.ComboBox {
                                    id: controllerTypeCombo
                                    Layout.fillWidth: true
                                    model: ["usb", "pci", "sata", "virtio-serial"]
                                }
                                Controls.Button {
                                    text: i18n("Add controller")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addController(root.domain, controllerTypeCombo.currentText)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Serial
                            ColumnLayout {
                                visible: root.categoryIndex === 4
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Add a serial port"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: i18n("Add serial port")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addSerialPort(root.domain)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Console
                            ColumnLayout {
                                visible: root.categoryIndex === 5
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Add a text console"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: i18n("Add text console")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addTextConsole(root.domain)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // Channel
                            ColumnLayout {
                                visible: root.categoryIndex === 6
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("VirtIO channel"); Layout.fillWidth: true }
                                Controls.ComboBox {
                                    id: channelCombo
                                    Layout.fillWidth: true
                                    model: [
                                        { label: i18n("QEMU Guest Agent"), name: "org.qemu.guest_agent.0" },
                                        { label: i18n("SPICE Agent"), name: "com.redhat.spice.0" },
                                        { label: i18n("SPICE WebDAV"), name: "org.spice-space.webdav.0" }
                                    ]
                                    textRole: "label"
                                }
                                Controls.Button {
                                    text: i18n("Add channel")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        if (Karton.addChannel(root.domain, channelCombo.model[channelCombo.currentIndex].name)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            // USB host
                            ColumnLayout {
                                visible: root.categoryIndex === 7
                                Layout.fillWidth: true
                                spacing: 8
                                Controls.Label { text: i18n("USB host devices"); Layout.fillWidth: true }
                                Repeater {
                                    model: root.usbHostDevices
                                    Controls.ItemDelegate {
                                        Layout.fillWidth: true
                                        contentItem: RowLayout {
                                            Controls.Label {
                                                text: modelData.label + (modelData.vendor ? " (" + modelData.vendor + ":" + modelData.product + ")" : "")
                                                elide: Text.ElideRight
                                                Layout.fillWidth: true
                                            }
                                            Controls.Button {
                                                text: i18n("Attach")
                                                icon.name: "list-add-symbolic"
                                                enabled: !modelData.hub
                                                onClicked: {
                                                    if (Karton.attachUsbDevicePersistent(root.domain, modelData.vendor, modelData.product, modelData.bus, modelData.device)) {
                                                        root.added();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // PCI host
                            ColumnLayout {
                                visible: root.categoryIndex === 8
                                Layout.fillWidth: true
                                spacing: 8
                                Controls.Label { text: i18n("PCI host devices"); Layout.fillWidth: true }
                                Repeater {
                                    model: root.pciHostDevices
                                    Controls.ItemDelegate {
                                        Layout.fillWidth: true
                                        contentItem: RowLayout {
                                            ColumnLayout {
                                                Layout.fillWidth: true
                                                spacing: 0
                                                Controls.Label { text: modelData.label; elide: Text.ElideRight; Layout.fillWidth: true }
                                                Controls.Label {
                                                    text: modelData.address + (modelData.iommuGroup.length > 0 ? " · IOMMU " + modelData.iommuGroup : "")
                                                    opacity: 0.7
                                                    font.pixelSize: 11
                                                }
                                            }
                                            Controls.Button {
                                                text: i18n("Attach")
                                                icon.name: "list-add-symbolic"
                                                enabled: modelData.iommuGroup.length > 0
                                                onClicked: {
                                                    if (Karton.attachPciDevice(root.domain, modelData.pciDomain, modelData.bus, modelData.slot, modelData.function)) {
                                                        root.added();
                                                    }
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            // Redirection
                            ColumnLayout {
                                visible: root.categoryIndex === 9
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("USB redirection over SPICE"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: i18n("Add redirection device")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: root.addXml("<redirdev bus='usb' type='spicevmc'/>")
                                }
                            }

                            // Watchdog
                            ColumnLayout {
                                visible: root.categoryIndex === 10
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Watchdog device"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: i18n("Add watchdog")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: root.addXml("<watchdog model='i6300esb' action='reset'/>")
                                }
                            }

                            // RNG
                            ColumnLayout {
                                visible: root.categoryIndex === 11
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label { text: i18n("Random number generator"); Layout.fillWidth: true }
                                Controls.Button {
                                    text: i18n("Add RNG")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: root.addXml("<rng model='virtio'><backend model='random'>/dev/urandom</backend></rng>")
                                }
                            }

                            // TPM
                            ColumnLayout {
                                visible: root.categoryIndex === 12
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label {
                                    text: i18n("Add a TPM 2.0 device")
                                    Layout.fillWidth: true
                                }
                                Controls.Button {
                                    text: i18n("Add TPM")
                                    icon.name: "security-high-symbolic"
                                    Layout.alignment: Qt.AlignRight
                                    onClicked: {
                                        const hardware = Karton.domainHardwareInfo(root.domain);
                                        hardware.tpmEnabled = true;
                                        hardware.tpmModel = "tpm-crb";
                                        hardware.tpmVersion = "2.0";
                                        if (Karton.editDomainHardware(root.domain, hardware)) {
                                            root.added();
                                        }
                                    }
                                }
                            }

                            Controls.Label {
                                visible: root.message.length > 0
                                text: root.message
                                color: Kirigami.Theme.positiveTextColor
                                Layout.fillWidth: true
                            }
                        }
                    }
                }
            }
        }
    }

    FileDialog {
        id: diskFileDialog
        title: i18n("Choose a disk image")
        nameFilters: [i18n("Disk images (*.qcow2 *.img *.raw *.vmdk)"), i18n("All files (*)")]
        onAccepted: {
            if (Karton.attachDiskImage(root.domain, root.localPath(selectedFile), "qcow2")) {
                root.added();
            }
        }
    }

    FileDialog {
        id: isoFileDialog
        title: i18n("Choose an ISO image")
        nameFilters: [i18n("ISO images (*.iso)"), i18n("All files (*)")]
        onAccepted: {
            if (Karton.attachIso(root.domain, root.localPath(selectedFile))) {
                root.added();
            }
        }
    }
}
