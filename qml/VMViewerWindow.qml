// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import QtQuick.Controls as Controls
import QtQuick.Layouts
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.ApplicationWindow {
    id: viewerWindow
    required property Domain domain

    title: domain ? i18nc("%1 is the name of the virtual machine", "VM Viewer - %1", domain.config.name) : i18n("VM Viewer")

    readonly property var screenInfo: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null

    // Open large enough for a modern guest resolution instead of a small
    // fixed window; Actual Size and Fit Guest to Window remain available.
    width: screenInfo ? Math.min(screenInfo.width, Math.max(Kirigami.Units.gridUnit * 60, Math.min(screenInfo.width * 0.85, Kirigami.Units.gridUnit * 120))) : Kirigami.Units.gridUnit * 70
    height: screenInfo ? Math.min(screenInfo.height, Math.max(Kirigami.Units.gridUnit * 40, Math.min(screenInfo.height * 0.85, Kirigami.Units.gridUnit * 72))) : Kirigami.Units.gridUnit * 45

    property bool viewerRegistered: false
    readonly property bool isRunning: domain ? domain.isActive : false
    property bool wasRunning: false
    property bool sizeRestored: false
    property alias domainViewerItem: domainViewer
    property int fixedContentWidth: 0
    property int fixedContentHeight: 0
    // QWindow's default maximum; used to make the window resizable again when
    // a fixed resolution is switched back to Auto or when going fullscreen.
    readonly property int unlimitedWindowSize: 16777215
    readonly property bool isFullScreen: visibility === Window.FullScreen
    // Floating fullscreen bar. It is only visible while
    // `fullScreenToolbarVisible` is true, so it can never swallow clicks
    // while invisible.
    readonly property int compactToolbarHeight: 52
    property bool fullScreenToolbarVisible: true
    // The desktop style's ToolButton.hovered is not refreshed when the bar
    // appears under a stationary pointer, so track the hovered button from the
    // page's HoverHandler instead.
    property string hoveredToolbarButton: ""
    property bool toolbarMoved: false

    function clampToolbar() {
        if (!toolbarMoved || !floatingToolbar) {
            return;
        }
        floatingToolbar.x = Math.max(0, Math.min(floatingToolbar.x, viewerPage.width - floatingToolbar.width));
        floatingToolbar.y = 0;
    }

    function toolbarButtonAt(pageX, pageY) {
        if (!floatingToolbar || !floatingToolbar.visible) {
            return "";
        }
        const p = floatingToolbar.mapFromItem(viewerPage, pageX, pageY);
        if (!floatingToolbar.contains(p)) {
            return "";
        }
        const frames = [
            ["power", powerFrame],
            ["pause", pauseFrame],
            ["reboot", rebootFrame],
            ["fullscreen", fullscreenFrame],
            ["more", moreFrame]
        ];
        for (let i = 0; i < frames.length; ++i) {
            const name = frames[i][0];
            const frame = frames[i][1];
            if (!frame || !frame.visible) {
                continue;
            }
            const local = frame.mapFromItem(floatingToolbar, p.x, p.y);
            if (local.x >= 0 && local.x < frame.width && local.y >= 0 && local.y < frame.height) {
                return name;
            }
        }
        return "";
    }

    Timer {
        id: toolbarHideTimer
        interval: 1200
        onTriggered: {
            // A drag started on the bar keeps the pointer grab even after it
            // leaves the bar, so the bar must not fade out from under it.
            if (viewerWindow.isFullScreen && !toolbarBarHover.hovered && !toolbarDrag.active
                    && !powerMenu.opened && !rebootMenu.opened && !viewerMenu.opened) {
                viewerWindow.fullScreenToolbarVisible = false;
                viewerWindow.hoveredToolbarButton = "";
            }
        }
    }

    onIsFullScreenChanged: {
        if (isFullScreen) {
            // Fixed-size limits must not constrain or stretch a fullscreen
            // window; the guest follows the whole screen there.
            resetWindowSizeLimits();
            fullScreenToolbarVisible = true;
            toolbarHideTimer.restart();
        } else {
            fullScreenToolbarVisible = true;
            toolbarHideTimer.stop();
            // Restore the configured windowed size (or fit the guest) once the
            // normal window geometry and the toolbar are back.
            Qt.callLater(applyDisplayMode);
        }
    }

    function toggleFullScreen() {
        if (visibility === Window.FullScreen) {
            showNormal();
            return;
        }
        // Release fixed limits before asking the compositor for fullscreen;
        // otherwise a FullHD window can enter fullscreen stretched.
        resetWindowSizeLimits();
        showFullScreen();
        // Start the auto-hide timeout explicitly; visibility changes can be
        // delivered before isFullScreen updates, which used to leave the
        // toolbar without a running hide timer until it was hovered.
        Qt.callLater(function() {
            fullScreenToolbarVisible = true;
            toolbarHideTimer.restart();
        });
    }

    function registerViewer() {
        if (!viewerRegistered) {
            viewerRegistered = true;
            Karton.viewerOpened();
            Karton.registerViewer(viewerWindow.domain, viewerWindow);
        }
    }

    // The window chrome above the guest is the Kirigami global toolbar. Using
    // the aspect-fitted DomainViewer height here used to compute the wrong
    // height (and even a negative one) whenever the guest aspect differed from
    // the saved window size.
    function chromeHeight() {
        if (viewerWindow.isFullScreen) {
            return 0;
        }
        const toolbar = pageStack.globalToolBar;
        if (toolbar && toolbar.height > 0) {
            return Math.round(toolbar.height);
        }
        const difference = viewerWindow.height - viewerPage.height;
        return difference > 0 ? Math.round(difference) : 48;
    }

    function resetWindowSizeLimits() {
        viewerWindow.minimumWidth = 0;
        viewerWindow.maximumWidth = viewerWindow.unlimitedWindowSize;
        viewerWindow.minimumHeight = 0;
        viewerWindow.maximumHeight = viewerWindow.unlimitedWindowSize;
    }

    function fitWindowToGuest() {
        if (viewerWindow.isFullScreen || fixedContentWidth > 0 || domainViewer.agentConnected || domainViewer.implicitWidth <= 0 || domainViewer.implicitHeight <= 0 || !viewerWindow.visible) {
            return;
        }
        const aspect = domainViewer.implicitWidth / domainViewer.implicitHeight;
        const chrome = chromeHeight();
        const maxContentHeight = Screen.height * 0.85 - chrome;
        let contentWidth = viewerWindow.width;
        let contentHeight = contentWidth / aspect;
        if (contentHeight > maxContentHeight) {
            contentHeight = maxContentHeight;
            contentWidth = contentHeight * aspect;
        }
        viewerWindow.width = Math.round(contentWidth);
        viewerWindow.height = Math.round(contentHeight + chrome);
    }

    // The configured guest resolution always wins over whatever size was last
    // written to karton.conf. This is what guarantees a fixed-resolution VM
    // (for example FullHD) reopens at that size even after a crash while the
    // viewer was fullscreen or had an unrelated geometry.
    function applyFixedWindowSize() {
        if (fixedContentWidth <= 0 || fixedContentHeight <= 0 || viewerWindow.isFullScreen) {
            return;
        }
        const scale = Math.min(1.0, Screen.width * 0.9 / fixedContentWidth, Screen.height * 0.85 / fixedContentHeight);
        const contentWidth = Math.round(fixedContentWidth * scale);
        const contentHeight = Math.round(fixedContentHeight * scale);
        const chrome = chromeHeight();
        const windowWidth = Math.max(320, contentWidth);
        const windowHeight = Math.max(240, contentHeight + chrome);
        if (viewerWindow.width === windowWidth
                && viewerWindow.height === windowHeight
                && viewerWindow.minimumWidth === windowWidth
                && viewerWindow.maximumWidth === windowWidth
                && viewerWindow.minimumHeight === windowHeight
                && viewerWindow.maximumHeight === windowHeight) {
            return;
        }
        // Clear stale limits before changing the size: going from a smaller
        // fixed mode to a larger one (or back from fullscreen) must not be
        // clamped by the previous maximum.
        resetWindowSizeLimits();
        viewerWindow.width = windowWidth;
        viewerWindow.height = windowHeight;
        viewerWindow.minimumWidth = windowWidth;
        viewerWindow.maximumWidth = windowWidth;
        viewerWindow.minimumHeight = windowHeight;
        viewerWindow.maximumHeight = windowHeight;
    }

    function applyDisplayMode() {
        if (fixedContentWidth > 0 && fixedContentHeight > 0) {
            applyFixedWindowSize();
        } else if (!viewerWindow.isFullScreen) {
            fitWindowToGuest();
        }
    }

    // Called when the Display settings page saves a resolution for this VM.
    function reloadDisplayResolution() {
        if (!domain) {
            return;
        }
        const resolution = Karton.domainDisplayResolution(domain);
        const newWidth = resolution.width > 0 ? resolution.width : 0;
        const newHeight = resolution.height > 0 ? resolution.height : 0;
        if (newWidth === fixedContentWidth && newHeight === fixedContentHeight) {
            return;
        }

        fixedContentWidth = newWidth;
        fixedContentHeight = newHeight;
        domainViewer.setFixedDisplayResolution(newWidth, newHeight);

        if (viewerWindow.isFullScreen) {
            // Fullscreen follows the screen, not the per-VM fixed mode.
            resetWindowSizeLimits();
            domainViewer.fitGuestToWindow();
        } else if (newWidth > 0) {
            applyFixedWindowSize();
        } else {
            resetWindowSizeLimits();
            Qt.callLater(fitWindowToGuest);
        }
    }

    Component.onCompleted: {
        registerViewer();
        const resolution = Karton.domainDisplayResolution(domain);
        if (resolution.width > 0 && resolution.height > 0) {
            fixedContentWidth = resolution.width;
            fixedContentHeight = resolution.height;
            domainViewer.setFixedDisplayResolution(resolution.width, resolution.height);
        }
        domainViewer.audioPlaybackEnabled = Karton.audioPlaybackEnabled(domain);
        domainViewer.microphoneEnabled = Karton.microphoneEnabled(domain);
        // A fixed resolution defines the window size; never let a stale size
        // from a previous (possibly crashed or fullscreen) session override it.
        if (fixedContentWidth <= 0) {
            const savedSize = Karton.viewerWindowSize(domain);
            if (savedSize.width > 0 && savedSize.height > 0) {
                width = Math.max(320, Math.min(savedSize.width, Screen.width * 0.95));
                height = Math.max(240, Math.min(savedSize.height, Screen.height * 0.95));
            }
        }
        sizeRestored = true;
        // Apply the fixed size before the window is mapped so a stale saved
        // geometry cannot even flash on screen, then refine it once the
        // toolbar has been laid out.
        applyDisplayMode();
        Qt.callLater(applyDisplayMode);
        wasRunning = isRunning;
        Qt.callLater(function() {
            domainViewer.forceActiveFocus();
        });
    }

    onWidthChanged: {
        if (sizeRestored && !isFullScreen) {
            viewerSizeTimer.restart();
        }
        clampToolbar();
    }
    onHeightChanged: {
        if (sizeRestored && !isFullScreen) {
            viewerSizeTimer.restart();
        }
        clampToolbar();
    }

    Component.onDestruction: {
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
    }

    onActiveChanged: {
        if (active) {
            Qt.callLater(function() {
                domainViewer.forceActiveFocus();
            });
        }
    }

    onVisibleChanged: {
        if (visible) {
            // The toolbar is laid out by now; reapply the fixed size so the
            // window opens at exactly the configured content size.
            Qt.callLater(applyDisplayMode);
        }
    }

    onClosing: (close) => {
        // Accepting the close lets QWindow::event() destroy the platform window
        // synchronously, which deadlocks Qt Wayland in QWaylandWindow::reset()
        // while the scene graph render thread still holds its lock (and then
        // aborts on the lock timeout). Keep the close event ignored, hide the
        // window and delete the QWindow later from the event loop instead.
        close.accepted = false;
        // A fullscreen geometry is not a meaningful windowed size; keep the
        // last normal size so a crash there cannot leave the next window huge.
        if (!viewerWindow.isFullScreen) {
            Karton.saveViewerWindowSize(viewerWindow.domain, viewerWindow.width, viewerWindow.height);
        }
        if (viewerRegistered) {
            viewerRegistered = false;
            Karton.unregisterViewer(viewerWindow);
            Karton.viewerClosed();
        }
        domainViewer.disconnectFromSpice();
        // Keep the hidden QWindow alive. Deleting it later still tears down
        // the Wayland platform window while the scene-graph render thread can
        // be inside QWaylandWindow, which deadlocks in QWaylandWindow::reset()
        // exactly like accepting the close event did. The process exits via
        // std::_Exit() on quit, before Qt destroys its windows.
        viewerWindow.hide();
    }

    Timer {
        id: viewerSizeTimer
        interval: 500
        repeat: false
        onTriggered: {
            if (!viewerWindow.isFullScreen) {
                Karton.saveViewerWindowSize(viewerWindow.domain, viewerWindow.width, viewerWindow.height);
            }
        }
    }

    Connections {
        target: Karton
        function onDomainRemoved(removedDomain) {
            if (removedDomain === viewerWindow.domain) {
                viewerWindow.close();
            }
        }
        function onDisplayResolutionChanged(changedDomain) {
            if (changedDomain === viewerWindow.domain) {
                viewerWindow.reloadDisplayResolution();
            }
        }
    }

    Connections {
        target: Karton
        function onClipboardSettingsChanged() {
            domainViewer.clipboardTextMode = Karton.clipboardTextMode();
            domainViewer.clipboardFilesMode = Karton.clipboardFilesMode();
        }
    }

    Connections {
        target: viewerWindow.domain
        function onIsActiveChanged(active) {
            if (active) {
                viewerWindow.wasRunning = true;
                domainViewer.setupSpiceSession();
            } else if (viewerWindow.wasRunning) {
                viewerWindow.close();
            }
        }
    }

    // In fullscreen the real header is removed and the floating bar below is
    // the only chrome over the guest.
    Binding {
        target: pageStack.globalToolBar
        property: "preferredHeight"
        value: 0
        when: viewerWindow.isFullScreen
        restoreMode: Binding.RestoreBindingOrValue
    }

    pageStack.initialPage: Kirigami.Page {
        id: viewerPage
        title: viewerWindow.title
        padding: 0

        HoverHandler {
            id: toolbarHover
            onPointChanged: {
                if (!viewerWindow.isFullScreen) {
                    return;
                }
                viewerWindow.hoveredToolbarButton = viewerWindow.fullScreenToolbarVisible
                        ? viewerWindow.toolbarButtonAt(point.position.x, point.position.y) : "";
            }
            onHoveredChanged: {
                if (!hovered) {
                    viewerWindow.hoveredToolbarButton = "";
                }
            }
        }

        // Invisible hover target that exactly matches the hidden toolbar.
        // The toolbar is revealed only when the pointer is over the toolbar
        // itself, not anywhere along the top edge of the display.
        Item {
            id: toolbarHoverArea
            x: floatingToolbar.x
            y: floatingToolbar.y
            width: floatingToolbar.width
            height: floatingToolbar.height
            z: 90
            visible: viewerWindow.isFullScreen
            enabled: !viewerWindow.fullScreenToolbarVisible

            HoverHandler {
                onPointChanged: {
                    if (!hovered || !viewerWindow.isFullScreen) {
                        return;
                    }
                    viewerWindow.fullScreenToolbarVisible = true;
                    toolbarHideTimer.stop();
                    const pagePoint = toolbarHoverArea.mapToItem(viewerPage, point.position.x, point.position.y);
                    viewerWindow.hoveredToolbarButton = viewerWindow.toolbarButtonAt(pagePoint.x, pagePoint.y);
                }
                onHoveredChanged: {
                    if (!viewerWindow.isFullScreen) {
                        return;
                    }
                    if (!hovered) {
                        toolbarHideTimer.restart();
                        return;
                    }
                    viewerWindow.fullScreenToolbarVisible = true;
                    toolbarHideTimer.stop();
                    const pagePoint = toolbarHoverArea.mapToItem(viewerPage, point.position.x, point.position.y);
                    viewerWindow.hoveredToolbarButton = viewerWindow.toolbarButtonAt(pagePoint.x, pagePoint.y);
                }
            }
        }

        Rectangle {
            id: floatingToolbar
            x: Math.max(0, (parent.width - width) / 2)
            y: 0
            width: Math.min(viewerPage.width - 24, toolbarContent.implicitWidth + 32)
            height: viewerWindow.compactToolbarHeight
            radius: 12
            topLeftRadius: 0
            topRightRadius: 0
            bottomLeftRadius: 12
            bottomRightRadius: 12
            color: Qt.rgba(0, 0, 0, 0.72)
            visible: viewerWindow.isFullScreen && (viewerWindow.fullScreenToolbarVisible || opacity > 0.01)
            opacity: viewerWindow.fullScreenToolbarVisible ? 1 : 0
            enabled: viewerWindow.fullScreenToolbarVisible && opacity > 0.5
            z: 100

            Behavior on opacity {
                NumberAnimation {
                    duration: 180
                    easing.type: Easing.InOutQuad
                }
            }

            onWidthChanged: {
                if (viewerWindow.toolbarMoved) {
                    viewerWindow.clampToolbar();
                }
            }

            DragHandler {
                id: toolbarDrag
                target: floatingToolbar
                acceptedButtons: Qt.LeftButton
                dragThreshold: 4
                xAxis.minimum: 0
                xAxis.maximum: Math.max(0, viewerPage.width - floatingToolbar.width)
                yAxis.enabled: false

                onActiveChanged: {
                    if (active) {
                        viewerWindow.toolbarMoved = true;
                        toolbarHideTimer.stop();
                    } else {
                        viewerWindow.clampToolbar();
                        toolbarHideTimer.restart();
                    }
                }
            }

            HoverHandler {
                id: toolbarBarHover
                onHoveredChanged: {
                    if (!viewerWindow.isFullScreen) {
                        return;
                    }
                    if (hovered) {
                        viewerWindow.fullScreenToolbarVisible = true;
                        toolbarHideTimer.stop();
                    } else if (!toolbarDrag.active) {
                        toolbarHideTimer.restart();
                    }
                }
            }

            RowLayout {
                id: toolbarContent
                anchors.fill: parent
                anchors.leftMargin: 16
                anchors.rightMargin: 8
                anchors.topMargin: 6
                anchors.bottomMargin: 6
                spacing: 8

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignVCenter
                    verticalAlignment: Text.AlignVCenter
                    text: viewerWindow.title
                    color: "white"
                    elide: Text.ElideRight
                    font.bold: true
                }

                Row {
                    spacing: 8
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        id: powerFrame
                        width: 120
                        height: 40
                        radius: 8
                        color: viewerWindow.hoveredToolbarButton === "power" || powerButton.hovered || powerButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: powerButton
                            anchors.fill: parent
                            text: viewerWindow.isRunning ? i18n("Shut Down") : i18n("Start")
                            icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                if (viewerWindow.isRunning) {
                                    Karton.stopDomain(viewerWindow.domain);
                                } else if (Karton.startDomain(viewerWindow.domain)) {
                                    viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Starting VM: %1!", viewerWindow.domain.config.name));
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: pauseFrame
                        visible: viewerWindow.isRunning
                        width: 100
                        height: 40
                        radius: 8
                        color: viewerWindow.hoveredToolbarButton === "pause" || pauseButton.hovered || pauseButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: pauseButton
                            anchors.fill: parent
                            text: viewerWindow.domain && viewerWindow.domain.isPaused ? i18n("Resume") : i18n("Pause")
                            icon.name: viewerWindow.domain && viewerWindow.domain.isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                if (viewerWindow.domain.isPaused) {
                                    Karton.resumeDomain(viewerWindow.domain);
                                } else {
                                    Karton.pauseDomain(viewerWindow.domain);
                                }
                            }
                        }
                    }

                    Rectangle {
                        id: rebootFrame
                        visible: viewerWindow.isRunning
                        width: 100
                        height: 40
                        radius: 8
                        color: viewerWindow.hoveredToolbarButton === "reboot" || rebootButton.hovered || rebootButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: rebootButton
                            anchors.fill: parent
                            text: i18n("Reboot")
                            icon.name: "system-reboot-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                Karton.rebootDomain(viewerWindow.domain);
                            }
                        }
                    }

                    Rectangle {
                        id: fullscreenFrame
                        width: 40
                        height: 40
                        radius: 8
                        color: viewerWindow.hoveredToolbarButton === "fullscreen" || fullscreenButton.hovered || fullscreenButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: fullscreenButton
                            anchors.fill: parent
                            icon.name: "view-fullscreen-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: i18n("Exit Fullscreen")
                            onClicked: viewerWindow.toggleFullScreen()
                        }
                    }

                    Rectangle {
                        id: moreFrame
                        width: 40
                        height: 40
                        radius: 8
                        color: viewerWindow.hoveredToolbarButton === "more" || moreButton.hovered || moreButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: moreButton
                            anchors.fill: parent
                            icon.name: "view-more-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: i18n("More")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                viewerMenu.popup();
                            }
                        }
                    }
                }
            }
        }

        onWidthChanged: if (domainViewer) domainViewer.updateDisplayResolution()
        onHeightChanged: if (domainViewer) domainViewer.updateDisplayResolution()

        // If the guest agent is absent, the guest cannot resize itself. Match
        // the window to the guest frame once so the display is not letterboxed
        // by black bars just because the window happened to open at 16:9.
        Connections {
            target: domainViewer
            function onImplicitWidthChanged() {
                viewerWindow.fitWindowToGuest();
            }
            function onImplicitHeightChanged() {
                viewerWindow.fitWindowToGuest();
            }
        }

        actions: [
            Kirigami.Action {
                text: viewerWindow.isRunning ? i18n("Shut Down") : i18n("Start")
                icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                onTriggered: {
                    if (viewerWindow.isRunning) {
                        Karton.stopDomain(viewerWindow.domain);
                    } else if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Starting VM: %1!", viewerWindow.domain.config.name));
                    }
                }
            },
            Kirigami.Action {
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: powerMenu.popup()
            },
            Kirigami.Action {
                text: viewerWindow.domain && viewerWindow.domain.isPaused ? i18n("Resume") : i18n("Pause")
                icon.name: viewerWindow.domain && viewerWindow.domain.isPaused ? "media-playback-start-symbolic" : "media-playback-pause-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    if (!viewerWindow.domain) {
                        return;
                    }
                    if (viewerWindow.domain.isPaused) {
                        Karton.resumeDomain(viewerWindow.domain);
                    } else {
                        Karton.pauseDomain(viewerWindow.domain);
                    }
                }
            },
            Kirigami.Action {
                text: i18n("Reboot")
                icon.name: "system-reboot-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: Karton.rebootDomain(viewerWindow.domain)
            },
            Kirigami.Action {
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: rebootMenu.popup()
            },
            Kirigami.Action {
                icon.name: "view-fullscreen-symbolic"
                onTriggered: viewerWindow.toggleFullScreen()
            },
            Kirigami.Action {
                text: i18n("More")
                icon.name: "view-more-symbolic"
                onTriggered: {
                    domainViewer.releasePointerGrab();
                    viewerMenu.popup();
                }
            }
        ]

        Connections {
            target: Karton
            function onClosePopupsRequested() {
                powerMenu.close();
                rebootMenu.close();
                viewerMenu.close();
            }
        }

        Controls.Menu {
            id: powerMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

            Controls.MenuItem {
                text: i18n("Power Off")
                icon.name: "process-stop-symbolic"
                onTriggered: Karton.forceStopDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: rebootMenu
        closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

            Controls.MenuItem {
                text: i18n("Reset")
                icon.name: "system-reboot-symbolic"
                onTriggered: Karton.resetDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: viewerMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape

            Controls.MenuItem {
                text: i18n("Display")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("Actual Size")
                icon.name: "zoom-original-symbolic"
                onTriggered: {
                    viewerWindow.width = Math.max(320, domainViewer.implicitWidth / domainViewer.dprHelper.devicePixelRatio);
                    viewerWindow.height = Math.max(240, domainViewer.implicitHeight / domainViewer.dprHelper.devicePixelRatio + pageStack.globalToolBar.height);
                }
            }
            Controls.MenuItem {
                text: i18n("Fit Guest to Window")
                icon.name: "transform-scale-symbolic"
                onTriggered: domainViewer.fitGuestToWindow()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: i18n("Devices")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("USB Devices")
                icon.name: "drive-removable-media-usb-symbolic"
                onTriggered: usbDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: i18n("Machine")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: i18n("Hibernate")
                icon.name: "document-save-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    if (Karton.saveDomainState(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Hibernating VM: %1", viewerWindow.domain.config.name));
                    }
                }
            }
            Controls.MenuItem {
                text: i18n("Settings")
                icon.name: "preferences-system-symbolic"
                onTriggered: settingsDialog.open()
            }
            Controls.MenuItem {
                text: i18n("Snapshots")
                icon.name: "camera-photo-symbolic"
                onTriggered: snapshotsDialog.open()
            }
        }

        Rectangle {
            anchors.fill: parent
            color: "black"
        }

        DomainViewer {
            id: domainViewer

            // Hovering the top edge of the guest (which is the item under the
            // pointer once the floating toolbar has faded out) must bring the
            // toolbar back. The page-level HoverHandler alone could miss this
            // after the toolbar was hidden under a stationary pointer.

            property DevicePixelRatioHelper dprHelper: DevicePixelRatioHelper {
                window: domainViewer.Window.window
            }

            readonly property real frameAspect: (implicitWidth > 0 && implicitHeight > 0) ? implicitWidth / implicitHeight : (16.0 / 9.0)

            anchors.centerIn: parent
            width: Math.min(parent.width, parent.height * frameAspect)
            height: width / frameAspect

            domain: viewerWindow.domain
            spiceTlsEnabled: Karton.spiceTlsEnabled()
            spiceCaCertificate: Karton.spiceCaCertificate()
            clipboardTextMode: Karton.clipboardTextMode()
            clipboardFilesMode: Karton.clipboardFilesMode()
            visible: viewerWindow.isRunning
            enabled: viewerWindow.isRunning

            focus: true
            activeFocusOnTab: true
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.pointerGrabbed && domainViewer.errorMessage.length === 0
            text: i18n("Pointer captured. Press Esc to release it.")
            type: Kirigami.MessageType.Information
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.errorMessage.length > 0
            text: domainViewer.errorMessage
            type: Kirigami.MessageType.Error
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.top: parent.top
            anchors.topMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && !domainViewer.agentConnected
                && !Karton.guestAgentWarningDismissed && domainViewer.errorMessage.length === 0
            text: i18n("Guest agent is not connected. Automatic resize requires spice-vdagent in the guest.")
            type: Kirigami.MessageType.Warning
            showCloseButton: true
            onVisibleChanged: {
                if (!visible && viewerWindow.isRunning && !domainViewer.agentConnected) {
                    Karton.guestAgentWarningDismissed = true;
                }
            }
        }

        Kirigami.Icon {
            id: startIcon
            anchors.centerIn: parent
            visible: !viewerWindow.isRunning
            source: "media-playback-start-symbolic"
            width: Kirigami.Units.gridUnit * 8
            height: width
            opacity: startMouseArea.containsMouse ? 0.75 : 1.0

            MouseArea {
                id: startMouseArea
                anchors.fill: parent
                hoverEnabled: true
                cursorShape: Qt.PointingHandCursor
                onClicked: {
                    if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(i18nc("%1 is the name of the virtual machine", "Starting VM: %1!", viewerWindow.domain.config.name));
                    }
                }
            }
        }

        UsbDevicesDialog {
            id: usbDialog
            domain: viewerWindow.domain
        }

        SettingsDialog {
            id: settingsDialog
            domain: viewerWindow.domain
            parentWindow: viewerWindow
        }

        SnapshotsDialog {
            id: snapshotsDialog
            domain: viewerWindow.domain
        }

    }
}
