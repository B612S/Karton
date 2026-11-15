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

    title: domain ? i18n("Storage — %1", domain.config.name) : i18n("Storage")
    property var diskList: []
    property string errorText: ""

    function refresh() {
        diskList = domain ? Karton.domainDisks(domain) : [];
    }

    onOpened: {
        errorText = "";
        imagePathField.text = "";
        isoPathField.text = "";
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

        Kirigami.Heading {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            level: 3
            text: i18n("Disks")
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

        ListView {
            Layout.fillWidth: true
            Layout.preferredHeight: Kirigami.Units.gridUnit * 9
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            clip: true
            model: root.diskList
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
                            text: modelData.device + " · " + modelData.target
                                  + (modelData.format.length > 0 ? " · " + modelData.format : "")
                            elide: Text.ElideRight
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            text: modelData.source
                            opacity: 0.7
                            elide: Text.ElideMiddle
                        }
                    }

                    Controls.Button {
                        text: i18n("Detach")
                        icon.name: "list-remove"
                        onClicked: {
                            if (Karton.detachDisk(root.domain, modelData.target)) {
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
        }

        Kirigami.Heading {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            level: 3
            text: i18n("Add a new disk")
        }

        RowLayout {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Controls.SpinBox {
                id: sizeSpinBox
                from: 1
                to: 2048
                value: 4
                editable: true
            }
            Controls.Label {
                text: i18n("GB")
            }
            Controls.ComboBox {
                id: diskFormatCombo
                model: ["qcow2", "raw", "vmdk"]
            }
            Controls.Button {
                text: i18n("Add disk")
                icon.name: "list-add"
                onClicked: {
                    if (Karton.addDisk(root.domain, sizeSpinBox.value, diskFormatCombo.currentText)) {
                        root.refresh();
                    }
                }
            }
        }

        Kirigami.Heading {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            level: 3
            text: i18n("Attach an existing disk or ISO")
        }

        RowLayout {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Controls.TextField {
                id: imagePathField
                Layout.fillWidth: true
                placeholderText: i18n("Path to a disk image")
            }
            Controls.ComboBox {
                id: imageFormatCombo
                model: ["qcow2", "raw", "vmdk"]
            }
            Controls.Button {
                text: i18n("Attach disk")
                enabled: imagePathField.text.trim().length > 0
                onClicked: {
                    if (Karton.attachDiskImage(root.domain, imagePathField.text, imageFormatCombo.currentText)) {
                        imagePathField.text = "";
                        root.refresh();
                    }
                }
            }
        }

        RowLayout {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Controls.TextField {
                id: isoPathField
                Layout.fillWidth: true
                placeholderText: i18n("Path to an ISO image")
            }
            Controls.Button {
                text: i18n("Attach ISO")
                enabled: isoPathField.text.trim().length > 0
                onClicked: {
                    if (Karton.attachIso(root.domain, isoPathField.text)) {
                        isoPathField.text = "";
                        root.refresh();
                    }
                }
            }
            Controls.Button {
                text: i18n("Close")
                onClicked: root.close()
            }
        }
    }
}
