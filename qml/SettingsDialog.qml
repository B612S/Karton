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

    modal: true
    padding: 0
    readonly property real cardWidth: applicationWindow() ? Math.min(1000, applicationWindow().width - 48) : 1000
    readonly property real cardHeight: applicationWindow() ? Math.min(680, applicationWindow().height - 48) : 680
    width: cardWidth
    height: cardHeight
    header: Item {}
    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.98)
        radius: 8
        border.color: "transparent"
    }

    property Domain domain: null
    property var parentWindow: null
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
    property bool xmlDirty: false
    property bool xmlLoading: false
    property bool optionalUpdating: false
    readonly property var categoryPages: [0, 1, 2, 3, 4, 5, 6, 10, 7, 8, 9, 15, 11, 12, 13, 16, 14]
    readonly property var categoryModel: [
        { label: i18n("Overview"), visible: true },
        { label: i18n("CPUs & Memory"), visible: true },
        { label: i18n("Boot Options"), visible: true },
        { label: i18n("Disks"), visible: true },
        { label: i18n("Network"), visible: true },
        { label: i18n("Display"), visible: true },
        { label: i18n("Sound"), visible: true },
        { label: i18n("Input"), visible: root.inputDevices.length > 0 },
        { label: i18n("USB"), visible: root.usbDevices.length > 0 },
        { label: i18n("PCI"), visible: root.pciDevices.length > 0 },
        { label: i18n("TPM"), visible: true },
        { label: i18n("Filesystem"), visible: root.xmlDevices.some(function(device) { return device.tag === "filesystem"; }) },
        { label: i18n("Controllers"), visible: root.xmlDevices.some(function(device) { return device.tag === "controller"; }) },
        { label: i18n("Serial & Console"), visible: root.xmlDevices.some(function(device) {
            return device.tag === "serial" || device.tag === "channel" || device.tag === "console";
        }) },
        { label: i18n("Other devices"), visible: true },
        { label: i18n("Clipboard"), visible: true },
        { label: i18n("XML"), visible: true }
    ]
    property string xmlContent: ""
    property string xmlLastValid: ""
    property var xmlDevices: []
    property var inputDevices: []

    readonly property var bootDevices: ["hd", "cdrom", "network"]
    readonly property var bootLabels: [i18n("Hard Disk"), i18n("CD-ROM"), i18n("Network")]
    readonly property var videoModels: ["qxl", "virtio", "vga"]
    readonly property var videoLabels: ["QXL", "VirtIO", "VGA"]
    readonly property var soundModels: ["ich9", "ac97", "none"]
    readonly property var soundLabels: ["ich9", "AC97", i18n("None")]
    readonly property var displayResolutions: [
        { label: i18n("Auto (fit window)"), width: 0, height: 0 },
        { label: "1280 × 720", width: 1280, height: 720 },
        { label: "1600 × 900", width: 1600, height: 900 },
        { label: "1920 × 1080", width: 1920, height: 1080 },
        { label: "2560 × 1440", width: 2560, height: 1440 },
        { label: "3840 × 2160", width: 3840, height: 2160 },
        { label: i18n("Custom…"), width: -1, height: -1 }
    ]

    title: domain ? i18n("Settings — %1", domain.config.name) : i18n("Settings")

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
        xmlDevices = Karton.domainXmlDevices(domain);
        inputDevices = Karton.domainInputDevices(domain);

        firmwareCombo.currentIndex = hardware.firmware === "uefi" ? 1 : 0;
        secureBootCheck.checked = hardware.secureBoot === true;
        secureBootCheck.visible = firmwareCombo.currentIndex === 1;
        bootCombo.currentIndex = Math.max(0, bootDevices.indexOf(hardware.bootDev));
        videoCombo.currentIndex = Math.max(0, videoModels.indexOf(hardware.videoModel));
        videoHeadsSpin.value = Math.max(1, hardware.videoHeads || 1);
        accel3dCheck.checked = hardware.accel3d === true;
        soundCombo.currentIndex = Math.max(0, soundModels.indexOf(hardware.soundModel));
        soundOutputCheck.checked = Karton.audioPlaybackEnabled(domain);
        microphoneCheck.checked = Karton.microphoneEnabled(domain);
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
        if (!root.xmlDirty) {
            loadXml(Karton.domainXml(domain));
        }
        root.optionalUpdating = true;
        const watchdog = root.deviceByTag("watchdog");
        watchdogCheck.checked = watchdog !== null;
        watchdogModelCombo.currentIndex = Math.max(0, ["i6300esb", "ib700"].indexOf(watchdog ? watchdog.model : ""));
        watchdogActionCombo.currentIndex = Math.max(0, ["reset", "poweroff", "shutdown", "pause", "none"].indexOf(watchdog ? watchdog.action : ""));
        const rng = root.deviceByTag("rng");
        rngCheck.checked = rng !== null;
        rngBackendCombo.currentIndex = Math.max(0, ["random", "egd"].indexOf(rng ? rng.backend : ""));
        panicCheck.checked = root.hasXmlDevice("panic");
        panicModelCombo.currentIndex = Math.max(0, ["isa", "pvpanic"].indexOf(panicCheck.checked ? root.deviceByTag("panic").model : ""));
        vsockCheck.checked = root.hasXmlDevice("vsock");
        const shmem = root.deviceByTag("shmem");
        shmemCheck.checked = shmem !== null;
        shmemNameField.text = shmem ? shmem.name : "karton";
        shmemSizeSpin.value = shmem ? Math.max(1, Math.round(Number(shmem.size) / (1024 * 1024))) : 64;
        shmemModelCombo.currentIndex = Math.max(0, ["ivshmem-plain", "ivshmem-doorbell"].indexOf(shmem ? shmem.model : ""));
        root.optionalUpdating = false;
        Qt.callLater(root.ensureVisibleCategory);

        const resolution = Karton.domainDisplayResolution(domain);
        let resolutionIndex = 0;
        for (let i = 0; i < displayResolutions.length - 1; ++i) {
            if (displayResolutions[i].width === (resolution.width || 0)
                && displayResolutions[i].height === (resolution.height || 0)) {
                resolutionIndex = i;
                break;
            }
        }
        if (resolution.width > 0 && resolution.height > 0 && resolutionIndex === 0) {
            resolutionIndex = displayResolutions.length - 1;
            displayCustomWidth.value = resolution.width;
            displayCustomHeight.value = resolution.height;
        }
        displayResolutionCombo.currentIndex = resolutionIndex;
    }

    function loadXml(xml) {
        root.xmlLoading = true;
        root.xmlContent = xml;
        root.xmlLastValid = xml;
        xmlEditor.text = xml;
        root.xmlLoading = false;
        root.xmlDirty = false;
    }

    function usbDeviceLabel(device) {
        for (let i = 0; i < usbHostDevices.length; ++i) {
            if (usbHostDevices[i].vendor === device.vendor
                && usbHostDevices[i].product === device.product
                && usbHostDevices[i].bus === device.bus
                && usbHostDevices[i].device === device.device) {
                return usbHostDevices[i].label;
            }
        }
        return device.vendor + ":" + device.product;
    }

    function ensureVisibleCategory() {
        if (categoryList.currentIndex < 0 || categoryList.currentIndex >= root.categoryModel.length) {
            categoryList.currentIndex = 0;
            return;
        }
        if (!root.categoryModel[categoryList.currentIndex].visible) {
            categoryList.currentIndex = 0;
        }
    }

    function deviceByTag(tag) {
        for (let i = 0; i < xmlDevices.length; ++i) {
            if (xmlDevices[i].tag === tag) {
                return xmlDevices[i];
            }
        }
        return null;
    }

    function hasXmlDevice(tag) {
        return root.deviceByTag(tag) !== null;
    }

    function updateOptionalDevice(tag, enabled, xml) {
        if (root.optionalUpdating) {
            return;
        }
        root.optionalUpdating = true;
        if (enabled) {
            Karton.setXmlDevice(root.domain, tag, xml);
        } else {
            const device = root.deviceByTag(tag);
            if (device) {
                Karton.detachXmlDevice(root.domain, device.index);
            }
        }
        root.optionalUpdating = false;
        root.refresh();
    }

    function applyOptionalSettings(tag, xml) {
        if (!root.optionalUpdating && root.hasXmlDevice(tag)) {
            Karton.setXmlDevice(root.domain, tag, xml);
            root.refresh();
        }
    }

    function watchdogXml() {
        return "<watchdog model='" + watchdogModelCombo.currentText + "' action='" + watchdogActionCombo.currentText + "'/>";
    }

    function rngXml() {
        return "<rng model='virtio'><backend model='" + rngBackendCombo.currentText + "'>/dev/urandom</backend></rng>";
    }

    function panicXml() {
        return "<panic model='" + panicModelCombo.currentText + "'/>";
    }

    function shmemXml() {
        const name = shmemNameField.text.trim().length > 0 ? shmemNameField.text.trim() : "karton";
        const size = shmemSizeSpin.value * 1024 * 1024;
        return "<shmem name='" + name + "' size='" + size + "'><model type='" + shmemModelCombo.currentText + "'/></shmem>";
    }

    function xmlEscape(value) {
        return value.replace(/&/g, "&amp;").replace(/</g, "&lt;").replace(/>/g, "&gt;").replace(/"/g, "&quot;").replace(/'/g, "&apos;");
    }

    function osIdMarkup(value) {
        if (!value) {
            return "";
        }
        if (value.indexOf("http://") === 0 || value.indexOf("https://") === 0) {
            const escaped = root.xmlEscape(value);
            return "<a href=\"" + escaped + "\">" + escaped + "</a>";
        }
        return root.xmlEscape(value);
    }

    function openFilesystemEditor(device) {
        filesystemEditDialog.deviceIndex = device.index;
        filesystemEditDialog.shareSource = device.source;
        filesystemEditDialog.shareTarget = device.target;
        filesystemEditDialog.shareDriver = device.driver;
        filesystemEditDialog.shareAccess = device.accessmode;
        filesystemEditDialog.shareReadonly = device.readonly;
        filesystemEditDialog.open();
    }

    function pciDeviceLabel(address) {
        for (let i = 0; i < pciHostDevices.length; ++i) {
            if (pciHostDevices[i].address === address) {
                return pciHostDevices[i].label;
            }
        }
        return address;
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

    onOpened: {
        if (!domain) {
            return;
        }
        categoryList.currentIndex = 0;
        categoryList.positionViewAtBeginning();
        saving = false;
        pendingName = "";
        nameField.text = domain.config.name;
        autostartCheck.checked = domain.autostart;
        memorySpinBox.value = domain.config.maxRam;
        cpuSpinBox.value = domain.config.cpus;
        xmlEditSwitch.checked = false;
        refresh();
    }

    onClosed: {
        categoryList.currentIndex = 0;
        categoryList.positionViewAtBeginning();
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

        let resolutionWidth = 0;
        let resolutionHeight = 0;
        if (displayResolutionCombo.currentIndex === displayResolutions.length - 1) {
            resolutionWidth = displayCustomWidth.value;
            resolutionHeight = displayCustomHeight.value;
        } else {
            const resolution = displayResolutions[displayResolutionCombo.currentIndex];
            resolutionWidth = resolution.width;
            resolutionHeight = resolution.height;
        }
        Karton.saveDomainDisplayResolution(domain, resolutionWidth, resolutionHeight);

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

    customFooterActions: [
        Kirigami.Action {
            text: i18n("Add…")
            icon.name: "list-add-symbolic"
            enabled: root.domain !== null
            onTriggered: addHardwareDialog.open()
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
            text: i18n("Settings")
            font.pixelSize: 16
            font.weight: Font.DemiBold
            Layout.alignment: Qt.AlignVCenter
        }
        Item { Layout.fillWidth: true }
        Controls.ToolButton {
            icon.name: "dialog-close"
            display: Controls.AbstractButton.IconOnly
            Layout.alignment: Qt.AlignVCenter
            onClicked: root.close()
        }
    }

    RowLayout {
        Layout.fillWidth: true
        Layout.fillHeight: true
        clip: true
        spacing: 0

        Rectangle {
            Layout.preferredWidth: root.width < 700 ? 160 : 200
            Layout.fillHeight: true
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.5)
            radius: 8

            ListView {
                id: categoryList
                anchors.fill: parent
                anchors.margins: 12
                spacing: 0
                currentIndex: 0

                model: root.categoryModel
                delegate: Rectangle {
                    id: categoryButton
                    width: ListView.view.width
                    height: modelData.visible ? 36 : 0
                    visible: modelData.visible
                    radius: 4
                    activeFocusOnTab: true
                    border.width: activeFocus ? 1 : 0
                    border.color: Kirigami.Theme.highlightColor
                    color: ListView.isCurrentItem
                        ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                  Kirigami.Theme.highlightColor.g,
                                  Kirigami.Theme.highlightColor.b, 0.35)
                        : (categoryMouse.containsMouse
                            ? Qt.rgba(Kirigami.Theme.textColor.r,
                                      Kirigami.Theme.textColor.g,
                                      Kirigami.Theme.textColor.b, 0.08)
                            : "transparent")

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

                    Text {
                        text: modelData.label
                        color: Kirigami.Theme.textColor
                        font.pixelSize: 14
                        font.weight: ListView.isCurrentItem ? Font.DemiBold : Font.Normal
                        anchors.verticalCenter: parent.verticalCenter
                        anchors.left: parent.left
                        anchors.leftMargin: 12
                    }

                    Keys.onSpacePressed: categoryList.currentIndex = index
                    Keys.onReturnPressed: categoryList.currentIndex = index
                    Keys.onEnterPressed: categoryList.currentIndex = index

                    MouseArea {
                        id: categoryMouse
                        anchors.fill: parent
                        hoverEnabled: true
                        onClicked: {
                            categoryButton.forceActiveFocus();
                            categoryList.currentIndex = index;
                        }
                    }
                }
            }
        }

        Kirigami.Separator {
            Layout.fillHeight: true
        }

        ColumnLayout {
            Layout.fillWidth: true
            Layout.fillHeight: true
            spacing: 0

            Controls.Label {
                Layout.fillWidth: true
                Layout.margins: 24
                text: categoryList.currentIndex >= 0 && categoryList.currentIndex < categoryList.model.length
                    ? categoryList.model[categoryList.currentIndex].label
                    : ""
                font.pixelSize: 20
                font.weight: Font.Bold
            }
            Kirigami.Separator {
                Layout.fillWidth: true
                opacity: 0.5
            }

            StackLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.margins: 24
                clip: true
                currentIndex: root.categoryPages[categoryList.currentIndex]

            // Overview
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: i18n("General")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Name")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: nameField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            validator: RegularExpressionValidator {
                                regularExpression: /^[A-Za-z0-9._-]+$/
                            }
                        }
                        Controls.Label {
                            text: i18n("Start on boot")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: autostartCheck
                            Layout.alignment: Qt.AlignRight
                            onToggled: if (root.domain && checked !== root.domain.autostart) Karton.setAutostart(root.domain, checked)
                        }
                        Controls.Label {
                            text: i18n("Description")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: descriptionField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }

                    Controls.Label {
                        text: i18n("Details")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("UUID")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.domain ? root.domain.config.uuid : ""
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            elide: Text.ElideRight
                        }
                        Controls.Label {
                            text: i18n("State")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.domain ? root.domain.config.state : ""
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }

                    Controls.Label {
                        text: i18n("Operating System")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("OS ID")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.osIdMarkup(root.domain ? root.domain.config.osId : "")
                            textFormat: Text.RichText
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            elide: Text.ElideRight
                            onLinkActivated: Qt.openUrlExternally(link)
                        }
                        Controls.Label {
                            text: i18n("Short OS ID")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.domain ? root.domain.config.shortOsId : ""
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            elide: Text.ElideRight
                        }
                    }

                    Controls.Label {
                        text: i18n("Performance")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("CPU Usage")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.performance.cpuPercent !== undefined ? i18n("%1 %", root.performance.cpuPercent.toFixed(1)) : "—"
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Memory Usage")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.performance.memoryUsedKiB !== undefined
                                ? i18n("%1 MiB of %2 MiB used", (root.performance.memoryUsedKiB / 1024).toFixed(0), (root.performance.memoryTotalKiB / 1024).toFixed(0))
                                : (root.performance.memoryKiB !== undefined
                                    ? i18n("%1 MiB allocated", (root.performance.memoryKiB / 1024).toFixed(0))
                                    : "—")
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("vCPUs")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.Label {
                            text: root.performance.vcpus !== undefined ? root.performance.vcpus : ""
                            opacity: 0.7
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // CPUs and memory
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Memory (GB)")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: memorySpinBox
                            from: 1
                            to: Math.max(1024, root.domain ? root.domain.config.maxRam : 64)
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("CPUs")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: cpuSpinBox
                            from: 1
                            to: 256
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: i18n("Changing memory or CPU count applies after the VM is restarted.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Boot options
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: i18n("Boot Options")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Firmware")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: firmwareCombo
                            model: ["BIOS", "UEFI"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            onCurrentIndexChanged: secureBootCheck.visible = currentIndex === 1
                        }
                        Controls.Label {
                            visible: firmwareCombo.currentIndex === 1
                            text: i18n("Secure Boot")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: secureBootCheck
                            visible: firmwareCombo.currentIndex === 1
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Boot device")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: bootCombo
                            model: root.bootLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: i18n("Firmware and boot order are written to the persistent VM configuration.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Disks
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: i18n("Disks and CD-ROMs")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.disks
                        spacing: 4
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
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
                        text: i18n("Advanced disk options")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                        Layout.topMargin: 8
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Disk")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: diskTargetCombo
                            model: root.disks.filter(function(disk) { return disk.device === "disk"; }).map(function(disk) { return disk.target; })
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                            onCurrentTextChanged: root.refreshDiskAdvanced()
                        }
                        Controls.Label {
                            text: i18n("Read-only")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: diskReadonlyCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Shareable")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: diskShareableCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Serial")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.TextField {
                            id: diskSerialField
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Cache mode")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: diskCacheCombo
                            model: [i18n("Hypervisor default"), "none", "writethrough", "writeback", "directsync", "unsafe"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Discard mode")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: diskDiscardCombo
                            model: [i18n("Hypervisor default"), "unmap", "ignore"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Item {
                            Layout.columnSpan: 2
                            Layout.fillWidth: true
                        }
                        Controls.Button {
                            text: i18n("Apply disk options")
                            icon.name: "dialog-ok-symbolic"
                            Layout.columnSpan: 2
                            Layout.alignment: Qt.AlignRight
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
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Network
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: i18n("VM interfaces")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.interfaces
                        spacing: 4
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            activeFocusOnTab: false
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.source.length > 0 ? modelData.source : modelData.type
                                        elide: Text.ElideRight
                                    }
                                    Controls.Label {
                                        Layout.fillWidth: true
                                        text: modelData.mac
                                              + (modelData.model.length > 0 ? " - " + modelData.model : "")
                                              + ((modelData.ip || "").length > 0 ? " - " + modelData.ip : "")
                                        opacity: 0.7
                                        elide: Text.ElideRight
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Edit")
                                    icon.name: "document-edit-symbolic"
                                    enabled: modelData.mac.length > 0
                                    onClicked: {
                                        editInterfaceDialog.originalMac = modelData.mac;
                                        editInterfaceDialog.model = modelData.model || "virtio";
                                        editInterfaceDialog.open();
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
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: i18n("MAC and model changes are applied to the persistent configuration; restart the VM to use them.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Display
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Video model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: videoCombo
                            model: root.videoLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Heads")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: videoHeadsSpin
                            from: 1
                            to: 4
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Resolution")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: displayResolutionCombo
                            model: root.displayResolutions
                            textRole: "label"
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            text: i18n("Width")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: displayCustomWidth
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            from: 320
                            to: 7680
                            stepSize: 2
                            value: 1920
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            text: i18n("Height")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.SpinBox {
                            id: displayCustomHeight
                            visible: displayResolutionCombo.currentIndex === root.displayResolutions.length - 1
                            from: 240
                            to: 4320
                            stepSize: 2
                            value: 1080
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("3D acceleration")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: accel3dCheck
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Sound
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Sound model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: soundCombo
                            model: root.soundLabels
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("Sound output")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: soundOutputCheck
                            Layout.alignment: Qt.AlignRight
                            onToggled: {
                                Karton.saveAudioPlaybackEnabled(root.domain, checked);
                                const viewer = Karton.viewerForDomain(root.domain);
                                if (viewer && viewer.domainViewerItem) {
                                    viewer.domainViewerItem.audioPlaybackEnabled = checked;
                                }
                            }
                        }
                        Controls.Label {
                            text: i18n("Microphone")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: microphoneCheck
                            Layout.alignment: Qt.AlignRight
                            onToggled: {
                                Karton.saveMicrophoneEnabled(root.domain, checked);
                                const viewer = Karton.viewerForDomain(root.domain);
                                if (viewer && viewer.domainViewerItem) {
                                    viewer.domainViewerItem.microphoneEnabled = checked;
                                }
                            }
                        }
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // USB
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
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
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.usbDevices
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: root.usbDeviceLabel(modelData)
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
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
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
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, contentHeight)
                        clip: true
                        model: root.pciDevices
                        spacing: Kirigami.Units.smallSpacing
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: Kirigami.Units.largeSpacing
                                Controls.Label {
                                    Layout.fillWidth: true
                                    text: root.pciDeviceLabel(modelData.address) + " · " + modelData.address
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
                        Layout.fillWidth: true
                        text: i18n("Attaching a PCI device detaches it from the host. Devices without an IOMMU group cannot be attached.")
                        opacity: 0.7
                        wrapMode: Text.WordWrap
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // TPM
            ScrollableSettingsView {
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true

                        Controls.Label {
                            text: i18n("Enable TPM")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.CheckBox {
                            id: tpmEnabledCheck
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("TPM model")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: tpmModelCombo
                            model: ["CRB", "TIS"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                        Controls.Label {
                            text: i18n("TPM version")
                            font.pixelSize: 14
                            Layout.fillWidth: true
                            Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                        }
                        Controls.ComboBox {
                            id: tpmVersionCombo
                            model: ["2.0", "1.2"]
                            Layout.preferredWidth: 260
                            Layout.alignment: Qt.AlignRight
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: i18n("TPM emulation requires swtpm in the host and is applied to the persistent VM configuration.")
                    }
                    Item { Layout.fillHeight: true }
                }
            }

            // Input devices
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 12

                Controls.Label {
                    text: i18n("Input devices")
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    Layout.fillWidth: true
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.inputDevices
                    spacing: 2
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            Controls.Label {
                                text: modelData.type + " - " + modelData.bus
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: i18n("Detach")
                                icon.name: "list-remove-symbolic"
                                onClicked: {
                                    if (Karton.detachInputDevice(root.domain, modelData.type, modelData.bus)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Controllers
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.xmlDevices.filter(function(device) { return device.tag === "controller"; })
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            spacing: 8
                            Kirigami.Icon {
                                source: modelData.icon
                                implicitWidth: 20
                                implicitHeight: 20
                            }
                            Controls.Label {
                                text: modelData.label
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: i18n("Detach")
                                icon.name: "list-remove-symbolic"
                                onClicked: {
                                    if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Serial and console
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.xmlDevices.filter(function(device) {
                        return device.tag === "serial" || device.tag === "channel" || device.tag === "console";
                    })
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            spacing: 8
                            Kirigami.Icon {
                                source: modelData.icon
                                implicitWidth: 20
                                implicitHeight: 20
                            }
                            Controls.Label {
                                text: modelData.label + (modelData.description.length > 0 ? " · " + modelData.description : "")
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: i18n("Detach")
                                icon.name: "list-remove-symbolic"
                                enabled: modelData.tag !== "emulator"
                                onClicked: {
                                    if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // Other devices
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 16

                Controls.Label {
                    text: i18n("System devices")
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    Layout.fillWidth: true
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                }

                Controls.CheckBox {
                    id: watchdogCheck
                    text: i18n("Watchdog")
                    Layout.fillWidth: true
                    onToggled: root.updateOptionalDevice("watchdog", checked, root.watchdogXml())
                }
                GridLayout {
                    visible: watchdogCheck.checked
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Controls.Label { text: i18n("Model"); Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: watchdogModelCombo
                        Layout.preferredWidth: 260
                        model: ["i6300esb", "ib700"]
                        onCurrentIndexChanged: root.applyOptionalSettings("watchdog", root.watchdogXml())
                    }
                    Controls.Label { text: i18n("Action"); Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: watchdogActionCombo
                        Layout.preferredWidth: 260
                        model: ["reset", "poweroff", "shutdown", "pause", "none"]
                        onCurrentIndexChanged: root.applyOptionalSettings("watchdog", root.watchdogXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                Controls.CheckBox {
                    id: rngCheck
                    text: i18n("Random Number Generator")
                    Layout.fillWidth: true
                    onToggled: root.updateOptionalDevice("rng", checked, root.rngXml())
                }
                GridLayout {
                    visible: rngCheck.checked
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Controls.Label { text: i18n("Backend"); Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: rngBackendCombo
                        Layout.preferredWidth: 260
                        model: ["random", "egd"]
                        onCurrentIndexChanged: root.applyOptionalSettings("rng", root.rngXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                Controls.CheckBox {
                    id: panicCheck
                    text: i18n("Panic notifier")
                    Layout.fillWidth: true
                    onToggled: root.updateOptionalDevice("panic", checked, root.panicXml())
                }
                GridLayout {
                    visible: panicCheck.checked
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Controls.Label { text: i18n("Model"); Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: panicModelCombo
                        Layout.preferredWidth: 260
                        model: ["isa", "pvpanic"]
                        onCurrentIndexChanged: root.applyOptionalSettings("panic", root.panicXml())
                    }
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                Controls.CheckBox {
                    id: vsockCheck
                    text: i18n("VirtIO VSOCK")
                    Layout.fillWidth: true
                    onToggled: root.updateOptionalDevice("vsock", checked, "<vsock model='virtio'><cid auto='yes'/></vsock>")
                }
                Kirigami.Separator { Layout.fillWidth: true; opacity: 0.35 }

                Controls.CheckBox {
                    id: shmemCheck
                    text: i18n("Shared memory (IVSHMEM)")
                    Layout.fillWidth: true
                    onToggled: root.updateOptionalDevice("shmem", checked, root.shmemXml())
                }
                GridLayout {
                    visible: shmemCheck.checked
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 16
                    Layout.fillWidth: true
                    Layout.leftMargin: 24
                    Controls.Label { text: i18n("Name"); Layout.fillWidth: true }
                    Controls.TextField {
                        id: shmemNameField
                        Layout.preferredWidth: 260
                        text: "karton"
                        onEditingFinished: root.applyOptionalSettings("shmem", root.shmemXml())
                    }
                    Controls.Label { text: i18n("Size (MiB)"); Layout.fillWidth: true }
                    Controls.SpinBox {
                        id: shmemSizeSpin
                        from: 1
                        to: 4096
                        value: 64
                        Layout.preferredWidth: 260
                        onValueChanged: root.applyOptionalSettings("shmem", root.shmemXml())
                    }
                    Controls.Label { text: i18n("Model"); Layout.fillWidth: true }
                    Controls.ComboBox {
                        id: shmemModelCombo
                        Layout.preferredWidth: 260
                        model: ["ivshmem-plain", "ivshmem-doorbell"]
                        onCurrentIndexChanged: root.applyOptionalSettings("shmem", root.shmemXml())
                    }
                }

                Controls.Label {
                    text: i18n("Additional XML devices")
                    font.pixelSize: 15
                    font.weight: Font.Bold
                    Layout.fillWidth: true
                    Layout.topMargin: 8
                }
                Rectangle {
                    Layout.fillWidth: true
                    height: 1
                    color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                }
                Controls.Label {
                    Layout.fillWidth: true
                    wrapMode: Text.WordWrap
                    opacity: 0.65
                    font.pixelSize: 12
                    text: i18n("Other devices declared in the domain XML, such as shared memory, IOMMU, filesystem and USB redirection devices.")
                }
                ListView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    clip: true
                    model: root.xmlDevices.filter(function(device) {
                        return ["disk", "interface", "input", "video", "sound", "hostdev", "tpm", "controller", "serial", "channel", "console", "emulator", "graphics", "audio", "watchdog", "rng", "panic", "vsock", "shmem", "filesystem"].indexOf(device.tag) === -1;
                    })
                    delegate: Controls.ItemDelegate {
                        width: ListView.view.width
                        focusPolicy: Qt.NoFocus
                        contentItem: RowLayout {
                            spacing: 8
                            Kirigami.Icon {
                                source: modelData.icon
                                implicitWidth: 20
                                implicitHeight: 20
                            }
                            Controls.Label {
                                text: modelData.label + (modelData.description.length > 0 ? " · " + modelData.description : "")
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: i18n("Detach")
                                icon.name: "list-remove-symbolic"
                                enabled: modelData.tag !== "emulator"
                                onClicked: {
                                    if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                        root.refresh();
                                    }
                                }
                            }
                        }
                    }
                }
            }

            // XML
            ColumnLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                Layout.preferredWidth: 0
                spacing: 12

                    RowLayout {
                        Layout.fillWidth: true
                        Controls.Label {
                            text: i18n("Domain XML")
                            font.pixelSize: 15
                            font.weight: Font.Bold
                            Layout.fillWidth: true
                        }
                        Controls.Label {
                            text: i18n("Edit")
                            font.pixelSize: 13
                        }
                        Controls.Switch {
                            id: xmlEditSwitch
                            onCheckedChanged: {
                                const savedX = xmlScroll.contentX;
                                const savedY = xmlScroll.contentY;
                                Qt.callLater(function() {
                                    if (xmlScroll.contentItem) {
                                        xmlScroll.contentItem.contentX = savedX;
                                        xmlScroll.contentItem.contentY = savedY;
                                    }
                                });
                            }
                        }
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                        text: i18n("Read-only by default. Enable editing only when you need to change the domain XML directly.")
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        implicitHeight: Kirigami.Units.gridUnit * 24
                        radius: 6
                        color: Kirigami.Theme.backgroundColor
                        border.width: 1
                        border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)

                        ScrollableSettingsView {
                            id: xmlScroll
                            anchors.fill: parent
                            anchors.margins: 8
                            clip: true

                            Controls.TextArea {
                                id: xmlEditor
                                width: parent.width
                                height: Math.max(xmlScroll.height, implicitHeight)
                                text: root.xmlContent
                                readOnly: !xmlEditSwitch.checked
                                wrapMode: TextEdit.NoWrap
                                textFormat: TextEdit.PlainText
                                selectByMouse: true
                                font.family: "monospace"
                                font.pixelSize: 12
                                color: Kirigami.Theme.textColor
                                background: null
                                onTextChanged: {
                                    // Do not mark the initial empty text or a
                                    // refresh-triggered update as a user edit.
                                    if (!root.xmlLoading && xmlEditSwitch.checked) {
                                        root.xmlDirty = true;
                                    }
                                }

                                XmlHighlighter {
                                    textDocument: xmlEditor.textDocument
                                }
                            }
                        }
                    }
                    Controls.Button {
                        text: i18n("Apply XML")
                        icon.name: "dialog-ok-symbolic"
                        enabled: xmlEditSwitch.checked && root.xmlDirty
                        Layout.alignment: Qt.AlignRight
                        onClicked: {
                            if (Karton.saveDomainXml(root.domain, xmlEditor.text)) {
                                root.xmlDirty = false;
                                root.refresh();
                                root.xmlEditSwitch.checked = true;
                            } else {
                                // Keep the user in edit mode, but restore the
                                // last configuration accepted by libvirt.
                                root.loadXml(root.xmlLastValid);
                                root.xmlEditSwitch.checked = true;
                            }
                        }
                    }
                }
            // Filesystem
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: i18n("Shared folders are added from Add Hardware. Each share is mounted in the guest under its mount tag.")
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                    }
                    Controls.Button {
                        text: i18n("How to mount…")
                        icon.name: "help-about-symbolic"
                        Layout.alignment: Qt.AlignLeft
                        onClicked: filesystemHelpDialog.open()
                    }
                    ListView {
                        Layout.fillWidth: true
                        Layout.fillHeight: true
                        Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 8, contentHeight)
                        clip: true
                        model: root.xmlDevices.filter(function(device) {
                            return device.tag === "filesystem";
                        })
                        delegate: Controls.ItemDelegate {
                            width: ListView.view.width
                            focusPolicy: Qt.NoFocus
                            contentItem: RowLayout {
                                spacing: 8
                                Kirigami.Icon {
                                    source: "folder-symbolic"
                                    implicitWidth: 20
                                    implicitHeight: 20
                                }
                                ColumnLayout {
                                    Layout.fillWidth: true
                                    spacing: 0
                                    Controls.Label {
                                        text: (modelData.target.length > 0 ? modelData.target : i18n("Filesystem")) + " · " + modelData.source
                                        elide: Text.ElideRight
                                        Layout.fillWidth: true
                                    }
                                    Controls.Label {
                                        text: (modelData.driver.length > 0 ? modelData.driver : "9p") + (modelData.readonly ? " · " + i18n("read-only") : "")
                                        opacity: 0.65
                                        font.pixelSize: 11
                                    }
                                }
                                Controls.Button {
                                    text: i18n("Edit")
                                    icon.name: "document-edit-symbolic"
                                    onClicked: root.openFilesystemEditor(modelData)
                                }
                                Controls.Button {
                                    text: i18n("Detach")
                                    icon.name: "list-remove-symbolic"
                                    onClicked: {
                                        if (Karton.detachXmlDevice(root.domain, modelData.index)) {
                                            root.refresh();
                                        }
                                    }
                                }
                            }
                        }
                    }
                    Controls.Label {
                        visible: root.xmlDevices.filter(function(device) { return device.tag === "filesystem"; }).length === 0
                        text: i18n("No filesystem shares configured.")
                        opacity: 0.65
                        Layout.fillWidth: true
                    }
                }
            }

            // Clipboard
            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0
                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: i18n("Clipboard synchronization between this computer and the guest.")
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                    }
                    GridLayout {
                        columns: 2
                        columnSpacing: 16
                        rowSpacing: 16
                        Layout.fillWidth: true
                        Controls.Label { text: i18n("Text"); Layout.fillWidth: true }
                        Controls.ComboBox {
                            id: clipboardTextCombo
                            Layout.preferredWidth: 260
                            model: [
                                i18n("Disabled"),
                                i18n("Host to guest"),
                                i18n("Guest to host"),
                                i18n("Bidirectional")
                            ]
                            currentIndex: Karton.clipboardTextMode()
                            onActivated: Karton.saveClipboardTextMode(index)
                        }
                        Controls.Label { text: i18n("Files"); Layout.fillWidth: true }
                        Controls.ComboBox {
                            id: clipboardFilesCombo
                            Layout.preferredWidth: 260
                            model: [
                                i18n("Disabled"),
                                i18n("Host to guest"),
                                i18n("Guest to host"),
                                i18n("Bidirectional")
                            ]
                            currentIndex: Karton.clipboardFilesMode()
                            onActivated: Karton.saveClipboardFilesMode(index)
                        }
                    }
                    Controls.Label {
                        text: i18n("Text clipboard requires spice-vdagent in the guest. On Wayland the viewer window must have focus. File clipboard additionally requires guest support.")
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.65
                        font.pixelSize: 12
                    }
                }
            }

    }

    }
    }

    }

    }

    Kirigami.Dialog {
        id: filesystemHelpDialog
        title: i18n("Mounting Filesystem Shares")
        modal: true
        padding: Kirigami.Units.largeSpacing
        preferredWidth: Kirigami.Units.gridUnit * 42

        background: Rectangle {
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.95)
            radius: 8
            border.color: "transparent"
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 10

            Controls.Label {
                text: i18n("Use the mount tag from the Filesystem page as the name of the share.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.Label {
                text: i18n("Linux — 9p")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.Label {
                text: "sudo mkdir -p /mnt/share\nsudo mount -t 9p -o trans=virtio,version=9p2000.L share /mnt/share"
                font.family: "monospace"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
            Controls.Label {
                text: i18n("Linux — virtiofs")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.Label {
                text: "sudo mkdir -p /mnt/share\nsudo mount -t virtiofs share /mnt/share"
                font.family: "monospace"
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
            }
            Controls.Label {
                text: i18n("Windows")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Controls.Label {
                text: i18n("9p is not supported natively. For virtiofs, install the VirtIO FS driver from virtio-win and WinFsp, then use the shared tag in the VirtIO FS client. Alternatively use a Windows/SMB share.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
            }
            Controls.Label {
                text: i18n("macOS and BSD variants need a guest driver for the selected filesystem type; 9p support depends on the guest kernel.")
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: i18n("Close")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: filesystemHelpDialog.close()
            }
        ]
    }

    Kirigami.Dialog {
        id: filesystemEditDialog
        title: i18n("Edit Filesystem Share")
        modal: true
        padding: Kirigami.Units.largeSpacing
        preferredWidth: Kirigami.Units.gridUnit * 32
        property int deviceIndex: -1
        property string shareSource: ""
        property string shareTarget: ""
        property string shareDriver: ""
        property string shareAccess: ""
        property bool shareReadonly: false

        background: Rectangle {
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.95)
            radius: 8
            border.color: "transparent"
        }

        onOpened: {
            fsEditSource.text = shareSource;
            fsEditTarget.text = shareTarget;
            fsEditDriver.currentIndex = Math.max(0, ["9p", "virtiofs"].indexOf(shareDriver));
            fsEditAccess.currentIndex = Math.max(0, ["passthrough", "mapped", "squash"].indexOf(shareAccess));
            fsEditReadonly.checked = shareReadonly;
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 12

            GridLayout {
                columns: 2
                columnSpacing: 12
                rowSpacing: 12
                Layout.fillWidth: true
                Controls.Label { text: i18n("Driver"); Layout.fillWidth: true }
                Controls.ComboBox {
                    id: fsEditDriver
                    Layout.preferredWidth: 220
                    model: ["9p", "virtiofs"]
                }
                Controls.Label { text: i18n("Source path"); Layout.fillWidth: true }
                Controls.TextField {
                    id: fsEditSource
                    Layout.preferredWidth: 220
                }
                Controls.Label { text: i18n("Name / mount tag"); Layout.fillWidth: true }
                Controls.TextField {
                    id: fsEditTarget
                    Layout.preferredWidth: 220
                }
                Controls.Label { text: i18n("Access mode"); Layout.fillWidth: true }
                Controls.ComboBox {
                    id: fsEditAccess
                    Layout.preferredWidth: 220
                    model: ["passthrough", "mapped", "squash"]
                }
                Controls.Label { text: i18n("Read-only"); Layout.fillWidth: true }
                Controls.CheckBox {
                    id: fsEditReadonly
                    Layout.alignment: Qt.AlignRight
                }
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: i18n("Cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: filesystemEditDialog.close()
            },
            Kirigami.Action {
                text: i18n("Save")
                icon.name: "dialog-ok-symbolic"
                enabled: fsEditSource.text.trim().length > 0
                onTriggered: {
                    const target = fsEditTarget.text.trim().length > 0 ? fsEditTarget.text.trim() : "share";
                    let xml = "<filesystem type='mount' accessmode='" + fsEditAccess.currentText + "'>";
                    xml += "<source dir='" + root.xmlEscape(fsEditSource.text.trim()) + "'/>";
                    xml += "<target dir='" + root.xmlEscape(target) + "'/>";
                    if (fsEditDriver.currentText === "virtiofs") {
                        xml += "<driver type='virtiofs'/>";
                    }
                    if (fsEditReadonly.checked) {
                        xml += "<readonly/>";
                    }
                    xml += "</filesystem>";
                    if (Karton.replaceXmlDevice(root.domain, filesystemEditDialog.deviceIndex, xml)) {
                        filesystemEditDialog.close();
                        root.refresh();
                    }
                }
            }
        ]
    }

    AddHardwareDialog {
        id: addHardwareDialog
        domain: root.domain
        onDeviceAdded: function(categoryIndex) {
            root.refresh();
            categoryList.currentIndex = categoryIndex;
            if (applicationWindow()) {
                applicationWindow().showPassiveNotification(i18n("Device added"));
            }
        }
    }

    Kirigami.Dialog {
        id: editInterfaceDialog
        title: i18n("Edit Network Interface")
        padding: Kirigami.Units.largeSpacing
        preferredWidth: Kirigami.Units.gridUnit * 28
        property string originalMac: ""
        property string model: "virtio"

        background: Rectangle {
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.95)
            radius: 8
            border.color: "transparent"
        }

        onOpened: {
            macField.text = originalMac;
            modelCombo.currentIndex = Math.max(0, modelCombo.model.indexOf(model));
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 16

            Controls.Label { text: i18n("MAC address"); font.pixelSize: 14 }
            Controls.TextField {
                id: macField
                Layout.fillWidth: true
            }
            Controls.Label { text: i18n("Model"); font.pixelSize: 14; Layout.topMargin: 8 }
            Controls.ComboBox {
                id: modelCombo
                model: ["virtio", "e1000"]
                Layout.fillWidth: true
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: i18n("Cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: editInterfaceDialog.close()
            },
            Kirigami.Action {
                text: i18n("Save")
                icon.name: "dialog-ok-symbolic"
                enabled: macField.text.trim().length > 0
                onTriggered: {
                    if (Karton.editNetworkInterface(root.domain, editInterfaceDialog.originalMac,
                                                    macField.text.trim(), modelCombo.currentText)) {
                        editInterfaceDialog.close();
                        root.refresh();
                    }
                }
            }
        ]
    }

    Connections {
        target: categoryList
        function onCurrentIndexChanged() {
            if (!root.domain || root.xmlDirty) {
                return;
            }
            if (categoryList.currentIndex === categoryList.count - 1) {
                root.loadXml(Karton.domainXml(root.domain));
            }
        }
    }

    Connections {
        target: Karton
        function onClosePopupsRequested() {
            // ComboBox popups are separate Qt Quick popups; moving focus back
            // to the settings card dismisses them when the frame/app is clicked.
            root.contentItem.forceActiveFocus();
        }
    }

}
