// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <libvirt/libvirt.h>

#include <QObject>
#include <QString>
#include <QStringList>
#include <QTimer>
#include <QVector>

#include <atomic>

#include "app/karton.h"
#include "domain/domainconfig.h"

class LibvirtMonitor;

// One libvirt connection and the VM list that belongs to it. Karton owns one
// object per configured URI, so every connection has its own handle, reader
// handle, reconnect loop, lifecycle monitor and VM list. Operations on a
// domain use the domain's own connection, which keeps connections
// independent of each other.
class LibvirtConnection : public QObject
{
    Q_OBJECT

public:
    enum class Status {
        Connecting,
        Connected,
        Reconnecting,
        Error,
    };
    Q_ENUM(Status)

    LibvirtConnection(Karton *karton, const QString &uri, QObject *parent = nullptr);
    ~LibvirtConnection() override;
    Q_DISABLE_COPY_MOVE(LibvirtConnection)

    [[nodiscard]] QString uri() const
    {
        return m_uri;
    }
    [[nodiscard]] QString displayName() const;
    [[nodiscard]] Status status() const
    {
        return m_status;
    }
    // Short text for the connection header: "Connecting…", "Connected",
    // "Reconnecting…", "Cannot connect".
    [[nodiscard]] QString statusText() const;
    [[nodiscard]] bool connected() const
    {
        return m_conn != nullptr;
    }
    [[nodiscard]] virConnectPtr connection() const
    {
        return m_conn;
    }
    [[nodiscard]] virConnectPtr readerConnection() const
    {
        return m_readerConn.load();
    }
    [[nodiscard]] QVector<Domain *> domains() const
    {
        return m_domains;
    }
    [[nodiscard]] int domainCount() const
    {
        return m_domains.size();
    }
    [[nodiscard]] bool hasDomain(const QString &uuid) const;
    [[nodiscard]] Domain *domainByUuid(const QString &uuid) const;

    // Starts probing/opening the connection. Safe to call repeatedly.
    void start();
    // Stops the reconnect loop and releases the libvirt handle. The domain
    // objects keep existing (without a handle) until Karton deletes the group.
    void stop();
    // Re-reads the VM list on this connection.
    void refreshDomains();
    // Reconciles domain states without recreating rows.
    void refreshStates();
    // Reorders the domains of this connection and stores the new order.
    void moveDomain(int fromRow, int toRow);

    // Drops a domain that was undefined through Karton (deleteDomain()).
    void removeDomain(Domain *domain);

Q_SIGNALS:
    // Status, VM list or domain count changed; the VM list model rebuilds.
    void changed();
    // A domain row was added/removed; Karton re-emits it for QML.
    void domainAdded(Domain *domain);
    void domainRemoved(Domain *domain);
    // A libvirt lifecycle event; Karton handles it like before.
    void domainStateChanged(virDomainPtr domainPtr, int event, int detail);

private Q_SLOTS:
    void handleConnectionClosed();

private:
    struct DomainListEntry {
        DomainConfig::DomainConfigData config;
        int state = 0;
    };

    struct DomainStateEntry {
        QString uuid;
        bool active = false;
        bool paused = false;
        int state = -1;
    };

    static void connectionCloseCallback(virConnectPtr conn, int reason, void *opaque);

    void setStatus(Status status);
    void beginProbe();
    void attach(virConnectPtr conn);
    void detach();
    void scheduleReconnect();
    void attemptReconnect();
    void startReader();
    void stopReader();
    QVector<DomainListEntry> collectDomains(virConnectPtr conn, bool *ok);
    void applyDomains(const QVector<DomainListEntry> &entries);
    void refreshDomainsSync();
    void applyStates(const QVector<DomainStateEntry> &states);

    Karton *m_karton;
    QString m_uri;
    virConnectPtr m_conn = nullptr;
    Status m_status = Status::Connecting;
    QVector<Domain *> m_domains;
    LibvirtMonitor *m_monitor = nullptr;
    bool m_closeCallbackRegistered = false;
    bool m_started = false;
    bool m_startPending = false;
    int m_reconnectAttempts = 0;
    QTimer m_reconnectTimer;
    QTimer m_stateTimer;

    std::atomic<virConnectPtr> m_readerConn{nullptr};
    std::atomic<bool> m_readerEnabled{false};
    bool m_listPending = false;
};
