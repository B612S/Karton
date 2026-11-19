// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

// Test-only component loaded by `karton --check-qml`. Keep it headless: it
// instantiates components that Main.qml only creates at runtime, so invalid
// property assignments in those components fail the QML test suite.
import QtQuick
import org.kde.kirigami as Kirigami
import org.kde.karton

Kirigami.ApplicationWindow {
    width: 1
    height: 1
    visible: false

    VMViewerWindow {
        domain: null
        visible: false
    }

    UsbDevicesDialog {
        domain: null
        visible: false
    }

    SettingsDialog {
        domain: null
        visible: false
    }

    AddHardwareDialog {
        domain: null
        visible: false
    }

    ErrorLogDialog {
        visible: false
    }

    NetworkDhcpDialog {
        visible: false
    }

    OvfImportDialog {
        visible: false
    }

    SnapshotsDialog {
        domain: null
        visible: false
    }
}
