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

    title: domain ? i18n("Clone — %1", domain.config.name) : i18n("Clone VM")
    property string errorText: ""
    property bool cloning: false

    function defaultName() {
        return (domain ? domain.config.name : "vm") + "-clone";
    }

    onOpened: {
        errorText = "";
        cloning = false;
        nameField.text = defaultName();
    }

    Connections {
        target: Karton
        function onDomainCloned(success, sourceName, newName) {
            if (!root.cloning) {
                return;
            }
            if (success) {
                root.cloning = false;
                root.close();
                if (applicationWindow()) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the new VM name", "Cloned VM: %1", newName));
                }
            } else {
                root.cloning = false;
                root.errorText = i18n("Cloning failed. See the notification for details.");
            }
        }
        function onErrorOccurred(errorMessage) {
            root.errorText = errorMessage;
        }
    }

    ColumnLayout {
        Layout.fillWidth: true
        width: root.availableWidth
        spacing: Kirigami.Units.largeSpacing

        Controls.Label {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.topMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            text: i18n("The configuration and all local disk images will be copied. The clone is created stopped.")
            wrapMode: Text.WordWrap
            opacity: 0.8
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

        Controls.TextField {
            id: nameField
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true
            placeholderText: i18n("New VM name")
            enabled: !root.cloning
        }

        RowLayout {
            Layout.leftMargin: Kirigami.Units.largeSpacing
            Layout.rightMargin: Kirigami.Units.largeSpacing
            Layout.bottomMargin: Kirigami.Units.largeSpacing
            Layout.fillWidth: true

            Controls.BusyIndicator {
                visible: root.cloning
                running: root.cloning
            }

            Item {
                Layout.fillWidth: true
            }

            Controls.Button {
                text: i18n("Close")
                enabled: !root.cloning
                onClicked: root.close()
            }

            Controls.Button {
                text: i18n("Clone")
                icon.name: "edit-copy"
                enabled: !root.cloning && nameField.text.trim().length > 0 && root.domain !== null
                onClicked: {
                    root.errorText = "";
                    root.cloning = true;
                    if (!Karton.cloneDomain(root.domain, nameField.text)) {
                        root.cloning = false;
                    }
                }
            }
        }
    }
}
