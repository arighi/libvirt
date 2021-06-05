/*
 * remote_sockets.c: helpers for getting remote driver socket paths
 *
 * Copyright (C) 2007-2019 Red Hat, Inc.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 */

#include <config.h>

#include "remote_sockets.h"
#include "virerror.h"
#include "virlog.h"
#include "virfile.h"
#include "virutil.h"
#include "configmake.h"

#define VIR_FROM_THIS VIR_FROM_REMOTE

VIR_LOG_INIT("remote.remote_sockets");

VIR_ENUM_IMPL(remoteDriverTransport,
              REMOTE_DRIVER_TRANSPORT_LAST,
              "tls",
              "unix",
              "ssh",
              "libssh2",
              "ext",
              "tcp",
              "libssh");

VIR_ENUM_IMPL(remoteDriverMode,
              REMOTE_DRIVER_MODE_LAST,
              "auto",
              "legacy",
              "direct");

#ifndef WIN32
static const char *
remoteGetDaemonPathEnv(void)
{
    /* We prefer a VIRTD_PATH env var to use for all daemons,
     * but if it is not set we will fallback to LIBVIRTD_PATH
     * for previous behaviour
     */
    if (getenv("VIRTD_PATH") != NULL) {
        return "VIRTD_PATH";
    } else {
        return "LIBVIRTD_PATH";
    }
}
#endif /* WIN32 */


int
remoteSplitURIScheme(virURI *uri,
                     char **driver,
                     remoteDriverTransport *transport)
{
    char *p = strchr(uri->scheme, '+');

    if (p)
        *driver = g_strndup(uri->scheme, p - uri->scheme);
    else
        *driver = g_strdup(uri->scheme);

    if (p) {
        g_autofree char *tmp = g_strdup(p + 1);
        int val;

        p = tmp;
        while (*p) {
            *p = g_ascii_tolower(*p);
            p++;
        }

        if ((val = remoteDriverTransportTypeFromString(tmp)) < 0) {
            virReportError(VIR_ERR_INVALID_ARG, "%s",
                           _("remote_open: transport in URL not recognised "
                             "(should be tls|unix|ssh|ext|tcp|libssh2|libssh)"));
            return -1;
        }

        if (val == REMOTE_DRIVER_TRANSPORT_UNIX &&
            uri->server) {
            virReportError(VIR_ERR_INVALID_ARG,
                           _("using unix socket and remote "
                             "server '%s' is not supported."),
                           uri->server);
            return -1;
        }

        *transport = val;
    } else {
        if (uri->server)
            *transport = REMOTE_DRIVER_TRANSPORT_TLS;
        else
            *transport = REMOTE_DRIVER_TRANSPORT_UNIX;
    }

    return 0;
}


static char *
remoteGetUNIXSocketHelper(remoteDriverTransport transport,
                          const char *sock_prefix,
                          unsigned int flags)
{
    char *sockname = NULL;
    g_autofree char *userdir = NULL;

    if (flags & REMOTE_DRIVER_OPEN_USER) {
        userdir = virGetUserRuntimeDirectory();

        sockname = g_strdup_printf("%s/%s-sock", userdir, sock_prefix);
    } else {
        /* Intentionally do *NOT* use RUNSTATEDIR here. We might
         * be connecting to a remote machine, and cannot assume
         * the remote host has /run. The converse is ok though,
         * any machine with /run will have a /var/run symlink.
         * The portable option is to thus use $LOCALSTATEDIR/run
         */
        sockname = g_strdup_printf("%s/run/libvirt/%s-%s", LOCALSTATEDIR,
                                   sock_prefix,
                                   flags & REMOTE_DRIVER_OPEN_RO ?
                                   "sock-ro" : "sock");
    }

    VIR_DEBUG("Built UNIX sockname=%s for transport=%s "
              "prefix=%s flags=0x%x",
              sockname, remoteDriverTransportTypeToString(transport),
              sock_prefix, flags);
    return sockname;
}


char *
remoteGetUNIXSocket(remoteDriverTransport transport,
                    remoteDriverMode mode,
                    const char *driver,
                    unsigned int flags,
                    char **daemon_path)
{
    char *sock_name = NULL;
    g_autofree char *direct_daemon = NULL;
    g_autofree char *legacy_daemon = NULL;
    g_autofree char *daemon_name = NULL;
    g_autofree char *direct_sock_name = NULL;
    g_autofree char *legacy_sock_name = NULL;
#ifndef WIN32
    const char *env_name = remoteGetDaemonPathEnv();
#else
    const char *env_name = NULL;
#endif

    VIR_DEBUG("Choosing remote socket for transport=%s mode=%s driver=%s flags=0x%x",
              remoteDriverTransportTypeToString(transport),
              remoteDriverModeTypeToString(mode),
              driver, flags);

    if (driver) {
        direct_daemon = g_strdup_printf("virt%sd", driver);
        direct_sock_name = remoteGetUNIXSocketHelper(transport, direct_daemon, flags);
    } else {
        direct_daemon = g_strdup("virtproxyd");
        direct_sock_name = remoteGetUNIXSocketHelper(transport, "libvirt", flags);
    }

    legacy_daemon = g_strdup("libvirtd");
    legacy_sock_name = remoteGetUNIXSocketHelper(transport, "libvirt", flags);

    if (mode == REMOTE_DRIVER_MODE_AUTO) {
        if (transport == REMOTE_DRIVER_TRANSPORT_UNIX) {
            if (direct_sock_name && virFileExists(direct_sock_name)) {
                mode = REMOTE_DRIVER_MODE_DIRECT;
            } else if (virFileExists(legacy_sock_name)) {
                mode = REMOTE_DRIVER_MODE_LEGACY;
            } else {
                /*
                 * This constant comes from the configure script and
                 * maps to either the direct or legacy mode constant
                 */
                mode = REMOTE_DRIVER_MODE_DEFAULT;
            }
        } else {
            mode = REMOTE_DRIVER_MODE_LEGACY;
        }
    }

    switch ((remoteDriverMode)mode) {
    case REMOTE_DRIVER_MODE_LEGACY:
        sock_name = g_steal_pointer(&legacy_sock_name);
        daemon_name = g_steal_pointer(&legacy_daemon);
        break;

    case REMOTE_DRIVER_MODE_DIRECT:
        if (transport != REMOTE_DRIVER_TRANSPORT_UNIX) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED,
                           _("Cannot use direct socket mode for %s transport"),
                           remoteDriverTransportTypeToString(transport));
            return NULL;
        }

        if (!direct_sock_name) {
            virReportError(VIR_ERR_OPERATION_UNSUPPORTED, "%s",
                           _("Cannot use direct socket mode if no URI is set"));
            return NULL;
        }

        sock_name = g_steal_pointer(&direct_sock_name);
        daemon_name = g_steal_pointer(&direct_daemon);
        break;

    case REMOTE_DRIVER_MODE_AUTO:
    case REMOTE_DRIVER_MODE_LAST:
    default:
        virReportEnumRangeError(remoteDriverMode, mode);
        return NULL;
    }

    if (flags & REMOTE_DRIVER_OPEN_AUTOSTART) {
        if (!(*daemon_path = virFileFindResourceFull(daemon_name,
                                                     NULL, NULL,
                                                     abs_top_builddir "/src",
                                                     SBINDIR,
                                                     env_name)))
            return NULL;
    } else {
        *daemon_path = NULL;
    }

    VIR_DEBUG("Chosen UNIX sockname=%s daemon_path=%s with mode=%s",
              sock_name, NULLSTR(*daemon_path),
              remoteDriverModeTypeToString(mode));
    return sock_name;
}


void
remoteGetURIDaemonInfo(virURI *uri,
                       remoteDriverTransport transport,
                       unsigned int *flags)
{
    const char *autostart_str = getenv("LIBVIRT_AUTOSTART");

    *flags = 0;

    /*
     * User session daemon is used for
     *
     *  - Any URI with /session suffix
     *  - Test driver, if a protocol is given
     *
     * provided we are running non-root
     */
    if (uri &&
        uri->path &&
        uri->scheme &&
        (STREQ(uri->path, "/session") ||
         STRPREFIX(uri->scheme, "test+")) &&
        geteuid() > 0) {
        VIR_DEBUG("User session daemon required");
        *flags |= REMOTE_DRIVER_OPEN_USER;

        /*
         * Furthermore if no servername is given,
         * and the transport is unix,
         * and uid is unprivileged then auto-spawn a daemon.
         */
        if (!uri->server &&
            (transport == REMOTE_DRIVER_TRANSPORT_UNIX) &&
            (!autostart_str ||
             STRNEQ(autostart_str, "0"))) {
            VIR_DEBUG("Try daemon autostart");
            *flags |= REMOTE_DRIVER_OPEN_AUTOSTART;
        }
    }

    /*
     * If URI is NULL, then do a UNIX connection possibly auto-spawning
     * unprivileged server and probe remote server for URI.
     */
    if (!uri) {
        VIR_DEBUG("Auto-probe remote URI");
        if (geteuid() > 0) {
            VIR_DEBUG("Auto-spawn user daemon instance");
            *flags |= REMOTE_DRIVER_OPEN_USER;
            if (!autostart_str ||
                STRNEQ(autostart_str, "0"))
                *flags |= REMOTE_DRIVER_OPEN_AUTOSTART;
        }
    }
}
