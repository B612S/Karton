// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

// Host networks of one libvirt connection. Every connection has its own
// networks, so this page is always scoped to one connection (see
// ConnectionSettingsDialog) and never needs a connection selector.
ColumnLayout {
    id: root

    property string connectionUri: ""
    property var networks: []
    property var hostInterfaces: []
    readonly property int controlWidth: 260
    readonly property var networkModes: ["nat", "route", "isolated", "open", "bridge"]
    readonly property var networkModeLabels: [qsTr("nat"), qsTr("routed"), qsTr("isolated"), qsTr("open"), qsTr("bridge")]

    spacing: 16

    function networkModeLabel(mode) {
        const index = root.networkModes.indexOf(mode);
        return index >= 0 ? root.networkModeLabels[index] : mode;
    }

    function refreshNetworks() {
        networks = Karton.networks(root.connectionUri);
        hostInterfaces = Karton.hostNetworkInterfaces();
    }

    function openNetworkEditor(network) {
        networkEditor.openFor(network);
    }

    onConnectionUriChanged: refreshNetworks()
    Component.onCompleted: refreshNetworks()

    Connections {
        target: Karton
        function onNetworksUpdated() {
            root.refreshNetworks();
        }
    }

        RowLayout {
            Layout.fillWidth: true
            Controls.Label {
                text: qsTr("virtual_networks")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
            }
            Controls.Button {
                text: qsTr("add_network")
                icon.name: "list-add-symbolic"
                onClicked: root.openNetworkEditor(null)
            }
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
        }

        ListView {
            Layout.fillWidth: true
            Layout.fillHeight: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 12
            clip: true
            model: root.networks
            spacing: 4
            delegate: Controls.ItemDelegate {
                width: ListView.view.width
                focusPolicy: Qt.NoFocus
                contentItem: RowLayout {
                    spacing: 8
                    ColumnLayout {
                        Layout.fillWidth: true
                        spacing: 0
                        Controls.Label {
                            text: modelData.name
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                        Controls.Label {
                            text: (modelData.active ? qsTr("active") : qsTr("inactive"))
                                  + " · " + root.networkModeLabel(modelData.forward)
                                  + (modelData.bridge.length > 0 ? " · " + modelData.bridge : "")
                            opacity: 0.7
                            Layout.fillWidth: true
                            elide: Text.ElideRight
                        }
                    }
                    Controls.Button {
                        text: qsTr("dhcp")
                        icon.name: "network-wired-symbolic"
                        enabled: modelData.ipv4Dhcp
                        onClicked: networkDhcpDialog.openFor(modelData.name)
                    }
                    Controls.Button {
                        text: qsTr("edit")
                        icon.name: "document-edit-symbolic"
                        onClicked: root.openNetworkEditor(modelData)
                    }
                    Controls.Button {
                        text: modelData.active ? qsTr("stop") : qsTr("start")
                        icon.name: modelData.active ? "media-playback-stop-symbolic" : "media-playback-start-symbolic"
                        onClicked: {
                            if (modelData.active) {
                                Karton.stopNetwork(modelData.name, root.connectionUri);
                            } else {
                                Karton.startNetwork(modelData.name, root.connectionUri);
                            }
                            root.refreshNetworks();
                        }
                    }
                    Controls.Button {
                        text: qsTr("delete")
                        icon.name: "edit-delete-symbolic"
                        enabled: !modelData.active
                        onClicked: {
                            if (Karton.deleteNetwork(modelData.name, root.connectionUri)) {
                                root.refreshNetworks();
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
            text: qsTr("these_networks_are_shared_by_all_virtual_machines")
        }


    NetworkDhcpDialog {
        id: networkDhcpDialog
        connectionUri: root.connectionUri
    }

    KartonDialog {
        id: networkEditor
        title: editing ? qsTr("edit_network") : qsTr("create_network")
        modal: true
        padding: 0
        showCloseButton: true
        standardButtons: Kirigami.Dialog.NoButton

        property bool editing: false
        property string editingName: ""
        property string originalIpv4Gateway: ""
        property string originalIpv6Gateway: ""
        property var interfaces: []
        property var forwardTargets: []
        property string savedState: ""
        readonly property int bridgeMode: 4
        readonly property bool isBridgeMode: modeCombo.currentIndex === bridgeMode

        function targetName(index) {
            if (index < 0 || index >= forwardTargets.length) {
                return "";
            }
            return forwardTargets[index].name;
        }

        function currentMode() {
            return root.networkModes[Math.max(0, modeCombo.currentIndex)];
        }

        function ipv4Active() {
            return ipv4Check.checked && !isBridgeMode;
        }

        function ipv6Active() {
            return ipv6Check.checked && !isBridgeMode;
        }

        function currentState() {
            const mode = currentMode();
            const ipv4On = ipv4Active();
            const ipv6On = ipv6Active();
            const useName = dnsUseName.checked;
            return JSON.stringify({
                name: nameField.text.trim(),
                mode: mode,
                forward: (mode === "nat" || mode === "route" || mode === "open") ? targetName(forwardToCombo.currentIndex) : "",
                bridge: mode === "bridge" ? bridgeNameField.text.trim() : "",
                ipv4Enabled: ipv4On,
                ipv4: ipv4On ? ipv4NetworkField.text.trim() : "",
                ipv4Dhcp: ipv4On && ipv4DhcpCheck.checked,
                ipv4DhcpStart: ipv4On && ipv4DhcpCheck.checked ? ipv4DhcpStart.text.trim() : "",
                ipv4DhcpEnd: ipv4On && ipv4DhcpCheck.checked ? ipv4DhcpEnd.text.trim() : "",
                ipv6Enabled: ipv6On,
                ipv6: ipv6On ? ipv6NetworkField.text.trim() : "",
                ipv6Dhcp: ipv6On && ipv6DhcpCheck.checked,
                ipv6DhcpStart: ipv6On && ipv6DhcpCheck.checked ? ipv6DhcpStart.text.trim() : "",
                ipv6DhcpEnd: ipv6On && ipv6DhcpCheck.checked ? ipv6DhcpEnd.text.trim() : "",
                domain: useName ? nameField.text.trim() : dnsCustomField.text.trim(),
                autostart: autostartCheck.checked
            });
        }

        function isValidConfig() {
            if (nameField.text.trim().length === 0) {
                return false;
            }
            if (isBridgeMode && bridgeNameField.text.trim().length === 0) {
                return false;
            }
            if (ipv4Active()) {
                if (ipv4NetworkField.text.trim().indexOf("/") < 0) {
                    return false;
                }
                if (ipv4DhcpCheck.checked && (ipv4DhcpStart.text.trim().length === 0 || ipv4DhcpEnd.text.trim().length === 0)) {
                    return false;
                }
            }
            if (ipv6Active()) {
                if (ipv6NetworkField.text.trim().indexOf("/") < 0) {
                    return false;
                }
                if (ipv6DhcpCheck.checked && (ipv6DhcpStart.text.trim().length === 0 || ipv6DhcpEnd.text.trim().length === 0)) {
                    return false;
                }
            }
            if (dnsCustom.checked && dnsCustomField.text.trim().length === 0) {
                return false;
            }
            return true;
        }

        function openFor(network) {
            const defaults = Karton.defaultNetworkConfig();
            editing = network !== null && network !== undefined;
            editingName = editing ? network.name : "";
            interfaces = root.hostInterfaces;

            let targets = [{ label: qsTr("any_physical_device"), name: "" }];
            for (let i = 0; i < interfaces.length; ++i) {
                targets.push({
                    label: interfaces[i].label + " (" + interfaces[i].name + ")",
                    name: interfaces[i].name
                });
            }
            forwardTargets = targets;

            const source = editing ? network : defaults;
            const mode = source.mode || source.forward || "nat";
            nameField.text = editing ? source.name : "";
            nameField.readOnly = editing;
            modeCombo.currentIndex = Math.max(0, root.networkModes.indexOf(mode));
            bridgeNameField.text = editing ? (source.bridge || "") : "";
            forwardToCombo.currentIndex = 0;
            const forwardInterface = editing ? (source.forwardInterface || "") : "";
            for (let i = 0; i < targets.length; ++i) {
                if (targets[i].name === forwardInterface) {
                    forwardToCombo.currentIndex = i;
                    break;
                }
            }

            const ipv4On = editing ? (source.ipv4 || "").length > 0 : defaults.ipv4Enabled;
            ipv4Check.checked = ipv4On;
            ipv4NetworkField.text = source.ipv4 || defaults.ipv4;
            ipv4DhcpCheck.checked = editing ? source.ipv4Dhcp === true : defaults.ipv4Dhcp;
            ipv4DhcpStart.text = source.ipv4DhcpStart || defaults.ipv4DhcpStart;
            ipv4DhcpEnd.text = source.ipv4DhcpEnd || defaults.ipv4DhcpEnd;
            originalIpv4Gateway = editing ? (source.ipv4Gateway || "") : defaults.ipv4Gateway;

            const ipv6On = editing ? (source.ipv6 || "").length > 0 : defaults.ipv6Enabled;
            ipv6Check.checked = ipv6On;
            ipv6NetworkField.text = source.ipv6 || defaults.ipv6;
            ipv6DhcpCheck.checked = editing ? source.ipv6Dhcp === true : defaults.ipv6Dhcp;
            ipv6DhcpStart.text = source.ipv6DhcpStart || defaults.ipv6DhcpStart;
            ipv6DhcpEnd.text = source.ipv6DhcpEnd || defaults.ipv6DhcpEnd;
            originalIpv6Gateway = editing ? (source.ipv6Gateway || "") : defaults.ipv6Gateway;

            const domainName = editing ? (source.domainName || "") : "";
            dnsUseName.checked = !editing || domainName.length === 0 || domainName === source.name;
            dnsCustom.checked = !dnsUseName.checked;
            dnsCustomField.text = domainName;
            autostartCheck.checked = editing ? Boolean(source.autostart) : true;

            savedState = currentState();
            open();
            Qt.callLater(function() {
                networkScroll.contentY = 0;
            });
        }

        function save() {
            if (!isValidConfig() || currentState() === savedState) {
                return;
            }
            const mode = currentMode();
            const config = {
                name: nameField.text.trim(),
                mode: mode,
                forwardInterface: (mode === "nat" || mode === "route" || mode === "open")
                    ? targetName(forwardToCombo.currentIndex) : "",
                bridgeName: mode === "bridge" ? bridgeNameField.text.trim() : "",
                domainName: dnsUseName.checked ? nameField.text.trim() : dnsCustomField.text.trim(),
                ipv4Enabled: ipv4Active(),
                ipv4: ipv4NetworkField.text.trim(),
                ipv4Gateway: originalIpv4Gateway,
                ipv4Dhcp: ipv4Check.checked && ipv4DhcpCheck.checked,
                ipv4DhcpStart: ipv4DhcpStart.text.trim(),
                ipv4DhcpEnd: ipv4DhcpEnd.text.trim(),
                ipv6Enabled: ipv6Active(),
                ipv6: ipv6NetworkField.text.trim(),
                ipv6Gateway: originalIpv6Gateway,
                ipv6Dhcp: ipv6Check.checked && ipv6DhcpCheck.checked,
                ipv6DhcpStart: ipv6DhcpStart.text.trim(),
                ipv6DhcpEnd: ipv6DhcpEnd.text.trim(),
                autostart: autostartCheck.checked
            };
            config.connectionUri = root.connectionUri;
            const success = editing ? Karton.updateNetwork(config) : Karton.createNetwork(config);
            if (success) {
                close();
                root.refreshNetworks();
                applicationWindow().showPassiveNotification(editing ? qsTr("network_updated") : qsTr("network_created"));
            }
        }

        contentItem: Item {
            implicitWidth: Kirigami.Units.gridUnit * 36
            implicitHeight: Kirigami.Units.gridUnit * 26

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: 0

                ScrollableSettingsView {
                    id: networkScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    flickableDirection: Flickable.VerticalFlick

                    ColumnLayout {
                        width: parent.width
                        spacing: 12

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 10
                    Layout.fillWidth: true

                    Controls.Label { text: qsTr("name"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.TextField {
                    id: nameField
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    validator: RegularExpressionValidator {
                        regularExpression: /^[A-Za-z0-9._-]+$/
                    }
                }
                Controls.Label { text: qsTr("mode"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.ComboBox {
                    id: modeCombo
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    model: root.networkModeLabels
                }
                Controls.Label {
                    text: qsTr("forward_to")
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    enabled: !networkEditor.isBridgeMode && modeCombo.currentIndex !== 2
                }
                Controls.ComboBox {
                    id: forwardToCombo
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    model: networkEditor.forwardTargets
                    textRole: "label"
                    enabled: !networkEditor.isBridgeMode && modeCombo.currentIndex !== 2
                }
                Controls.Label {
                    text: qsTr("bridge_name")
                    visible: networkEditor.isBridgeMode
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.TextField {
                    id: bridgeNameField
                    visible: networkEditor.isBridgeMode
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    placeholderText: "br0"
                }
                Controls.Label {
                    text: qsTr("start_on_boot")
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.CheckBox {
                    id: autostartCheck
                    Layout.alignment: Qt.AlignRight
                }
            }

            Controls.Label {
                text: qsTr("ipv4_configuration")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 10
                Layout.fillWidth: true

                Controls.Label { text: qsTr("enable_ipv4"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv4Check
                    Layout.alignment: Qt.AlignRight
                    enabled: !networkEditor.isBridgeMode
                }
                Controls.Label { text: qsTr("network"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.TextField {
                    id: ipv4NetworkField
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    placeholderText: "192.168.100.0/24"
                    enabled: networkEditor.ipv4Active()
                }
                Controls.Label { text: qsTr("enable_dhcpv4"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv4DhcpCheck
                    Layout.alignment: Qt.AlignRight
                    enabled: networkEditor.ipv4Active()
                }
                Controls.Label { text: qsTr("dhcpv4_range"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                RowLayout {
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Controls.TextField {
                        id: ipv4DhcpStart
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv4Active() && ipv4DhcpCheck.checked
                        placeholderText: qsTr("start")
                    }
                    Controls.Label { text: "–" }
                    Controls.TextField {
                        id: ipv4DhcpEnd
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv4Active() && ipv4DhcpCheck.checked
                        placeholderText: qsTr("end")
                    }
                }
            }

            Controls.Label {
                text: qsTr("ipv6_configuration")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 10
                Layout.fillWidth: true

                Controls.Label { text: qsTr("enable_ipv6"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv6Check
                    Layout.alignment: Qt.AlignRight
                    enabled: !networkEditor.isBridgeMode
                }
                Controls.Label { text: qsTr("network"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.TextField {
                    id: ipv6NetworkField
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    placeholderText: "fd00::/64"
                    enabled: networkEditor.ipv6Active()
                }
                Controls.Label { text: qsTr("enable_dhcpv6"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv6DhcpCheck
                    Layout.alignment: Qt.AlignRight
                    enabled: networkEditor.ipv6Active()
                }
                Controls.Label { text: qsTr("dhcpv6_range"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                RowLayout {
                    Layout.preferredWidth: 260
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Controls.TextField {
                        id: ipv6DhcpStart
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv6Active() && ipv6DhcpCheck.checked
                        placeholderText: qsTr("start")
                    }
                    Controls.Label { text: "–" }
                    Controls.TextField {
                        id: ipv6DhcpEnd
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv6Active() && ipv6DhcpCheck.checked
                        placeholderText: qsTr("end")
                    }
                }
            }

            Controls.Label {
                text: qsTr("dns_domain_name")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            Controls.ButtonGroup {
                id: dnsDomainGroup
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 8
                Layout.fillWidth: true

                Controls.Label { text: qsTr("domain"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignTop }
                ColumnLayout {
                    Layout.preferredWidth: 260
                    Layout.maximumWidth: 260
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Controls.RadioButton {
                        id: dnsUseName
                        text: qsTr("use_network_name")
                        checked: true
                        Controls.ButtonGroup.group: dnsDomainGroup
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Controls.RadioButton {
                            id: dnsCustom
                            text: qsTr("custom")
                            Controls.ButtonGroup.group: dnsDomainGroup
                        }
                        Controls.TextField {
                            id: dnsCustomField
                            Layout.fillWidth: true
                            Layout.preferredWidth: 180
                            enabled: dnsCustom.checked
                            placeholderText: qsTr("example_lan")
                        }
                    }
                }
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("changes_to_an_active_network_briefly_restart_it_so_the_new_configuration_takes_effect")
            }
                    }
                }
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: networkEditor.close()
            },
            Kirigami.Action {
                text: networkEditor.editing ? qsTr("save") : qsTr("create")
                icon.name: "dialog-ok-symbolic"
                enabled: networkEditor.isValidConfig() && networkEditor.currentState() !== networkEditor.savedState
                onTriggered: networkEditor.save()
            }
        ]
    }

}
