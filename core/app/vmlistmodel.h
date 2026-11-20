// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <libvirt/libvirt.h>

#include <QAbstractListModel>
#include <QByteArray>
#include <QHash>
#include <QModelIndex>
#include <QObject>
#include <QString>
#include <QVariant>
#include <QVector>

#include "app/karton.h"
#include "domain/domain.h"

// Flat list of VM list rows: one header per libvirt connection followed by its
// domains. Headers can be collapsed, carry the connection status and can be
// right-clicked for the connection menu; domain rows open the VM page and can
// be right-clicked for the VM menu.
class VMModel : public QAbstractListModel
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON

    Q_PROPERTY(QString filterText READ filterText WRITE setFilterText NOTIFY filterChanged)
    Q_PROPERTY(int filterMode READ filterMode WRITE setFilterMode NOTIFY filterChanged)
    Q_PROPERTY(bool hasFilter READ hasFilter NOTIFY filterChanged)

public:
    enum Roles {
        DomainRole = Qt::UserRole,
        RowTypeRole,
        ConnectionUriRole,
        ConnectionStatusRole,
        ConnectionStatusTextRole,
        ConnectionDomainCountRole,
        ConnectionPrimaryRole,
        CollapsedRole,
    };
    // Keep the values in sync with the VM list page's row delegates.
    enum RowType {
        ConnectionRow = 0,
        DomainRow = 1,
    };
    Q_ENUM(RowType)
    // Keep the values in sync with the VM list page's filter combo box.
    enum FilterMode {
        AllVms = 0,
        RunningVms = 1,
        StoppedVms = 2,
    };
    Q_ENUM(FilterMode)

    VMModel(QObject *parent = nullptr);
    ~VMModel() override;
    Q_DISABLE_COPY_MOVE(VMModel)

    int rowCount(const QModelIndex &parent = QModelIndex()) const override;
    QHash<int, QByteArray> roleNames() const override;
    QVariant data(const QModelIndex &index, int role = Qt::DisplayRole) const override;
    void updateDomains(const virDomainPtr domainPtr = nullptr);
    void refreshAllDomains();
    // Reorders two domain rows of the same connection. Returns false when the
    // rows belong to different connection groups (or are not domain rows), so
    // the drag code can keep dragging the original row.
    Q_INVOKABLE bool moveItem(int from, int to);
    Q_INVOKABLE int rowOfUuid(const QString &uuid) const;
    Q_INVOKABLE bool toggleConnection(const QString &uri);
    Q_INVOKABLE int firstDomainRow() const;
    Q_INVOKABLE Domain *domainAtRow(int row) const;

    QString filterText() const
    {
        return m_filterText;
    }
    void setFilterText(const QString &text);
    int filterMode() const
    {
        return m_filterMode;
    }
    void setFilterMode(int mode);
    bool hasFilter() const
    {
        return !m_filterText.isEmpty() || m_filterMode != AllVms;
    }

Q_SIGNALS:
    void filterChanged();

private Q_SLOTS:
    void onDomainsChanged(const virDomainPtr domainPtr, int event, int detail);
    void onDomainRemoved(Domain *domain);
    void onConnectionsChanged();

private:
    struct Row {
        RowType type = ConnectionRow;
        QString connectionUri;
        Domain *domain = nullptr;
    };

    void applyFilter();
    void reloadDomains();
    void scheduleReload();
    void rebuildRows();
    QVector<Domain *> domainsFor(const QString &uri) const;
    bool matchesFilter(const Domain *domain) const;
    bool isCollapsed(const QString &uri) const;
    void setCollapsed(const QString &uri, bool collapsed);

    QVector<Row> m_rows;
    QHash<QString, QVariantMap> m_connectionInfo;
    QHash<QString, bool> m_collapsed;
    QString m_filterText;
    int m_filterMode = AllVms;
    Karton *m_karton;
    bool m_resetting = false;
    bool m_reloadQueued = false;
    bool m_reordering = false;
};
