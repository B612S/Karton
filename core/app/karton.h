// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>
#pragma once

#include <libvirt/libvirt.h>

#include <QHash>
#include <QJSEngine>
#include <QObject>
#include <QPointer>
#include <QQmlEngine>
#include <QVariantList>
#include <QVariantMap>
#include <QVector>

#include "domain/domain.h"
#include "util/commandrunner.h"
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
    Q_PROPERTY(QString connectionUri READ connectionUri CONSTANT)
    Q_PROPERTY(int viewerCount READ viewerCount NOTIFY viewerCountChanged)
    Q_PROPERTY(bool guestAgentWarningDismissed READ guestAgentWarningDismissed WRITE setGuestAgentWarningDismissed NOTIFY guestAgentWarningDismissedChanged)

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
    [[nodiscard]] QString connectionUri() const;
    [[nodiscard]] int viewerCount() const
    {
        return m_viewerCount;
    }
    [[nodiscard]] bool guestAgentWarningDismissed() const
    {
        return m_guestAgentWarningDismissed;
    }
    void setGuestAgentWarningDismissed(bool dismissed);

    QVector<Domain *> domains();
    void refreshDomain(const virDomainPtr domainPtr);
    int searchDomain(virDomainPtr domainPtr);
    void refreshDomainList();
    QString getVirtualDiskPath(const QString &domainName, const QString &format = QStringLiteral("qcow2"));
    QString getXmlConfigPath(const QString &domainName);
    int diskSizeGB(const QString &path);

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

Q_SIGNALS:
    void closePopupsRequested();
    void domainsChanged(const virDomainPtr domainPtr, int event, int detail);
    void errorOccurred(const QString &errorMessage);
    void clipboardSettingsChanged();
    void domainAdded(Domain *domain);
    void domainRemoved(Domain *domain);
    void domainCreated(bool success, const QString &name);
    void domainEdited(bool success, const QString &name);
    void domainCloned(bool success, const QString &sourceName, const QString &newName);
    void viewerCountChanged();
    void guestAgentWarningDismissedChanged();

public Q_SLOTS:
    Q_INVOKABLE bool startDomain(const Domain *domain);
    Q_INVOKABLE bool stopDomain(const Domain *domain);
    Q_INVOKABLE bool forceStopDomain(const Domain *domain);
    Q_INVOKABLE bool pauseDomain(const Domain *domain);
    Q_INVOKABLE bool resumeDomain(const Domain *domain);
    Q_INVOKABLE bool rebootDomain(const Domain *domain);
    Q_INVOKABLE bool resetDomain(const Domain *domain);
    Q_INVOKABLE bool saveDomainState(const Domain *domain);
    Q_INVOKABLE bool deleteSavedState(const Domain *domain);
    Q_INVOKABLE QVariantMap domainStats(const Domain *domain);
    Q_INVOKABLE QString domainOsIcon(const Domain *domain) const;
    Q_INVOKABLE QString domainOsName(const Domain *domain) const;
    Q_INVOKABLE QVariantMap mainWindowSize() const;
    Q_INVOKABLE void saveMainWindowSize(int width, int height);
    Q_INVOKABLE QVariantMap viewerWindowSize(const Domain *domain) const;
    Q_INVOKABLE void saveViewerWindowSize(const Domain *domain, int width, int height);
    Q_INVOKABLE int clipboardTextMode() const;
    Q_INVOKABLE void saveClipboardTextMode(int mode);
    Q_INVOKABLE int clipboardFilesMode() const;
    Q_INVOKABLE void saveClipboardFilesMode(int mode);
    Q_INVOKABLE bool spiceTlsEnabled() const;
    Q_INVOKABLE void saveSpiceTlsEnabled(bool enabled);
    Q_INVOKABLE QString spiceCaCertificate() const;
    Q_INVOKABLE void saveSpiceCaCertificate(const QString &path);
    Q_INVOKABLE bool audioPlaybackEnabled(const Domain *domain) const;
    Q_INVOKABLE void saveAudioPlaybackEnabled(const Domain *domain, bool enabled);
    Q_INVOKABLE bool microphoneEnabled(const Domain *domain) const;
    Q_INVOKABLE void saveMicrophoneEnabled(const Domain *domain, bool enabled);
    Q_INVOKABLE QVariantMap domainDisplayResolution(const Domain *domain) const;
    Q_INVOKABLE void saveDomainDisplayResolution(const Domain *domain, int width, int height);
    Q_INVOKABLE QString domainXml(const Domain *domain) const;
    Q_INVOKABLE QVariantList domainXmlDevices(const Domain *domain) const;
    Q_INVOKABLE QVariantList domainInputDevices(const Domain *domain) const;
    Q_INVOKABLE bool attachInputDevice(const Domain *domain, const QString &type, const QString &bus);
    Q_INVOKABLE bool detachInputDevice(const Domain *domain, const QString &type, const QString &bus);
    Q_INVOKABLE bool detachXmlDevice(const Domain *domain, int index);
    Q_INVOKABLE bool setXmlDevice(const Domain *domain, const QString &tag, const QString &xml);
    Q_INVOKABLE bool replaceXmlDevice(const Domain *domain, int index, const QString &xml);
    Q_INVOKABLE bool attachXmlDevice(const Domain *domain, const QString &xml);
    Q_INVOKABLE bool addController(const Domain *domain, const QString &type);
    Q_INVOKABLE bool addSerialPort(const Domain *domain);
    Q_INVOKABLE bool addTextConsole(const Domain *domain);
    Q_INVOKABLE bool addChannel(const Domain *domain, const QString &name);
    Q_INVOKABLE bool saveDomainXml(const Domain *domain, const QString &xml);
    Q_INVOKABLE QVariantMap domainHardwareInfo(const Domain *domain) const;
    Q_INVOKABLE bool editDomainHardware(const Domain *domain, const QVariantMap &settings);
    Q_INVOKABLE bool refreshDomainPreview(const Domain *domain);
    Q_INVOKABLE void viewerOpened();
    Q_INVOKABLE void viewerClosed();
    Q_INVOKABLE QObject *viewerForDomain(Domain *domain) const;
    Q_INVOKABLE void registerViewer(Domain *domain, QObject *window);
    Q_INVOKABLE void unregisterViewer(QObject *window);
    Q_INVOKABLE bool createDomain(const QVariantMap &config);
    Q_INVOKABLE bool editDomain(const Domain *domain, const QVariantMap &config);
    Q_INVOKABLE bool deleteDomain(const Domain *domain, const bool deleteDisk);
    Q_INVOKABLE bool ejectDisk(const Domain *domain);
    Q_INVOKABLE void saveConnectionUri(const QString &uri);
    Q_INVOKABLE bool setAutostart(const Domain *domain, bool autostart);
    Q_INVOKABLE QVariantList networks() const;
    Q_INVOKABLE bool startNetwork(const QString &name);
    Q_INVOKABLE bool stopNetwork(const QString &name);
    Q_INVOKABLE bool setNetworkAutostart(const QString &name, bool autostart);
    Q_INVOKABLE bool createNatNetwork(const QString &name);
    Q_INVOKABLE bool deleteNetwork(const QString &name);
    Q_INVOKABLE QVariantList domainInterfaces(const Domain *domain) const;
    Q_INVOKABLE bool attachNetwork(const Domain *domain, const QString &network, const QString &model);
    Q_INVOKABLE bool detachNetwork(const Domain *domain, const QString &mac);
    Q_INVOKABLE bool editNetworkInterface(const Domain *domain, const QString &mac, const QString &newMac, const QString &model);
    Q_INVOKABLE QVariantList usbDevices() const;
    Q_INVOKABLE QVariantList domainUsbDevices(const Domain *domain) const;
    Q_INVOKABLE bool attachUsbDevice(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE bool detachUsbDevice(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE bool attachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE bool detachUsbDevicePersistent(const Domain *domain, const QString &vendor, const QString &product, const QString &bus, const QString &device);
    Q_INVOKABLE QVariantList domainSnapshots(const Domain *domain) const;
    Q_INVOKABLE bool createSnapshot(const Domain *domain, const QString &name, const QString &description, bool includeMemory);
    Q_INVOKABLE bool deleteSnapshot(const Domain *domain, const QString &name);
    Q_INVOKABLE bool revertSnapshot(const Domain *domain, const QString &name);
    Q_INVOKABLE QVariantList pciDevices() const;
    Q_INVOKABLE QVariantList domainPciDevices(const Domain *domain) const;
    Q_INVOKABLE bool attachPciDevice(const Domain *domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function);
    Q_INVOKABLE bool detachPciDevice(const Domain *domain, const QString &pciDomain, const QString &bus, const QString &slot, const QString &function);
    Q_INVOKABLE bool cloneDomain(const Domain *domain, const QString &newName);
    Q_INVOKABLE QVariantList domainDisks(const Domain *domain) const;
    Q_INVOKABLE bool addDisk(const Domain *domain, int sizeGB, const QString &format);
    Q_INVOKABLE bool attachDiskImage(const Domain *domain, const QString &path, const QString &format);
    Q_INVOKABLE bool attachIso(const Domain *domain, const QString &path);
    Q_INVOKABLE bool detachDisk(const Domain *domain, const QString &targetDev, bool deleteFile);
    Q_INVOKABLE QVariantMap domainDiskAdvanced(const Domain *domain, const QString &targetDev) const;
    Q_INVOKABLE bool editDomainDiskAdvanced(const Domain *domain, const QString &targetDev, const QVariantMap &settings);

Q_SIGNALS:
    void networksChanged();

private Q_SLOTS:
    void onDomainStateChanged(const virDomainPtr domainPtr, int event, int detail);

private:
    virConnectPtr m_conn;
    QString m_activeUri;
    QVector<Domain *> m_domains;
    LibvirtMonitor *m_monitor;
    CommandRunner *m_commandRunner;
    QHash<QString, int> m_diskSizeCache;

    struct CpuSample {
        qint64 cpuTime = 0;
        qint64 timestamp = 0;
    };
    QHash<QString, CpuSample> m_cpuSamples;
    int m_viewerCount = 0;
    bool m_guestAgentWarningDismissed = false;
    QHash<QString, QPointer<QObject>> m_viewers;
    bool m_previewCapturing = false;

    bool init();
};
