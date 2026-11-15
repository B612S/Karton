// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.ScrollablePage {
    id: root

    title: i18nc("noun, Virtual Machines", "Virtual Machines")
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0

    function openDomain(domain) {
        const stack = applicationWindow().pageStack;
        if (stack.depth > 1 && stack.currentItem && stack.currentItem.domain === domain) {
            return;
        }

        const page = Qt.resolvedUrl("VMPage.qml");
        if (stack.depth > 1) {
            stack.replace(page, {
                domain: domain
            });
        } else {
            stack.push(page, {
                domain: domain
            });
        }
    }

    Connections {
        target: Karton
        function onErrorOccurred(errorMessage) {
            root.showPassiveNotification(errorMessage, "long");
        }
    }

    actions: [
        Kirigami.Action {
            icon.name: "list-add-symbolic"
            text: i18nc("verb, to create a new virtual machine", "Create")
            onTriggered: source => {
                addDomainDialog.open();
            }
        },
        Kirigami.Action {
            icon.name: "settings-configure"
            text: i18n("Connection Settings")
            onTriggered: connectionDialog.open()
        }
    ]

    InstallationDialog {
        id: addDomainDialog
    }

    ConnectionSettingsDialog {
        id: connectionDialog
    }

    ListView {
        id: view
        model: VMModel

        currentIndex: 0

        // When the VM list is empty, make the placeholder welcome text the full page.
        onCountChanged: {
            if (count === 0) {
                while (applicationWindow().pageStack.depth > 1) {
                    applicationWindow().pageStack.pop();
                }
            }
        }

        delegate: Controls.ItemDelegate {
            width: ListView.view.width
            highlighted: ListView.isCurrentItem

            leftPadding: Kirigami.Units.gridUnit
            rightPadding: leftPadding
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: topPadding

            ListView.onIsCurrentItemChanged: {
                if (ListView.isCurrentItem && applicationWindow().isWidescreen) {
                    // Defer the push until the delegate has been placed in the
                    // scene, otherwise the page cannot be shown yet.
                    Qt.callLater(() => root.openDomain(domain));
                }
            }

            onClicked: {
                view.currentIndex = model.index;
                root.openDomain(domain);
            }

            contentItem: RowLayout {
                id: delegateLayout
                spacing: Kirigami.Units.largeSpacing

                Kirigami.Icon {
                    source: "computer-symbolic"
                    // TODO: Add OS icon -> eventually, have screencap of VM window
                    Layout.fillHeight: true
                    Layout.maximumHeight: Kirigami.Units.iconSizes.huge
                    Layout.preferredWidth: height
                }
                ColumnLayout {
                    Kirigami.Heading {
                        Layout.fillWidth: true
                        font.weight: Font.DemiBold
                        level: 3
                        text: domain.config.name
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: domain.config.state + " | " + domain.config.shortOsId
                    }
                }
            }
        }

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - (Kirigami.Units.largeSpacing * 4)

            visible: view.count === 0

            text: Karton.connected ? i18nc("@title, greet user to Karton", "Welcome to Karton!") : i18n("Cannot connect to libvirt")
            explanation: Karton.connected ? i18nc("%1 is the libvirt connection URI", "Connected to %1. Create a new virtual machine to proceed.", Karton.connectionUri) : i18nc("%1 is the libvirt connection URI", "Check that libvirt is running and that the %1 connection is available.", Karton.connectionUri)
            icon.name: Karton.connected ? "computer-symbolic" : "dialog-error"
        }
    }
}
