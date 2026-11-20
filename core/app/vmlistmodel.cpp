// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/vmlistmodel.h"

#include <QDateTime>
#include <QSettings>
#include <QStringList>
#include <QTimer>

#include "app/karton.h"
#include "app/karton_private.h"
#include "karton_debug.h"

VMModel::VMModel(QObject *parent)
    : QAbstractListModel(parent)
    , m_karton(Karton::self())
{
    if (!m_karton) {
        qCCritical(KARTON_DEBUG) << "vmmodellist: karton is null!";
        return;
    }

    // Collapsed connection groups survive restarts.
    QSettings settings(KartonCommon::kartonConfigPath(), QSettings::IniFormat);
    const QStringList collapsed = settings.value(QStringLiteral("connections/collapsed")).toStringList();
    for (const QString &uri : collapsed) {
        m_collapsed.insert(uri, true);
    }

    connect(m_karton, &Karton::domainsChanged, this, &VMModel::onDomainsChanged);
    connect(m_karton, &Karton::domainAdded, this, [this] {
        scheduleReload();
    });
    connect(m_karton, &Karton::domainRemoved, this, [this](Domain *) {
        scheduleReload();
    });
    connect(m_karton, &Karton::connectionsChanged, this, &VMModel::onConnectionsChanged);
    connect(m_karton, &Karton::connectedChanged, this, &VMModel::onConnectionsChanged);
    connect(m_karton, &Karton::connectingChanged, this, &VMModel::onConnectionsChanged);
    connect(m_karton, &Karton::reconnectingChanged, this, &VMModel::onConnectionsChanged);

    reloadDomains();
}

VMModel::~VMModel() = default;

int VMModel::rowCount(const QModelIndex &parent) const
{
    if (parent.isValid()) {
        return 0;
    }
    return m_rows.size();
}

QVariant VMModel::data(const QModelIndex &index, int role) const
{
    if (!index.isValid() || index.row() < 0 || index.row() >= m_rows.size()) {
        return {};
    }
    const Row &row = m_rows.at(index.row());
    const QVariantMap info = m_connectionInfo.value(row.connectionUri);

    switch (role) {
    case DomainRole:
        return row.domain ? QVariant::fromValue(row.domain) : QVariant();
    case RowTypeRole:
        return static_cast<int>(row.type);
    case ConnectionUriRole:
        return row.connectionUri;
    case ConnectionStatusRole:
        return info.value(QStringLiteral("status"));
    case ConnectionStatusTextRole:
        return info.value(QStringLiteral("statusText"));
    case ConnectionDomainCountRole:
        return info.value(QStringLiteral("domainCount"));
    case ConnectionPrimaryRole:
        return info.value(QStringLiteral("primary"), false);
    case CollapsedRole:
        return isCollapsed(row.connectionUri);
    default:
        return {};
    }
}

QHash<int, QByteArray> VMModel::roleNames() const
{
    return {
        {DomainRole, "domain"},
        {RowTypeRole, "rowType"},
        {ConnectionUriRole, "connectionUri"},
        {ConnectionStatusRole, "connectionStatus"},
        {ConnectionStatusTextRole, "connectionStatusText"},
        {ConnectionDomainCountRole, "connectionDomainCount"},
        {ConnectionPrimaryRole, "connectionPrimary"},
        {CollapsedRole, "collapsed"},
    };
}

void VMModel::onDomainsChanged(const virDomainPtr domainPtr, int event, int detail)
{
    Q_UNUSED(detail);

    if (event == VIR_DOMAIN_EVENT_UNDEFINED) {
        refreshAllDomains();
        return;
    }
    if (domainPtr) {
        updateDomains(domainPtr);
    }
}

void VMModel::onDomainRemoved(Domain *domain)
{
    Q_UNUSED(domain);
    scheduleReload();
}

void VMModel::onConnectionsChanged()
{
    if (m_reordering) {
        return;
    }
    scheduleReload();
}

void VMModel::scheduleReload()
{
    if (m_reloadQueued) {
        return;
    }
    m_reloadQueued = true;
    QTimer::singleShot(0, this, [this] {
        m_reloadQueued = false;
        reloadDomains();
    });
}

void VMModel::reloadDomains()
{
    if (!m_karton) {
        return;
    }
    m_resetting = true;
    beginResetModel();
    rebuildRows();
    endResetModel();
    m_resetting = false;
}

QVector<Domain *> VMModel::domainsFor(const QString &uri) const
{
    return m_karton->domainsForConnection(uri);
}

void VMModel::rebuildRows()
{
    m_rows.clear();
    m_connectionInfo.clear();

    const QVariantList infos = m_karton->connectionInfos();
    for (const QVariant &value : infos) {
        const QVariantMap info = value.toMap();
        const QString uri = info.value(QStringLiteral("uri")).toString();
        if (uri.isEmpty()) {
            continue;
        }
        m_connectionInfo.insert(uri, info);

        QVector<Domain *> visible;
        const QVector<Domain *> domains = domainsFor(uri);
        visible.reserve(domains.size());
        for (Domain *domain : domains) {
            if (domain && matchesFilter(domain)) {
                visible.append(domain);
            }
        }
        if (hasFilter() && visible.isEmpty()) {
            continue;
        }

        Row header;
        header.type = ConnectionRow;
        header.connectionUri = uri;
        m_rows.append(header);

        if (isCollapsed(uri)) {
            continue;
        }
        for (Domain *domain : std::as_const(visible)) {
            Row row;
            row.type = DomainRow;
            row.connectionUri = uri;
            row.domain = domain;
            m_rows.append(row);
        }
    }
}

bool VMModel::matchesFilter(const Domain *domain) const
{
    if (!domain || !domain->config()) {
        return false;
    }
    if (m_filterMode == RunningVms && !domain->isActive()) {
        return false;
    }
    if (m_filterMode == StoppedVms && domain->isActive()) {
        return false;
    }
    if (m_filterText.isEmpty()) {
        return true;
    }

    const DomainConfig *config = domain->config();
    const QStringList fields = {
        config->name(),
        config->uuid(),
        config->state(),
        m_karton ? m_karton->domainOsName(domain) : QString(),
    };
    for (const QString &field : fields) {
        if (field.contains(m_filterText, Qt::CaseInsensitive)) {
            return true;
        }
    }
    return false;
}

void VMModel::refreshAllDomains()
{
    qCInfo(KARTON_DEBUG) << "vmlistmodel: Refreshing all domains.";
    if (m_karton) {
        m_karton->refreshDomainList();
    }
    reloadDomains();
}

void VMModel::updateDomains(const virDomainPtr domainPtr)
{
    const QString uuid = Domain::uuidString(domainPtr);
    if (uuid.isEmpty()) {
        scheduleReload();
        return;
    }

    int rowIndex = -1;
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).domain && m_rows.at(i).domain->config()->uuid() == uuid) {
            rowIndex = i;
            break;
        }
    }

    if (rowIndex < 0) {
        // The domain is new (or hidden by a filter); rebuild once the list is
        // refreshed instead of inserting a row with a stale connection.
        scheduleReload();
        return;
    }

    m_karton->refreshDomain(domainPtr);

    if (hasFilter()) {
        beginResetModel();
        rebuildRows();
        endResetModel();
    } else {
        const QModelIndex index = createIndex(rowIndex, 0);
        Q_EMIT dataChanged(index, index);
    }
}

void VMModel::setFilterText(const QString &text)
{
    const QString trimmed = text.trimmed();
    if (m_filterText == trimmed) {
        return;
    }
    m_filterText = trimmed;
    applyFilter();
}

void VMModel::setFilterMode(int mode)
{
    const int bounded = qBound(int(AllVms), mode, int(StoppedVms));
    if (m_filterMode == bounded) {
        return;
    }
    m_filterMode = bounded;
    applyFilter();
}

void VMModel::applyFilter()
{
    beginResetModel();
    rebuildRows();
    endResetModel();
    Q_EMIT filterChanged();
}

bool VMModel::moveItem(int from, int to)
{
    if (from == to || from < 0 || to < 0 || from >= m_rows.size() || to >= m_rows.size()) {
        return false;
    }
    const Row fromRow = m_rows.at(from);
    const Row toRow = m_rows.at(to);
    if (!fromRow.domain || !toRow.domain || fromRow.connectionUri != toRow.connectionUri) {
        // Reordering across connection groups would move a VM to another
        // libvirt connection; only reorder within one group.
        return false;
    }

    // Domain indices inside the connection group (headers excluded).
    int fromDomain = -1;
    int toDomain = -1;
    int domainIndex = 0;
    for (int i = 0; i < m_rows.size(); ++i) {
        const Row &row = m_rows.at(i);
        if (row.connectionUri != fromRow.connectionUri || row.type != DomainRow) {
            continue;
        }
        if (i == from) {
            fromDomain = domainIndex;
        }
        if (i == to) {
            toDomain = domainIndex;
        }
        ++domainIndex;
    }
    if (fromDomain < 0 || toDomain < 0) {
        return false;
    }

    const bool ascending = to > from;
    m_reordering = true;
    beginMoveRows(QModelIndex(), from, from, QModelIndex(), ascending ? to + 1 : to);
    m_rows.move(from, to);
    endMoveRows();
    m_karton->moveDomain(fromRow.connectionUri, fromDomain, toDomain);
    m_reordering = false;
    return true;
}

int VMModel::rowOfUuid(const QString &uuid) const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).domain && m_rows.at(i).domain->config()->uuid() == uuid) {
            return i;
        }
    }
    return -1;
}

bool VMModel::toggleConnection(const QString &uri)
{
    if (uri.isEmpty()) {
        return false;
    }
    setCollapsed(uri, !isCollapsed(uri));
    reloadDomains();
    return true;
}

int VMModel::firstDomainRow() const
{
    for (int i = 0; i < m_rows.size(); ++i) {
        if (m_rows.at(i).type == DomainRow) {
            return i;
        }
    }
    return -1;
}

Domain *VMModel::domainAtRow(int row) const
{
    if (row < 0 || row >= m_rows.size()) {
        return nullptr;
    }
    return m_rows.at(row).domain;
}

bool VMModel::isCollapsed(const QString &uri) const
{
    return m_collapsed.value(uri, false);
}

void VMModel::setCollapsed(const QString &uri, bool collapsed)
{
    if (isCollapsed(uri) == collapsed) {
        return;
    }
    if (collapsed) {
        m_collapsed.insert(uri, true);
    } else {
        m_collapsed.remove(uri);
    }

    QSettings settings(KartonCommon::kartonConfigPath(), QSettings::IniFormat);
    QStringList collapsedUris;
    for (auto it = m_collapsed.cbegin(); it != m_collapsed.cend(); ++it) {
        if (it.value()) {
            collapsedUris.append(it.key());
        }
    }
    collapsedUris.sort();
    settings.setValue(QStringLiteral("connections/collapsed"), collapsedUris);
}
