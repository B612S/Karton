// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2025 Derek Lin <derekhongdalin@gmail.com>

#pragma once

#include <domainconfig.h>
#include <libvirt/libvirt.h>

#include <QImage>
#include <QObject>
#include <QQmlEngine>
#include <QString>

class Domain : public QObject
{
    Q_OBJECT
    QML_ELEMENT

    Q_PROPERTY(bool isActive READ isActive NOTIFY isActiveChanged)
    Q_PROPERTY(bool isPaused READ isPaused NOTIFY isPausedChanged)
    Q_PROPERTY(bool hasSavedState READ hasSavedState NOTIFY hasSavedStateChanged)
    Q_PROPERTY(QString state READ state NOTIFY stateChanged)
    Q_PROPERTY(int ramUsage READ ramUsage NOTIFY ramUsageChanged)
    Q_PROPERTY(bool autostart READ autostart NOTIFY autostartChanged)
    Q_PROPERTY(DomainConfig *config READ config CONSTANT)
    Q_PROPERTY(QImage previewFrame READ previewFrame NOTIFY previewFrameChanged)

Q_SIGNALS:
    void isActiveChanged(const bool active);
    void isPausedChanged(const bool paused);
    void hasSavedStateChanged(const bool hasSavedState);
    void stateChanged(const QString &state);
    void ramUsageChanged(const int usage);
    void autostartChanged(const bool autostart);
    void previewChanged();
    void previewFrameChanged();

public:
    Domain(QObject *parent = nullptr);
    Domain(const virDomainPtr domainPtr, DomainConfig *config, QObject *parent = nullptr);
    Q_DISABLE_COPY_MOVE(Domain);
    ~Domain() override;

    // getters
    [[nodiscard]] virDomainPtr domainPtr() const
    {
        return m_domainPtr;
    }

    [[nodiscard]] DomainConfig *config() const
    {
        return m_config;
    }

    [[nodiscard]] QImage previewFrame() const
    {
        return m_previewFrame;
    }

    [[nodiscard]] bool isActive() const
    {
        return m_config->isActive();
    }
    [[nodiscard]] bool isPaused() const
    {
        return m_config->isPaused();
    }
    [[nodiscard]] bool hasSavedState() const
    {
        return m_config->hasSavedState();
    }
    [[nodiscard]] int ramUsage() const
    {
        return m_config->ramUsage();
    }
    [[nodiscard]] QString state() const
    {
        return m_config->state();
    }
    [[nodiscard]] bool autostart() const
    {
        return m_config->autostart();
    }

    // Takes ownership of domainPtr and releases the previously held reference.
    void setDomainPtr(virDomainPtr domainPtr);
    void apply(const DomainConfig::DomainConfigData &data);

    void setActive(bool active);
    void setPaused(bool paused);
    void setHasSavedState(bool hasSavedState);
    void setState(const QString &state);
    void setRamUsage(int ramUsage);
    void setAutostart(bool autostart);
    static QString uuidString(virDomainPtr domainPtr);
    void savePreviewFrame(QImage frame);
    void updateLivePreviewFrame(QImage frame);

private:
    virDomainPtr m_domainPtr;
    DomainConfig *m_config;
    QImage m_previewFrame;
};
