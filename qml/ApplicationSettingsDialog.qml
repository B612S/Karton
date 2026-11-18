// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.Dialog {
    id: root

    property var networks: []
    property var hostInterfaces: []
    property int categoryIndex: 0
    property string savedUri: ""
    property string imagesFolderError: ""
    property var grantedFolders: []
    readonly property int controlWidth: 260
    readonly property var networkModes: ["nat", "route", "isolated", "open", "bridge"]
    readonly property var networkModeLabels: [qsTr("nat"), qsTr("routed"), qsTr("isolated"), qsTr("open"), qsTr("bridge")]

    function networkModeLabel(mode) {
        const index = root.networkModes.indexOf(mode);
        return index >= 0 ? root.networkModeLabels[index] : mode;
    }
    readonly property var categories: [
        { label: qsTr("application") },
        { label: qsTr("console") },
        { label: qsTr("storage") },
        { label: qsTr("libvirt_networks") },
        { label: qsTr("about") }
    ]

    modal: true
    padding: 0
    width: applicationWindow() ? Math.min(760, applicationWindow().width - 48) : 760
    height: applicationWindow() ? Math.min(520, applicationWindow().height - 48) : 520
    header: Item {}

    background: Rectangle {
        color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                       Kirigami.Theme.backgroundColor.g,
                       Kirigami.Theme.backgroundColor.b, 0.95)
        radius: 8
        border.color: "transparent"
    }

    onOpened: {
        uriField.text = Karton.connectionUri;
        savedUri = uriField.text.trim();
        spiceTlsCheck.checked = Karton.spiceTlsEnabled();
        spiceCaField.text = Karton.spiceCaCertificate();
        consoleFontSpin.value = Karton.globalConsoleFontSize();
        imagesFolderField.text = Karton.imagesDirectory();
        imagesFolderError = "";
        grantedFolders = Karton.grantedDiskDirectories();
        refreshNetworks();
    }

    function refreshNetworks() {
        networks = Karton.networks();
        hostInterfaces = Karton.hostNetworkInterfaces();
    }

    function openNetworkEditor(network) {
        networkEditor.openFor(network);
    }

    contentItem: Item {
        implicitWidth: root.width
        implicitHeight: root.height

        ColumnLayout {
            anchors.fill: parent
            spacing: 0

            RowLayout {
                Layout.fillWidth: true
                Layout.preferredHeight: 44
                Layout.leftMargin: 24
                Layout.rightMargin: 12
                Controls.Label {
                    text: qsTr("settings")
                    font.pixelSize: 16
                    font.weight: Font.DemiBold
                }
                Item { Layout.fillWidth: true }
                Controls.ToolButton {
                    icon.name: "dialog-close"
                    display: Controls.AbstractButton.IconOnly
                    onClicked: root.close()
                }
            }

            RowLayout {
                Layout.fillWidth: true
                Layout.fillHeight: true
                spacing: 0

                Rectangle {
                    Layout.preferredWidth: 180
                    Layout.fillHeight: true
                    radius: 8
                    color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                                   Kirigami.Theme.backgroundColor.g,
                                   Kirigami.Theme.backgroundColor.b, 0.5)

                    ListView {
                        id: categoryList
                        anchors.fill: parent
                        anchors.margins: 12
                        spacing: 4
                        currentIndex: 0
                        model: root.categories
                        delegate: Rectangle {
                            id: categoryButton
                            width: ListView.view.width
                            height: 36
                            radius: 4
                            color: ListView.isCurrentItem
                                ? Qt.rgba(Kirigami.Theme.highlightColor.r,
                                          Kirigami.Theme.highlightColor.g,
                                          Kirigami.Theme.highlightColor.b, 0.35)
                                : (categoryMouse.containsMouse
                                    ? Qt.rgba(Kirigami.Theme.textColor.r,
                                              Kirigami.Theme.textColor.g,
                                              Kirigami.Theme.textColor.b, 0.08)
                                    : "transparent")
                            Text {
                                text: modelData.label
                                color: Kirigami.Theme.textColor
                                font.pixelSize: 14
                                font.weight: ListView.isCurrentItem ? Font.DemiBold : Font.Normal
                                anchors.verticalCenter: parent.verticalCenter
                                anchors.left: parent.left
                                anchors.leftMargin: 12
                            }
                            Rectangle {
                                visible: ListView.isCurrentItem
                                anchors.left: parent.left
                                anchors.top: parent.top
                                anchors.bottom: parent.bottom
                                anchors.topMargin: 4
                                anchors.bottomMargin: 4
                                width: 3
                                radius: 1.5
                                color: Kirigami.Theme.highlightColor
                            }
                            MouseArea {
                                id: categoryMouse
                                anchors.fill: parent
                                hoverEnabled: true
                                onClicked: categoryList.currentIndex = index
                            }
                        }
                    }
                }

                ScrollableSettingsView {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.margins: 24
                    clip: true
                    scrollBarPolicy: networkEditor.visible ? Controls.ScrollBar.AlwaysOff : Controls.ScrollBar.AsNeeded

                    ColumnLayout {
                        width: parent.width
                        spacing: 16

                        Controls.Label {
                            text: root.categoryIndex === 0 ? qsTr("application")
                                : root.categoryIndex === 1 ? qsTr("console")
                                : root.categoryIndex === 2 ? qsTr("storage")
                                : root.categoryIndex === 3 ? qsTr("libvirt_networks")
                                : qsTr("about")
                            font.pixelSize: 20
                            font.weight: Font.Bold
                            Layout.fillWidth: true
                        }
                        Rectangle {
                            Layout.fillWidth: true
                            height: 1
                            color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 0
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("settings_interface")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                            }

                            GridLayout {
                                columns: 2
                                columnSpacing: 16
                                rowSpacing: 16
                                Layout.fillWidth: true

                                Controls.Label {
                                    text: qsTr("settings_language")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.ComboBox {
                                    id: languageCombo
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    model: [
                                        { value: "auto", text: qsTr("language_auto") },
                                        { value: "en", text: "English" },
                                        { value: "ru", text: "Русский" }
                                    ]
                                    textRole: "text"
                                    valueRole: "value"
                                    currentIndex: {
                                        const index = model.findIndex(item => item.value === Karton.language);
                                        return index !== -1 ? index : 0;
                                    }
                                    onActivated: (index) => {
                                        const newLanguage = model[index].value;
                                        if (Karton.language !== newLanguage) {
                                            Karton.language = newLanguage;
                                        }
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                Layout.topMargin: 4
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13)
                            }

                            Controls.Label {
                                text: qsTr("libvirt_uri")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                            }

                            GridLayout {
                                columns: 2
                                columnSpacing: 16
                                rowSpacing: 16
                                Layout.fillWidth: true

                                Controls.Label {
                                    text: qsTr("connection_uri")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.TextField {
                                    id: uriField
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    placeholderText: "qemu:///session"
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("the_new_connection_is_used_after_restarting_karton_karton_libvirt_uri_overrides_this_setti")
                                }
                                Controls.Button {
                                    text: qsTr("save_connection")
                                    icon.name: "dialog-ok-symbolic"
                                    Layout.columnSpan: 2
                                    Layout.alignment: Qt.AlignRight
                                    enabled: uriField.text.trim().length > 0 && uriField.text.trim() !== savedUri
                                    onClicked: {
                                        const uri = uriField.text.trim();
                                        if (uri.length === 0) {
                                            return;
                                        }
                                        Karton.saveConnectionUri(uri);
                                        savedUri = uri;
                                        applicationWindow().showPassiveNotification(qsTr("restart_karton_to_apply_the_new_connection"));
                                    }
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                Layout.topMargin: 4
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13)
                            }

                            Controls.Label {
                                text: qsTr("spice_tls")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                            }

                            GridLayout {
                                columns: 2
                                columnSpacing: 16
                                rowSpacing: 16
                                Layout.fillWidth: true

                                Controls.Label {
                                    text: qsTr("use_tls_when_the_vm_provides_a_tls_port")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.CheckBox {
                                    id: spiceTlsCheck
                                    Layout.alignment: Qt.AlignRight | Qt.AlignVCenter
                                    onToggled: Karton.saveSpiceTlsEnabled(checked)
                                }
                                Controls.Label {
                                    text: qsTr("ca_certificate")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                RowLayout {
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 8
                                    Controls.TextField {
                                        id: spiceCaField
                                        Layout.fillWidth: true
                                        placeholderText: qsTr("path_to_the_ca_certificate")
                                        onEditingFinished: Karton.saveSpiceCaCertificate(text.trim())
                                    }
                                    Controls.Button {
                                        text: qsTr("choose")
                                        icon.name: "document-open-symbolic"
                                        onClicked: caFileDialog.open()
                                    }
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    text: qsTr("used_for_spice_tls_connections_the_certificate_must_be_trusted_by_the_host_and_match_the_s")
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                Layout.topMargin: 4
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.13)
                            }

                            Controls.Label {
                                text: qsTr("maintenance")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
                            }

                            Controls.Button {
                                text: qsTr("clear_preview_images")
                                icon.name: "image-x-generic-symbolic"
                                Layout.alignment: Qt.AlignRight
                                onClicked: {
                                    const removed = Karton.clearPreviewCache();
                                    applicationWindow().showPassiveNotification(
                                        qsTr("removed_1_preview_images").arg(removed));
                                }
                            }

                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: qsTr("data_for_deleted_vms_previews_and_per_vm_window_audio_display_and_console_settings_is_remo")
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 1
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("serial_console")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
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

                                Controls.Label {
                                    text: qsTr("font_size")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                Controls.SpinBox {
                                    id: consoleFontSpin
                                    from: 8
                                    to: 96
                                    editable: true
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    onValueModified: Karton.saveGlobalConsoleFontSize(value)
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("used_by_the_serial_console_of_text_only_virtual_machines_a_per_vm_value_in_the_display_set")
                                }
                            }

                            Rectangle {
                                Layout.fillWidth: true
                                Layout.preferredHeight: Math.max(Kirigami.Units.gridUnit * 4, consolePreviewText.implicitHeight + 24)
                                radius: 6
                                color: "#101010"
                                border.color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.12)

                                Controls.Label {
                                    id: consolePreviewText
                                    anchors.left: parent.left
                                    anchors.right: parent.right
                                    anchors.top: parent.top
                                    anchors.margins: 12
                                    wrapMode: Text.WrapAtWordBoundaryOrAnywhere
                                    verticalAlignment: Text.AlignTop
                                    color: "#d0d0d0"
                                    font.family: "monospace"
                                    font.pixelSize: consoleFontSpin.value
                                    text: "The quick brown fox jumps over the lazy dog\n0123456789"
                                }
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 2
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("vm_storage")
                                font.pixelSize: 15
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                                Layout.topMargin: 2
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

                                Controls.Label {
                                    text: qsTr("image_folder")
                                    font.pixelSize: 14
                                    Layout.fillWidth: true
                                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                                }
                                RowLayout {
                                    Layout.preferredWidth: root.controlWidth
                                    Layout.maximumWidth: root.controlWidth
                                    Layout.alignment: Qt.AlignRight
                                    spacing: 8
                                    Controls.TextField {
                                        id: imagesFolderField
                                        Layout.fillWidth: true
                                        readOnly: true
                                        placeholderText: Karton.defaultImagesDirectory()
                                    }
                                    Controls.Button {
                                        text: qsTr("choose")
                                        icon.name: "folder-open-symbolic"
                                        onClicked: imagesFolderDialog.open()
                                    }
                                }
                                Controls.Label {
                                    Layout.columnSpan: 2
                                    Layout.fillWidth: true
                                    wrapMode: Text.WordWrap
                                    opacity: 0.65
                                    font.pixelSize: 12
                                    text: qsTr("new_vm_disks_are_created_in_this_folder_by_default_default_1").arg(Karton.defaultImagesDirectory())
                                }
                            }

                            Kirigami.InlineMessage {
                                Layout.fillWidth: true
                                type: Kirigami.MessageType.Error
                                visible: root.imagesFolderError.length > 0
                                text: root.imagesFolderError
                            }

                            RowLayout {
                                Layout.fillWidth: true
                                Item { Layout.fillWidth: true }
                                Controls.Button {
                                    text: qsTr("restore_default_folder")
                                    icon.name: "edit-undo-symbolic"
                                    onClicked: {
                                        const path = Karton.defaultImagesDirectory();
                                        imagesFolderField.text = path;
                                        const error = Karton.checkDiskDirectoryAccess(path);
                                        if (error.length > 0) {
                                            root.imagesFolderError = "";
                                            permissionGrantDialog.openFor(path);
                                            return;
                                        }
                                        root.imagesFolderError = "";
                                        Karton.saveImagesDirectory("");
                                        imagesFolderField.text = Karton.imagesDirectory();
                                    }
                                }
                            }

                            Controls.Label {
                                text: qsTr("granted_access")
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
                            Repeater {
                                model: root.grantedFolders
                                delegate: Controls.Label {
                                    Layout.fillWidth: true
                                    text: modelData
                                    wrapMode: Text.WrapAnywhere
                                    font.pixelSize: 12
                                    opacity: 0.7
                                }
                            }
                            Controls.Label {
                                visible: root.grantedFolders.length === 0
                                text: qsTr("no_folders_have_been_granted_access_by_karton")
                                opacity: 0.65
                                font.pixelSize: 12
                                Layout.fillWidth: true
                            }
                            Controls.Button {
                                text: qsTr("clear_granted_permissions")
                                icon.name: "edit-clear-symbolic"
                                enabled: root.grantedFolders.length > 0
                                Layout.alignment: Qt.AlignRight
                                onClicked: revokeAccessDialog.open()
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 3
                            Layout.fillWidth: true
                            Layout.fillHeight: true
                            spacing: 16

                            RowLayout {
                                Layout.fillWidth: true
                                Controls.Label {
                                    text: qsTr("virtual_networks")
                                    font.pixelSize: 15
                                    font.weight: Font.Bold
                                    Layout.fillWidth: true
                                }
                                Controls.Button {
                                    text: qsTr("add_network")
                                    icon.name: "list-add-symbolic"
                                    onClicked: root.openNetworkEditor(null)
                                }
                            }
                            Rectangle {
                                Layout.fillWidth: true
                                height: 1
                                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
                            }

                            ListView {
                                Layout.fillWidth: true
                                Layout.fillHeight: true
                                Layout.preferredHeight: Kirigami.Units.gridUnit * 12
                                clip: true
                                model: root.networks
                                spacing: 4
                                delegate: Controls.ItemDelegate {
                                    width: ListView.view.width
                                    focusPolicy: Qt.NoFocus
                                    contentItem: RowLayout {
                                        spacing: 8
                                        ColumnLayout {
                                            Layout.fillWidth: true
                                            spacing: 0
                                            Controls.Label {
                                                text: modelData.name
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                            Controls.Label {
                                                text: (modelData.active ? qsTr("active") : qsTr("inactive"))
                                                      + " · " + root.networkModeLabel(modelData.forward)
                                                      + (modelData.bridge.length > 0 ? " · " + modelData.bridge : "")
                                                opacity: 0.7
                                                Layout.fillWidth: true
                                                elide: Text.ElideRight
                                            }
                                        }
                                        Controls.Button {
                                            text: qsTr("edit")
                                            icon.name: "document-edit-symbolic"
                                            onClicked: root.openNetworkEditor(modelData)
                                        }
                                        Controls.Button {
                                            text: modelData.active ? qsTr("stop") : qsTr("start")
                                            icon.name: modelData.active ? "media-playback-stop-symbolic" : "media-playback-start-symbolic"
                                            onClicked: {
                                                if (modelData.active) {
                                                    Karton.stopNetwork(modelData.name);
                                                } else {
                                                    Karton.startNetwork(modelData.name);
                                                }
                                                root.refreshNetworks();
                                            }
                                        }
                                        Controls.Button {
                                            text: qsTr("delete")
                                            icon.name: "edit-delete-symbolic"
                                            enabled: !modelData.active
                                            onClicked: {
                                                if (Karton.deleteNetwork(modelData.name)) {
                                                    root.refreshNetworks();
                                                }
                                            }
                                        }
                                    }
                                }
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.65
                                font.pixelSize: 12
                                text: qsTr("these_networks_are_shared_by_all_virtual_machines")
                            }
                        }

                        ColumnLayout {
                            visible: root.categoryIndex === 4
                            Layout.fillWidth: true
                            spacing: 16

                            Controls.Label {
                                text: qsTr("karton")
                                font.pixelSize: 22
                                font.weight: Font.Bold
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: qsTr("virtual_machine_manager")
                                font.pixelSize: 14
                                opacity: 0.7
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                text: qsTr("version_1").arg(Qt.application.version)
                                font.pixelSize: 14
                                Layout.fillWidth: true
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.7
                                text: qsTr("a_libvirt_based_virtual_machine_manager_for_kde_built_with_qt_and_kirigami")
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                textFormat: Text.RichText
                                text: "<a href=\"https://github.com/B612S/karton\">" + qsTr("github_repository") + "</a> · <a href=\"https://kde.org\">KDE</a>"
                                onLinkActivated: link => Qt.openUrlExternally(link)
                            }
                            Controls.Label {
                                Layout.fillWidth: true
                                wrapMode: Text.WordWrap
                                opacity: 0.55
                                font.pixelSize: 12
                                text: qsTr("gpl_3_0_or_later_karton_contributors")
                            }
                        }
                    }
                }
            }
        }
    }

    Controls.ButtonGroup {
        id: dnsDomainGroup
    }

    Kirigami.Dialog {
        id: networkEditor
        title: editing ? qsTr("edit_network") : qsTr("create_network")
        modal: true
        padding: Kirigami.Units.largeSpacing
        showCloseButton: true
        standardButtons: Kirigami.Dialog.NoButton

        property bool editing: false
        property string editingName: ""
        property string originalIpv4Gateway: ""
        property string originalIpv6Gateway: ""
        property var interfaces: []
        property var forwardTargets: []
        property string savedState: ""
        readonly property int bridgeMode: 4
        readonly property bool isBridgeMode: modeCombo.currentIndex === bridgeMode

        background: Rectangle {
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.97)
            radius: 8
            border.color: "transparent"
        }

        function targetName(index) {
            if (index < 0 || index >= forwardTargets.length) {
                return "";
            }
            return forwardTargets[index].name;
        }

        function currentMode() {
            return root.networkModes[Math.max(0, modeCombo.currentIndex)];
        }

        function ipv4Active() {
            return ipv4Check.checked && !isBridgeMode;
        }

        function ipv6Active() {
            return ipv6Check.checked && !isBridgeMode;
        }

        function currentState() {
            const mode = currentMode();
            const ipv4On = ipv4Active();
            const ipv6On = ipv6Active();
            const useName = dnsUseName.checked;
            return JSON.stringify({
                name: nameField.text.trim(),
                mode: mode,
                forward: (mode === "nat" || mode === "route" || mode === "open") ? targetName(forwardToCombo.currentIndex) : "",
                bridge: mode === "bridge" ? bridgeNameField.text.trim() : "",
                ipv4Enabled: ipv4On,
                ipv4: ipv4On ? ipv4NetworkField.text.trim() : "",
                ipv4Dhcp: ipv4On && ipv4DhcpCheck.checked,
                ipv4DhcpStart: ipv4On && ipv4DhcpCheck.checked ? ipv4DhcpStart.text.trim() : "",
                ipv4DhcpEnd: ipv4On && ipv4DhcpCheck.checked ? ipv4DhcpEnd.text.trim() : "",
                ipv6Enabled: ipv6On,
                ipv6: ipv6On ? ipv6NetworkField.text.trim() : "",
                ipv6Dhcp: ipv6On && ipv6DhcpCheck.checked,
                ipv6DhcpStart: ipv6On && ipv6DhcpCheck.checked ? ipv6DhcpStart.text.trim() : "",
                ipv6DhcpEnd: ipv6On && ipv6DhcpCheck.checked ? ipv6DhcpEnd.text.trim() : "",
                domain: useName ? nameField.text.trim() : dnsCustomField.text.trim(),
                autostart: autostartCheck.checked
            });
        }

        function isValidConfig() {
            if (nameField.text.trim().length === 0) {
                return false;
            }
            if (isBridgeMode && bridgeNameField.text.trim().length === 0) {
                return false;
            }
            if (ipv4Active()) {
                if (ipv4NetworkField.text.trim().indexOf("/") < 0) {
                    return false;
                }
                if (ipv4DhcpCheck.checked && (ipv4DhcpStart.text.trim().length === 0 || ipv4DhcpEnd.text.trim().length === 0)) {
                    return false;
                }
            }
            if (ipv6Active()) {
                if (ipv6NetworkField.text.trim().indexOf("/") < 0) {
                    return false;
                }
                if (ipv6DhcpCheck.checked && (ipv6DhcpStart.text.trim().length === 0 || ipv6DhcpEnd.text.trim().length === 0)) {
                    return false;
                }
            }
            if (dnsCustom.checked && dnsCustomField.text.trim().length === 0) {
                return false;
            }
            return true;
        }

        function openFor(network) {
            const defaults = Karton.defaultNetworkConfig();
            editing = network !== null && network !== undefined;
            editingName = editing ? network.name : "";
            interfaces = root.hostInterfaces;

            let targets = [{ label: qsTr("any_physical_device"), name: "" }];
            for (let i = 0; i < interfaces.length; ++i) {
                targets.push({
                    label: interfaces[i].label + " (" + interfaces[i].name + ")",
                    name: interfaces[i].name
                });
            }
            forwardTargets = targets;

            const source = editing ? network : defaults;
            const mode = source.mode || source.forward || "nat";
            nameField.text = editing ? source.name : "";
            nameField.readOnly = editing;
            modeCombo.currentIndex = Math.max(0, root.networkModes.indexOf(mode));
            bridgeNameField.text = editing ? (source.bridge || "") : "";
            forwardToCombo.currentIndex = 0;
            const forwardInterface = editing ? (source.forwardInterface || "") : "";
            for (let i = 0; i < targets.length; ++i) {
                if (targets[i].name === forwardInterface) {
                    forwardToCombo.currentIndex = i;
                    break;
                }
            }

            const ipv4On = editing ? (source.ipv4 || "").length > 0 : defaults.ipv4Enabled;
            ipv4Check.checked = ipv4On;
            ipv4NetworkField.text = source.ipv4 || defaults.ipv4;
            ipv4DhcpCheck.checked = editing ? source.ipv4Dhcp === true : defaults.ipv4Dhcp;
            ipv4DhcpStart.text = source.ipv4DhcpStart || defaults.ipv4DhcpStart;
            ipv4DhcpEnd.text = source.ipv4DhcpEnd || defaults.ipv4DhcpEnd;
            originalIpv4Gateway = editing ? (source.ipv4Gateway || "") : defaults.ipv4Gateway;

            const ipv6On = editing ? (source.ipv6 || "").length > 0 : defaults.ipv6Enabled;
            ipv6Check.checked = ipv6On;
            ipv6NetworkField.text = source.ipv6 || defaults.ipv6;
            ipv6DhcpCheck.checked = editing ? source.ipv6Dhcp === true : defaults.ipv6Dhcp;
            ipv6DhcpStart.text = source.ipv6DhcpStart || defaults.ipv6DhcpStart;
            ipv6DhcpEnd.text = source.ipv6DhcpEnd || defaults.ipv6DhcpEnd;
            originalIpv6Gateway = editing ? (source.ipv6Gateway || "") : defaults.ipv6Gateway;

            const domainName = editing ? (source.domainName || "") : "";
            dnsUseName.checked = !editing || domainName.length === 0 || domainName === source.name;
            dnsCustom.checked = !dnsUseName.checked;
            dnsCustomField.text = domainName;
            autostartCheck.checked = editing ? Boolean(source.autostart) : true;

            savedState = currentState();
            open();
            Qt.callLater(function() {
                networkScroll.contentY = 0;
            });
        }

        function save() {
            if (!isValidConfig() || currentState() === savedState) {
                return;
            }
            const mode = currentMode();
            const config = {
                name: nameField.text.trim(),
                mode: mode,
                forwardInterface: (mode === "nat" || mode === "route" || mode === "open")
                    ? targetName(forwardToCombo.currentIndex) : "",
                bridgeName: mode === "bridge" ? bridgeNameField.text.trim() : "",
                domainName: dnsUseName.checked ? nameField.text.trim() : dnsCustomField.text.trim(),
                ipv4Enabled: ipv4Active(),
                ipv4: ipv4NetworkField.text.trim(),
                ipv4Gateway: originalIpv4Gateway,
                ipv4Dhcp: ipv4Check.checked && ipv4DhcpCheck.checked,
                ipv4DhcpStart: ipv4DhcpStart.text.trim(),
                ipv4DhcpEnd: ipv4DhcpEnd.text.trim(),
                ipv6Enabled: ipv6Active(),
                ipv6: ipv6NetworkField.text.trim(),
                ipv6Gateway: originalIpv6Gateway,
                ipv6Dhcp: ipv6Check.checked && ipv6DhcpCheck.checked,
                ipv6DhcpStart: ipv6DhcpStart.text.trim(),
                ipv6DhcpEnd: ipv6DhcpEnd.text.trim(),
                autostart: autostartCheck.checked
            };
            const success = editing ? Karton.updateNetwork(config) : Karton.createNetwork(config);
            if (success) {
                close();
                root.refreshNetworks();
                applicationWindow().showPassiveNotification(editing ? qsTr("network_updated") : qsTr("network_created"));
            }
        }

        contentItem: Item {
            implicitWidth: Kirigami.Units.gridUnit * 36
            implicitHeight: Kirigami.Units.gridUnit * 26

            ColumnLayout {
                anchors.fill: parent
                anchors.margins: Kirigami.Units.largeSpacing
                spacing: 0

                ScrollableSettingsView {
                    id: networkScroll
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    flickableDirection: Flickable.VerticalFlick

                    ColumnLayout {
                        width: parent.width
                        spacing: 12

                GridLayout {
                    columns: 2
                    columnSpacing: 16
                    rowSpacing: 10
                    Layout.fillWidth: true

                    Controls.Label { text: qsTr("name"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.TextField {
                    id: nameField
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    validator: RegularExpressionValidator {
                        regularExpression: /^[A-Za-z0-9._-]+$/
                    }
                }
                Controls.Label { text: qsTr("mode"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.ComboBox {
                    id: modeCombo
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    model: root.networkModeLabels
                }
                Controls.Label {
                    text: qsTr("forward_to")
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                    enabled: !networkEditor.isBridgeMode && modeCombo.currentIndex !== 2
                }
                Controls.ComboBox {
                    id: forwardToCombo
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    model: networkEditor.forwardTargets
                    textRole: "label"
                    enabled: !networkEditor.isBridgeMode && modeCombo.currentIndex !== 2
                }
                Controls.Label {
                    text: qsTr("bridge_name")
                    visible: networkEditor.isBridgeMode
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.TextField {
                    id: bridgeNameField
                    visible: networkEditor.isBridgeMode
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    placeholderText: "br0"
                }
                Controls.Label {
                    text: qsTr("start_on_boot")
                    Layout.fillWidth: true
                    Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter
                }
                Controls.CheckBox {
                    id: autostartCheck
                    Layout.alignment: Qt.AlignRight
                }
            }

            Controls.Label {
                text: qsTr("ipv4_configuration")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 10
                Layout.fillWidth: true

                Controls.Label { text: qsTr("enable_ipv4"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv4Check
                    Layout.alignment: Qt.AlignRight
                    enabled: !networkEditor.isBridgeMode
                }
                Controls.Label { text: qsTr("network"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.TextField {
                    id: ipv4NetworkField
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    placeholderText: "192.168.100.0/24"
                    enabled: networkEditor.ipv4Active()
                }
                Controls.Label { text: qsTr("enable_dhcpv4"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv4DhcpCheck
                    Layout.alignment: Qt.AlignRight
                    enabled: networkEditor.ipv4Active()
                }
                Controls.Label { text: qsTr("dhcpv4_range"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                RowLayout {
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Controls.TextField {
                        id: ipv4DhcpStart
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv4Active() && ipv4DhcpCheck.checked
                        placeholderText: qsTr("start")
                    }
                    Controls.Label { text: "–" }
                    Controls.TextField {
                        id: ipv4DhcpEnd
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv4Active() && ipv4DhcpCheck.checked
                        placeholderText: qsTr("end")
                    }
                }
            }

            Controls.Label {
                text: qsTr("ipv6_configuration")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 10
                Layout.fillWidth: true

                Controls.Label { text: qsTr("enable_ipv6"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv6Check
                    Layout.alignment: Qt.AlignRight
                    enabled: !networkEditor.isBridgeMode
                }
                Controls.Label { text: qsTr("network"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.TextField {
                    id: ipv6NetworkField
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    placeholderText: "fd00::/64"
                    enabled: networkEditor.ipv6Active()
                }
                Controls.Label { text: qsTr("enable_dhcpv6"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                Controls.CheckBox {
                    id: ipv6DhcpCheck
                    Layout.alignment: Qt.AlignRight
                    enabled: networkEditor.ipv6Active()
                }
                Controls.Label { text: qsTr("dhcpv6_range"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignVCenter }
                RowLayout {
                    Layout.preferredWidth: 280
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Controls.TextField {
                        id: ipv6DhcpStart
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv6Active() && ipv6DhcpCheck.checked
                        placeholderText: qsTr("start")
                    }
                    Controls.Label { text: "–" }
                    Controls.TextField {
                        id: ipv6DhcpEnd
                        Layout.fillWidth: true
                        enabled: networkEditor.ipv6Active() && ipv6DhcpCheck.checked
                        placeholderText: qsTr("end")
                    }
                }
            }

            Controls.Label {
                text: qsTr("dns_domain_name")
                font.pixelSize: 15
                font.weight: Font.Bold
                Layout.fillWidth: true
                Layout.topMargin: 4
            }
            Rectangle {
                Layout.fillWidth: true
                height: 1
                color: Qt.rgba(Kirigami.Theme.textColor.r, Kirigami.Theme.textColor.g, Kirigami.Theme.textColor.b, 0.2)
            }
            GridLayout {
                columns: 2
                columnSpacing: 16
                rowSpacing: 8
                Layout.fillWidth: true

                Controls.Label { text: qsTr("domain"); Layout.fillWidth: true; Layout.alignment: Qt.AlignLeft | Qt.AlignTop }
                ColumnLayout {
                    Layout.preferredWidth: 280
                    Layout.maximumWidth: 280
                    Layout.alignment: Qt.AlignRight
                    spacing: 6
                    Controls.RadioButton {
                        id: dnsUseName
                        text: qsTr("use_network_name")
                        checked: true
                        Controls.ButtonGroup.group: dnsDomainGroup
                    }
                    RowLayout {
                        Layout.fillWidth: true
                        spacing: 6
                        Controls.RadioButton {
                            id: dnsCustom
                            text: qsTr("custom")
                            Controls.ButtonGroup.group: dnsDomainGroup
                        }
                        Controls.TextField {
                            id: dnsCustomField
                            Layout.fillWidth: true
                            Layout.preferredWidth: 180
                            enabled: dnsCustom.checked
                            placeholderText: qsTr("example_lan")
                        }
                    }
                }
            }
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                opacity: 0.65
                font.pixelSize: 12
                text: qsTr("changes_to_an_active_network_briefly_restart_it_so_the_new_configuration_takes_effect")
            }
                    }
                }
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: networkEditor.editing ? qsTr("save") : qsTr("create")
                icon.name: "dialog-ok-symbolic"
                enabled: networkEditor.isValidConfig() && networkEditor.currentState() !== networkEditor.savedState
                onTriggered: networkEditor.save()
            },
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: networkEditor.close()
            }
        ]
    }

    FileDialog {
        id: caFileDialog
        title: qsTr("choose_a_ca_certificate")
        nameFilters: [qsTr("certificates_pem_crt_cer"), qsTr("all_files")]
        onAccepted: {
            spiceCaField.text = decodeURIComponent(selectedFile.toString().replace(/^file:\/\//, ""));
            Karton.saveSpiceCaCertificate(spiceCaField.text);
        }
    }

    Connections {
        target: root
        function onCategoryIndexChanged() {
            categoryList.currentIndex = root.categoryIndex;
        }
    }

    Connections {
        target: categoryList
        function onCurrentIndexChanged() {
            root.categoryIndex = categoryList.currentIndex;
        }
    }

    PermissionGrantDialog {
        id: permissionGrantDialog
        onCancelled: {
            root.imagesFolderError = Karton.checkDiskDirectoryAccess(targetPath);
        }
        onConfirmed: {
            const path = targetPath;
            const error = Karton.grantDiskDirectoryAccess(path);
            if (error.length > 0) {
                root.imagesFolderError = error;
                return;
            }
            const check = Karton.checkDiskDirectoryAccess(path);
            root.imagesFolderError = check;
            root.grantedFolders = Karton.grantedDiskDirectories();
            if (check.length === 0) {
                Karton.saveImagesDirectory(path);
                imagesFolderField.text = Karton.imagesDirectory();
            }
        }
    }

    Kirigami.Dialog {
        id: revokeAccessDialog
        title: qsTr("clear_granted_permissions_2")
        modal: true
        padding: Kirigami.Units.largeSpacing
        width: Kirigami.Units.gridUnit * 36

        background: Rectangle {
            color: Qt.rgba(Kirigami.Theme.backgroundColor.r,
                           Kirigami.Theme.backgroundColor.g,
                           Kirigami.Theme.backgroundColor.b, 0.98)
            radius: 8
            border.color: "transparent"
        }

        ColumnLayout {
            Layout.fillWidth: true
            spacing: 12
            Controls.Label {
                Layout.fillWidth: true
                wrapMode: Text.WordWrap
                text: qsTr("karton_will_remove_the_acl_entries_it_added_for_the_libvirt_qemu_user_ownership_and_other")
            }
        }

        customFooterActions: [
            Kirigami.Action {
                text: qsTr("cancel")
                icon.name: "dialog-cancel-symbolic"
                onTriggered: revokeAccessDialog.close()
            },
            Kirigami.Action {
                text: qsTr("clear")
                icon.name: "edit-clear-symbolic"
                onTriggered: {
                    revokeAccessDialog.close();
                    const error = Karton.revokeGrantedDiskAccess();
                    root.grantedFolders = Karton.grantedDiskDirectories();
                    root.imagesFolderError = error;
                }
            }
        ]
    }

    FolderDialog {
        id: imagesFolderDialog
        title: qsTr("choose_vm_image_folder")
        onAccepted: {
            const path = decodeURIComponent(imagesFolderDialog.selectedFolder.toString().replace(/^file:\/\//, ""));
            imagesFolderField.text = path;
            const error = Karton.checkDiskDirectoryAccess(path);
            if (error.length > 0) {
                root.imagesFolderError = "";
                permissionGrantDialog.openFor(path);
                return;
            }
            root.imagesFolderError = "";
            Karton.saveImagesDirectory(path);
            imagesFolderField.text = Karton.imagesDirectory();
        }
    }
}
