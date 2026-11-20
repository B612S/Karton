// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

// Settings for one libvirt connection: its address, host networks and storage pools.
// Every connection has independent networks, so this dialog is opened from the
// connection's own context menu and never shows another connection's data.
KartonDialog {
    id: root

    property string connectionUri: ""
    property var connectionInfo: ({})
    property string errorText: ""
    property string statusMessage: ""
    readonly property bool isPrimary: connectionUri === Karton.connectionUri

    function openFor(uri) {
        connectionUri = uri;
        errorText = "";
        statusMessage = "";
        refreshInfo();
        uriField.text = uri;
        open();
    }

    function refreshInfo() {
        const infos = Karton.connectionInfos();
        for (const info of infos) {
            if (info.uri === root.connectionUri) {
                root.connectionInfo = info;
                return;
            }
        }
        root.connectionInfo = ({});
    }

    function saveUri() {
        const newUri = uriField.text.trim();
        errorText = "";
        statusMessage = "";
        if (newUri.length === 0 || newUri === root.connectionUri) {
            return;
        }
        const primary = root.isPrimary;
        if (!Karton.editConnection(root.connectionUri, newUri)) {
            errorText = qsTr("connection_uri_already_used");
            return;
        }
        if (primary) {
            statusMessage = qsTr("restart_karton_to_apply_the_new_connection");
        } else {
            root.connectionUri = newUri;
            statusMessage = qsTr("connection_uri_applied_immediately");
        }
        refreshInfo();
    }

    modal: true
    padding: 0
    readonly property real cardWidth: applicationWindow() ? Math.max(560, Math.min(760, applicationWindow().width - 48)) : 760
    readonly property real cardHeight: applicationWindow() ? Math.max(420, Math.min(560, applicationWindow().height - 48)) : 560
    width: cardWidth
    height: cardHeight
    header: Item {}
    title: qsTr("connection_settings")

    onOpened: {
        uriField.text = root.connectionUri;
        root.refreshInfo();
    }

    Connections {
        target: Karton
        function onConnectionsChanged() {
            root.refreshInfo();
        }
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
                    text: root.title
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

            ScrollableSettingsView {
                Layout.fillWidth: true
                Layout.fillHeight: true
                implicitHeight: 0
                contentMargins: 24

                ColumnLayout {
                    width: parent.width
                    spacing: 16

                    Controls.Label {
                        text: qsTr("connection_uri")
                        font.pixelSize: 15
                        font.weight: Font.Bold
                        Layout.fillWidth: true
                        Layout.topMargin: 2
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: Kirigami.Units.smallSpacing

                        Controls.TextField {
                            id: uriField
                            Layout.fillWidth: true
                            placeholderText: "qemu:///system"
                            onAccepted: root.saveUri()
                        }
                        Controls.Button {
                            text: qsTr("save_connection")
                            icon.name: "dialog-ok-symbolic"
                            enabled: uriField.text.trim().length > 0 && uriField.text.trim() !== root.connectionUri
                            onClicked: root.saveUri()
                        }
                    }
                    Controls.Label {
                        visible: root.statusMessage.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        opacity: 0.75
                        font.pixelSize: 12
                        text: root.statusMessage
                    }
                    Controls.Label {
                        visible: root.errorText.length > 0
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        color: Kirigami.Theme.negativeTextColor
                        font.pixelSize: 12
                        text: root.errorText
                    }
                    Controls.Label {
                        visible: text.length > 0
                        Layout.fillWidth: true
                        opacity: 0.75
                        font.pixelSize: 12
                        text: root.connectionInfo.statusText ? root.connectionInfo.statusText : ""
                    }

                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13)
                    }

                    NetworksPage {
                        Layout.fillWidth: true
                        connectionUri: root.connectionUri
                    }
                    Rectangle {
                        Layout.fillWidth: true
                        height: 1
                        color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13)
                    }
                    StoragePoolsPage {
                        Layout.fillWidth: true
                        connectionUri: root.connectionUri
                    }
                }
            }
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
