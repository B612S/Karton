// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import org.kde.kirigami as Kirigami
import org.kde.karton

// Pick an existing storage volume of a libvirt connection. Used to point a
// disk at a pool volume (local or remote) in the VM Settings/Add Hardware
// dialogs. Inactive pools are started automatically before their volumes are
// listed.
KartonDialog {
    id: root

    property string connectionUri: ""
    property var pools: []
    property var volumes: []
    property bool forIso: false
    property string selectedPath: ""
    property string selectedName: ""
    property string selectedFormat: ""
    property int selectedCapacityGB: 0
    property int selectedAllocationGB: 0
    property bool loading: false

    signal volumePicked(string path)

    title: qsTr("select_storage_volume")
    preferredDialogWidth: Kirigami.Units.gridUnit * 36

    function updateSelection() {
        const index = volumeCombo.currentIndex;
        if (index >= 0 && index < volumes.length) {
            selectedPath = volumes[index].path;
            selectedName = volumes[index].name;
            selectedFormat = volumes[index].format || "";
            selectedCapacityGB = volumes[index].capacityGB || 0;
            selectedAllocationGB = volumes[index].allocationGB || 0;
        } else {
            selectedPath = "";
            selectedName = "";
            selectedFormat = "";
            selectedCapacityGB = 0;
            selectedAllocationGB = 0;
        }
    }

    function loadVolumes() {
        if (loading) {
            return;
        }
        let index = poolCombo.currentIndex;
        if (index < 0 || index >= pools.length) {
            volumes = [];
            updateSelection();
            return;
        }
        loading = true;
        const pool = pools[index];
        if (!pool.active) {
            Karton.startStoragePool(pool.name, connectionUri);
            pools = Karton.storagePools(connectionUri, true);
            index = pools.findIndex(entry => entry.name === pool.name);
            poolCombo.currentIndex = index;
            if (index < 0) {
                volumes = [];
                updateSelection();
                loading = false;
                return;
            }
        }
        const all = Karton.storageVolumes(pools[index].name, connectionUri);
        volumes = forIso ? all.filter(volume => /\.(iso|img)$/i.test(volume.name)) : all;
        volumeCombo.currentIndex = volumes.length > 0 ? 0 : -1;
        updateSelection();
        loading = false;
    }

    function load() {
        loading = true;
        pools = Karton.storagePools(connectionUri, true);
        poolCombo.currentIndex = pools.length > 0 ? 0 : -1;
        loading = false;
        loadVolumes();
    }

    onOpened: load()

    Connections {
        target: Karton
        function onStoragePoolsUpdated(uri) {
            if (root.connectionUri.length === 0 || uri === root.connectionUri) {
                root.pools = Karton.storagePools(root.connectionUri, true);
                if (poolCombo.currentIndex < 0 && root.pools.length > 0) {
                    poolCombo.currentIndex = 0;
                }
                root.loadVolumes();
            }
        }
        function onStorageVolumesUpdated(poolName, uri) {
            if ((root.connectionUri.length === 0 || uri === root.connectionUri)
                && poolCombo.currentIndex >= 0 && poolCombo.currentIndex < root.pools.length
                && root.pools[poolCombo.currentIndex].name === poolName) {
                const all = Karton.storageVolumes(poolName, root.connectionUri);
                root.volumes = root.forIso ? all.filter(volume => /\.(iso|img)$/i.test(volume.name)) : all;
                volumeCombo.currentIndex = root.volumes.length > 0 ? 0 : -1;
                root.updateSelection();
            }
        }
    }

    customFooterActions: [
        Kirigami.Action {
            text: qsTr("cancel")
            icon.name: "dialog-cancel-symbolic"
            onTriggered: root.close()
        },
        Kirigami.Action {
            text: qsTr("select")
            icon.name: "dialog-ok-symbolic"
            enabled: root.selectedPath.length > 0
            onTriggered: {
                root.volumePicked(root.selectedPath);
                root.close();
            }
        }
    ]

    ColumnLayout {
        width: root.availableWidth
        spacing: 16

        Controls.Label {
            text: qsTr("storage_pool")
            Layout.fillWidth: true
        }
        Controls.ComboBox {
            id: poolCombo
            Layout.fillWidth: true
            model: root.pools
            textRole: "name"
            onCurrentIndexChanged: root.loadVolumes()
        }
        Controls.Label {
            text: poolCombo.currentIndex >= 0 && poolCombo.currentIndex < root.pools.length
                  ? root.pools[poolCombo.currentIndex].path
                  : ""
            Layout.fillWidth: true
            opacity: 0.7
            font.pixelSize: 12
            elide: Text.ElideMiddle
        }

        Controls.Label {
            text: qsTr("storage_volume")
            Layout.fillWidth: true
            Layout.topMargin: 8
        }
        Controls.ComboBox {
            id: volumeCombo
            Layout.fillWidth: true
            model: root.volumes
            textRole: "name"
            onCurrentIndexChanged: root.updateSelection()
        }
        Controls.Label {
            text: root.selectedPath
            Layout.fillWidth: true
            wrapMode: Text.WrapAnywhere
            opacity: 0.7
            font.pixelSize: 12
            elide: Text.ElideMiddle
        }
        Controls.Label {
            visible: root.selectedPath.length > 0
            text: qsTr("capacity_allocation").arg(root.selectedCapacityGB).arg(root.selectedAllocationGB)
                  + (root.selectedFormat.length > 0 ? " · " + root.selectedFormat : "")
            Layout.fillWidth: true
            opacity: 0.7
            font.pixelSize: 12
        }
        Controls.Label {
            visible: root.volumes.length === 0
            text: root.forIso ? qsTr("no_matching_storage_volumes") : qsTr("no_storage_volumes")
            Layout.fillWidth: true
            wrapMode: Text.WordWrap
            opacity: 0.7
        }
    }
}
