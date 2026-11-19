// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

KartonDialog {
    id: root
    property Domain domain: null

    title: domain ? qsTr("usb_devices_1").arg(domain.config.name) : qsTr("usb_devices")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36
    preferredHeight: applicationWindow() ? Math.max(460, Math.min(620, applicationWindow().height - 48)) : 620

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
            text: qsTr("close")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        }
    ]

    ColumnLayout {
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.fillWidth: true
            visible: root.errorText.length > 0
            text: root.errorText
            color: Kirigami.Theme.negativeTextColor
            wrapMode: Text.WordWrap
        }

        Controls.Label {
            text: qsTr("host_usb_devices")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }

        Kirigami.Separator {
            Layout.fillWidth: true
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.hostDevices.length === 0
            text: qsTr("no_usb_devices_found")
            opacity: 0.7
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 10
            visible: root.hostDevices.length > 0
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
                        text: qsTr("attach")
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

        Controls.Label {
            visible: root.domain !== null
            text: qsTr("attached_to_this_vm")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }

        Kirigami.Separator {
            Layout.fillWidth: true
            visible: root.domain !== null
        }

        Controls.Label {
            Layout.fillWidth: true
            visible: root.domain !== null && root.vmDevices.length === 0
            text: qsTr("no_usb_devices_attached")
            opacity: 0.7
        }

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 5
            visible: root.domain !== null && root.vmDevices.length > 0
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
                            text: qsTr("bus_1_device_2").arg(modelData.bus).arg(modelData.device)
                            opacity: 0.7
                        }
                    }

                    Controls.Button {
                        text: qsTr("detach")
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
