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

    title: domain ? i18n("USB Devices — %1", domain.config.name) : i18n("USB Devices")
    preferredWidth: Math.min(680, applicationWindow() ? applicationWindow().width - 48 : 680)
    preferredHeight: Math.min(620, applicationWindow() ? applicationWindow().height - 48 : 620)

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.95)
        radius: 8
        border.color: "transparent"
    }


    property var hostDevices: []
    property var vmDevices: []
    property string errorText: ""

    function refresh() {
        hostDevices = Karton.usbDevices();
        vmDevices = domain ? Karton.domainUsbDevices(domain) : [];
    }

    onOpened: {
        errorText = "";
        refresh();
    }

    Component.onCompleted: refresh()

    Connections {
        target: Karton
        function onErrorOccurred(errorMessage) {
            root.errorText = errorMessage;
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: i18n("Close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        }
    ]

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Kirigami.Heading {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            level: 3
            text: i18n("Host USB devices")
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
            visible: root.hostDevices.length === 0
            text: i18n("No USB devices found.")
            opacity: 0.7
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 10
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            clip: true
            model: root.hostDevices
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
                            text: modelData.vendor + ":" + modelData.product
                                  + " · bus " + modelData.bus + ", device " + modelData.device
                                  + (modelData.hub ? " · hub" : "")
                                  + (modelData.devnode.length > 0 ? " · " + modelData.devnode : "")
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }

                    Controls.Button {
                        text: i18n("Attach")
                        icon.name: "list-add-symbolic"
                        enabled: root.domain !== null && !modelData.hub
                        onClicked: {
                            if (Karton.attachUsbDevice(root.domain,
                                                       modelData.vendor,
                                                       modelData.product,
                                                       modelData.bus,
                                                       modelData.device)) {
                                root.refresh();
                            }
                        }
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
            text: i18n("Attached to this VM")
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 5
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            visible: root.domain !== null
            clip: true
            model: root.vmDevices
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
                            visible: modelData.bus.length > 0 || modelData.device.length > 0
                            text: "bus " + modelData.bus + ", device " + modelData.device
                            opacity: 0.7
                        }
                    }

                    Controls.Button {
                        text: i18n("Detach")
                        icon.name: "list-remove-symbolic"
                        onClicked: {
                            if (Karton.detachUsbDevice(root.domain,
                                                       modelData.vendor,
                                                       modelData.product,
                                                       modelData.bus,
                                                       modelData.device)) {
                                root.refresh();
                            }
                        }
                    }
                }
            }
        }

    }
}
