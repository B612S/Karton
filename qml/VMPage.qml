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
        interval: 5000
        repeat: true
        // virDomainScreenshot() is synchronous and briefly pauses the guest.
        // Do not poll it while the viewer window is active (or any other
        // window has focus), otherwise mouse input visibly stalls every few
        // seconds during a live session.
        running: root.isRunning && root.visible && Karton.viewerCount === 0
        triggeredOnStart: true
        onTriggered: {
            if (root.domain) {
                Karton.refreshDomainPreview(root.domain);
            }
        }
    }

    actions: [
        Kirigami.Action {
            text: i18n("Power")
            icon.name: isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
            onTriggered: powerMenu.popup()
        },
        Kirigami.Action {
            text: i18n("Pause")
            icon.name: "media-playback-pause-symbolic"
            visible: isRunning && !isPaused
            onTriggered: {
                if (Karton.pauseDomain(domain)) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Pausing VM: %1!", domain.config.name));
                }
            }
        },
        Kirigami.Action {
            text: i18n("Resume")
            icon.name: "media-playback-start-symbolic"
            visible: isPaused
            onTriggered: {
                if (Karton.resumeDomain(domain)) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Resuming VM: %1!", domain.config.name));
                }
            }
        },
        Kirigami.Action {
            text: i18n("Reboot")
            icon.name: "system-reboot-symbolic"
            visible: isRunning
            onTriggered: rebootMenu.popup()
        },
        Kirigami.Action {
            text: i18n("Save State")
            icon.name: "document-save-symbolic"
            visible: isRunning
            onTriggered: {
                if (Karton.saveDomainState(domain)) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Saving VM state: %1!", domain.config.name));
                }
            }
        },
        Kirigami.Action {
            text: i18n("Delete Saved State")
            icon.name: "edit-delete-symbolic"
            visible: !isRunning && hasSavedState
            onTriggered: {
                if (Karton.deleteSavedState(domain)) {
                    applicationWindow().showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Deleted saved state for VM: %1", domain.config.name));
                }
            }
        },
        Kirigami.Action {
            text: i18n("Snapshots")
            icon.name: "camera-photo-symbolic"
            shortcut: "Ctrl+Shift+S"
            onTriggered: snapshotsDialog.open()
        },
        Kirigami.Action {
            text: i18n("Clone")
            icon.name: "edit-copy-symbolic"
            onTriggered: cloneDialog.open()
        },
        Kirigami.Action {
            text: i18nc("verb, delete a VM", "Delete")
            icon.name: "delete-symbolic"
            visible: !isRunning
            onTriggered: {
                deleteConfirmationDialog.domain = domain;
                deleteConfirmationDialog.open();
            }
        },
        Kirigami.Action {
            id: viewVMAction
            text: i18nc("verb, open viewer for VM", "View VM")
            icon.name: "computer-laptop-symbolic"
            onTriggered: {
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
        },
        Kirigami.Action {
            icon.name: "settings-configure-symbolic"
            enabled: !isRunning
            onTriggered: {
                settingsDialog.open();
            }
        }
    ]


    Controls.Menu {
        id: powerMenu

        Controls.MenuItem {
            text: isRunning ? i18n("Shut Down") : i18n("Start")
            icon.name: isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
            onTriggered: {
                if (isRunning) {
                    Karton.stopDomain(domain);
                } else {
                    Karton.startDomain(domain);
                }
            }
        }
        Controls.MenuItem {
            text: i18n("Force Off")
            icon.name: "process-stop-symbolic"
            visible: isRunning
            onTriggered: Karton.forceStopDomain(domain)
        }
        Controls.MenuItem {
            text: isPaused ? i18n("Resume") : i18n("Pause")
            icon.name: isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
            visible: isRunning
            onTriggered: {
                if (isPaused) {
                    Karton.resumeDomain(domain);
                } else {
                    Karton.pauseDomain(domain);
                }
            }
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: i18n("Save State")
            icon.name: "document-save-symbolic"
            visible: isRunning
            onTriggered: Karton.saveDomainState(domain)
        }
    }

    Controls.Menu {
        id: rebootMenu

        Controls.MenuItem {
            text: i18n("Reboot")
            icon.name: "system-reboot-symbolic"
            onTriggered: Karton.rebootDomain(domain)
        }
        Controls.MenuItem {
            text: i18n("Reset")
            icon.name: "system-reboot-symbolic"
            onTriggered: Karton.resetDomain(domain)
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

            Image {
                id: previewImage
                anchors.fill: parent
                fillMode: Image.PreserveAspectFit
                source: domain ? "file://" + domain.config.screenshotPreviewPath : ""

                Connections {
                    target: domain
                    function onPreviewChanged() {
                        previewImage.source = "file://" + domain.config.screenshotPreviewPath + "?" + Date.now();
                    }
                }
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
                onClicked: viewVMAction.trigger()
            }
        }
        FormCard.FormCard {
            maximumWidth: parent.width - Kirigami.Units.gridUnit * 2

            FormCard.FormTextDelegate {
                text: i18n("UUID")
                description: domain ? domain.config.uuid : ""
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("State")
                description: domain ? domain.config.state : ""
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("CPU Usage")
                description: root.stats.cpuPercent !== undefined ? i18n("%1 %", root.stats.cpuPercent.toFixed(1)) : "—"
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("Maximum Memory (GB)")
                description: domain ? domain.config.maxRam : ""
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("Memory Usage")
                description: {
                    if (!domain) {
                        return "";
                    }
                    if (root.stats.memoryKiB !== undefined) {
                        return i18n("%1 MiB of %2 MiB", (root.stats.memoryKiB / 1024).toFixed(0), (root.stats.maxMemoryKiB / 1024).toFixed(0));
                    }
                    return domain.config.ramUsage + " GB";
                }
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("CPUs")
                description: domain ? domain.config.cpus : ""
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("Virtual Disk Path")
                description: {
                    if (!domain) {
                        return "";
                    }
                    const count = domain.config.virtualDiskPaths.length;
                    return count > 1 ? domain.config.virtualDiskPath + " (+" + (count - 1) + ")" : domain.config.virtualDiskPath;
                }
            }
            FormCard.FormDelegateSeparator {}
            FormCard.FormTextDelegate {
                text: i18n("ISO Disk Path")
                description: domain ? domain.config.isoDiskPath : ""
                trailing: Controls.Button {
                    text: "Eject"
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
