// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Layouts
import QtQuick.Controls as Controls
import QtQuick.Dialogs
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.ScrollablePage {
    id: root

    title: qsTr("virtual_machines")
    topPadding: 0
    bottomPadding: 0
    leftPadding: 0
    rightPadding: 0

    // Fixed above the list so it stays reachable while scrolling.
    header: Item {
        implicitHeight: searchRow.implicitHeight + Kirigami.Units.smallSpacing * 2
        // Keep the filter reachable when nothing matches, so the user can
        // clear it instead of being stuck on an empty page.
        visible: view.count > 0 || VMModel.hasFilter

        RowLayout {
            id: searchRow
            anchors.fill: parent
            anchors.leftMargin: Kirigami.Units.largeSpacing
            anchors.rightMargin: Kirigami.Units.largeSpacing
            anchors.topMargin: Kirigami.Units.smallSpacing
            anchors.bottomMargin: Kirigami.Units.smallSpacing
            spacing: Kirigami.Units.smallSpacing

            Kirigami.SearchField {
                id: searchField
                Layout.fillWidth: true
                placeholderText: qsTr("search_virtual_machines")
                onTextChanged: VMModel.filterText = text
            }

            Controls.ComboBox {
                id: stateFilter
                Layout.preferredWidth: Kirigami.Units.gridUnit * 6
                model: [qsTr("all"), qsTr("running"), qsTr("stopped")]
                onActivated: index => VMModel.filterMode = index
                popup.width: stateFilter.width
                Controls.ToolTip.visible: hovered
                Controls.ToolTip.text: qsTr("filter_by_state")
            }
        }
    }

    function localPath(url) {
        let path = url.toString();
        if (path.startsWith("file://")) {
            path = path.substring(7);
        }
        return decodeURIComponent(path);
    }

    function openViewer(domain) {
        if (!domain) {
            return;
        }
        const existing = Karton.viewerForDomain(domain);
        if (existing) {
            if (existing.previewMode) {
                existing.openAsViewer();
            } else {
                existing.showViewer();
            }
            return;
        }
        if (!Karton.reserveViewer(domain)) {
            return;
        }
        // Create the window from the main window's context: viewer windows
        // outlive the pages that open them and a page-owned context becomes
        // invalid when switching to another VM.
        const window = applicationWindow().createViewerWindow(domain, false);
        if (!window) {
            Karton.releaseViewerReservation(domain);
        }
    }

    function openViewerForDomain(domain) {
        openViewer(domain);
    }

    // The wide layout always keeps a page in the right column, so the list
    // keeps its own column while Karton connects and loads the VMs instead of
    // filling the window and jumping into a two-column layout afterwards.
    // The desktop layout always keeps the list column; the details page lives
    // in the right column, so opening a VM never replaces the list.
    readonly property bool splitView: true

    function pushEmptyDetailsPage() {
        const stack = applicationWindow().pageStack;
        if (!splitView || !stack || stack.depth !== 1) {
            return;
        }
        const component = Qt.createComponent("EmptyDetailsPage.qml");
        if (component.status !== Component.Ready) {
            console.error("Failed to create empty details page:", component.errorString());
            return;
        }
        const page = component.createObject(null);
        if (page) {
            stack.push(page);
        }
    }

    function resetDetailColumn() {
        const stack = applicationWindow().pageStack;
        if (!stack) {
            return;
        }
        while (stack.depth > 1) {
            stack.pop();
        }
        pushEmptyDetailsPage();
    }

    function ensureDetailColumn() {
        const stack = applicationWindow().pageStack;
        if (splitView && stack && stack.depth === 1) {
            pushEmptyDetailsPage();
        }
    }

    Component.onCompleted: root.ensureDetailColumn()

    // Open the first VM once after the initial connection load, so the wide
    // layout starts with useful details on the right. It is not repeated after
    // the user navigates back to the list.
    property bool initialAutoOpenDone: false

    function autoOpenInitialDomain() {
        if (initialAutoOpenDone || !splitView) {
            return;
        }
        const stack = applicationWindow().pageStack;
        if (stack.lastItem && stack.lastItem.domain !== undefined && stack.lastItem.domain) {
            initialAutoOpenDone = true;
            return;
        }
        const row = VMModel.firstDomainRow();
        if (row < 0) {
            return;
        }
        const domain = VMModel.domainAtRow(row);
        if (!domain) {
            return;
        }
        initialAutoOpenDone = true;
        view.currentIndex = row;
        root.openDomain(domain);
    }

    Connections {
        target: VMModel
        function onModelReset() {
            Qt.callLater(root.autoOpenInitialDomain);
        }
        function onFilterChanged() {
            root.cancelDragHold();
            if (root.isDragging) {
                root.endDrag();
            }
            // Suppress the detail-page auto-open caused by rebuilding the
            // list; a real selection after the filter settles still opens it.
            root.autoOpenSuspended = true;
            filterSettleTimer.restart();
        }
    }

    function openDomain(domain) {
        if (domain) {
            selectedDomain = domain;
        }
        const stack = applicationWindow().pageStack;
        // In Kirigami's fixed-column layout currentItem can still point at the
        // first column, while the detail page is the last one. Using
        // replace() there would replace the VM list itself.
        if (stack.lastItem && stack.lastItem.domain === domain) {
            return;
        }

        const component = Qt.createComponent("VMPage.qml");
        if (component.status !== Component.Ready) {
            console.error("Failed to create VM page:", component.errorString());
            return;
        }
        // Create the page before pushing it. Passing a URL or a Component to
        // pageStack.push() creates the object with a non-item JS parent,
        // which makes Qt warn "Created graphical object was not placed in the
        // graphics scene". A parentless object is not warned about, and the
        // page stack takes ownership when the page is inserted.
        const page = component.createObject(null, {
            domain: domain
        });
        if (!page) {
            console.error("Failed to create VM page");
            return;
        }
        // The wide layout keeps the empty details page in the right column;
        // replacing it avoids a pop/push flash and keeps the list column in place.
        // PageRow.replace() removes every page above the *current* one and then
        // replaces the new topmost page. Clicking a row in the list column (for
        // example the header that expands a collapsed connection after a
        // restart) makes that column current, so replace() would pop the empty
        // details page and then replace the VM list itself, leaving the window
        // with the VM page only. Point the page row at the details column first.
        if (stack.depth > 1 && stack.lastItem && stack.lastItem.isEmptyDetailsPage === true) {
            stack.currentIndex = stack.depth - 1;
            stack.replace(page);
            return;
        }
        while (stack.depth > 1) {
            stack.pop();
        }
        stack.push(page);
    }

    // Reordering uses a long press, like Luma's playlist: hold a row for a
    // second and then move the pointer; the row under the pointer is swapped
    // through the model while the delegate keeps the mouse grab.
    property bool isDragging: false
    property bool dragOccurred: false
    // The domain whose details are shown in the fixed-column layout. Reordering
    // must move the selection along with it instead of selecting whichever VM
    // now happens to occupy the old row.
    property var selectedDomain: null
    property int draggedIndex: -1
    property real dragGlobalY: -1
    property int dragScrollDirection: 0
    readonly property real dragEdgeHeight: 40
    // A filter change resets the model and re-emits the current-item change;
    // that must not open the first matching VM's details page.
    property bool autoOpenSuspended: false

    // A single hold timer for the whole list instead of one per delegate.
    property int dragHoldRow: -1
    property real dragHoldSceneY: -1

    function holdForDrag(index, sceneY) {
        // Source and view rows differ while a filter is active, so reordering
        // through the filtered list would move the wrong domains.
        if (VMModel.hasFilter) {
            return;
        }
        dragHoldRow = index;
        dragHoldSceneY = sceneY;
        dragHoldTimer.start();
    }

    function cancelDragHold() {
        dragHoldTimer.stop();
        dragHoldRow = -1;
        dragHoldSceneY = -1;
    }

    function startHeldDrag() {
        if (dragHoldRow >= 0 && dragHoldRow < view.count) {
            beginDrag(dragHoldRow, dragHoldSceneY);
        }
        dragHoldRow = -1;
        dragHoldSceneY = -1;
    }

    function beginDrag(index, sceneY) {
        if (VMModel.hasFilter) {
            return;
        }
        if (!view || index < 0 || index >= view.count) {
            return;
        }
        // The selection follows selectedDomain; only the dragged row is
        // highlighted explicitly. The flag must be set first, because model
        // moves below emit onIsCurrentItemChanged, which must not open a page
        // mid-drag.
        isDragging = true;
        dragOccurred = true;
        draggedIndex = index;
        dragGlobalY = sceneY;
        updateDragScrollDirection(sceneY);
    }

    // Latch the scroll direction so edge auto-scrolling keeps going even when
    // the pointer leaves the list and no more move events arrive.
    function updateDragScrollDirection(sceneY) {
        if (!view || !isDragging || sceneY < 0) {
            dragScrollDirection = 0;
            return;
        }
        const localY = view.mapFromItem(null, 0, sceneY).y;
        if (localY < dragEdgeHeight) {
            dragScrollDirection = -1;
        } else if (localY > view.height - dragEdgeHeight) {
            dragScrollDirection = 1;
        } else {
            dragScrollDirection = 0;
        }
    }

    function viewIndexAtSceneY(sceneY) {
        if (!view || view.count === 0) {
            return -1;
        }
        const contentY = view.mapFromItem(null, 0, sceneY).y + view.contentY;
        // Clamp the pointer to the laid-out content. When it leaves the list,
        // the dragged row must stay on the first/last visible row instead of
        // being dropped by the view.
        const visibleTop = view.contentY;
        const visibleBottom = Math.max(visibleTop, Math.min(view.contentY + view.height, view.contentHeight) - 1);
        const y = Math.max(visibleTop, Math.min(contentY, visibleBottom));
        return Math.max(0, Math.min(view.count - 1, view.indexAt(0, y)));
    }

    function updateDrag(sceneY) {
        if (!isDragging || sceneY < 0) {
            return;
        }
        dragGlobalY = sceneY;
        updateDragScrollDirection(sceneY);
        const target = viewIndexAtSceneY(sceneY);
        // Connection headers are not draggable targets.
        if (target < 0 || target === draggedIndex || !VMModel.domainAtRow(target)) {
            return;
        }
        // Rows from different connection groups must not be reordered. When
        // the move is rejected, keep dragging the original row instead of
        // adopting a row from the other group.
        if (!VMModel.moveItem(draggedIndex, target)) {
            return;
        }
        // The move may end the drag synchronously if the grab is dropped.
        if (!isDragging) {
            return;
        }
        draggedIndex = target;
        // Keep the selected VM selected (its row moved, not the selection) so
        // the highlight still matches the details page.
        if (selectedDomain) {
            const selectedRow = VMModel.rowOfUuid(selectedDomain.config.uuid);
            if (selectedRow >= 0) {
                view.currentIndex = selectedRow;
            }
        }
        // ListView can defer the relayout of moved rows while the pointer keeps
        // moving, which leaves the row under the pointer out of sync.
        view.forceLayout();
        if (!isDragging) {
            draggedIndex = -1;
        }
    }

    function endDrag() {
        isDragging = false;
        draggedIndex = -1;
        dragGlobalY = -1;
        dragScrollDirection = 0;
    }

    Timer {
        id: dragHoldTimer
        interval: 1000
        repeat: false
        onTriggered: root.startHeldDrag()
    }

    Timer {
        id: dragScrollTimer
        interval: 16
        repeat: true
        running: root.isDragging
        onTriggered: {
            if (!root.isDragging || root.dragScrollDirection === 0) {
                return;
            }
            const maxY = Math.max(0, view.contentHeight - view.height);
            view.contentY = Math.max(0, Math.min(maxY, view.contentY + root.dragScrollDirection * 18));
            root.updateDrag(root.dragGlobalY);
        }
    }

    Component.onDestruction: cancelDragHold()

    Timer {
        id: filterSettleTimer
        interval: 120
        repeat: false
        onTriggered: root.autoOpenSuspended = false
    }

    Connections {
        target: Karton
        function onErrorOccurred(errorMessage) {
            applicationWindow().showPassiveNotification(errorMessage, "long");
        }
        function onDomainImported(success, name) {
            if (success) {
                applicationWindow().showPassiveNotification(qsTr("imported_vm_1").arg(name));
            }
        }
        function onDomainRemoved(removedDomain) {
            // Do not keep a pointer to a Domain that is about to be destroyed.
            if (root.selectedDomain === removedDomain) {
                root.selectedDomain = null;
            }
            const stack = applicationWindow().pageStack;
            if (stack.lastItem && stack.lastItem.domain === removedDomain) {
                stack.pop();
                root.ensureDetailColumn();
            }
        }
        function onClosePopupsRequested() {
            connectionMenu.close();
            vmMenu.close();
        }
    }

    actions: [
        Kirigami.Action {
            icon.name: "list-add-symbolic"
            text: qsTr("create")
            // Keep the primary action on the toolbar at every width instead of
            // hiding it in the overflow menu.
            displayHint: Kirigami.DisplayHint.KeepVisible
            onTriggered: source => {
                addDomainDialog.open();
            }
        },
        Kirigami.Action {
            icon.name: "document-open-symbolic"
            text: qsTr("import")
            Kirigami.Action {
                text: qsTr("domain_xml")
                onTriggered: importDomainDialog.openDialog()
            }
            Kirigami.Action {
                text: qsTr("ovf_ova")
                onTriggered: ovfImportDialog.open()
            }
        },
        Kirigami.Action {
            icon.name: "preferences-system-symbolic"
            text: qsTr("settings")
            onTriggered: applicationSettingsDialog.open()
        }
    ]

    InstallationDialog {
        id: addDomainDialog
    }

    ApplicationSettingsDialog {
        id: applicationSettingsDialog
    }

    ConnectionSettingsDialog {
        id: connectionSettingsDialog
    }

    OvfImportDialog {
        id: ovfImportDialog
    }

    ImportDomainDialog {
        id: importDomainDialog
    }

    Controls.Menu {
        id: connectionMenu
        property string connectionUri: ""
        property bool connectionConnected: false
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: qsTr("reconnect")
            icon.name: "view-refresh-symbolic"
            onTriggered: Karton.reconnectConnection(connectionMenu.connectionUri)
        }
        Controls.MenuItem {
            text: qsTr("disconnect")
            icon.name: "network-disconnect-symbolic"
            visible: connectionMenu.connectionConnected
            onTriggered: Karton.disconnectConnection(connectionMenu.connectionUri)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: qsTr("settings")
            icon.name: "preferences-system-symbolic"
            onTriggered: connectionSettingsDialog.openFor(connectionMenu.connectionUri)
        }
    }

    Controls.Menu {
        id: vmMenu
        property Domain domain: null
        readonly property bool running: domain ? domain.isActive : false
        readonly property bool paused: domain ? domain.isPaused : false
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

        Controls.MenuItem {
            text: qsTr("open_viewer")
            icon.name: "computer-symbolic"
            onTriggered: root.openViewerForDomain(vmMenu.domain)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: vmMenu.running ? qsTr("shut_down") : qsTr("start")
            icon.name: vmMenu.running ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
            onTriggered: {
                if (vmMenu.running) {
                    Karton.stopDomain(vmMenu.domain);
                } else {
                    Karton.startDomain(vmMenu.domain);
                }
            }
        }
        Controls.MenuItem {
            text: qsTr("power_off")
            icon.name: "process-stop-symbolic"
            visible: vmMenu.running
            onTriggered: Karton.forceStopDomain(vmMenu.domain)
        }
        Controls.MenuItem {
            text: vmMenu.paused ? qsTr("resume") : qsTr("pause")
            icon.name: vmMenu.paused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
            visible: vmMenu.running
            onTriggered: {
                if (vmMenu.paused) {
                    Karton.resumeDomain(vmMenu.domain);
                } else {
                    Karton.pauseDomain(vmMenu.domain);
                }
            }
        }
        Controls.MenuItem {
            text: qsTr("reboot")
            icon.name: "system-reboot-symbolic"
            visible: vmMenu.running
            onTriggered: Karton.rebootDomain(vmMenu.domain)
        }
        Controls.MenuItem {
            text: qsTr("reset")
            icon.name: "system-reboot-symbolic"
            visible: vmMenu.running
            onTriggered: Karton.resetDomain(vmMenu.domain)
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: qsTr("snapshots")
            icon.name: "camera-photo-symbolic"
            onTriggered: contextSnapshotsDialog.open()
        }
        Controls.MenuItem {
            text: qsTr("clone")
            icon.name: "edit-copy-symbolic"
            enabled: !Karton.connectionIsRemote(Karton.domainConnectionUri(vmMenu.domain))
            onTriggered: contextCloneDialog.open()
        }
        Controls.MenuSeparator {}
        Controls.MenuItem {
            text: qsTr("settings")
            icon.name: "preferences-system-symbolic"
            onTriggered: contextSettingsDialog.open()
        }
        Controls.MenuItem {
            text: qsTr("delete")
            icon.name: "delete-symbolic"
            visible: !vmMenu.running
            onTriggered: {
                contextDeleteDialog.domain = vmMenu.domain;
                contextDeleteDialog.open();
            }
        }
    }

    SettingsDialog {
        id: contextSettingsDialog
        domain: vmMenu.domain
        parentWindow: applicationWindow()
    }

    SnapshotsDialog {
        id: contextSnapshotsDialog
        domain: vmMenu.domain
    }

    CloneDialog {
        id: contextCloneDialog
        domain: vmMenu.domain
    }

    DeleteDomainDialog {
        id: contextDeleteDialog
    }

    ListView {
        id: view
        model: VMModel

        currentIndex: 0
        highlightFollowsCurrentItem: false
        interactive: !root.isDragging

        // Scrolling while a drag is active (wheel, trackpad, or auto-scroll)
        // changes the row under the pointer without a mouse move.
        onContentYChanged: {
            if (root.isDragging && root.dragGlobalY >= 0) {
                root.updateDrag(root.dragGlobalY);
            }
        }

        delegate: Controls.ItemDelegate {
            id: rowDelegate

            width: ListView.view.width
            readonly property bool isHeader: rowType === VMModel.ConnectionRow
            highlighted: !isHeader && (ListView.isCurrentItem || root.draggedIndex === index)
            leftPadding: isHeader ? Kirigami.Units.smallSpacing : Kirigami.Units.gridUnit
            rightPadding: leftPadding
            topPadding: isHeader ? Kirigami.Units.smallSpacing : Kirigami.Units.largeSpacing
            bottomPadding: topPadding

            readonly property bool isBeingDragged: !isHeader && root.draggedIndex === index

            // A drag cannot survive the delegate being unloaded: stop it
            // instead of leaving the list non-interactive behind.
            Component.onDestruction: {
                if (isBeingDragged && root.isDragging) {
                    root.endDrag();
                }
            }

            contentItem: Item {
                width: rowDelegate.availableWidth
                implicitHeight: rowDelegate.isHeader ? connectionLayout.implicitHeight : vmLayout.implicitHeight

                RowLayout {
                    id: connectionLayout
                    anchors.fill: parent
                    visible: rowDelegate.isHeader
                    spacing: Kirigami.Units.smallSpacing

                    Kirigami.Icon {
                        source: collapsed ? "arrow-right-symbolic" : "arrow-down-symbolic"
                        Layout.preferredWidth: Kirigami.Units.iconSizes.small
                        Layout.preferredHeight: Kirigami.Units.iconSizes.small
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        Layout.alignment: Qt.AlignVCenter
                        text: connectionUri
                        font.weight: Font.DemiBold
                        elide: Text.ElideRight
                    }
                    Controls.BusyIndicator {
                        running: connectionStatus === 0 || connectionStatus === 2
                        visible: running
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter
                    }
                    Kirigami.Icon {
                        // Only the error state needs a glyph; a connected group
                        // just shows its VM count.
                        visible: connectionStatus === 3
                        source: "data-error-symbolic"
                        Layout.preferredWidth: 16
                        Layout.preferredHeight: 16
                        Layout.alignment: Qt.AlignVCenter
                    }
                    // A connected header only needs the VM count; the
                    // connecting/reconnecting/error text is shown as-is.
                    Controls.Label {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.rightMargin: Kirigami.Units.smallSpacing * 2
                        visible: connectionStatus !== 1
                        text: connectionStatusText
                        opacity: 0.75
                        font.pixelSize: 11
                    }
                    Controls.Label {
                        Layout.alignment: Qt.AlignVCenter
                        Layout.rightMargin: Kirigami.Units.smallSpacing * 2
                        visible: connectionStatus === 1
                        text: qsTr("vm_count").arg(connectionDomainCount)
                        opacity: 0.75
                        font.pixelSize: 11
                    }
                }

                RowLayout {
                    id: vmLayout
                    anchors.fill: parent
                    visible: !rowDelegate.isHeader
                    spacing: Kirigami.Units.largeSpacing

                    Kirigami.Icon {
                        source: {
                            const icon = domain ? Karton.domainOsIcon(domain) : "karton-os-generic";
                            return Karton.holidayMode && icon === "karton-os-linux" ? "karton-os-linux-newyear" : icon;
                        }
                        Layout.fillHeight: true
                        Layout.maximumHeight: Kirigami.Units.iconSizes.huge
                        Layout.preferredWidth: height
                    }
                    ColumnLayout {
                        Kirigami.Heading {
                            Layout.fillWidth: true
                            font.weight: Font.DemiBold
                            level: 3
                            text: domain ? domain.config.name : ""
                            elide: Text.ElideRight
                        }
                        Controls.Label {
                            Layout.fillWidth: true
                            wrapMode: Text.WordWrap
                            text: {
                                if (!domain) {
                                    return "";
                                }
                                return Karton.domainOsName(domain).length > 0
                                    ? domain.config.state + " | " + Karton.domainOsName(domain)
                                    : domain.config.state;
                            }
                        }
                    }
                }
            }

            // The ItemDelegate's own mouse handling is bypassed on purpose:
            // this area reports the pointer and lets the page move the row,
            // so the delegate itself never gets detached from the pointer.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton | Qt.RightButton
                // Once reordering starts, no other item (the list's flickable)
                // may take the grab away: the drag must survive the pointer
                // leaving the row, the list or the window.
                preventStealing: root.isDragging

                property real pressX: 0
                property real pressY: 0
                property real lastSceneY: -1

                onClicked: (mouse) => {
                    // Right-click already opened the context menu; it must not
                    // also toggle a connection group or open a VM.
                    if (mouse.button !== Qt.LeftButton) {
                        return;
                    }
                    if (rowDelegate.isHeader) {
                        VMModel.toggleConnection(connectionUri);
                        return;
                    }
                    if (root.dragOccurred) {
                        return;
                    }
                    view.currentIndex = index;
                    root.openDomain(domain);
                }

                onDoubleClicked: (mouse) => {
                    if (rowDelegate.isHeader || root.dragOccurred) {
                        return;
                    }
                    root.openViewerForDomain(domain);
                }

                onPressed: (mouse) => {
                    if (mouse.button === Qt.RightButton) {
                        if (rowDelegate.isHeader) {
                            connectionMenu.connectionUri = connectionUri;
                            connectionMenu.connectionConnected = connectionStatus === 1;
                            connectionMenu.popup();
                        } else if (domain) {
                            vmMenu.domain = domain;
                            vmMenu.popup();
                        }
                        return;
                    }
                    root.dragOccurred = false;
                    if (rowDelegate.isHeader) {
                        return;
                    }
                    pressX = mouse.x;
                    pressY = mouse.y;
                    lastSceneY = mapToItem(null, mouse.x, mouse.y).y;
                    root.holdForDrag(index, lastSceneY);
                }

                onPositionChanged: (mouse) => {
                    if (rowDelegate.isHeader) {
                        return;
                    }
                    lastSceneY = mapToItem(null, mouse.x, mouse.y).y;
                    if (!root.isDragging) {
                        if (Math.abs(mouse.x - pressX) > 5 || Math.abs(mouse.y - pressY) > 5) {
                            root.cancelDragHold();
                        }
                        return;
                    }
                    root.updateDrag(lastSceneY);
                }

                onReleased: {
                    root.cancelDragHold();
                    if (root.isDragging) {
                        root.endDrag();
                    }
                }

                onCanceled: {
                    root.cancelDragHold();
                    if (root.isDragging) {
                        root.endDrag();
                    }
                }
            }
        }
    }
}
