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

    title: domain ? i18n("PCI Devices — %1", domain.config.name) : i18n("PCI Devices")

    property var hostDevices: []
    property var vmDevices: []
    property string errorText: ""

    function refresh() {
        hostDevices = Karton.pciDevices();
        vmDevices = domain ? Karton.domainPciDevices(domain) : [];
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

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            type: Kirigami.MessageType.Warning
            text: i18n("PCI passthrough detaches the device from the host and requires IOMMU. Only attach devices dedicated to this VM. The guest may need the vfio-pci driver.")
        }

        Kirigami.Heading {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            level: 3
            text: i18n("Host PCI devices")
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
            text: i18n("No PCI devices found.")
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
                            text: modelData.address + " · " + modelData.vendor + ":" + modelData.product
                                  + (modelData.iommuGroup.length > 0 ? " · IOMMU group " + modelData.iommuGroup : " · no IOMMU group")
                                  + (modelData.driver.length > 0 ? " · " + modelData.driver : "")
                            opacity: 0.7
                            elide: Text.ElideRight
                        }
                    }

                    Controls.Button {
                        text: i18n("Attach")
                        icon.name: "list-add"
                        enabled: root.domain !== null && modelData.iommuGroup.length > 0
                        onClicked: {
                            if (Karton.attachPciDevice(root.domain,
                                                       modelData.pciDomain,
                                                       modelData.bus,
                                                       modelData.slot,
                                                       modelData.function)) {
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

                    Controls.Label {
                        Layout.fillWidth: true
                        text: modelData.address
                        elide: Text.ElideRight
                    }

                    Controls.Button {
                        text: i18n("Detach")
                        icon.name: "list-remove"
                        onClicked: {
                            if (Karton.detachPciDevice(root.domain,
                                                       modelData.pciDomain,
                                                       modelData.bus,
                                                       modelData.slot,
                                                       modelData.function)) {
                                root.refresh();
                            }
                        }
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
