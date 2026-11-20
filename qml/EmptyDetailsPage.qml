// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

import QtQuick
import org.kde.kirigami as Kirigami

// The right column of the wide layout while no VM page is open. It only keeps
// the column structure stable: the VM list keeps its own column while Karton
// connects and loads the list, instead of expanding to the whole window first
// and jumping into a two-column layout afterwards. It deliberately shows no
// welcome or connection text; connection state belongs in the list column.
Kirigami.Page {
    id: root

    // VMListPage uses this to tell this page apart from a real VM page.
    readonly property bool isEmptyDetailsPage: true

    // No title: the list column already shows "Virtual Machines", and an empty
    // second title next to it only duplicates it.
    title: ""
    padding: 0
}
