/*
 * virsh-completer-network.c: virsh completer callbacks related to networks
 *
 * Copyright (C) 2019 Red Hat, Inc.
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

#include "virsh-completer-network.h"
#include "viralloc.h"
#include "virsh-network.h"
#include "virsh.h"
#include "virstring.h"

char **
virshNetworkNameCompleter(vshControl *ctl,
                          const vshCmd *cmd G_GNUC_UNUSED,
                          unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNetworkPtr *nets = NULL;
    int nnets = 0;
    size_t i = 0;
    char **ret = NULL;
    g_auto(GStrv) tmp = NULL;

    virCheckFlags(VIR_CONNECT_LIST_NETWORKS_INACTIVE |
                  VIR_CONNECT_LIST_NETWORKS_ACTIVE |
                  VIR_CONNECT_LIST_NETWORKS_PERSISTENT,
                  NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nnets = virConnectListAllNetworks(priv->conn, &nets, flags)) < 0)
        return NULL;

    tmp = g_new0(char *, nnets + 1);

    for (i = 0; i < nnets; i++) {
        const char *name = virNetworkGetName(nets[i]);

        tmp[i] = g_strdup(name);
    }

    ret = g_steal_pointer(&tmp);

    for (i = 0; i < nnets; i++)
        virNetworkFree(nets[i]);
    g_free(nets);
    return ret;
}


char **
virshNetworkEventNameCompleter(vshControl *ctl G_GNUC_UNUSED,
                               const vshCmd *cmd G_GNUC_UNUSED,
                               unsigned int flags)
{
    size_t i = 0;
    g_auto(GStrv) tmp = NULL;

    virCheckFlags(0, NULL);

    tmp = g_new0(char *, VIR_NETWORK_EVENT_ID_LAST + 1);

    for (i = 0; i < VIR_NETWORK_EVENT_ID_LAST; i++)
        tmp[i] = g_strdup(virshNetworkEventCallbacks[i].name);

    return g_steal_pointer(&tmp);
}


char **
virshNetworkPortUUIDCompleter(vshControl *ctl,
                              const vshCmd *cmd G_GNUC_UNUSED,
                              unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNetworkPtr net = NULL;
    virNetworkPortPtr *ports = NULL;
    int nports = 0;
    size_t i = 0;
    char **ret = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(net = virshCommandOptNetwork(ctl, cmd, NULL)))
        return NULL;

    if ((nports = virNetworkListAllPorts(net, &ports, flags)) < 0)
        return NULL;

    ret = g_new0(char *, nports + 1);

    for (i = 0; i < nports; i++) {
        char uuid[VIR_UUID_STRING_BUFLEN];

        if (virNetworkPortGetUUIDString(ports[i], uuid) < 0)
            goto error;

        ret[i] = g_strdup(uuid);

        virNetworkPortFree(ports[i]);
    }
    g_free(ports);

    return ret;

 error:
    for (; i < nports; i++)
        virNetworkPortFree(ports[i]);
    g_free(ports);
    for (i = 0; i < nports; i++)
        g_free(ret[i]);
    g_free(ret);
    return NULL;
}


char **
virshNetworkUUIDCompleter(vshControl *ctl,
                          const vshCmd *cmd G_GNUC_UNUSED,
                          unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNetworkPtr *nets = NULL;
    int nnets = 0;
    size_t i = 0;
    char **ret = NULL;
    g_auto(GStrv) tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if ((nnets = virConnectListAllNetworks(priv->conn, &nets, flags)) < 0)
        return NULL;

    tmp = g_new0(char *, nnets + 1);

    for (i = 0; i < nnets; i++) {
        char uuid[VIR_UUID_STRING_BUFLEN];

        if (virNetworkGetUUIDString(nets[i], uuid) < 0)
            goto cleanup;

        tmp[i] = g_strdup(uuid);
    }

    ret = g_steal_pointer(&tmp);

 cleanup:
    for (i = 0; i < nnets; i++)
        virNetworkFree(nets[i]);
    g_free(nets);
    return ret;
}


char **
virshNetworkDhcpMacCompleter(vshControl *ctl,
                             const vshCmd *cmd,
                             unsigned int flags)
{
    virshControlPtr priv = ctl->privData;
    virNetworkDHCPLeasePtr *leases = NULL;
    virNetworkPtr network = NULL;
    int nleases;
    size_t i = 0;
    char **ret = NULL;
    g_auto(GStrv) tmp = NULL;

    virCheckFlags(0, NULL);

    if (!priv->conn || virConnectIsAlive(priv->conn) <= 0)
        return NULL;

    if (!(network = virshCommandOptNetwork(ctl, cmd, NULL)))
        return NULL;

    if ((nleases = virNetworkGetDHCPLeases(network, NULL, &leases, flags)) < 0)
        goto cleanup;

    tmp = g_new0(char *, nleases + 1);

    for (i = 0; i < nleases; i++) {
        virNetworkDHCPLeasePtr lease = leases[i];
        tmp[i] = g_strdup(lease->mac);
    }

    ret = g_steal_pointer(&tmp);

 cleanup:
    if (leases) {
        for (i = 0; i < nleases; i++)
            virNetworkDHCPLeaseFree(leases[i]);
        VIR_FREE(leases);
    }
    virNetworkFree(network);
    return ret;
}
