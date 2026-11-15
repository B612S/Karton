// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

import QtQuick
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

Kirigami.Dialog {
    id: connectionDialog

    title: i18n("Connection Settings")
    modal: true
    padding: Kirigami.Units.largeSpacing
    preferredWidth: Kirigami.Units.gridUnit * 30
    preferredHeight: Kirigami.Units.gridUnit * 14
    standardButtons: Kirigami.Dialog.NoButton

    onOpened: {
        uriField.text = Karton.connectionUri;
        uriField.forceActiveFocus();
    }

    customFooterActions: [
        Kirigami.Action {
            text: i18nc("verb, discard connection settings changes", "Cancel")
            icon.name: "dialog-cancel"
            onTriggered: connectionDialog.close()
        },
        Kirigami.Action {
            text: i18nc("verb, save connection settings", "Save")
            icon.name: "dialog-ok"
            onTriggered: {
                const uri = uriField.text.trim();
                if (uri === "") {
                    return;
                }
                Karton.saveConnectionUri(uri);
                connectionDialog.close();
                applicationWindow().showPassiveNotification(i18n("Restart Karton to apply the new connection."));
            }
        }
    ]

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        FormCard.FormCard {
            Layout.fillWidth: true

            FormCard.FormTextFieldDelegate {
                id: uriField
                label: i18n("Libvirt URI:")
                placeholderText: "qemu:///session"
                Layout.fillWidth: true
            }
        }

        Kirigami.InlineMessage {
            Layout.fillWidth: true
            text: i18n("The new connection is used after restarting Karton. KARTON_LIBVIRT_URI overrides this setting.")
            type: Kirigami.MessageType.Information
            visible: true
        }
    }
}
