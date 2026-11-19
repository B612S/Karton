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

    // Fixed above the list so it stays reachable while scrolling. It is hidden
    // when there is nothing to filter, but stays visible when a filter matches
    // nothing so the user can clear it.
    header: Item {
        implicitHeight: searchRow.implicitHeight + Kirigami.Units.smallSpacing * 2
        visible: Karton.connected && (view.count > 0 || VMModel.hasFilter)

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
                // Keep the style's normal item delegates, but do not let the
                // popup grow wider than the control.
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

        while (stack.depth > 1) {
            stack.pop();
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
        if (target < 0 || target === draggedIndex) {
            return;
        }
        VMModel.moveItem(draggedIndex, target);
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

    Connections {
        target: VMModel
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
        }
    }

    actions: [
        Kirigami.Action {
            icon.name: "list-add-symbolic"
            text: qsTr("create")
            onTriggered: source => {
                addDomainDialog.open();
            }
        },
        Kirigami.Action {
            icon.name: "document-open-symbolic"
            text: qsTr("import")
            Kirigami.Action {
                text: qsTr("import_domain_xml")
                onTriggered: importDomainDialog.open()
            }
            Kirigami.Action {
                text: qsTr("import_ovf_ova")
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

    OvfImportDialog {
        id: ovfImportDialog
    }

    FileDialog {
        id: importDomainDialog
        title: qsTr("import_vm")
        fileMode: FileDialog.OpenFile
        nameFilters: [qsTr("libvirt_domain_xml"), qsTr("all_files")]
        onAccepted: Karton.importDomainXml(root.localPath(selectedFile))
    }

    ListView {
        id: view
        model: VMModel

        currentIndex: 0

        // Selection is updated explicitly while reordering, so the view must
        // not scroll itself to the row that is being moved.
        highlightFollowsCurrentItem: false
        interactive: !root.isDragging

        // When the VM list is empty, make the placeholder welcome text the full page.
        onCountChanged: {
            if (count === 0 && !VMModel.hasFilter) {
                while (applicationWindow().pageStack.depth > 1) {
                    applicationWindow().pageStack.pop();
                }
            }
        }

        // Scrolling while a drag is active (wheel, trackpad, or auto-scroll)
        // changes the row under the pointer without a mouse move.
        onContentYChanged: {
            if (root.isDragging && root.dragGlobalY >= 0) {
                root.updateDrag(root.dragGlobalY);
            }
        }

        delegate: Controls.ItemDelegate {
            width: ListView.view.width
            highlighted: ListView.isCurrentItem || root.draggedIndex === index

            leftPadding: Kirigami.Units.gridUnit
            rightPadding: leftPadding
            topPadding: Kirigami.Units.largeSpacing
            bottomPadding: topPadding

            readonly property bool isBeingDragged: root.draggedIndex === index

            ListView.onIsCurrentItemChanged: {
                if (ListView.isCurrentItem && !root.isDragging && !root.autoOpenSuspended) {
                    root.selectedDomain = domain;
                    if (applicationWindow().isWidescreen) {
                        // Defer the push until the delegate has been placed in
                        // the scene, otherwise the page cannot be shown yet.
                        Qt.callLater(() => root.openDomain(domain));
                    }
                }
            }

            // A drag cannot survive the delegate being unloaded: stop it
            // instead of leaving the list non-interactive behind.
            Component.onDestruction: {
                if (isBeingDragged && root.isDragging) {
                    root.endDrag();
                }
            }

            contentItem: RowLayout {
                id: delegateLayout
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
                        text: domain.config.name
                        elide: Text.ElideRight
                    }
                    Controls.Label {
                        Layout.fillWidth: true
                        wrapMode: Text.WordWrap
                        text: Karton.domainOsName(domain).length > 0
                            ? domain.config.state + " | " + Karton.domainOsName(domain)
                            : domain.config.state
                    }
                }
            }

            // The ItemDelegate's own mouse handling is bypassed on purpose:
            // this area reports the pointer and lets the page move the row,
            // so the delegate itself never gets detached from the pointer.
            MouseArea {
                anchors.fill: parent
                acceptedButtons: Qt.LeftButton
                // Once reordering starts, no other item (the list's flickable)
                // may take the grab away: the drag must survive the pointer
                // leaving the row, the list or the window.
                preventStealing: root.isDragging

                property real pressX: 0
                property real pressY: 0
                property real lastSceneY: -1

                onClicked: {
                    if (root.dragOccurred) {
                        return;
                    }
                    view.currentIndex = index;
                    root.openDomain(domain);
                }

                onDoubleClicked: {
                    if (root.dragOccurred) {
                        return;
                    }
                    root.openViewerForDomain(domain);
                }

                onPressed: (mouse) => {
                    root.dragOccurred = false;
                    pressX = mouse.x;
                    pressY = mouse.y;
                    lastSceneY = mapToItem(null, mouse.x, mouse.y).y;
                    if (mouse.button === Qt.LeftButton) {
                        root.holdForDrag(index, lastSceneY);
                    }
                }

                onPositionChanged: (mouse) => {
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

        Kirigami.PlaceholderMessage {
            anchors.centerIn: parent
            width: parent.width - (Kirigami.Units.largeSpacing * 4)

            visible: view.count === 0

            text: VMModel.hasFilter
                ? qsTr("no_matching_vms")
                : (Karton.connected ? qsTr("welcome_to_karton") : qsTr("cannot_connect_to_libvirt"))
            explanation: VMModel.hasFilter
                ? qsTr("try_a_different_search_or_filter")
                : (Karton.connected ? qsTr("connected_to_1_create_a_new_virtual_machine_to_proceed").arg(Karton.connectionUri) : qsTr("check_that_libvirt_is_running_and_that_the_1_connection_is_available").arg(Karton.connectionUri))
            icon.name: VMModel.hasFilter ? "edit-none-symbolic" : (Karton.connected ? "computer-symbolic" : "error-symbolic")
        }
    }
}
