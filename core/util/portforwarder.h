// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#pragma once

#include <QHash>
#include <QObject>
#include <QPointer>
#include <QString>

class QTcpServer;

// User-space TCP port forwarding for NAT guests. libvirt does not manage
// destination NAT rules, so Karton forwards host TCP connections to the
// guest address itself. No root privileges are required for ports >= 1024.
class PortForwarder : public QObject
{
    Q_OBJECT

public:
    explicit PortForwarder(QObject *parent = nullptr);
    ~PortForwarder() override;

    bool start(quint16 hostPort, const QString &guestAddress, quint16 guestPort, bool localOnly, QString *error = nullptr);
    void stop(quint16 hostPort);
    void stopAll();
    bool isActive(quint16 hostPort) const;

Q_SIGNALS:
    void errorOccurred(const QString &message);

private:
    QHash<quint16, QPointer<QTcpServer>> m_servers;
};
