// SPDX-License-Identifier: GPL-3.0-or-later
// SPDX-FileCopyrightText: 2026 B612S <sem.alimov@inbox.ru>

#include "app/shortcutinhibitor.h"

#include <QGuiApplication>
#include <QWindow>

#if QT_CONFIG(wayland)
#include <QtGui/qguiapplication_platform.h>
#include <wayland-client.h>

#include "keyboard-shortcuts-inhibit-unstable-v1-client-protocol.h"
#endif

#include <algorithm>
#include <cstring>

class ShortcutInhibitor::Private
{
public:
    QWindow *window = nullptr;
    bool inhibited = false;
    bool x11Grabbed = false;

#if QT_CONFIG(wayland)
    wl_display *display = nullptr;
    wl_registry *registry = nullptr;
    zwp_keyboard_shortcuts_inhibit_manager_v1 *manager = nullptr;
    zwp_keyboard_shortcuts_inhibitor_v1 *inhibitor = nullptr;

    static void global(void *data, wl_registry *registry, uint32_t name, const char *interface, uint32_t version)
    {
        auto *self = static_cast<Private *>(data);
        if (std::strcmp(interface, zwp_keyboard_shortcuts_inhibit_manager_v1_interface.name) == 0) {
            self->manager = static_cast<zwp_keyboard_shortcuts_inhibit_manager_v1 *>(
                wl_registry_bind(registry, name, &zwp_keyboard_shortcuts_inhibit_manager_v1_interface, std::min(version, 1u)));
        }
    }

    static void globalRemove(void *, wl_registry *, uint32_t)
    {
    }

    void releaseWayland()
    {
        if (inhibitor) {
            zwp_keyboard_shortcuts_inhibitor_v1_destroy(inhibitor);
            inhibitor = nullptr;
        }
        if (manager) {
            zwp_keyboard_shortcuts_inhibit_manager_v1_destroy(manager);
            manager = nullptr;
        }
        if (registry) {
            wl_registry_destroy(registry);
            registry = nullptr;
        }
        display = nullptr;
    }
#endif

    void releaseX11()
    {
        if (x11Grabbed && window) {
            window->setKeyboardGrabEnabled(false);
        }
        x11Grabbed = false;
    }
};

ShortcutInhibitor::ShortcutInhibitor(QObject *parent)
    : QObject(parent)
    , d(std::make_unique<Private>())
{
}

ShortcutInhibitor::~ShortcutInhibitor()
{
    release();
}

void ShortcutInhibitor::setWindow(QWindow *window)
{
    if (d->window == window) {
        return;
    }
    release();
    d->window = window;
}

bool ShortcutInhibitor::setInhibited(bool inhibited)
{
    if (inhibited == d->inhibited) {
        return d->inhibited;
    }
    if (!inhibited) {
        release();
        return false;
    }
    if (!d->window) {
        return false;
    }

    const QString platform = QGuiApplication::platformName();
#if QT_CONFIG(wayland)
    if (platform == QStringLiteral("wayland")) {
        auto *wayland = qGuiApp->nativeInterface<QNativeInterface::QWaylandApplication>();
        if (wayland) {
            d->display = wayland->display();
            if (d->display) {
                d->registry = wl_display_get_registry(d->display);
                static const wl_registry_listener listener = {Private::global, Private::globalRemove};
                wl_registry_add_listener(d->registry, &listener, d.get());
                wl_display_roundtrip(d->display);
                auto *surface = reinterpret_cast<wl_surface *>(d->window->winId());
                if (d->manager && surface) {
                    d->inhibitor = zwp_keyboard_shortcuts_inhibit_manager_v1_inhibit_shortcuts(d->manager, surface, wayland->seat());
                    wl_display_roundtrip(d->display);
                    if (d->inhibitor) {
                        d->inhibited = true;
                        return true;
                    }
                }
                d->releaseWayland();
            }
        }
    }
#endif

    if (platform == QStringLiteral("xcb")) {
        d->window->setKeyboardGrabEnabled(true);
        d->x11Grabbed = true;
        d->inhibited = true;
        return true;
    }

    return false;
}

void ShortcutInhibitor::release()
{
#if QT_CONFIG(wayland)
    d->releaseWayland();
#endif
    d->releaseX11();
    d->inhibited = false;
}
