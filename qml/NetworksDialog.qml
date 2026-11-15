// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.Dialog {
    id: root
    property Domain domain: null

    title: domain ? i18n("Networks — %1", domain.config.name) : i18n("Networks")
    preferredWidth: Kirigami.Units.gridUnit * 34

    property var networkList: []
    property var interfaceList: []
    property string errorText: ""

    function refresh() {
        networkList = Karton.networks();
        interfaceList = domain ? Karton.domainInterfaces(domain) : [];
    }

    function report(message) {
        errorText = message;
    }

    onOpened: {
        errorText = "";
        refresh();
    }
    Component.onCompleted: refresh()

    Connections {
        target: Karton
        function onNetworksChanged() {
            root.refresh();
        }
        function onErrorOccurred(errorMessage) {
            root.report(errorMessage);
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

            Kirigami.Heading {
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.topMargin: Kirigami.Units.largeSpacing
                level: 3
                text: i18n("Libvirt networks")
            }

            Controls.Label {
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.fillWidth: true
                visible: root.errorText.length > 0
                text: root.errorText
                color: Kirigami.Theme.negativeTextColor
                wrapMode: Text.WordWrap
            }

            Controls.Label {
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                visible: root.networkList.length === 0
                text: i18n("No libvirt networks are defined.")
                opacity: 0.7
            }

            ListView {
                id: networkView
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 9
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                clip: true
                model: root.networkList
                spacing: Kirigami.Units.smallSpacing

                delegate: Controls.ItemDelegate {
                    width: ListView.view.width

                    contentItem: RowLayout {
                        spacing: Kirigami.Units.largeSpacing

                        ColumnLayout {
                            Layout.fillWidth: true
                            spacing: 0

                            Kirigami.Heading {
                                level: 4
                                text: modelData.name
                                elide: Text.ElideRight
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                text: (modelData.active ? i18n("active") : i18n("inactive"))
                                      + (modelData.bridge.length > 0 ? " · " + modelData.bridge : "")
                                      + " · " + modelData.forward
                                opacity: 0.7
                                elide: Text.ElideRight
                            }
                        }

                        Controls.Button {
                            text: modelData.active ? i18n("Stop") : i18n("Start")
                            icon.name: modelData.active ? "media-playback-stop" : "media-playback-start"
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
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.fillWidth: true

                Controls.TextField {
                    id: newNetworkField
                    Layout.fillWidth: true
                    placeholderText: i18n("New NAT network name")
                    text: "karton-nat"
                }
                Controls.Button {
                    text: i18n("Create NAT")
                    icon.name: "list-add"
                    enabled: newNetworkField.text.trim().length > 0
                    onClicked: {
                        if (Karton.createNatNetwork(newNetworkField.text)) {
                            root.refresh();
                        }
                    }
                }
            }

            Kirigami.Separator {
                Layout.fillWidth: true
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                visible: root.domain !== null
            }

            Kirigami.Heading {
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                visible: root.domain !== null
                level: 3
                text: i18n("VM interfaces")
            }

            ListView {
                Layout.fillWidth: true
                Layout.preferredHeight: Kirigami.Units.gridUnit * 6
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                visible: root.domain !== null
                clip: true
                model: root.interfaceList
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
                                text: modelData.source.length > 0 ? modelData.source : modelData.type
                                elide: Text.ElideRight
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                text: modelData.mac + " · " + modelData.type + " · " + modelData.model
                                opacity: 0.7
                                elide: Text.ElideRight
                            }
                        }

                        Controls.Button {
                            text: i18n("Detach")
                            icon.name: "list-remove"
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
                Layout.leftMargin: Kirigami.Units.largeSpacing
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.fillWidth: true
                visible: root.domain !== null

                Controls.ComboBox {
                    id: attachNetworkCombo
                    Layout.fillWidth: true
                    model: root.networkList.filter(function(network) { return network.active; })
                    textRole: "name"
                }
                Controls.ComboBox {
                    id: attachModelCombo
                    model: ["virtio", "e1000"]
                }
                Controls.Button {
                    text: i18n("Attach")
                    icon.name: "list-add"
                    enabled: attachNetworkCombo.count > 0
                    onClicked: {
                        if (Karton.attachNetwork(root.domain, attachNetworkCombo.currentText, attachModelCombo.currentText)) {
                            root.refresh();
                        }
                    }
                }
            }

            Controls.Button {
                Layout.alignment: Qt.AlignRight
                Layout.rightMargin: Kirigami.Units.largeSpacing
                Layout.bottomMargin: Kirigami.Units.largeSpacing
                text: i18n("Close")
                onClicked: root.close()
            }
        }
}
