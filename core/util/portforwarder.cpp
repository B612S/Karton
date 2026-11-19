// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "util/portforwarder.h"

#include <QAbstractSocket>
#include <QHostAddress>
#include <QTcpServer>
#include <QTcpSocket>

#include "karton_debug.h"

PortForwarder::PortForwarder(QObject *parent)
    : QObject(parent)
{
}

PortForwarder::~PortForwarder()
{
    stopAll();
}

bool PortForwarder::start(quint16 hostPort, const QString &guestAddress, quint16 guestPort, bool localOnly, QString *error)
{
    if (hostPort == 0 || guestPort == 0 || guestAddress.isEmpty()) {
        if (error) {
            *error = tr("Invalid forwarding rule.");
        }
        return false;
    }

    stop(hostPort);

    auto *server = new QTcpServer(this);
    const QHostAddress bindAddress = localOnly ? QHostAddress(QHostAddress::LocalHost) : QHostAddress(QHostAddress::Any);
    if (!server->listen(bindAddress, hostPort)) {
        if (error) {
            *error = server->errorString();
        }
        delete server;
        return false;
    }

    connect(server, &QTcpServer::newConnection, this, [this, server, guestAddress, guestPort]() {
        while (QTcpSocket *client = server->nextPendingConnection()) {
            auto *remote = new QTcpSocket(client);
            // Keep the remote socket alive with the client; both are torn down
            // when either side disconnects.
            QObject::connect(client, &QTcpSocket::readyRead, remote, [client, remote] {
                remote->write(client->readAll());
            });
            QObject::connect(remote, &QTcpSocket::readyRead, client, [client, remote] {
                client->write(remote->readAll());
            });
            QObject::connect(client, &QTcpSocket::disconnected, remote, &QTcpSocket::deleteLater);
            QObject::connect(remote, &QTcpSocket::disconnected, client, &QTcpSocket::deleteLater);
            QObject::connect(remote, &QAbstractSocket::errorOccurred, client, [client](QAbstractSocket::SocketError) {
                client->disconnectFromHost();
            });
            remote->connectToHost(guestAddress, guestPort);
        }
    });

    m_servers.insert(hostPort, server);
    qCInfo(KARTON_DEBUG) << "Port forward" << hostPort << "->" << guestAddress << guestPort << (localOnly ? "(localhost)" : "(all interfaces)");
    return true;
}

void PortForwarder::stop(quint16 hostPort)
{
    const auto it = m_servers.find(hostPort);
    if (it == m_servers.end()) {
        return;
    }
    QTcpServer *server = it.value();
    m_servers.erase(it);
    if (server) {
        server->close();
        server->deleteLater();
    }
}

void PortForwarder::stopAll()
{
    const auto servers = m_servers;
    m_servers.clear();
    for (const QPointer<QTcpServer> &server : servers) {
        if (server) {
            server->close();
            server->deleteLater();
        }
    }
}

bool PortForwarder::isActive(quint16 hostPort) const
{
    const auto it = m_servers.constFind(hostPort);
    return it != m_servers.constEnd() && it.value() && it.value()->isListening();
}
