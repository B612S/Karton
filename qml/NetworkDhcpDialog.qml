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
Kirigami.Dialog {
    id: root

    property string networkName: ""
    property var leases: []
    property var hosts: []
    property string errorText: ""

    title: networkName.length > 0 ? qsTr("dhcp_1").arg(networkName) : qsTr("dhcp")
    preferredWidth: applicationWindow() ? Math.max(640, Math.min(820, applicationWindow().width - 48)) : 820
    preferredHeight: applicationWindow() ? Math.max(480, Math.min(660, applicationWindow().height - 48)) : 660

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.98)
        radius: 8
        border.color: "transparent"
    }

    function refresh() {
        leases = Karton.networkLeases(networkName);
        hosts = Karton.networkDhcpHosts(networkName);
    }

    function openFor(name) {
        networkName = name;
        errorText = "";
        macField.text = "";
        ipField.text = "";
        hostnameField.text = "";
        refresh();
        open();
    }

    onOpened: leaseTimer.restart()
    onClosed: leaseTimer.stop()

    Timer {
        id: leaseTimer
        interval: 3000
        repeat: true
        onTriggered: root.refresh()
    }

    Connections {
        target: Karton
        function onErrorOccurred(message) {
            root.errorText = message;
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.smallSpacing

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        Kirigami.Heading {
            level: 4
            text: qsTr("active_leases")
        }
        Controls.Label {
            visible: root.leases.length === 0
            text: qsTr("no_active_leases")
            opacity: 0.7
        }
        ListView {
            id: leaseList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 13,
                                             Math.max(Kirigami.Units.gridUnit * 5, leaseList.contentHeight))
            clip: true
            model: root.leases
            spacing: 2

            delegate: RowLayout {
                width: ListView.view.width
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.preferredWidth: 140
                    text: modelData.ip
                }
                Controls.Label {
                    Layout.preferredWidth: 160
                    text: modelData.mac
                    opacity: 0.8
                }
                Controls.Label {
                    Layout.fillWidth: true
                    text: modelData.hostname
                    elide: Text.ElideRight
                }
                Controls.Label {
                    text: modelData.expiryTime
                    opacity: 0.6
                    font.pixelSize: 11
                }
            }
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
        }

        Kirigami.Heading {
            level: 4
            text: qsTr("reservations")
        }
        Controls.Label {
            visible: root.hosts.length === 0
            text: qsTr("no_reservations")
            opacity: 0.7
        }
        ListView {
            id: reservationList
            Layout.fillWidth: true
            Layout.preferredHeight: Math.min(Kirigami.Units.gridUnit * 10,
                                             Math.max(Kirigami.Units.gridUnit * 5, reservationList.contentHeight))
            clip: true
            model: root.hosts
            spacing: 2

            delegate: RowLayout {
                width: ListView.view.width
                spacing: Kirigami.Units.largeSpacing

                Controls.Label {
                    Layout.preferredWidth: 160
                    text: modelData.mac
                }
                Controls.Label {
                    Layout.preferredWidth: 140
                    text: modelData.ip
                }
                Controls.Label {
                    Layout.fillWidth: true
                    text: modelData.hostname
                    elide: Text.ElideRight
                }
                Controls.Button {
                    text: qsTr("remove")
                    icon.name: "list-remove-symbolic"
                    onClicked: {
                        if (Karton.removeNetworkDhcpHost(root.networkName, modelData.mac)) {
                            root.refresh();
                        }
                    }
                }
            }
        }

        RowLayout {
            Layout.fillWidth: true
            spacing: Kirigami.Units.smallSpacing

            Controls.TextField {
                id: macField
                Layout.preferredWidth: 160
                placeholderText: qsTr("mac_address")
            }
            Controls.TextField {
                id: ipField
                Layout.preferredWidth: 140
                placeholderText: qsTr("ip_address")
            }
            Controls.TextField {
                id: hostnameField
                Layout.fillWidth: true
                placeholderText: qsTr("hostname_optional")
            }
            Controls.Button {
                text: qsTr("add_reservation")
                icon.name: "list-add-symbolic"
                enabled: macField.text.trim().length > 0 && ipField.text.trim().length > 0
                onClicked: {
                    if (Karton.addNetworkDhcpHost(root.networkName,
                                                  macField.text.trim(),
                                                  ipField.text.trim(),
                                                  hostnameField.text.trim())) {
                        root.refresh();
                        macField.text = "";
                        ipField.text = "";
                        hostnameField.text = "";
                    }
                }
            }
        }

        Controls.Label {
            Layout.fillWidth: true
            Layout.topMargin: Kirigami.Units.smallSpacing
            text: qsTr("reservations_apply_to_the_network_configuration")
            opacity: 0.65
            font.pixelSize: 12
            wrapMode: Text.WordWrap
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        }
    ]
}
