// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.kirigamiaddons.formcard 1.0 as FormCard
import org.kde.karton

Kirigami.ScrollablePage {
    id: root

    title: domain ? domain.config.name : ""
    property Domain domain: null
    readonly property bool isRunning: domain ? domain.isActive : false
    readonly property bool isPaused: domain ? domain.isPaused : false
    readonly property bool hasSavedState: domain ? domain.hasSavedState : false
    property var stats: ({})

    onDomainChanged: stats = domain && domain.isActive ? Karton.domainStats(domain) : ({})
    onIsRunningChanged: {
        if (!isRunning) {
            stats = ({});
        }
    }

    topPadding: 0
    bottomPadding: Kirigami.Units.gridUnit
    leftPadding: 0
    rightPadding: 0

    Connections {
        target: Karton
        function onDomainRemoved(removedDomain) {
            if (removedDomain === root.domain && applicationWindow().pageStack.currentItem === root) {
                // Pop before the Domain object is destroyed, otherwise the
                // bindings below would briefly see a null domain.
                applicationWindow().pageStack.pop();
            }
        }
    }

    Timer {
        id: statsTimer
        interval: 2000
        repeat: true
        running: root.isRunning && root.visible
        triggeredOnStart: true
        onTriggered: {
            if (root.domain) {
                root.stats = Karton.domainStats(root.domain);
            }
        }
    }

    Timer {
        id: previewTimer
        interval: 1000
        repeat: true
        // Do not capture while a SPICE viewer is open: the guest is already
        // visible there and virsh screenshot briefly pauses QEMU.
        running: root.isRunning && root.visible && Karton.viewerCount === 0
        triggeredOnStart: true
        onTriggered: {
            if (root.domain) {
                Karton.refreshDomainPreview(root.domain);
            }
        }
    }

    function openViewerWindow() {
        if (!domain) {
            return;
        }
        const existing = Karton.viewerForDomain(domain);
        if (existing) {
            existing.show();
            existing.raise();
            existing.requestActivate();
            return;
        }
        const component = Qt.createComponent("VMViewerWindow.qml");
        if (component.status !== Component.Ready) {
            console.error("Failed to create VM viewer:", component.errorString());
            return;
        }
        const window = component.createObject(null, {
            domain: domain
        });
        if (!window) {
            console.error("Failed to create VM viewer window");
        }
    }

    function togglePause() {
        if (!domain || !isRunning) {
            return;
        }
        if (isPaused) {
            Karton.resumeDomain(domain);
        } else {
            Karton.pauseDomain(domain);
        }
    }

    actions: [
        Kirigami.Action {
            text: isRunning ? i18n("Shut Down") : i18n("Start")
            icon.name: isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
            onTriggered: {
                if (isRunning) {
                    Karton.stopDomain(domain);
                } else {
                    Karton.startDomain(domain);
                }
            }
        },
        Kirigami.Action {
            icon.name: "arrow-down-symbolic"
            displayHint: Kirigami.DisplayHint.IconOnly
            visible: isRunning
            onTriggered: powerMenu.popup()
        },
        Kirigami.Action {
            text: isPaused ? i18n("Resume") : i18n("Pause")
            icon.name: isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
            visible: isRunning
            onTriggered: root.togglePause()
        },
        Kirigami.Action {
            text: i18n("Reboot")
            icon.name: "system-reboot-symbolic"
            visible: isRunning
            onTriggered: Karton.rebootDomain(domain)
        },
        Kirigami.Action {
            icon.name: "arrow-down-symbolic"
            displayHint: Kirigami.DisplayHint.IconOnly
            visible: isRunning
            onTriggered: rebootMenu.popup()
        },
        Kirigami.Action {
            text: i18n("More")
            icon.name: "view-more-symbolic"
            onTriggered: moreMenu.popup()
        }
    ]

    Connections {
        target: Karton
        function onClosePopupsRequested() {
            powerMenu.close();
            rebootMenu.close();
            moreMenu.close();
        }
    }

    Controls.Menu {
        id: powerMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: i18n("Power Off")
            icon.name: "process-stop-symbolic"
            onTriggered: Karton.forceStopDomain(domain)
        }
    }

    Controls.Menu {
        id: rebootMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: i18n("Reset")
            icon.name: "system-reboot-symbolic"
            onTriggered: Karton.resetDomain(domain)
        }
    }

    Controls.Menu {
        id: moreMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: i18n("Hibernate")
            icon.name: "document-save-symbolic"
            visible: isRunning
            onTriggered: {
                if (Karton.saveDomainState(domain)) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Hibernating VM: %1", domain.config.name));
                }
            }
        }
        Controls.MenuItem {
            text: i18n("Delete Hibernation")
            icon.name: "edit-delete-symbolic"
            visible: !isRunning && hasSavedState
            onTriggered: {
                if (Karton.deleteSavedState(domain)) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Deleted hibernation for VM: %1", domain.config.name));
                }
            }
        }
        Controls.MenuSeparator {
            visible: hasSavedState || isRunning
        }
        Controls.MenuItem {
            text: i18n("Snapshots")
            icon.name: "camera-photo-symbolic"
            onTriggered: snapshotsDialog.open()
        }
        Controls.MenuItem {
            text: i18n("Clone")
            icon.name: "edit-copy-symbolic"
            onTriggered: cloneDialog.open()
        }
        Controls.MenuItem {
            text: i18n("Settings")
            icon.name: "settings-configure-symbolic"
            onTriggered: settingsDialog.show()
        }
        Controls.MenuSeparator {
            visible: !isRunning
        }
        Controls.MenuItem {
            text: i18nc("verb, delete a VM", "Delete")
            icon.name: "delete-symbolic"
            visible: !isRunning
            onTriggered: {
                deleteConfirmationDialog.domain = domain;
                deleteConfirmationDialog.open();
            }
        }
    }

    SnapshotsDialog {
        id: snapshotsDialog
        domain: root.domain
    }

    CloneDialog {
        id: cloneDialog
        domain: root.domain
    }

    Shortcut {
        sequence: "Ctrl+Shift+C"
        onActivated: cloneDialog.open()
    }

    ColumnLayout {
        spacing: Kirigami.Units.largeSpacing

        Rectangle {
            Layout.fillWidth: true
            color: "black"
            Layout.preferredHeight: Kirigami.Units.gridUnit * 16

            PreviewItem {
                anchors.fill: parent
                image: domain ? domain.previewFrame : null
            }

            Rectangle {
                anchors.fill: parent
                color: {
                    if (previewMouseArea.containsMouse) {
                        return Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.5);
                    } else {
                        return Qt.rgba(Kirigami.Theme.backgroundColor.r, Kirigami.Theme.backgroundColor.g, Kirigami.Theme.backgroundColor.b, 0.6);
                    }
                }
                Behavior on color {
                    ColorAnimation {
                        duration: Kirigami.Units.shortDuration
                    }
                }
            }

            MouseArea {
                id: previewMouseArea
                anchors.fill: parent
                cursorShape: Qt.PointingHandCursor
                hoverEnabled: true
                onClicked: root.openViewerWindow()
            }
        }
        Controls.Label {
            text: i18n("Details")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
        }
        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 16
            Layout.fillWidth: true

            Controls.Label { text: i18n("UUID"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.uuid : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
            }
            Controls.Label { text: i18n("State"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.state : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
            Controls.Label { text: i18n("CPU Usage"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: root.stats.cpuPercent !== undefined ? i18n("%1 %", root.stats.cpuPercent.toFixed(1)) : "—"
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
            Controls.Label { text: i18n("CPUs"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.cpus : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
        }

        Controls.Label {
            text: i18n("Memory")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
            Layout.topMargin: 8
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
        }
        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 16
            Layout.fillWidth: true

            Controls.Label { text: i18n("Maximum Memory (GB)"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.maxRam : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
            Controls.Label { text: i18n("Memory Usage"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: {
                    if (!domain) {
                        return "";
                    }
                    if (root.stats.memoryKiB !== undefined) {
                        return i18n("%1 MiB of %2 MiB", (root.stats.memoryKiB / 1024).toFixed(0), (root.stats.maxMemoryKiB / 1024).toFixed(0));
                    }
                    return domain.config.ramUsage + " GB";
                }
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
        }

        Controls.Label {
            text: i18n("Storage")
            font.pixelSize: 15
            font.weight: Font.Bold
            Layout.fillWidth: true
            Layout.topMargin: 8
        }
        Rectangle {
            Layout.fillWidth: true
            height: 1
            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
        }
        GridLayout {
            columns: 2
            columnSpacing: 16
            rowSpacing: 16
            Layout.fillWidth: true

            Controls.Label { text: i18n("Virtual Disk Path"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: {
                    if (!domain) {
                        return "";
                    }
                    const count = domain.config.virtualDiskPaths.length;
                    return count > 1 ? domain.config.virtualDiskPath + " (+" + (count - 1) + ")" : domain.config.virtualDiskPath;
                }
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideMiddle
            }
            Controls.Label { text: i18n("ISO Disk Path"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            RowLayout {
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                spacing: 8
                Controls.Label {
                    Layout.fillWidth: true
                    text: domain ? domain.config.isoDiskPath : ""
                    font.pixelSize: 14
                    horizontalAlignment: Text.AlignRight
                    elide: Text.ElideMiddle
                }
                Controls.Button {
                    text: i18n("Eject")
                    icon.name: "media-eject-symbolic"
                    onClicked: {
                        if (Karton.ejectDisk(domain)) {
                            applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "ISO Disk ejected for VM: %1", domain.config.name));
                        }
                    }
                }
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
        domain: root.domain
    }

    Kirigami.Dialog {
        id: deleteConfirmationDialog
        title: i18nc("Confirm deleting %1 (virtual machine name)", "Delete '%1'?",
            deleteConfirmationDialog.domain ? deleteConfirmationDialog.domain.config.name : "")

        padding: Kirigami.Units.largeSpacing
        preferredWidth: Math.max(Kirigami.Units.gridUnit * 20, root.width - Kirigami.Units.gridUnit * 30)
        preferredHeight: Math.max(Kirigami.Units.gridUnit * 14, root.height - Kirigami.Units.gridUnit * 30)

        showCloseButton: false
        standardButtons: Kirigami.Dialog.NoButton
        flatFooterButtons: false

        property var domain: null

        Controls.Label {
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            text: deleteConfirmationDialog.domain ? i18nc("%1 is the virtual machine name",
`You are about to remove the virtual machine, '%1'.
Would you like to remove the disk image as well?
This action cannot be undone.`,
            deleteConfirmationDialog.domain.config.name) : ""
        }
        customFooterActions: [
            Kirigami.Action {
                text: i18nc("verb, close confirmation dialog", "Cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: {
                    deleteConfirmationDialog.domain = null;
                    deleteConfirmationDialog.close();
                }
            },
            Kirigami.Action {
                text: i18nc("action, delete VM but keep disk image", "Keep File")
                icon.name: "edit-delete-remove-symbolic"

                onTriggered: {
                    const domainToDelete = deleteConfirmationDialog.domain;
                    if (!domainToDelete) {
                        return;
                    }
                    const domainName = domainToDelete.config.name;
                    const window = applicationWindow();
                    deleteConfirmationDialog.domain = null;
                    deleteConfirmationDialog.close();
                    if (Karton.deleteDomain(domainToDelete, false) && window) {
                        window.showPassiveNotification(i18nc("%1 is the virtual machine name", "Deleted VM: %1", domainName));
                    }
                }
            },
            Kirigami.Action {
                text: i18nc("action, delete VM and disk image", "Delete Disk")
                icon.name: "delete-symbolic"

                onTriggered: {
                    const domainToDelete = deleteConfirmationDialog.domain;
                    if (!domainToDelete) {
                        return;
                    }
                    const domainName = domainToDelete.config.name;
                    const window = applicationWindow();
                    deleteConfirmationDialog.domain = null;
                    deleteConfirmationDialog.close();
                    if (Karton.deleteDomain(domainToDelete, true) && window) {
                        window.showPassiveNotification(i18nc("%1 is the virtual machine name", "Deleted VM: %1", domainName));
                    }
                }
            }
        ]
    }
}
