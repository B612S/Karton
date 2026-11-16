// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.Dialog {
    id: root

    property var networks: []
    property int categoryIndex: 0
    readonly property var categories: [
        { label: i18n("Application") },
        { label: i18n("Libvirt Networks") },
        { label: i18n("About") }
    ]

    modal: true
    padding: 0
    width: applicationWindow() ? Math.min(760, applicationWindow().width - 48) : 760
    height: applicationWindow() ? Math.min(520, applicationWindow().height - 48) : 520
    header: Item {}

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.95)
        radius: 8
        border.color: "transparent"
    }

    onOpened: {
        uriField.text = Karton.connectionUri;
        refreshNetworks();
    }

    function refreshNetworks() {
        networks = Karton.networks();
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
                    text: i18n("Settings")
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
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
                        currentIndex: 0
                        model: root.categories
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
                                onClicked: categoryList.currentIndex = index
                            }
                        }
                    }
                }

                ScrollableSettingsView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 24
                    contentWidth: availableWidth
                    clip: true

                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        Controls.Label {
                            text: root.categoryIndex === 0 ? i18n("Application")
                                : root.categoryIndex === 1 ? i18n("Libvirt Networks")
                                : i18n("About")
                            font.pixelSize: 20
                            font.weight: Font.Bold
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 0
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label { text: i18n("Libvirt URI"); font.pixelSize: 14; Layout.fillWidth: true }
                            Controls.TextField {
                                id: uriField
                                Layout.fillWidth: true
                                placeholderText: "qemu:///session"
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: i18n("The new connection is used after restarting Karton. KARTON_LIBVIRT_URI overrides this setting.")
                            }
                            Controls.Button {
                                text: i18n("Save connection")
                                icon.name: "dialog-ok-symbolic"
                                Layout.alignment: Qt.AlignRight
                                onClicked: {
                                    const uri = uriField.text.trim();
                                    if (uri.length === 0) {
                                        return;
                                    }
                                    Karton.saveConnectionUri(uri);
                                    applicationWindow().showPassiveNotification(i18n("Restart Karton to apply the new connection."));
                                }
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 1
                            Layout.fillWidth: true
                            spacing: 16

                            ListView {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 10
                                clip: true
                                model: root.networks
                                spacing: 4
                                delegate: Controls.ItemDelegate {
                                    width: ListView.view.width
                                    focusPolicy: Qt.NoFocus
                                    contentItem: RowLayout {
                                        spacing: 12
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Controls.Label {
                                                text: modelData.name
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            Controls.Label {
                                                text: (modelData.active ? i18n("active") : i18n("inactive"))
                                                      + " · " + modelData.forward
                                                      + (modelData.bridge.length > 0 ? " · " + modelData.bridge : "")
                                                opacity: 0.7
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                        }
                                        Controls.CheckBox {
                                            text: i18n("Autostart")
                                            checked: modelData.autostart
                                            onToggled: {
                                                Karton.setNetworkAutostart(modelData.name, checked);
                                                root.refreshNetworks();
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
                                                root.refreshNetworks();
                                            }
                                        }
                                        Controls.Button {
                                            text: i18n("Delete")
                                            icon.name: "edit-delete-symbolic"
                                            enabled: !modelData.active
                                            onClicked: {
                                                if (Karton.deleteNetwork(modelData.name)) {
                                                    root.refreshNetworks();
                                                }
                                            }
                                        }
                                    }
                                }
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                spacing: 8
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
                                            root.refreshNetworks();
                                        }
                                    }
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: i18n("These networks are shared by all virtual machines.")
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 2
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: i18n("Karton")
                                font.pixelSize: 22
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: i18n("Virtual Machine Manager")
                                font.pixelSize: 14
                                opacity: 0.7
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: i18n("Version %1", Qt.application.version)
                                font.pixelSize: 14
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.7
                                text: i18n("A libvirt-based virtual machine manager for KDE, built with Qt and Kirigami.")
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                textFormat: Text.RichText
                                text: "<a href=\"https://github.com/B612S/karton\">GitHub repository</a> · <a href=\"https://kde.org\">KDE</a>"
                                onLinkActivated: link => Qt.openUrlExternally(link)
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.55
                                font.pixelSize: 12
                                text: i18n("GPL-3.0-or-later · © Karton contributors")
                            }
                        }
                    }
                }
            }
        }
    }

    Connections {
        target: root
        function onCategoryIndexChanged() {
            categoryList.currentIndex = root.categoryIndex;
        }
    }

    Connections {
        target: categoryList
        function onCurrentIndexChanged() {
            root.categoryIndex = categoryList.currentIndex;
        }
    }
}
