// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 Karton contributors

#pragma once

#include <QString>

class OsIconMapper
{
public:
    static QString iconName(const QString &shortOsId, const QString &osId = {});
    static QString displayName(const QString &shortOsId, const QString &osId = {});
};
