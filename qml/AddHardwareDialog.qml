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
    property bool messageIsError: false
    property int initialCategory: 0

    signal deviceAdded(int categoryIndex)

    modal: true
    padding: 0
    readonly property real cardWidth: applicationWindow() ? Math.min(680, applicationWindow().width - 48) : 680
    readonly property real cardHeight: applicationWindow() ? Math.min(560, applicationWindow().height - 48) : 560
    width: cardWidth
    height: cardHeight
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
        { label: i18n("Filesystem") }
    ]

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.98)
        radius: 8
        border.color: "transparent"
    }

    function openAt(category) {
        initialCategory = category;
        open();
    }

    onOpened: {
        refresh();
        categoryList.currentIndex = initialCategory;
        categoryIndex = initialCategory;
        message = "";
        messageIsError = false;
    }
    onClosed: initialCategory = 0

    function refresh() {
        networks = Karton.networks();
        usbHostDevices = Karton.usbDevices();
        pciHostDevices = Karton.pciDevices();
    }

    function settingsCategoryFor(category) {
        switch (category) {
        case 0: return 3;  // Storage -> Disks
        case 1: return 4;  // Network
        case 2: return 7;  // Input
        case 3: return 12; // Controllers
        case 4: return 13; // Serial
        case 5: return 13; // Console
        case 6: return 13; // Channel
        case 7: return 8;  // USB
        case 8: return 9;  // PCI
        case 9: return 14; // Redirection -> Other
        case 10: return 11; // Filesystem
        default: return 0;
        }
    }

    function xmlEscape(value) {
        return value.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&apos;");
    }

    function addFilesystem() {
        const source = fsSourceField.text.trim();
        if (source.length === 0) {
            message = i18n("Choose a source directory first.");
            messageIsError = true;
            return;
        }
        const target = fsTargetField.text.trim().length > 0 ? fsTargetField.text.trim() : "share";
        let xml = "<filesystem type='mount' accessmode='" + fsAccessCombo.currentText + "'>";
        xml += "<source dir='" + xmlEscape(source) + "'/>";
        xml += "<target dir='" + xmlEscape(target) + "'/>";
        if (fsDriverCombo.currentText === "virtiofs") {
            xml += "<driver type='virtiofs'/>";
        }
        if (fsReadonlyCheck.checked) {
            xml += "<readonly/>";
        }
        xml += "</filesystem>";
        root.addXml(xml);
    }

    function added() {
        refresh();
        message = i18n("Device added");
        messageIsError = false;
        // Close the wizard and let Settings show the page where the new
        // device can be seen and edited.
        deviceAdded(settingsCategoryFor(categoryIndex));
        close();
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

    Connections {
        target: Karton
        function onErrorOccurred(errorMessage) {
            root.message = errorMessage;
            root.messageIsError = true;
        }
    }

    contentItem: Item {
        implicitWidth: root.cardWidth
        implicitHeight: root.cardHeight

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
                    Layout.preferredWidth: 180
                    Layout.fillHeight: true
                    radius: 8
                    color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                   Kirigami.Theme.backgroundColor.g,
                                   Kirigami.Theme.backgroundColor.b, 0.5)

                    ListView {
                        id: categoryList
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        model: root.categories
                        currentIndex: 0
                        clip: true
                        delegate: Rectangle {
                            id: categoryButton
                            width: ListView.view.width
                            height: 36
                            radius: 4
                            color: ListView.isCurrentItem
                                ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                          Kirigami.Theme.highlightColor.g,
                                          Kirigami.Theme.highlightColor.b, 0.35)
                                : (categoryMouse.containsMouse
                                    ? Qt.rgba(Kirigami.Theme.textColor.r,
                                              Kirigami.Theme.textColor.g,
                                              Kirigami.Theme.textColor.b, 0.08)
                                    : "transparent")
                            Text {
                                text: modelData.label
                                color: Kirigami.Theme.textColor
                                font.pixelSize: 14
                                font.weight: ListView.isCurrentItem ? Font.DemiBold : Font.Normal
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                                anchors.right: parent.right
                                anchors.rightMargin: 8
                                elide: Text.ElideRight
                            }
                            Rectangle {
                                visible: ListView.isCurrentItem
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.topMargin: 4
                                anchors.bottomMargin: 4
                                width: 3
                                radius: 1.5
                                color: Kirigami.Theme.highlightColor
                            }
                            MouseArea {
                                id: categoryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: {
                                    categoryList.currentIndex = index;
                                    root.categoryIndex = index;
                                }
                            }
                        }
                    }
                }

                ColumnLayout {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 20
                    spacing: 14

                    Controls.Label {
                        text: root.categories[root.categoryIndex].label
                        font.pixelSize: 20
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }

                    ScrollableSettingsView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        clip: true

                        ColumnLayout {
                            width: parent.width
                            spacing: 12

                            // Storage
                            ColumnLayout {
                                visible: root.categoryIndex === 0
                                Layout.fillWidth: true
                                spacing: 12
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: i18n("Disk image"); Layout.fillWidth: true }
                                    Controls.Button {
                                        text: i18n("Choose…")
                                        icon.name: "document-open-symbolic"
                                        Layout.preferredWidth: 220
                                        onClicked: diskFileDialog.open()
                                    }
                                    Controls.Label { text: i18n("ISO image"); Layout.fillWidth: true }
                                    Controls.Button {
                                        text: i18n("Choose…")
                                        icon.name: "media-optical-symbolic"
                                        Layout.preferredWidth: 220
                                        onClicked: isoFileDialog.open()
                                    }
                                    Controls.Label { text: i18n("New disk"); Layout.fillWidth: true }
                                    RowLayout {
                                        Layout.preferredWidth: 220
                                        spacing: 6
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
                                            Layout.preferredWidth: 90
                                        }
                                        Controls.Button {
                                            text: i18n("Create")
                                            icon.name: "list-add-symbolic"
                                            onClicked: {
                                                if (Karton.addDisk(root.domain, addDiskSize.value, addDiskFormat.currentText)) {
                                                    root.added();
                                                }
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
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: i18n("Network"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: networkCombo
                                        Layout.preferredWidth: 220
                                        model: root.networks.map(function(network) { return network.name; })
                                    }
                                    Controls.Label { text: i18n("Model"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: networkModelCombo
                                        Layout.preferredWidth: 220
                                        model: ["virtio", "e1000"]
                                    }
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
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: i18n("Device"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: inputTypeCombo
                                        Layout.preferredWidth: 220
                                        model: ["tablet", "mouse", "keyboard"]
                                    }
                                    Controls.Label { text: i18n("Bus"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: inputBusCombo
                                        Layout.preferredWidth: 220
                                        model: ["usb", "ps2", "virtio"]
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Add input device")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignLeft
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
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: i18n("Type"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: controllerTypeCombo
                                        Layout.preferredWidth: 220
                                        model: ["usb", "pci", "sata", "virtio-serial"]
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Add controller")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignLeft
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
                                    Layout.alignment: Qt.AlignLeft
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
                                    Layout.alignment: Qt.AlignLeft
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
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: i18n("Channel"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: channelCombo
                                        Layout.preferredWidth: 220
                                        model: [
                                            { label: i18n("QEMU Guest Agent"), name: "org.qemu.guest_agent.0" },
                                            { label: i18n("SPICE Agent"), name: "com.redhat.spice.0" },
                                            { label: i18n("SPICE WebDAV"), name: "org.spice-space.webdav.0" }
                                        ]
                                        textRole: "label"
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Add channel")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignLeft
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
                                    Layout.alignment: Qt.AlignLeft
                                    onClicked: root.addXml("<redirdev bus='usb' type='spicevmc'/>")
                                }
                            }

                            // Filesystem
                            ColumnLayout {
                                visible: root.categoryIndex === 10
                                Layout.fillWidth: true
                                spacing: 12
                                Controls.Label {
                                    text: i18n("Share a host directory with the guest.")
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                }
                                GridLayout {
                                    columns: 2
                                    columnSpacing: 16
                                    rowSpacing: 12
                                    Layout.fillWidth: true
                                    Controls.Label { text: i18n("Driver"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: fsDriverCombo
                                        Layout.preferredWidth: 220
                                        model: ["9p", "virtiofs"]
                                    }
                                    Controls.Label { text: i18n("Source path"); Layout.fillWidth: true }
                                    RowLayout {
                                        Layout.preferredWidth: 220
                                        spacing: 6
                                        Controls.TextField {
                                            id: fsSourceField
                                            Layout.fillWidth: true
                                            placeholderText: "/home/user/Shared"
                                        }
                                        Controls.Button {
                                            text: i18n("Browse…")
                                            icon.name: "document-open-symbolic"
                                            onClicked: fsFolderDialog.open()
                                        }
                                    }
                                    Controls.Label { text: i18n("Name / mount tag"); Layout.fillWidth: true }
                                    Controls.TextField {
                                        id: fsTargetField
                                        Layout.preferredWidth: 220
                                        text: "share"
                                    }
                                    Controls.Label { text: i18n("Access mode"); Layout.fillWidth: true }
                                    Controls.ComboBox {
                                        id: fsAccessCombo
                                        Layout.preferredWidth: 220
                                        model: ["passthrough", "mapped", "squash"]
                                    }
                                    Controls.Label { text: i18n("Read-only"); Layout.fillWidth: true }
                                    Controls.CheckBox {
                                        id: fsReadonlyCheck
                                        Layout.alignment: Qt.AlignRight
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Add filesystem")
                                    icon.name: "list-add-symbolic"
                                    Layout.alignment: Qt.AlignLeft
                                    enabled: fsSourceField.text.trim().length > 0
                                    onClicked: root.addFilesystem()
                                }
                            }

                            Controls.Label {
                                visible: root.message.length > 0
                                text: root.message
                                color: root.messageIsError ? Kirigami.Theme.negativeTextColor : Kirigami.Theme.positiveTextColor
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
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
                        text: i18n("Close")
                        icon.name: "dialog-cancel-symbolic"
                        onClicked: root.close()
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

    FolderDialog {
        id: fsFolderDialog
        title: i18n("Choose a shared directory")
        onAccepted: {
            fsSourceField.text = decodeURIComponent(selectedFolder.toString().replace(/^file:\/\//, ""));
        }
    }
}
