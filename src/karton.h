// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
#pragma once

#include <libvirt/libvirt.h>

#include <QJSEngine>
#include <QObject>
#include <QQmlEngine>
#include <QVector>

#include "commandrunner.h"
#include "domain.h"
#include <qqmlintegration.h>

class LibvirtMonitor;

struct _virDomain; // need this for compile issues
Q_DECLARE_OPAQUE_POINTER(_virDomain *)
Q_DECLARE_METATYPE(_virDomain *)

class Karton : public QObject
{
    Q_OBJECT
    QML_ELEMENT
    QML_SINGLETON
    Q_PROPERTY(bool connected READ connected CONSTANT)

public:
    // Deliberately not default-constructible: Qt then uses create() for the
    // QML singleton instead of constructing a second instance.
    explicit Karton(QObject *parent);
    ~Karton();
    Q_DISABLE_COPY_MOVE(Karton)
    static Karton *create(QQmlEngine *qmlEngine, QJSEngine *);
    static Karton *self();

    [[nodiscard]] bool connected() const
    {
        return m_conn != nullptr;
    }

    QVector<Domain *> domains();
    void refreshDomain(const virDomainPtr domainPtr);
    int searchDomain(virDomainPtr domainPtr);
    void refreshDomainList();
    QString getVirtualDiskPath(const QString &domainName);
    QString getXmlConfigPath(const QString &domainName);

Q_SIGNALS:
    void domainsChanged(const virDomainPtr domainPtr, int event, int detail);
    void errorOccurred(const QString &errorMessage);
    void domainAdded(Domain *domain);
    void domainRemoved(Domain *domain);

public Q_SLOTS:
    Q_INVOKABLE bool startDomain(const Domain *domain);
    Q_INVOKABLE bool stopDomain(const Domain *domain);
    Q_INVOKABLE bool forceStopDomain(const Domain *domain);
    Q_INVOKABLE bool createDomain(const QVariantMap &config);
    Q_INVOKABLE bool editDomain(const Domain *domain, const QVariantMap &config);
    Q_INVOKABLE bool deleteDomain(const Domain *domain, const bool deleteDisk);
    Q_INVOKABLE bool ejectDisk(const Domain *domain);

private Q_SLOTS:
    void onDomainStateChanged(const virDomainPtr domainPtr, int event, int detail);

private:
    virConnectPtr m_conn;
    QVector<Domain *> m_domains;
    LibvirtMonitor *m_monitor;
    CommandRunner *m_commandRunner;

    bool init();
};
