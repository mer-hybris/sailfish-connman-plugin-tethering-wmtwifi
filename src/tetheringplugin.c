/*
 *  Connection Manager plugin
 *
 *  Copyright (C) 2018 Jolla Ltd.
 *
 *  This program is free software; you can redistribute it and/or modify
 *  it under the terms of the GNU General Public License version 2 as
 *  published by the Free Software Foundation.
 *
 *  This program is distributed in the hope that it will be useful,
 *  but WITHOUT ANY WARRANTY; without even the implied warranty of
 *  MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *  GNU General Public License for more details.
 */

#include <connman/notifier.h>
#include <connman/plugin.h>
#include <connman/log.h>

#include <gsupplicant.h>
#include <gsupplicant_interface.h>

#include <gutil_strv.h>

#include <sys/socket.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <errno.h>

#define CMD_DEV_NODE "/dev/wmtWifi"
#define CMD_AP_MODE 'A'
#define CMD_STA_MODE 'S'

#define WAIT_TIMEOUT_MS (1000)

enum {
    SUPPLICANT_EVENT_VALID,
    SUPPLICANT_EVENT_INTERFACES,
    SUPPLICANT_EVENT_COUNT
};

enum {
    INTERFACE_EVENT_VALID,
    INTERFACE_EVENT_CAPS,
    INTERFACE_EVENT_COUNT
};

/*
 * Here is how it's supposed to work:
 *
 * Tethering on
 * ============
 *
 * 1. Write "A" to /dev/wmtWifi
 * 2. Wait for AP-capable interface to show up
 * 3. Tell wpa_supplicant to remove all other WiFi interfaces
 *
 * Then we can proceed and let sailfish_wifi plugin to finish the job.
 *
 * Tethering off
 * =============
 *
 * 1. Just write "S" to /dev/wmtWifi
 *
 * The rest should happen more or less by itself.
 */

typedef
gboolean
(*TetheringWaitCheckFunc)(
    GHashTable* ifaces);

typedef struct tethering_wait_supplicant {
    guint timeout_id;
    gulong supplicant_events[SUPPLICANT_EVENT_COUNT];
    GSupplicant* supplicant;
    GHashTable* ifaces;
    GMainLoop* loop;
    TetheringWaitCheckFunc check;
} TetheringWait;

typedef struct tethering_wait_interface {
    GSupplicantInterface* supplicant_interface;
    gulong interface_events[INTERFACE_EVENT_COUNT];
} TetheringWaitInterface;

static GSupplicant* tethering_supplicant;

static
gboolean
tethering_wifi_timeout(
    gpointer user_data)
{
    TetheringWait* wait = user_data;

    DBG("Wait timed out, continuing anyway");
    wait->timeout_id = 0;
    g_main_loop_quit(wait->loop);
    return G_SOURCE_REMOVE;
}

static
void
tethering_wifi_check(
    TetheringWait* wait)
{
    if (!wait->check || wait->check(wait->ifaces)) {
        g_main_loop_quit(wait->loop);
    }
}

static
void
tethering_wifi_interface_event_handler(
    GSupplicantInterface* iface,
    GSUPPLICANT_INTERFACE_PROPERTY property,
    void* user_data)
{
    tethering_wifi_check((TetheringWait*)user_data);
}

static
void
tethering_wifi_interface_free(
    gpointer user_data)
{
    TetheringWaitInterface* iface = user_data;

    gsupplicant_interface_remove_all_handlers(iface->supplicant_interface,
        iface->interface_events);
    gsupplicant_interface_unref(iface->supplicant_interface);
    g_slice_free(TetheringWaitInterface, iface);
}

static
TetheringWaitInterface*
tethering_wifi_interface_new(
    TetheringWait* wait,
    const char* path)
{
    TetheringWaitInterface* iface = g_slice_new0(TetheringWaitInterface);

    iface->supplicant_interface = gsupplicant_interface_new(path);
    iface->interface_events[INTERFACE_EVENT_VALID] =
        gsupplicant_interface_add_property_changed_handler(iface->
            supplicant_interface, GSUPPLICANT_INTERFACE_PROPERTY_VALID,
            tethering_wifi_interface_event_handler, wait);
    iface->interface_events[INTERFACE_EVENT_CAPS] =
        gsupplicant_interface_add_property_changed_handler(iface->
            supplicant_interface, GSUPPLICANT_INTERFACE_PROPERTY_CAPS,
            tethering_wifi_interface_event_handler, wait);
    return iface;
}

static
void
tethering_wifi_wait_update_interfaces(
    TetheringWait* wait)
{
    GSupplicant* supplicant = wait->supplicant;

    if (supplicant->valid) {
        const GStrV* ifaces = wait->supplicant->interfaces;

        if (ifaces) {
            GHashTableIter it;
            gpointer key;

            /* Remove non-existent interfaces */
            g_hash_table_iter_init(&it, wait->ifaces);
            while (g_hash_table_iter_next(&it, &key, NULL)) {
                if (!gutil_strv_contains(ifaces, (const char*)key)) {
                    g_hash_table_iter_remove(&it);
                }
            }

            /* Add new ones */
            while (*ifaces) {
                const char* path = *ifaces++;

                if (!g_hash_table_contains(wait->ifaces, path)) {
                    g_hash_table_insert(wait->ifaces, g_strdup(path),
                        tethering_wifi_interface_new(wait, path));
                }
            }
        }
    }
}

static
void
tethering_wifi_supplicant_event_handler(
    GSupplicant* supplicant,
    GSUPPLICANT_PROPERTY property,
    void* user_data)
{
    TetheringWait* wait = user_data;

    tethering_wifi_wait_update_interfaces(wait);
    tethering_wifi_check(wait);
}

static
void
tethering_wait(
    TetheringWaitCheckFunc check)
{
    static TetheringWait* tethering_waiting = NULL;

    if (tethering_waiting) {
        /* We shouldn't recurse but just in case... */
        DBG("Already waiting!");
        tethering_waiting->check = check;
        if (!check) {
            g_main_loop_quit(tethering_waiting->loop);
        }
    } else if (check) {
        TetheringWait wait;

        /* Initialize part of the wait context (without even loop) */
        wait.supplicant = gsupplicant_ref(tethering_supplicant);
        wait.ifaces = g_hash_table_new_full(g_str_hash, g_str_equal, g_free,
            tethering_wifi_interface_free);

        /* See what's known already */
        tethering_wifi_wait_update_interfaces(&wait);
        if (!wait.supplicant->valid || !check(wait.ifaces)) {
            /* We need to wait */
            wait.check = check;
            wait.loop = g_main_loop_new(NULL, TRUE);
            wait.timeout_id = g_timeout_add(WAIT_TIMEOUT_MS,
                tethering_wifi_timeout, &wait);

            /* Register event handlers */
            wait.supplicant_events[SUPPLICANT_EVENT_VALID] =
                gsupplicant_add_property_changed_handler(wait.supplicant,
                    GSUPPLICANT_PROPERTY_VALID,
                    tethering_wifi_supplicant_event_handler, &wait);
            wait.supplicant_events[SUPPLICANT_EVENT_INTERFACES] =
                gsupplicant_add_property_changed_handler(wait.supplicant,
                    GSUPPLICANT_PROPERTY_INTERFACES,
                    tethering_wifi_supplicant_event_handler, &wait);

            /* Run nested event loop */
            DBG("Waiting...");
            tethering_waiting = &wait;
            g_main_loop_run(wait.loop);
            tethering_waiting = NULL;
            DBG("Done waiting");

            /* tethering_wifi_timeout() may zero wait.timeout_id */
            if (wait.timeout_id) {
                g_source_remove(wait.timeout_id);
            }
            g_main_loop_unref(wait.loop);
            gsupplicant_remove_all_handlers(wait.supplicant,
                wait.supplicant_events);
        }

        /* Clean things up */
        g_hash_table_destroy(wait.ifaces);
        gsupplicant_unref(wait.supplicant);
    }
}

static
gboolean
tethering_check_ap(
    GHashTable* ifaces)
{
    GHashTableIter it;
    gpointer value;
    const char* ap_ifname = NULL;
    gboolean all_valid = TRUE;

    g_hash_table_iter_init(&it, ifaces);
    while (g_hash_table_iter_next(&it, NULL, &value) && all_valid) {
        TetheringWaitInterface* iface = value;
        GSupplicantInterface* i = iface->supplicant_interface;

        if (!i->valid) {
            all_valid = FALSE;
        } else if (i->caps.modes & GSUPPLICANT_INTERFACE_CAPS_MODES_AP) {
            ap_ifname = i->ifname;
        }
    }

    if (all_valid && ap_ifname) {
        /* Bring down non-AP interfaces */
        g_hash_table_iter_init(&it, ifaces);
        while (g_hash_table_iter_next(&it, NULL, &value)) {
            TetheringWaitInterface* iface = value;
            GSupplicantInterface* i = iface->supplicant_interface;

            if (i->valid) {
                if (strcmp(i->ifname, ap_ifname)) {
                    // Tell wps_supplicant to remove all other interfaces
                    DBG("Removing %s (%s)", i->path, i->ifname);
                    gsupplicant_remove_interface(i->supplicant, i->path,
                        NULL, NULL);
                } else {
                    DBG("%s (%s) is the AP interface", i->path, i->ifname);
                }
            }
        }
        return TRUE;
    } else {
        return FALSE;
    }
}

static
gboolean
tethering_command(
    const char cmd)
{
    gboolean ok = FALSE;
    int fd = open(CMD_DEV_NODE, O_RDWR | O_SYNC);

    if (fd >= 0) {
        ssize_t written = write(fd, &cmd, 1);

        if (written == 1) {
            ok = TRUE;
        } else if (written < 0) {
            DBG("Error writing \"%c\" command to %s: %s", cmd, CMD_DEV_NODE,
                strerror(errno));
        } else {
            DBG("Failed to write \"%c\" command to %s", cmd, CMD_DEV_NODE);
        }
        close(fd);
    } else {
        DBG("Failed to open %s: %s", CMD_DEV_NODE, strerror(errno));
    }
    return ok;
}

static
void
tethering_changed_notify(
    struct connman_technology* tech,
    bool on)
{
    if (on) {
        DBG("Tethering on");
        if (tethering_command(CMD_AP_MODE)) {
            tethering_wait(tethering_check_ap);
        }
    } else {
        DBG("Tethering off");
        if (tethering_command(CMD_STA_MODE)) {
            tethering_wait(NULL);
        }
    }
}

struct connman_notifier tethering_plugin_notifier = {
    .name = "wmtWifi tethering notifier",
    .tethering_changed = tethering_changed_notify
};

static
int
tethering_plugin_init(
    void)
{
    connman_info("Initializing wmtWifi tethering plugin.");
    tethering_supplicant = gsupplicant_new();
    connman_notifier_register(&tethering_plugin_notifier);
    return 0;
}

static
void
tethering_plugin_exit(
    void)
{
    DBG("");
    gsupplicant_unref(tethering_supplicant);
    tethering_supplicant = NULL;
    connman_notifier_unregister(&tethering_plugin_notifier);
}

CONNMAN_PLUGIN_DEFINE(tethering_plugin, "wmtWifi tethering plugin",
    CONNMAN_VERSION, CONNMAN_PLUGIN_PRIORITY_DEFAULT,
    tethering_plugin_init, tethering_plugin_exit)

/*
 * Local Variables:
 * mode: C
 * c-basic-offset: 4
 * indent-tabs-mode: nil
 * End:
 */
