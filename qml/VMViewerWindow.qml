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

    // The guest fills the window; a theme-coloured window background would
    // shine through for a frame while the compositor restores the windowed
    // geometry, which looked like a bright flash on top of the black page.
    color: "black"

    title: domain ? qsTr("vm_viewer_1").arg(domain.config.name) : qsTr("vm_viewer")

    readonly property var screenInfo: Qt.application.screens.length > 0 ? Qt.application.screens[0] : null

    // Open large enough for a modern guest resolution instead of a small
    // fixed window; Actual Size and Fit Guest to Window remain available.
    width: screenInfo ? Math.min(screenInfo.width, Math.max(Kirigami.Units.gridUnit * 60, Math.min(screenInfo.width * 0.85, Kirigami.Units.gridUnit * 120))) : Kirigami.Units.gridUnit * 70
    height: screenInfo ? Math.min(screenInfo.height, Math.max(Kirigami.Units.gridUnit * 40, Math.min(screenInfo.height * 0.85, Kirigami.Units.gridUnit * 72))) : Kirigami.Units.gridUnit * 45

    property bool viewerRegistered: false
    readonly property bool isRunning: domain ? domain.isActive : false
    // A running viewer captures shortcuts only while the pointer is over the
    // guest area. The toolbar, title bar and dialogs must keep normal
    // shortcuts: Ctrl+Q closes the viewer from there, but goes to the guest
    // when the pointer is inside the VM.
    readonly property bool guestPointerInside: (domainViewer && (domainViewer.pointerInside || domainViewer.pointerGrabbed))
        || (serialTerminal && serialTerminal.visible && serialTerminalHover.hovered)
    readonly property bool shortcutsCaptured: isRunning && active && guestPointerInside
        && !(usbDialog && usbDialog.opened)
        && !(settingsDialog && settingsDialog.opened)
        && !(snapshotsDialog && snapshotsDialog.opened)
        && !(powerMenu && powerMenu.opened)
        && !(rebootMenu && rebootMenu.opened)
        && !(viewerMenu && viewerMenu.opened)
    property bool wasRunning: false
    property bool sizeRestored: false
    property int consoleFontSize: Karton.consoleFontSize(domain)

    // The guest agent is absent while the VM boots and while Windows is at the
    // login screen. Do not show the resize hint immediately: wait for a grace
    // period, remember VMs where the agent has connected before, and show the
    // hint at most once per VM.
    property bool guestAgentSeen: false
    property bool guestAgentWarningSuppressed: false
    property bool guestAgentWarningGraceElapsed: false

    onDomainChanged: {
        consoleFontSize = Karton.consoleFontSize(domain);
        guestAgentSeen = domain ? Karton.guestAgentSeen(domain) : false;
        guestAgentWarningSuppressed = domain ? Karton.guestAgentWarningShown(domain) : false;
    }
    property alias domainViewerItem: domainViewer
    property int fixedContentWidth: 0
    property int fixedContentHeight: 0
    // QWindow's default maximum; used to make the window resizable again when
    // a fixed resolution is switched back to Auto or when going fullscreen.
    readonly property int unlimitedWindowSize: 16777215
    readonly property bool isFullScreen: visibility === Window.FullScreen
    property bool fullScreenTransition: false
    // Floating fullscreen bar. It is only visible while
    // `fullScreenToolbarVisible` is true, so it can never swallow clicks
    // while invisible.
    readonly property int compactToolbarHeight: 40
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
            ["more", moreFrame],
            ["minimize", minimizeFrame],
            ["fullscreen", fullscreenFrame],
            ["close", closeFrame]
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

    Timer {
        id: agentWarningGraceTimer
        // Long enough for a normal boot and for the login screen to appear.
        interval: 60000
        repeat: false
        onTriggered: {
            if (viewerWindow.isRunning && !domainViewer.agentConnected
                    && !viewerWindow.guestAgentSeen && !viewerWindow.guestAgentWarningSuppressed) {
                viewerWindow.guestAgentWarningGraceElapsed = true;
            }
        }
    }

    Timer {
        id: agentVerificationTimer
        // The agent was seen for this image before but is absent now. Give a
        // reboot, login or VM restart time to bring it back. If it stays away,
        // the guest tools were probably removed or the image was replaced, so
        // forget the cached state and let the hint appear again.
        interval: 180000
        repeat: false
        onTriggered: {
            if (viewerWindow.isRunning && !domainViewer.agentConnected && viewerWindow.guestAgentSeen) {
                if (viewerWindow.domain) {
                    Karton.clearGuestAgentState(viewerWindow.domain);
                }
                viewerWindow.guestAgentSeen = false;
                viewerWindow.guestAgentWarningSuppressed = false;
                viewerWindow.guestAgentWarningGraceElapsed = false;
                viewerWindow.updateAgentWarning();
            }
        }
    }

    onIsFullScreenChanged: {
        fullScreenTransition = false;
        fullScreenTransitionTimer.stop();
        if (isFullScreen) {
            windowedRestoreTimer.stop();
            // Fixed-size limits must not constrain or stretch a fullscreen
            // window; the guest follows the whole screen there.
            resetWindowSizeLimits();
            fullScreenToolbarVisible = true;
            toolbarHideTimer.restart();
            Qt.callLater(function() {
                if (viewerWindow.isFullScreen) {
                    viewerWindow.resetWindowSizeLimits();
                    domainViewer.updateDisplayResolution();
                }
            });
        } else {
            fullScreenToolbarVisible = true;
            toolbarHideTimer.stop();
            // Let the compositor finish restoring the windowed geometry before
            // touching the size. Resizing from this handler used to race the
            // fullscreen exit animation, which made the window flash while
            // KWin was still moving it back.
            windowedRestoreTimer.restart();
        }
    }

    Timer {
        id: windowedRestoreTimer
        interval: 200
        repeat: false
        onTriggered: viewerWindow.applyDisplayMode()
    }

    Timer {
        id: fullScreenTransitionTimer
        interval: 1500
        repeat: false
        onTriggered: {
            viewerWindow.fullScreenTransition = false;
            viewerWindow.applyDisplayMode();
        }
    }

    function toggleFullScreen() {
        if (visibility === Window.FullScreen) {
            showNormal();
            return;
        }
        // Release fixed limits before asking the compositor for fullscreen.
        // Do it one event-loop turn before the request so KWin does not keep
        // the old fixed-size maximum for the fullscreen surface.
        fullScreenTransition = true;
        fullScreenTransitionTimer.restart();
        resetWindowSizeLimits();
        Qt.callLater(function() {
            if (!viewerWindow.fullScreenTransition) {
                return;
            }
            viewerWindow.showFullScreen();
            // Start the auto-hide timeout explicitly; visibility changes can
            // be delivered before isFullScreen updates, which used to leave
            // the toolbar without a running hide timer until it was hovered.
            Qt.callLater(function() {
                viewerWindow.fullScreenToolbarVisible = true;
                toolbarHideTimer.restart();
            });
        });
    }

    // Keyboard focus must stay on the guest display (or the serial terminal),
    // not on whichever toolbar button was last clicked. Otherwise arrows,
    // Tab, Space and similar keys are consumed by the controls instead of
    // reaching the VM.
    function closeViewer() {
        close();
    }

    function stopAgentWarningGrace() {
        agentWarningGraceTimer.stop();
        guestAgentWarningGraceElapsed = false;
    }

    function startAgentWarningGrace() {
        guestAgentWarningGraceElapsed = false;
        if (!agentWarningGraceTimer.running) {
            agentWarningGraceTimer.restart();
        }
    }

    function stopAgentVerification() {
        agentVerificationTimer.stop();
    }

    function startAgentVerification() {
        if (!agentVerificationTimer.running) {
            agentVerificationTimer.restart();
        }
    }

    function reloadGuestAgentState() {
        guestAgentSeen = domain ? Karton.guestAgentSeen(domain) : false;
        guestAgentWarningSuppressed = domain ? Karton.guestAgentWarningShown(domain) : false;
        guestAgentWarningGraceElapsed = false;
        agentWarningGraceTimer.stop();
        agentVerificationTimer.stop();
        updateAgentWarning();
    }

    function updateAgentWarning() {
        if (domainViewer.agentConnected) {
            stopAgentWarningGrace();
            stopAgentVerification();
            if (!guestAgentSeen) {
                guestAgentSeen = true;
                if (domain) {
                    Karton.markGuestAgentSeen(domain);
                }
            }
        } else if (isRunning && guestAgentSeen) {
            // The agent worked for this image before. Wait quietly for it to
            // come back (boot, reboot, login) instead of nagging immediately.
            stopAgentWarningGrace();
            startAgentVerification();
        } else if (isRunning && !guestAgentWarningSuppressed) {
            stopAgentVerification();
            startAgentWarningGrace();
        } else {
            stopAgentWarningGrace();
            stopAgentVerification();
        }
    }

    onIsRunningChanged: {
        if (isRunning) {
            // The disk path or OS metadata may have changed while the VM was
            // stopped; re-read the per-image state before starting the timers.
            guestAgentSeen = domain ? Karton.guestAgentSeen(domain) : false;
            guestAgentWarningSuppressed = domain ? Karton.guestAgentWarningShown(domain) : false;
            guestAgentWarningGraceElapsed = false;
        }
        updateAgentWarning();
    }

    function syncShortcutCapture() {
        // Disable both the quit shortcut and the action itself. Clearing only
        // the sequence is not enough: Kirigami installs an application-wide
        // Ctrl+Q shortcut, and its sequence binding can survive an empty
        // value. `enabled` reliably switches it off.
        quitAction.enabled = !shortcutsCaptured;
        quitAction.shortcut = shortcutsCaptured ? "" : StandardKey.Quit;
        // KDE/KWin global shortcuts are handled before the application. While
        // the pointer is over the guest, block them through KGlobalAccel so
        // keys such as Ctrl+Q can reach the guest. They are restored when the
        // pointer moves to the toolbar/title bar, the viewer loses focus,
        // stops, opens a dialog/menu, or closes.
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, shortcutsCaptured);
    }

    onShortcutsCapturedChanged: syncShortcutCapture()

    function focusDisplay() {
        // A dialog opened from a menu is allowed to keep the keyboard focus.
        if ((usbDialog && usbDialog.opened) || (settingsDialog && settingsDialog.opened)
                || (snapshotsDialog && snapshotsDialog.opened)) {
            return;
        }
        if (serialTerminal && serialTerminal.visible) {
            serialTerminal.forceActiveFocus();
        } else if (domainViewer && domainViewer.visible) {
            domainViewer.forceActiveFocus();
        }
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
        if (fixedContentWidth <= 0 || fixedContentHeight <= 0 || viewerWindow.isFullScreen || viewerWindow.fullScreenTransition) {
            return;
        }
        const scale = Math.min(1.0, Screen.width * 0.9 / fixedContentWidth, Screen.height * 0.85 / fixedContentHeight);
        const contentWidth = Math.round(fixedContentWidth * scale);
        const contentHeight = Math.round(fixedContentHeight * scale);
        const chrome = chromeHeight();
        const windowWidth = Math.max(320, contentWidth);
        const windowHeight = Math.max(240, contentHeight + chrome);
        if (viewerWindow.width === windowWidth && viewerWindow.height === windowHeight) {
            return;
        }
        // Set the size but do not pin min/max to it. KWin keeps a client's
        // maximum size even when the window becomes fullscreen, which left the
        // viewer (and therefore the guest resolution) at the fixed 1920x1126
        // size on a 3440x1440 screen. The size is re-applied when returning
        // from fullscreen, so the configured resolution still wins on open.
        resetWindowSizeLimits();
        viewerWindow.width = windowWidth;
        viewerWindow.height = windowHeight;
    }

    function applyDisplayMode() {
        if (viewerWindow.fullScreenTransition) {
            return;
        }
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
        // Kirigami installs an application-wide Ctrl+Q shortcut. It is disabled
        // while the pointer is over the running guest; DomainViewer forwards
        // every key to the guest in that state, and the toolbar/title bar keep
        // the normal shortcut.
        syncShortcutCapture();
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
        reloadGuestAgentState();
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
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, false);
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
        // Keyboard shortcuts are inhibited at the compositor/KGlobalAccel
        // level while the guest is active, so a close request here is a real
        // user action (title-bar X, window menu, More -> Close Viewer), not
        // Alt+F4. Allow it; only the viewer closes and the VM keeps running.
        Karton.setViewerGlobalShortcutsBlocked(viewerWindow, false);
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
        function onConsoleFontSizeChanged() {
            viewerWindow.consoleFontSize = Karton.consoleFontSize(viewerWindow.domain);
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
            width: Math.min(viewerPage.width - 24, toolbarContent.implicitWidth + 16)
            height: viewerWindow.compactToolbarHeight
            radius: 8
            topLeftRadius: 0
            topRightRadius: 0
            bottomLeftRadius: 8
            bottomRightRadius: 8
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
                anchors.leftMargin: 12
                anchors.rightMargin: 6
                anchors.topMargin: 4
                anchors.bottomMargin: 4
                spacing: 4

                Controls.Label {
                    Layout.fillWidth: true
                    Layout.fillHeight: true
                    Layout.alignment: Qt.AlignVCenter
                    verticalAlignment: Text.AlignVCenter
                    text: viewerWindow.title
                    color: "white"
                    elide: Text.ElideRight
                    font.bold: true
                    font.pixelSize: 13
                }

                Row {
                    spacing: 4
                    Layout.alignment: Qt.AlignVCenter

                    Rectangle {
                        id: powerFrame
                        width: 104
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "power" || powerButton.hovered || powerButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: powerButton
                            anchors.fill: parent
                            text: viewerWindow.isRunning ? qsTr("shut_down") : qsTr("start")
                            icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                if (viewerWindow.isRunning) {
                                    Karton.stopDomain(viewerWindow.domain);
                                } else if (Karton.startDomain(viewerWindow.domain)) {
                                    viewerWindow.showPassiveNotification(qsTr("starting_vm_1").arg(viewerWindow.domain.config.name));
                                }
                                viewerWindow.focusDisplay();
                            }
                        }
                    }

                    Rectangle {
                        id: pauseFrame
                        visible: viewerWindow.isRunning
                        width: 88
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "pause" || pauseButton.hovered || pauseButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: pauseButton
                            anchors.fill: parent
                            text: viewerWindow.domain && viewerWindow.domain.isPaused ? qsTr("resume") : qsTr("pause")
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
                                viewerWindow.focusDisplay();
                            }
                        }
                    }

                    Rectangle {
                        id: rebootFrame
                        visible: viewerWindow.isRunning
                        width: 88
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "reboot" || rebootButton.hovered || rebootButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: rebootButton
                            anchors.fill: parent
                            text: qsTr("reboot")
                            icon.name: "system-reboot-symbolic"
                            flat: true
                            hoverEnabled: true
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            onClicked: {
                                Karton.rebootDomain(viewerWindow.domain);
                                viewerWindow.focusDisplay();
                            }
                        }
                    }

                    Rectangle {
                        id: moreFrame
                        width: 32
                        height: 32
                        radius: 6
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
                            Controls.ToolTip.text: qsTr("more")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                viewerMenu.popup();
                            }
                        }
                    }                    Rectangle {
                        id: minimizeFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "minimize" || minimizeButton.hovered || minimizeButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: minimizeButton
                            anchors.fill: parent
                            icon.name: "window-minimize-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("minimize")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                viewerWindow.showMinimized();
                            }
                        }
                    }

                    Rectangle {
                        id: fullscreenFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "fullscreen" || fullscreenButton.hovered || fullscreenButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: fullscreenButton
                            anchors.fill: parent
                            icon.name: "view-restore-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("exit_fullscreen")
                            onClicked: {
                                viewerWindow.toggleFullScreen();
                                Qt.callLater(viewerWindow.focusDisplay);
                            }
                        }
                    }

                    Rectangle {
                        id: closeFrame
                        width: 32
                        height: 32
                        radius: 6
                        color: viewerWindow.hoveredToolbarButton === "close" || closeButton.hovered || closeButton.down ? Qt.rgba(1, 1, 1, 0.18) : "transparent"

                        Controls.ToolButton {
                            id: closeButton
                            anchors.fill: parent
                            icon.name: "window-close-symbolic"
                            flat: true
                            hoverEnabled: true
                            display: Controls.AbstractButton.IconOnly
                            Kirigami.Theme.inherit: false
                            Kirigami.Theme.colorSet: Kirigami.Theme.Complementary
                            Controls.ToolTip.visible: hovered
                            Controls.ToolTip.text: qsTr("close_viewer")
                            onClicked: {
                                domainViewer.releasePointerGrab();
                                viewerWindow.close();
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
            function onAgentConnectedChanged() {
                viewerWindow.updateAgentWarning();
            }
        }

        actions: [
            Kirigami.Action {
                text: viewerWindow.isRunning ? qsTr("shut_down") : qsTr("start")
                icon.name: viewerWindow.isRunning ? "system-shutdown-symbolic" : "media-playback-start-symbolic"
                onTriggered: {
                    if (viewerWindow.isRunning) {
                        Karton.stopDomain(viewerWindow.domain);
                    } else if (Karton.startDomain(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(qsTr("starting_vm_1").arg(viewerWindow.domain.config.name));
                    }
                    viewerWindow.focusDisplay();
                }
            },
            Kirigami.Action {
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: powerMenu.popup()
            },
            Kirigami.Action {
                text: viewerWindow.domain && viewerWindow.domain.isPaused ? qsTr("resume") : qsTr("pause")
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
                    viewerWindow.focusDisplay();
                }
            },
            Kirigami.Action {
                text: qsTr("reboot")
                icon.name: "system-reboot-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    Karton.rebootDomain(viewerWindow.domain);
                    viewerWindow.focusDisplay();
                }
            },
            Kirigami.Action {
                icon.name: "arrow-down-symbolic"
                displayHint: Kirigami.DisplayHint.IconOnly
                visible: viewerWindow.isRunning
                onTriggered: rebootMenu.popup()
            },
            Kirigami.Action {
                text: qsTr("more")
                icon.name: "view-more-symbolic"
                onTriggered: {
                    domainViewer.releasePointerGrab();
                    viewerMenu.popup();
                }
            },
            Kirigami.Action {
                icon.name: "view-fullscreen-symbolic"
                onTriggered: {
                    viewerWindow.toggleFullScreen();
                    Qt.callLater(viewerWindow.focusDisplay);
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
            onClosed: Qt.callLater(viewerWindow.focusDisplay)

            Controls.MenuItem {
                text: qsTr("power_off")
                icon.name: "process-stop-symbolic"
                onTriggered: Karton.forceStopDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: rebootMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape
            onClosed: Qt.callLater(viewerWindow.focusDisplay)

            Controls.MenuItem {
                text: qsTr("reset")
                icon.name: "system-reboot-symbolic"
                onTriggered: Karton.resetDomain(viewerWindow.domain)
            }
        }

        Controls.Menu {
            id: viewerMenu
            closePolicy: Controls.Popup.CloseOnPressOutside | Controls.Popup.CloseOnEscape
            onClosed: Qt.callLater(viewerWindow.focusDisplay)

            Controls.MenuItem {
                text: qsTr("display")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("actual_size")
                icon.name: "zoom-original-symbolic"
                enabled: !domainViewer.serialConsoleActive
                onTriggered: {
                    viewerWindow.width = Math.max(320, domainViewer.implicitWidth / domainViewer.dprHelper.devicePixelRatio);
                    viewerWindow.height = Math.max(240, domainViewer.implicitHeight / domainViewer.dprHelper.devicePixelRatio + pageStack.globalToolBar.height);
                }
            }
            Controls.MenuItem {
                text: qsTr("fit_guest_to_window")
                icon.name: "transform-scale-symbolic"
                enabled: !domainViewer.serialConsoleActive
                onTriggered: domainViewer.fitGuestToWindow()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: qsTr("devices")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("usb_devices")
                icon.name: "drive-removable-media-usb-symbolic"
                onTriggered: usbDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: qsTr("machine")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("hibernate")
                icon.name: "document-save-symbolic"
                visible: viewerWindow.isRunning
                onTriggered: {
                    if (Karton.saveDomainState(viewerWindow.domain)) {
                        viewerWindow.showPassiveNotification(qsTr("hibernating_vm_1").arg(viewerWindow.domain.config.name));
                    }
                }
            }
            Controls.MenuItem {
                text: qsTr("snapshots")
                icon.name: "camera-photo-symbolic"
                onTriggered: snapshotsDialog.open()
            }
            Controls.MenuSeparator {}

            Controls.MenuItem {
                text: qsTr("configuration")
                enabled: false
                font.bold: true
            }
            Controls.MenuItem {
                text: qsTr("settings")
                icon.name: "preferences-system-symbolic"
                onTriggered: settingsDialog.open()
            }
            Controls.MenuSeparator {}
            Controls.MenuItem {
                text: qsTr("close_viewer")
                icon.name: "window-close-symbolic"
                onTriggered: viewerWindow.closeViewer()
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
            visible: viewerWindow.isRunning && !domainViewer.serialConsoleActive
            enabled: viewerWindow.isRunning && !domainViewer.serialConsoleActive

            focus: true
            activeFocusOnTab: true
        }

        TerminalView {
            id: serialTerminal
            anchors.fill: parent
            fontSize: viewerWindow.consoleFontSize
            visible: viewerWindow.isRunning && domainViewer.serialConsoleActive
            enabled: visible
            focus: visible
            activeFocusOnTab: visible

            HoverHandler {
                id: serialTerminalHover
            }
            onVisibleChanged: {
                if (visible) {
                    // The dialogs declared later in the page may not exist yet
                    // while the console is being opened; focus after the whole
                    // component tree has been created.
                    Qt.callLater(viewerWindow.focusDisplay);
                }
            }
            onDataToSend: function(text) {
                domainViewer.sendConsoleText(text);
            }
        }

        Connections {
            target: domainViewer
            function onConsoleDataReceived(text) {
                serialTerminal.feed(text);
            }
        }

        Kirigami.InlineMessage {
            anchors.horizontalCenter: parent.horizontalCenter
            anchors.bottom: parent.bottom
            anchors.bottomMargin: Kirigami.Units.largeSpacing
            width: Math.min(parent.width - Kirigami.Units.gridUnit * 4, Kirigami.Units.gridUnit * 40)
            visible: viewerWindow.isRunning && domainViewer.pointerGrabbed && domainViewer.errorMessage.length === 0
            text: qsTr("pointer_captured_press_ctrl_alt_to_release_it")
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
            visible: viewerWindow.visible && viewerWindow.isRunning
                && !domainViewer.serialConsoleActive
                && !domainViewer.agentConnected && !viewerWindow.guestAgentSeen
                && viewerWindow.guestAgentWarningGraceElapsed
                && !viewerWindow.guestAgentWarningSuppressed
                && domainViewer.errorMessage.length === 0
            text: qsTr("guest_agent_is_not_connected_automatic_resize_requires_spice_vdagent_in_the_guest")
            type: Kirigami.MessageType.Warning
            showCloseButton: true
            onVisibleChanged: {
                if (visible) {
                    // Show the hint at most once per VM. Persisting it as soon
                    // as it appears means a reboot or a new viewer window will
                    // not repeat it, even if the user never clicks the close
                    // button.
                    if (viewerWindow.domain) {
                        Karton.markGuestAgentWarningShown(viewerWindow.domain);
                    }
                } else if (viewerWindow.isRunning && !domainViewer.agentConnected
                        && !viewerWindow.guestAgentSeen && domainViewer.errorMessage.length === 0) {
                    // The user closed the message. The automatic hides after
                    // the agent connects or the VM stops are excluded above.
                    viewerWindow.guestAgentWarningSuppressed = true;
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
                        viewerWindow.showPassiveNotification(qsTr("starting_vm_1").arg(viewerWindow.domain.config.name));
                    }
                    viewerWindow.focusDisplay();
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

        Connections {
            target: usbDialog
            function onClosed() {
                viewerWindow.focusDisplay();
            }
        }

        Connections {
            target: settingsDialog
            function onClosed() {
                viewerWindow.focusDisplay();
            }
        }

        Connections {
            target: snapshotsDialog
            function onClosed() {
                viewerWindow.focusDisplay();
            }
        }

    }
}
