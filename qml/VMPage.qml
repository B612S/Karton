// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

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

    onDomainChanged: stats = domain && domain.isActive && Karton.viewerCount === 0 ? Karton.domainStats(domain) : ({})
    onIsRunningChanged: {
        if (!isRunning) {
            stats = ({});
        }
    }

    topPadding: 24
    bottomPadding: 24
    leftPadding: 24
    rightPadding: 24

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
        // The page is hidden behind an open viewer; polling libvirt stats
        // while the guest runs is unnecessary and any synchronous libvirt
        // call here would block the GUI thread of the viewer as well.
        running: root.isRunning && root.visible && Karton.viewerCount === 0
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
        // visible there and virsh screenshot briefly pauses QEMU. Text-only
        // VMs have no screen at all, so there is nothing to capture.
        running: root.isRunning && root.visible && Karton.viewerCount === 0
                 && root.domain !== null && root.domain.config.hasGraphics
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
            text: isRunning ? qsTr("shut_down") : qsTr("start")
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
            text: qsTr("power_off")
            icon.name: "arrow-down-symbolic"
            displayHint: Kirigami.DisplayHint.IconOnly
            visible: isRunning
            onTriggered: powerMenu.popup()
        },
        Kirigami.Action {
            text: isPaused ? qsTr("resume") : qsTr("pause")
            icon.name: isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
            visible: isRunning
            onTriggered: root.togglePause()
        },
        Kirigami.Action {
            text: qsTr("reboot")
            icon.name: "system-reboot-symbolic"
            visible: isRunning
            onTriggered: Karton.rebootDomain(domain)
        },
        Kirigami.Action {
            text: qsTr("reset")
            icon.name: "arrow-down-symbolic"
            displayHint: Kirigami.DisplayHint.IconOnly
            visible: isRunning
            onTriggered: rebootMenu.popup()
        },
        Kirigami.Action {
            text: qsTr("more")
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
            text: qsTr("power_off")
            icon.name: "process-stop-symbolic"
            onTriggered: Karton.forceStopDomain(domain)
        }
    }

    Controls.Menu {
        id: rebootMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: qsTr("reset")
            icon.name: "system-reboot-symbolic"
            onTriggered: Karton.resetDomain(domain)
        }
    }

    Controls.Menu {
        id: moreMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: qsTr("hibernate")
            icon.name: "document-save-symbolic"
            visible: isRunning
            onTriggered: {
                if (Karton.saveDomainState(domain)) {
                    applicationWindow().showPassiveNotification(qsTr("hibernating_vm_1").arg(domain.config.name));
                }
            }
        }
        Controls.MenuItem {
            text: qsTr("delete_hibernation")
            icon.name: "edit-delete-symbolic"
            visible: !isRunning && hasSavedState
            onTriggered: {
                if (Karton.deleteSavedState(domain)) {
                    applicationWindow().showPassiveNotification(qsTr("deleted_hibernation_for_vm_1").arg(domain.config.name));
                }
            }
        }
        Controls.MenuSeparator {
            visible: hasSavedState || isRunning
        }
        Controls.MenuItem {
            text: qsTr("snapshots")
            icon.name: "camera-photo-symbolic"
            onTriggered: snapshotsDialog.open()
        }
        Controls.MenuItem {
            text: qsTr("clone")
            icon.name: "edit-copy-symbolic"
            onTriggered: cloneDialog.open()
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: qsTr("settings")
            icon.name: "preferences-system-symbolic"
            onTriggered: settingsDialog.open()
        }
        Controls.MenuSeparator {
            visible: !isRunning
        }
        Controls.MenuItem {
            text: qsTr("delete")
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
                // A stopped VM has no screen: never leave the last captured
                // frame (or the frame saved by an earlier session) on display.
                image: root.isRunning && domain ? domain.previewFrame : null
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

            Kirigami.Icon {
                anchors.centerIn: parent
                visible: !root.isRunning
                source: {
                    const icon = domain ? Karton.domainOsIcon(domain) : "karton-os-generic";
                    return Karton.holidayMode && icon === "karton-os-linux" ? "karton-os-linux-newyear" : icon;
                }
                width: Kirigami.Units.iconSizes.huge
                height: width
                opacity: 0.35
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
            text: qsTr("details")
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

            Controls.Label { text: qsTr("uuid"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.uuid : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
                elide: Text.ElideRight
            }
            Controls.Label { text: qsTr("state"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.state : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
            Controls.Label { text: qsTr("cpu_usage"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: root.stats.cpuPercent !== undefined ? qsTr("s_1").arg(root.stats.cpuPercent.toFixed(1)) : "—"
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
            Controls.Label { text: qsTr("cpus"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.cpus : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
        }

        Controls.Label {
            text: qsTr("memory")
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

            Controls.Label { text: qsTr("maximum_memory_gb"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: domain ? domain.config.maxRam : ""
                font.pixelSize: 14
                Layout.preferredWidth: 280
                Layout.alignment: Qt.AlignRight
                horizontalAlignment: Text.AlignRight
            }
            Controls.Label { text: qsTr("memory_usage"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
            Controls.Label {
                text: {
                    if (!domain) {
                        return "";
                    }
                    if (root.stats.memoryUsedKiB !== undefined) {
                        return qsTr("s_1_mib_of_2_mib_used").arg((root.stats.memoryUsedKiB / 1024).toFixed(0)).arg((root.stats.memoryTotalKiB / 1024).toFixed(0));
                    }
                    if (root.stats.memoryKiB !== undefined) {
                        return qsTr("s_1_mib_allocated").arg((root.stats.memoryKiB / 1024).toFixed(0));
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
            text: qsTr("storage")
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

            Controls.Label { text: qsTr("virtual_disk_path"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
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
            Controls.Label { text: qsTr("iso_disk_path"); font.pixelSize: 14; Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter; opacity: 0.8 }
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
                    text: qsTr("eject")
                    icon.name: "media-eject-symbolic"
                    onClicked: {
                        if (Karton.ejectDisk(domain)) {
                            applicationWindow().showPassiveNotification(qsTr("iso_disk_ejected_for_vm_1").arg(domain.config.name));
                        }
                    }
                }
            }
        }
    }

    SettingsDialog {
        id: settingsDialog
        domain: root.domain
        parentWindow: applicationWindow()
    }

    Kirigami.Dialog {
        id: deleteConfirmationDialog
        title: qsTr("delete_1").arg(deleteConfirmationDialog.domain ? deleteConfirmationDialog.domain.config.name : "")

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
            text: deleteConfirmationDialog.domain ? qsTr("delete_vm_confirmation").arg(deleteConfirmationDialog.domain.config.name) : ""
        }
        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: {
                    deleteConfirmationDialog.domain = null;
                    deleteConfirmationDialog.close();
                }
            },
            Kirigami.Action {
                text: qsTr("keep_file")
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
                        window.showPassiveNotification(qsTr("deleted_vm_1").arg(domainName));
                    }
                }
            },
            Kirigami.Action {
                text: qsTr("delete_disk")
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
                        window.showPassiveNotification(qsTr("deleted_vm_1").arg(domainName));
                    }
                }
            }
        ]
    }
}
