// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

// DHCP leases and static reservations for one libvirt network. Leases are
// polled while the dialog is open; reservations are written to the inactive
// and live network configuration through virNetworkUpdate.
//
// A reservation always belongs to a VM interface attached to this network.
// The VM picker hides VMs whose interfaces in this network are all reserved,
// so a VM with two interfaces stays listed until both are bound.
KartonDialog {
    id: root

    property string networkName: ""
    // Network names repeat across libvirt connections, so every request is
    // scoped to the connection whose Libvirt Networks page opened this dialog.
    property string connectionUri: ""

    // IPv4 configuration of the network, used to constrain the host part.
    property string networkCidr: ""
    property string gateway: ""
    property int prefixLength: 0
    property int prefixOctetCount: 0
    property int hostOctetCount: 0
    property bool maskedInput: false
    property string ipPrefixText: ""
    property double networkAddressInt: 0

    property var leases: []
    property var hosts: []
    property var candidates: []
    property var vmOptions: []
    property var interfaceOptions: []
    property string errorText: ""
    property var selectedMacs: []
    readonly property int tableMacWidth: 140
    readonly property int tableIpWidth: 110
    readonly property int tableTimeWidth: 130
    readonly property int tableRowHeight: Kirigami.Units.gridUnit * 2

    title: networkName.length > 0 ? qsTr("dhcp_1").arg(networkName) : qsTr("dhcp")
    preferredDialogWidth: Kirigami.Units.gridUnit * 46
    height: applicationWindow() ? Math.max(420, Math.min(660, applicationWindow().height - 48)) : 660

    function parseIpv4(text) {
        const parts = (text || "").trim().split(".");
        if (parts.length !== 4) {
            return null;
        }
        const bytes = [];
        for (let i = 0; i < 4; ++i) {
            if (!/^\d{1,3}$/.test(parts[i])) {
                return null;
            }
            const value = Number(parts[i]);
            if (value > 255) {
                return null;
            }
            bytes.push(value);
        }
        return bytes;
    }

    function ipToInt(bytes) {
        return ((bytes[0] * 16777216) + (bytes[1] * 65536) + (bytes[2] * 256) + bytes[3]) >>> 0;
    }

    function applyNetworkInfo(cidr, gatewayAddress) {
        networkCidr = cidr || "";
        gateway = gatewayAddress || "";
        prefixLength = 0;
        prefixOctetCount = 0;
        hostOctetCount = 0;
        maskedInput = false;
        ipPrefixText = "";
        networkAddressInt = 0;

        const slash = networkCidr.lastIndexOf("/");
        if (slash <= 0) {
            return;
        }
        const address = parseIpv4(networkCidr.substring(0, slash));
        const prefix = Number(networkCidr.substring(slash + 1));
        if (!address || !(prefix > 0 && prefix <= 32)) {
            return;
        }
        prefixLength = prefix;
        const mask = prefix >= 32 ? 0xFFFFFFFF : ((0xFFFFFFFF << (32 - prefix)) >>> 0);
        networkAddressInt = (ipToInt(address) & mask) >>> 0;

        // Octet-aligned networks (/8, /16, /24) get a fixed prefix label and
        // only the host octets are editable. Other prefixes fall back to a
        // full address field that is still validated against the subnet.
        if (prefix % 8 === 0 && prefix <= 24) {
            maskedInput = true;
            prefixOctetCount = prefix / 8;
            hostOctetCount = 4 - prefixOctetCount;
            const networkBytes = [
                (networkAddressInt >>> 24) & 255,
                (networkAddressInt >>> 16) & 255,
                (networkAddressInt >>> 8) & 255,
                networkAddressInt & 255
            ];
            ipPrefixText = networkBytes.slice(0, prefixOctetCount).join(".") + ".";
        }
    }

    function composedIp() {
        const host = ipField.text.trim();
        if (!maskedInput) {
            return host;
        }
        if (host.length === 0 || host.split(".").length !== hostOctetCount) {
            return "";
        }
        return ipPrefixText + host;
    }

    function isIpUsable() {
        const value = composedIp();
        const bytes = parseIpv4(value);
        if (!bytes || prefixLength <= 0) {
            return false;
        }
        const mask = prefixLength >= 32 ? 0xFFFFFFFF : ((0xFFFFFFFF << (32 - prefixLength)) >>> 0);
        const address = ipToInt(bytes);
        if (((address & mask) >>> 0) !== networkAddressInt) {
            return false;
        }
        if (value === gateway) {
            return false;
        }
        const broadcast = (networkAddressInt | (~mask >>> 0)) >>> 0;
        if (address === networkAddressInt || address === broadcast) {
            return false;
        }
        return true;
    }

    function hostPlaceholder() {
        if (!maskedInput) {
            return qsTr("ip_address");
        }
        if (hostOctetCount === 1) {
            return "0";
        }
        if (hostOctetCount === 2) {
            return "0.0";
        }
        return "0.0.0";
    }

    function vmNameForMac(mac) {
        const key = (mac || "").toLowerCase();
        for (const candidate of root.candidates) {
            if ((candidate.mac || "").toLowerCase() === key && candidate.vmName.length > 0) {
                return candidate.vmName;
            }
        }
        return "";
    }

    function reservationName(host) {
        const vmName = vmNameForMac(host.mac);
        if (vmName.length > 0) {
            return vmName;
        }
        return host.hostname.length > 0 ? host.hostname : "—";
    }

    function leaseVmName(lease) {
        const vmName = vmNameForMac(lease.mac);
        if (vmName.length > 0) {
            return vmName;
        }
        return lease.hostname && lease.hostname.length > 0 ? lease.hostname : "—";
    }

    function formatAdded(iso) {
        if (!iso || iso.length === 0) {
            return "";
        }
        const date = new Date(iso);
        return isNaN(date.getTime()) ? "" : Qt.formatDateTime(date, "yyyy-MM-dd HH:mm");
    }

    function selectedInterface() {
        const index = interfaceCombo.currentIndex;
        return index >= 0 && index < interfaceOptions.length ? interfaceOptions[index] : null;
    }

    function rebuildInterfaceOptions() {
        // currentText lags behind currentIndexChanged with a plain JS-array
        // model, so resolve the selected VM from the index instead.
        const index = vmCombo.currentIndex;
        const vm = index >= 0 && index < root.vmOptions.length ? root.vmOptions[index] : "";
        const options = [];
        if (vm.length > 0) {
            for (const candidate of root.candidates) {
                if (!candidate.reserved && candidate.vmName === vm) {
                    options.push(candidate);
                }
            }
        }
        interfaceOptions = options;
        interfaceCombo.currentIndex = options.length > 0 ? 0 : -1;
    }

    function rebuildVmOptions() {
        const names = [];
        for (const candidate of root.candidates) {
            if (!candidate.reserved && names.indexOf(candidate.vmName) < 0) {
                names.push(candidate.vmName);
            }
        }
        vmOptions = names;
        vmCombo.currentIndex = names.length > 0 ? 0 : -1;
        rebuildInterfaceOptions();
    }

    function refreshLeases() {
        leases = Karton.networkLeases(networkName, root.connectionUri);
    }

    function refreshReservations() {
        hosts = Karton.networkDhcpHosts(networkName, root.connectionUri);
        candidates = Karton.networkDhcpCandidates(networkName, root.connectionUri);
        rebuildVmOptions();
        selectedMacs = [];
    }

    function isReservationSelected(mac) {
        return root.selectedMacs.indexOf(mac) >= 0;
    }

    function toggleReservationSelection(mac, additive) {
        const selected = root.selectedMacs.slice();
        const index = selected.indexOf(mac);
        if (additive) {
            if (index >= 0) {
                selected.splice(index, 1);
            } else {
                selected.push(mac);
            }
        } else {
            root.selectedMacs = index >= 0 ? [] : [mac];
            return;
        }
        root.selectedMacs = selected;
    }

    function selectedSummary() {
        const lines = [];
        for (const host of root.hosts) {
            if (root.selectedMacs.indexOf(host.mac) >= 0) {
                lines.push(root.reservationName(host) + " — " + host.ip);
            }
        }
        return lines.join("\n");
    }

    function removeSelectedReservations() {
        const macs = root.selectedMacs.slice();
        root.selectedMacs = [];
        root.errorText = "";
        for (const mac of macs) {
            Karton.removeNetworkDhcpHost(root.networkName, mac, root.connectionUri);
        }
        root.refreshReservations();
        removeReservationDialog.close();
    }

    function addReservation() {
        const target = selectedInterface();
        if (!target || !isIpUsable()) {
            return;
        }
        errorText = "";
        if (!Karton.addNetworkDhcpHost(networkName, target.mac, composedIp(), root.connectionUri)) {
            return;
        }
        ipField.text = "";
    }

    function openFor(name) {
        networkName = name;
        errorText = "";
        ipField.text = "";
        const info = Karton.networks(root.connectionUri).find(network => network.name === name);
        applyNetworkInfo(info ? info.ipv4 : "", info ? info.ipv4Gateway : "");
        refreshLeases();
        refreshReservations();
        open();
    }

    onOpened: leaseTimer.restart()
    onClosed: leaseTimer.stop()

    Timer {
        id: leaseTimer
        interval: 3000
        repeat: true
        onTriggered: root.refreshLeases()
    }

    Connections {
        target: Karton
        function onErrorOccurred(message) {
            root.errorText = message;
        }
        function onNetworksChanged() {
            if (root.opened) {
                root.refreshReservations();
            }
        }
    }

    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: root.height

        ScrollableSettingsView {
            anchors.fill: parent
            contentMargins: Kirigami.Units.largeSpacing

            ColumnLayout {
                width: parent.width
                spacing: Kirigami.Units.smallSpacing

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        Controls.Label {
            text: qsTr("active_leases")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: root.leases.length === 0
            text: qsTr("no_active_leases")
            opacity: 0.7
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            visible: root.leases.length > 0

            Controls.Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("vm")
                opacity: 0.55
                font.pixelSize: 11
            }
            Controls.Label {
                Layout.preferredWidth: root.tableMacWidth
                text: qsTr("mac_address")
                opacity: 0.55
                font.pixelSize: 11
            }
            Controls.Label {
                Layout.preferredWidth: root.tableIpWidth
                text: qsTr("ip_address")
                opacity: 0.55
                font.pixelSize: 11
            }
            Controls.Label {
                Layout.preferredWidth: root.tableTimeWidth
                text: qsTr("expires")
                opacity: 0.55
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
            }
        }
        ListView {
            id: leaseList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 10,
                                             Math.max(Kirigami.Units.gridUnit * 4, leaseList.contentHeight))
            visible: root.leases.length > 0
            clip: true
            model: root.leases
            spacing: 2

            delegate: RowLayout {
                width: ListView.view.width
                height: root.tableRowHeight
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    text: root.leaseVmName(modelData)
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                Controls.Label {
                    Layout.preferredWidth: root.tableMacWidth
                    text: modelData.mac
                    font.family: "monospace"
                    opacity: 0.8
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
                Controls.Label {
                    Layout.preferredWidth: root.tableIpWidth
                    text: modelData.ip
                    font.family: "monospace"
                    verticalAlignment: Text.AlignVCenter
                }
                Controls.Label {
                    Layout.preferredWidth: root.tableTimeWidth
                    text: modelData.expiryTime
                    opacity: 0.6
                    font.pixelSize: 11
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideRight
                    verticalAlignment: Text.AlignVCenter
                }
            }
        }

        Controls.Label {
            Layout.topMargin: Kirigami.Units.largeSpacing
            text: qsTr("reservations")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: root.hosts.length === 0
            text: qsTr("no_reservations")
            opacity: 0.7
        }
        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.largeSpacing
            visible: root.hosts.length > 0

            Controls.Label {
                Layout.fillWidth: true
                Layout.minimumWidth: 0
                text: qsTr("vm")
                opacity: 0.55
                font.pixelSize: 11
            }
            Controls.Label {
                Layout.preferredWidth: root.tableMacWidth
                text: qsTr("mac_address")
                opacity: 0.55
                font.pixelSize: 11
            }
            Controls.Label {
                Layout.preferredWidth: root.tableIpWidth
                text: qsTr("ip_address")
                opacity: 0.55
                font.pixelSize: 11
            }
            Controls.Label {
                Layout.preferredWidth: root.tableTimeWidth
                text: qsTr("added")
                opacity: 0.55
                font.pixelSize: 11
                horizontalAlignment: Text.AlignRight
            }
        }
        ListView {
            id: reservationList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 10,
                                             Math.max(Kirigami.Units.gridUnit * 3, reservationList.contentHeight))
            visible: root.hosts.length > 0
            clip: true
            model: root.hosts
            spacing: 2

            delegate: Item {
                width: ListView.view.width
                height: root.tableRowHeight

                Rectangle {
                    anchors.fill: parent
                    radius: 4
                    color: root.isReservationSelected(modelData.mac)
                        ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                  Kirigami.Theme.highlightColor.g,
                                  Kirigami.Theme.highlightColor.b, 0.35)
                        : (rowMouse.hovered
                           ? Qt.rgba(Kirigami.Theme.textColor.r,
                                     Kirigami.Theme.textColor.g,
                                     Kirigami.Theme.textColor.b, 0.06)
                           : "transparent")
                }

                RowLayout {
                    anchors.fill: parent
                    spacing: Kirigami.Units.largeSpacing

                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.minimumWidth: 0
                        text: root.reservationName(modelData)
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    Controls.Label {
                        Layout.preferredWidth: root.tableMacWidth
                        text: modelData.mac
                        font.family: "monospace"
                        opacity: 0.8
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                    Controls.Label {
                        Layout.preferredWidth: root.tableIpWidth
                        text: modelData.ip
                        font.family: "monospace"
                        verticalAlignment: Text.AlignVCenter
                    }
                    Controls.Label {
                        Layout.preferredWidth: root.tableTimeWidth
                        text: root.formatAdded(modelData.addedAt) || "—"
                        opacity: 0.6
                        font.pixelSize: 11
                        horizontalAlignment: Text.AlignRight
                        elide: Text.ElideRight
                        verticalAlignment: Text.AlignVCenter
                    }
                }

                MouseArea {
                    id: rowMouse
                    anchors.fill: parent
                    hoverEnabled: true
                    cursorShape: Qt.PointingHandCursor
                    onClicked: (mouse) => root.toggleReservationSelection(
                        modelData.mac,
                        (mouse.modifiers & Qt.ControlModifier) !== 0)
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            visible: root.hosts.length > 0
            spacing: Kirigami.Units.largeSpacing

            Controls.Label {
                Layout.fillWidth: true
                text: qsTr("select_a_reservation_to_remove_it")
                opacity: 0.65
                font.pixelSize: 12
                wrapMode: Text.WordWrap
            }
            Controls.Button {
                text: root.selectedMacs.length > 1
                    ? qsTr("remove_selected_1").arg(root.selectedMacs.length)
                    : qsTr("remove")
                icon.name: "list-remove-symbolic"
                enabled: root.selectedMacs.length > 0
                onClicked: removeReservationDialog.open()
            }
        }

        Controls.Label {
            Layout.topMargin: Kirigami.Units.largeSpacing
            text: qsTr("add_reservation")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }
        Kirigami.Separator {
            Layout.fillWidth: true
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: root.vmOptions.length === 0
            text: root.candidates.length === 0
                ? qsTr("no_vm_interfaces_are_connected_to_network_1").arg(root.networkName)
                : qsTr("all_connected_interfaces_already_have_a_reservation")
            opacity: 0.7
            wrapMode: Text.WordWrap
        }
        Controls.Label {
            Layout.fillWidth: true
            visible: root.vmOptions.length > 0 && root.prefixLength === 0
            text: qsTr("no_ipv4_configuration_for_this_network")
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        GridLayout {
            Layout.fillWidth: true
            visible: root.vmOptions.length > 0 && root.prefixLength > 0
            columns: 2
            columnSpacing: Kirigami.Units.largeSpacing
            rowSpacing: 12

            Controls.Label {
                text: qsTr("vm")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: vmCombo
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                model: root.vmOptions
                onCurrentIndexChanged: root.rebuildInterfaceOptions()
            }

            Controls.Label {
                text: qsTr("interface")
                Layout.fillWidth: true
            }
            Controls.ComboBox {
                id: interfaceCombo
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                model: root.interfaceOptions
                textRole: "label"
            }

            Controls.Label {
                text: qsTr("ip_address")
                Layout.fillWidth: true
            }
            RowLayout {
                Layout.preferredWidth: 260
                Layout.maximumWidth: 260
                Layout.minimumWidth: 0
                spacing: 0

                Controls.Label {
                    visible: root.maskedInput
                    text: root.ipPrefixText
                    font.family: "monospace"
                    opacity: 0.8
                }
                Controls.TextField {
                    id: ipField
                    Layout.fillWidth: true
                    Layout.minimumWidth: 0
                    inputMethodHints: Qt.ImhFormattedNumbersOnly
                    placeholderText: root.hostPlaceholder()
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.vmOptions.length > 0 && root.prefixLength > 0
            text: root.maskedInput
                ? qsTr("enter_only_the_host_part_inside_the_network_1").arg(root.networkCidr)
                : qsTr("enter_an_address_inside_the_network_1").arg(root.networkCidr)
            opacity: 0.65
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }

                Controls.Label {
                    Layout.fillWidth: true
                    text: qsTr("reservations_apply_to_the_network_configuration")
                    opacity: 0.65
                    font.pixelSize: 12
                    wrapMode: Text.WordWrap
                }
            }
        }
    }

    KartonDialog {
        id: removeReservationDialog
        title: root.selectedMacs.length === 1 ? qsTr("remove_reservation") : qsTr("remove_reservations")
        preferredDialogWidth: Kirigami.Units.gridUnit * 36

        ColumnLayout {
            width: removeReservationDialog.availableWidth
            spacing: 12

            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: root.selectedMacs.length === 1
                    ? qsTr("remove_the_selected_reservation")
                    : qsTr("remove_the_selected_reservations_1").arg(root.selectedMacs.length)
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WrapAnywhere
                font.family: "monospace"
                font.pixelSize: 12
                opacity: 0.8
                text: root.selectedSummary()
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: removeReservationDialog.close()
            },
            Kirigami.Action {
                text: qsTr("remove")
                icon.name: "list-remove-symbolic"
                enabled: root.selectedMacs.length > 0
                onTriggered: root.removeSelectedReservations()
            }
        ]
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("add_reservation")
            icon.name: "list-add-symbolic"
            enabled: root.selectedInterface() !== null && root.isIpUsable()
            onTriggered: root.addReservation()
        }
    ]
}
