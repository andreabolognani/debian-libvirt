/*
 * qemu_migration_cookie.c: QEMU migration cookie handling
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

#include <gnutls/gnutls.h>
#include <gnutls/x509.h>

#include "locking/domain_lock.h"
#include "viralloc.h"
#include "virerror.h"
#include "virlog.h"
#include "virnetdevopenvswitch.h"
#include "virstring.h"
#include "virutil.h"

#include "qemu_domain.h"
#include "qemu_migration_cookie.h"
#include "qemu_migration_params.h"


#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_migration_cookie");

VIR_ENUM_IMPL(qemuMigrationCookieFlag,
              QEMU_MIGRATION_COOKIE_FLAG_LAST,
              "graphics",
              "lockstate",
              "persistent",
              "network",
              "nbd",
              "statistics",
              "memory-hotplug",
              "cpu-hotplug",
              "cpu",
              "allowReboot",
              "capabilities",
);


static void
qemuMigrationCookieGraphicsFree(qemuMigrationCookieGraphicsPtr grap)
{
    if (!grap)
        return;
    g_free(grap->listen);
    g_free(grap->tlsSubject);
    g_free(grap);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuMigrationCookieGraphics,
                              qemuMigrationCookieGraphicsFree);


static void
qemuMigrationCookieNetworkFree(qemuMigrationCookieNetworkPtr network)
{
    size_t i;

    if (!network)
        return;

    if (network->net) {
        for (i = 0; i < network->nnets; i++)
            g_free(network->net[i].portdata);
    }
    g_free(network->net);
    g_free(network);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuMigrationCookieNetwork,
                              qemuMigrationCookieNetworkFree);

static void
qemuMigrationCookieNBDFree(qemuMigrationCookieNBDPtr nbd)
{
    if (!nbd)
        return;

    while (nbd->ndisks)
        g_free(nbd->disks[--nbd->ndisks].target);
    g_free(nbd->disks);
    g_free(nbd);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuMigrationCookieNBD,
                              qemuMigrationCookieNBDFree);

static void
qemuMigrationCookieCapsFree(qemuMigrationCookieCapsPtr caps)
{
    if (!caps)
        return;

    virBitmapFree(caps->supported);
    virBitmapFree(caps->automatic);
    g_free(caps);
}

G_DEFINE_AUTOPTR_CLEANUP_FUNC(qemuMigrationCookieCaps,
                              qemuMigrationCookieCapsFree);

void
qemuMigrationCookieFree(qemuMigrationCookiePtr mig)
{
    if (!mig)
        return;

    qemuMigrationCookieGraphicsFree(mig->graphics);
    virDomainDefFree(mig->persistent);
    qemuMigrationCookieNetworkFree(mig->network);
    qemuMigrationCookieNBDFree(mig->nbd);

    g_free(mig->localHostname);
    g_free(mig->remoteHostname);
    g_free(mig->name);
    g_free(mig->lockState);
    g_free(mig->lockDriver);
    g_clear_pointer(&mig->jobInfo, qemuDomainJobInfoFree);
    virCPUDefFree(mig->cpu);
    qemuMigrationCookieCapsFree(mig->caps);
    g_free(mig);
}


static char *
qemuDomainExtractTLSSubject(const char *certdir)
{
    g_autofree char *certfile = NULL;
    char *subject = NULL;
    g_autofree char *pemdata = NULL;
    gnutls_datum_t pemdatum;
    gnutls_x509_crt_t cert;
    int ret;
    size_t subjectlen;

    certfile = g_strdup_printf("%s/server-cert.pem", certdir);

    if (virFileReadAll(certfile, 8192, &pemdata) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unable to read server cert %s"), certfile);
        return NULL;
    }

    ret = gnutls_x509_crt_init(&cert);
    if (ret < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot initialize cert object: %s"),
                       gnutls_strerror(ret));
        return NULL;
    }

    pemdatum.data = (unsigned char *)pemdata;
    pemdatum.size = strlen(pemdata);

    ret = gnutls_x509_crt_import(cert, &pemdatum, GNUTLS_X509_FMT_PEM);
    if (ret < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("cannot load cert data from %s: %s"),
                       certfile, gnutls_strerror(ret));
        return NULL;
    }

    subjectlen = 1024;
    subject = g_new0(char, subjectlen + 1);

    gnutls_x509_crt_get_dn(cert, subject, &subjectlen);
    subject[subjectlen] = '\0';

    return subject;
}


static qemuMigrationCookieGraphicsPtr
qemuMigrationCookieGraphicsSpiceAlloc(virQEMUDriverPtr driver,
                                      virDomainGraphicsDefPtr def,
                                      virDomainGraphicsListenDefPtr glisten)
{
    g_autoptr(qemuMigrationCookieGraphics) mig = g_new0(qemuMigrationCookieGraphics, 1);
    const char *listenAddr;
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);

    mig->type = VIR_DOMAIN_GRAPHICS_TYPE_SPICE;
    mig->port = def->data.spice.port;
    if (cfg->spiceTLS)
        mig->tlsPort = def->data.spice.tlsPort;
    else
        mig->tlsPort = -1;

    if (!glisten || !(listenAddr = glisten->address))
        listenAddr = cfg->spiceListen;

    if (cfg->spiceTLS &&
        !(mig->tlsSubject = qemuDomainExtractTLSSubject(cfg->spiceTLSx509certdir)))
        return NULL;

    mig->listen = g_strdup(listenAddr);

    return g_steal_pointer(&mig);
}


static qemuMigrationCookieNetworkPtr
qemuMigrationCookieNetworkAlloc(virQEMUDriverPtr driver G_GNUC_UNUSED,
                                virDomainDefPtr def)
{
    g_autoptr(qemuMigrationCookieNetwork) mig = g_new0(qemuMigrationCookieNetwork, 1);
    size_t i;

    mig->nnets = def->nnets;
    mig->net = g_new0(qemuMigrationCookieNetData, def->nnets);

    for (i = 0; i < def->nnets; i++) {
        virDomainNetDefPtr netptr;
        const virNetDevVPortProfile *vport;

        netptr = def->nets[i];
        vport = virDomainNetGetActualVirtPortProfile(netptr);

        if (vport) {
            mig->net[i].vporttype = vport->virtPortType;

            switch (vport->virtPortType) {
            case VIR_NETDEV_VPORT_PROFILE_NONE:
            case VIR_NETDEV_VPORT_PROFILE_8021QBG:
            case VIR_NETDEV_VPORT_PROFILE_8021QBH:
               break;
            case VIR_NETDEV_VPORT_PROFILE_OPENVSWITCH:
                if (virNetDevOpenvswitchGetMigrateData(&mig->net[i].portdata,
                                                       netptr->ifname) != 0) {
                        virReportError(VIR_ERR_INTERNAL_ERROR,
                                       _("Unable to run command to get OVS port data for "
                                         "interface %s"), netptr->ifname);
                        return NULL;
                }
                break;
            default:
                break;
            }
        }
    }
    return g_steal_pointer(&mig);
}


qemuMigrationCookiePtr
qemuMigrationCookieNew(const virDomainDef *def,
                       const char *origname)
{
    qemuMigrationCookiePtr mig = NULL;
    unsigned char localHostUUID[VIR_UUID_BUFLEN];
    g_autofree char *localHostname = NULL;

    if (!(localHostname = virGetHostname()))
        return NULL;

    if (virGetHostUUID(localHostUUID) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Unable to obtain host UUID"));
        return NULL;
    }

    mig = g_new0(qemuMigrationCookie, 1);

    if (origname)
        mig->name = g_strdup(origname);
    else
        mig->name = g_strdup(def->name);

    memcpy(mig->uuid, def->uuid, VIR_UUID_BUFLEN);
    memcpy(mig->localHostuuid, localHostUUID, VIR_UUID_BUFLEN);
    mig->localHostname = g_steal_pointer(&localHostname);

    return mig;
}


static int
qemuMigrationCookieAddGraphics(qemuMigrationCookiePtr mig,
                               virQEMUDriverPtr driver,
                               virDomainObjPtr dom)
{
    size_t i = 0;

    if (mig->flags & QEMU_MIGRATION_COOKIE_GRAPHICS) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration graphics data already present"));
        return -1;
    }

    for (i = 0; i < dom->def->ngraphics; i++) {
        if (dom->def->graphics[i]->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
            virDomainGraphicsListenDefPtr glisten =
                virDomainGraphicsGetListen(dom->def->graphics[i], 0);

            if (!glisten) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("missing listen element"));
                return -1;
            }

            switch (glisten->type) {
            case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_ADDRESS:
            case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NETWORK:
                /* Seamless migration is supported only for listen types
                 * 'address and 'network'. */
                if (!(mig->graphics =
                      qemuMigrationCookieGraphicsSpiceAlloc(driver,
                                                            dom->def->graphics[i],
                                                            glisten)))
                    return -1;
                mig->flags |= QEMU_MIGRATION_COOKIE_GRAPHICS;
                break;

            case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_SOCKET:
            case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_NONE:
            case VIR_DOMAIN_GRAPHICS_LISTEN_TYPE_LAST:
                break;
            }

            /* Seamless migration is supported only for one graphics. */
            if (mig->graphics)
                break;
        }
    }

    return 0;
}


static int
qemuMigrationCookieAddLockstate(qemuMigrationCookiePtr mig,
                                virQEMUDriverPtr driver,
                                virDomainObjPtr dom)
{
    qemuDomainObjPrivatePtr priv = dom->privateData;

    if (mig->flags & QEMU_MIGRATION_COOKIE_LOCKSTATE) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration lockstate data already present"));
        return -1;
    }

    if (virDomainObjGetState(dom, NULL) == VIR_DOMAIN_PAUSED) {
        mig->lockState = g_strdup(priv->lockState);
    } else {
        if (virDomainLockProcessInquire(driver->lockManager, dom, &mig->lockState) < 0)
            return -1;
    }

    mig->lockDriver = g_strdup(virLockManagerPluginGetName(driver->lockManager));

    mig->flags |= QEMU_MIGRATION_COOKIE_LOCKSTATE;
    mig->flagsMandatory |= QEMU_MIGRATION_COOKIE_LOCKSTATE;

    return 0;
}


int
qemuMigrationCookieAddPersistent(qemuMigrationCookiePtr mig,
                                 virDomainDefPtr *def)
{
    if (mig->flags & QEMU_MIGRATION_COOKIE_PERSISTENT) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration persistent data already present"));
        return -1;
    }

    if (!def || !*def)
        return 0;

    mig->persistent = *def;
    *def = NULL;
    mig->flags |= QEMU_MIGRATION_COOKIE_PERSISTENT;
    mig->flagsMandatory |= QEMU_MIGRATION_COOKIE_PERSISTENT;
    return 0;
}


virDomainDefPtr
qemuMigrationCookieGetPersistent(qemuMigrationCookiePtr mig)
{
    virDomainDefPtr def = mig->persistent;

    mig->persistent = NULL;
    mig->flags &= ~QEMU_MIGRATION_COOKIE_PERSISTENT;
    mig->flagsMandatory &= ~QEMU_MIGRATION_COOKIE_PERSISTENT;

    return def;
}


static int
qemuMigrationCookieAddNetwork(qemuMigrationCookiePtr mig,
                              virQEMUDriverPtr driver,
                              virDomainObjPtr dom)
{
    if (mig->flags & QEMU_MIGRATION_COOKIE_NETWORK) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Network migration data already present"));
        return -1;
    }

    if (dom->def->nnets > 0) {
        mig->network = qemuMigrationCookieNetworkAlloc(driver, dom->def);
        if (!mig->network)
            return -1;
        mig->flags |= QEMU_MIGRATION_COOKIE_NETWORK;
    }

    return 0;
}


static int
qemuMigrationCookieAddNBD(qemuMigrationCookiePtr mig,
                          virQEMUDriverPtr driver,
                          virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;
    g_autoptr(GHashTable) stats = virHashNew(g_free);
    bool blockdev = virQEMUCapsGet(priv->qemuCaps, QEMU_CAPS_BLOCKDEV);
    size_t i;
    int rc;

    /* It is not a bug if there already is a NBD data */
    qemuMigrationCookieNBDFree(mig->nbd);

    mig->nbd = g_new0(qemuMigrationCookieNBD, 1);

    mig->nbd->port = priv->nbdPort;
    mig->flags |= QEMU_MIGRATION_COOKIE_NBD;

    if (vm->def->ndisks == 0)
        return 0;

    mig->nbd->disks = g_new0(struct qemuMigrationCookieNBDDisk, vm->def->ndisks);
    mig->nbd->ndisks = 0;

    if (qemuDomainObjEnterMonitorAsync(driver, vm, priv->job.asyncJob) < 0)
        return -1;
    if (blockdev)
        rc = qemuMonitorBlockStatsUpdateCapacityBlockdev(priv->mon, stats);
    else
        rc = qemuMonitorBlockStatsUpdateCapacity(priv->mon, stats, false);
    if (qemuDomainObjExitMonitor(driver, vm) < 0 || rc < 0)
        return -1;

    for (i = 0; i < vm->def->ndisks; i++) {
        virDomainDiskDefPtr disk = vm->def->disks[i];
        qemuBlockStats *entry;

        if (blockdev) {
            if (!(entry = virHashLookup(stats, disk->src->nodeformat)))
                continue;
        } else {
            if (!disk->info.alias ||
                !(entry = virHashLookup(stats, disk->info.alias)))
                continue;
        }

        mig->nbd->disks[mig->nbd->ndisks].target = g_strdup(disk->dst);
        mig->nbd->disks[mig->nbd->ndisks].capacity = entry->capacity;
        mig->nbd->ndisks++;
    }

    return 0;
}


static int
qemuMigrationCookieAddStatistics(qemuMigrationCookiePtr mig,
                                 virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    if (!priv->job.completed)
        return 0;

    g_clear_pointer(&mig->jobInfo, qemuDomainJobInfoFree);
    mig->jobInfo = qemuDomainJobInfoCopy(priv->job.completed);

    mig->flags |= QEMU_MIGRATION_COOKIE_STATS;

    return 0;
}


static int
qemuMigrationCookieAddCPU(qemuMigrationCookiePtr mig,
                          virDomainObjPtr vm)
{
    if (mig->cpu)
        return 0;

    if (!(mig->cpu = virCPUDefCopy(vm->def->cpu)))
        return -1;

    if (qemuDomainMakeCPUMigratable(mig->cpu) < 0)
        return -1;

    mig->flags |= QEMU_MIGRATION_COOKIE_CPU;

    return 0;
}


static void
qemuMigrationCookieAddAllowReboot(qemuMigrationCookiePtr mig,
                                  virDomainObjPtr vm)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    mig->allowReboot = priv->allowReboot;

    mig->flags |= QEMU_MIGRATION_COOKIE_ALLOW_REBOOT;
}


static int
qemuMigrationCookieAddCaps(qemuMigrationCookiePtr mig,
                           virDomainObjPtr vm,
                           qemuMigrationParty party)
{
    qemuDomainObjPrivatePtr priv = vm->privateData;

    qemuMigrationCookieCapsFree(mig->caps);
    mig->caps = g_new0(qemuMigrationCookieCaps, 1);

    if (priv->migrationCaps)
        mig->caps->supported = virBitmapNewCopy(priv->migrationCaps);
    else
        mig->caps->supported = virBitmapNew(0);

    mig->caps->automatic = qemuMigrationParamsGetAlwaysOnCaps(party);

    mig->flags |= QEMU_MIGRATION_COOKIE_CAPS;

    return 0;
}


static void
qemuMigrationCookieGraphicsXMLFormat(virBufferPtr buf,
                                     qemuMigrationCookieGraphicsPtr grap)
{
    g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(buf);

    virBufferAsprintf(&attrBuf, " type='%s' port='%d' listen='%s'",
                      virDomainGraphicsTypeToString(grap->type),
                      grap->port, grap->listen);

    if (grap->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE)
        virBufferAsprintf(&attrBuf, " tlsPort='%d'", grap->tlsPort);

    virBufferEscapeString(&childBuf, "<cert info='subject' value='%s'/>\n", grap->tlsSubject);

    virXMLFormatElement(buf, "graphics", &attrBuf, &childBuf);
}


static void
qemuMigrationCookieNetworkXMLFormat(virBufferPtr buf,
                                    qemuMigrationCookieNetworkPtr optr)
{
    g_auto(virBuffer) interfaceBuf = VIR_BUFFER_INIT_CHILD(buf);
    size_t i;

    for (i = 0; i < optr->nnets; i++) {
        g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
        g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(&interfaceBuf);

        /* If optr->net[i].vporttype is not set, there is nothing to transfer */
        if (optr->net[i].vporttype == VIR_NETDEV_VPORT_PROFILE_NONE)
            continue;

        virBufferAsprintf(&attrBuf, " index='%zu' vporttype='%s'",
                          i, virNetDevVPortTypeToString(optr->net[i].vporttype));

        virBufferEscapeString(&childBuf, "<portdata>%s</portdata>\n",
                              optr->net[i].portdata);

        virXMLFormatElement(&interfaceBuf, "interface", &attrBuf, &childBuf);
    }

    virXMLFormatElement(buf, "network", NULL, &interfaceBuf);
}


static void
qemuMigrationCookieStatisticsXMLFormat(virBufferPtr buf,
                                       qemuDomainJobInfoPtr jobInfo)
{
    qemuMonitorMigrationStats *stats = &jobInfo->stats.mig;

    virBufferAddLit(buf, "<statistics>\n");
    virBufferAdjustIndent(buf, 2);

    virBufferAsprintf(buf, "<started>%llu</started>\n", jobInfo->started);
    virBufferAsprintf(buf, "<stopped>%llu</stopped>\n", jobInfo->stopped);
    virBufferAsprintf(buf, "<sent>%llu</sent>\n", jobInfo->sent);
    if (jobInfo->timeDeltaSet)
        virBufferAsprintf(buf, "<delta>%lld</delta>\n", jobInfo->timeDelta);

    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_TIME_ELAPSED,
                      jobInfo->timeElapsed);
    if (stats->downtime_set)
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_DOWNTIME,
                          stats->downtime);
    if (stats->setup_time_set)
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_SETUP_TIME,
                          stats->setup_time);

    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_TOTAL,
                      stats->ram_total);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_PROCESSED,
                      stats->ram_transferred);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_REMAINING,
                      stats->ram_remaining);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_BPS,
                      stats->ram_bps);

    if (stats->ram_duplicate_set) {
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_MEMORY_CONSTANT,
                          stats->ram_duplicate);
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_MEMORY_NORMAL,
                          stats->ram_normal);
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_MEMORY_NORMAL_BYTES,
                          stats->ram_normal_bytes);
    }

    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_DIRTY_RATE,
                      stats->ram_dirty_rate);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_ITERATION,
                      stats->ram_iteration);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_POSTCOPY_REQS,
                      stats->ram_postcopy_reqs);

    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_MEMORY_PAGE_SIZE,
                      stats->ram_page_size);

    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_DISK_TOTAL,
                      stats->disk_total);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_DISK_PROCESSED,
                      stats->disk_transferred);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_DISK_REMAINING,
                      stats->disk_remaining);
    virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                      VIR_DOMAIN_JOB_DISK_BPS,
                      stats->disk_bps);

    if (stats->xbzrle_set) {
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_COMPRESSION_CACHE,
                          stats->xbzrle_cache_size);
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_COMPRESSION_BYTES,
                          stats->xbzrle_bytes);
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_COMPRESSION_PAGES,
                          stats->xbzrle_pages);
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_COMPRESSION_CACHE_MISSES,
                          stats->xbzrle_cache_miss);
        virBufferAsprintf(buf, "<%1$s>%2$llu</%1$s>\n",
                          VIR_DOMAIN_JOB_COMPRESSION_OVERFLOW,
                          stats->xbzrle_overflow);
    }

    virBufferAsprintf(buf, "<%1$s>%2$d</%1$s>\n",
                      VIR_DOMAIN_JOB_AUTO_CONVERGE_THROTTLE,
                      stats->cpu_throttle_percentage);

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</statistics>\n");
}


static void
qemuMigrationCookieCapsXMLFormat(virBufferPtr buf,
                                 qemuMigrationCookieCapsPtr caps)
{
    qemuMigrationCapability cap;

    virBufferAddLit(buf, "<capabilities>\n");
    virBufferAdjustIndent(buf, 2);

    for (cap = 0; cap < QEMU_MIGRATION_CAP_LAST; cap++) {
        bool supported = false;
        bool automatic = false;

        ignore_value(virBitmapGetBit(caps->supported, cap, &supported));
        ignore_value(virBitmapGetBit(caps->automatic, cap, &automatic));
        if (supported) {
            virBufferAsprintf(buf, "<cap name='%s' auto='%s'/>\n",
                              qemuMigrationCapabilityTypeToString(cap),
                              automatic ? "yes" : "no");
        }
    }

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</capabilities>\n");
}


static void
qemuMigrationCookieNBDXMLFormat(qemuMigrationCookieNBDPtr nbd,
                                virBufferPtr buf)
{
    g_auto(virBuffer) attrBuf = VIR_BUFFER_INITIALIZER;
    g_auto(virBuffer) childBuf = VIR_BUFFER_INIT_CHILD(buf);
    size_t i;

    if (nbd->port)
        virBufferAsprintf(&attrBuf, " port='%d'", nbd->port);

    for (i = 0; i < nbd->ndisks; i++) {
        virBufferEscapeString(&childBuf, "<disk target='%s'", nbd->disks[i].target);
        virBufferAsprintf(&childBuf, " capacity='%llu'/>\n", nbd->disks[i].capacity);
    }

    virXMLFormatElement(buf, "nbd", &attrBuf, &childBuf);
}


int
qemuMigrationCookieXMLFormat(virQEMUDriverPtr driver,
                             virQEMUCapsPtr qemuCaps,
                             virBufferPtr buf,
                             qemuMigrationCookiePtr mig)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];
    char hostuuidstr[VIR_UUID_STRING_BUFLEN];
    size_t i;

    virUUIDFormat(mig->uuid, uuidstr);
    virUUIDFormat(mig->localHostuuid, hostuuidstr);

    virBufferAddLit(buf, "<qemu-migration>\n");
    virBufferAdjustIndent(buf, 2);
    virBufferEscapeString(buf, "<name>%s</name>\n", mig->name);
    virBufferAsprintf(buf, "<uuid>%s</uuid>\n", uuidstr);
    virBufferEscapeString(buf, "<hostname>%s</hostname>\n", mig->localHostname);
    virBufferAsprintf(buf, "<hostuuid>%s</hostuuid>\n", hostuuidstr);

    for (i = 0; i < QEMU_MIGRATION_COOKIE_FLAG_LAST; i++) {
        if (mig->flagsMandatory & (1 << i))
            virBufferAsprintf(buf, "<feature name='%s'/>\n",
                              qemuMigrationCookieFlagTypeToString(i));
    }

    if ((mig->flags & QEMU_MIGRATION_COOKIE_GRAPHICS) &&
        mig->graphics)
        qemuMigrationCookieGraphicsXMLFormat(buf, mig->graphics);

    if ((mig->flags & QEMU_MIGRATION_COOKIE_LOCKSTATE) &&
        mig->lockState) {
        virBufferAsprintf(buf, "<lockstate driver='%s'>\n",
                          mig->lockDriver);
        virBufferAdjustIndent(buf, 2);
        virBufferAsprintf(buf, "<leases>%s</leases>\n",
                          mig->lockState);
        virBufferAdjustIndent(buf, -2);
        virBufferAddLit(buf, "</lockstate>\n");
    }

    if ((mig->flags & QEMU_MIGRATION_COOKIE_PERSISTENT) &&
        mig->persistent) {
        if (qemuDomainDefFormatBuf(driver,
                                   qemuCaps,
                                   mig->persistent,
                                   VIR_DOMAIN_XML_INACTIVE |
                                   VIR_DOMAIN_XML_SECURE |
                                   VIR_DOMAIN_XML_MIGRATABLE,
                                   buf) < 0)
            return -1;
    }

    if ((mig->flags & QEMU_MIGRATION_COOKIE_NETWORK) && mig->network)
        qemuMigrationCookieNetworkXMLFormat(buf, mig->network);

    if ((mig->flags & QEMU_MIGRATION_COOKIE_NBD) && mig->nbd)
        qemuMigrationCookieNBDXMLFormat(mig->nbd, buf);

    if (mig->flags & QEMU_MIGRATION_COOKIE_STATS && mig->jobInfo)
        qemuMigrationCookieStatisticsXMLFormat(buf, mig->jobInfo);

    if (mig->flags & QEMU_MIGRATION_COOKIE_CPU && mig->cpu)
        virCPUDefFormatBufFull(buf, mig->cpu, NULL);

    if (mig->flags & QEMU_MIGRATION_COOKIE_ALLOW_REBOOT)
        qemuDomainObjPrivateXMLFormatAllowReboot(buf, mig->allowReboot);

    if (mig->flags & QEMU_MIGRATION_COOKIE_CAPS)
        qemuMigrationCookieCapsXMLFormat(buf, mig->caps);

    virBufferAdjustIndent(buf, -2);
    virBufferAddLit(buf, "</qemu-migration>\n");
    return 0;
}


static qemuMigrationCookieGraphicsPtr
qemuMigrationCookieGraphicsXMLParse(xmlXPathContextPtr ctxt)
{
    g_autoptr(qemuMigrationCookieGraphics) grap = g_new0(qemuMigrationCookieGraphics, 1);
    g_autofree char *graphicstype = NULL;

    if (!(graphicstype = virXPathString("string(./graphics/@type)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing type attribute in migration data"));
        return NULL;
    }
    if ((grap->type = virDomainGraphicsTypeFromString(graphicstype)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("unknown graphics type %s"), graphicstype);
        return NULL;
    }
    if (virXPathInt("string(./graphics/@port)", ctxt, &grap->port) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing port attribute in migration data"));
        return NULL;
    }
    if (grap->type == VIR_DOMAIN_GRAPHICS_TYPE_SPICE) {
        if (virXPathInt("string(./graphics/@tlsPort)", ctxt, &grap->tlsPort) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("missing tlsPort attribute in migration data"));
            return NULL;
        }
    }
    if (!(grap->listen = virXPathString("string(./graphics/@listen)", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing listen attribute in migration data"));
        return NULL;
    }
    /* Optional */
    grap->tlsSubject = virXPathString("string(./graphics/cert[@info='subject']/@value)", ctxt);

    return g_steal_pointer(&grap);
}


static qemuMigrationCookieNetworkPtr
qemuMigrationCookieNetworkXMLParse(xmlXPathContextPtr ctxt)
{
    g_autoptr(qemuMigrationCookieNetwork) optr = g_new0(qemuMigrationCookieNetwork, 1);
    size_t i;
    int n;
    g_autofree xmlNodePtr *interfaces = NULL;
    VIR_XPATH_NODE_AUTORESTORE(ctxt)

    if ((n = virXPathNodeSet("./network/interface", ctxt, &interfaces)) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing interface information"));
        return NULL;
    }

    optr->nnets = n;
    optr->net = g_new0(qemuMigrationCookieNetData, optr->nnets);

    for (i = 0; i < n; i++) {
        g_autofree char *vporttype = NULL;

        /* portdata is optional, and may not exist */
        ctxt->node = interfaces[i];
        optr->net[i].portdata = virXPathString("string(./portdata[1])", ctxt);

        if (!(vporttype = virXMLPropString(interfaces[i], "vporttype"))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("missing vporttype attribute in migration data"));
            return NULL;
        }
        optr->net[i].vporttype = virNetDevVPortTypeFromString(vporttype);
    }

    return g_steal_pointer(&optr);
}


static qemuMigrationCookieNBDPtr
qemuMigrationCookieNBDXMLParse(xmlXPathContextPtr ctxt)
{
    g_autoptr(qemuMigrationCookieNBD) ret = g_new0(qemuMigrationCookieNBD, 1);
    g_autofree char *port = NULL;
    size_t i;
    int n;
    g_autofree xmlNodePtr *disks = NULL;
    VIR_XPATH_NODE_AUTORESTORE(ctxt)

    port = virXPathString("string(./nbd/@port)", ctxt);
    if (port && virStrToLong_i(port, NULL, 10, &ret->port) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Malformed nbd port '%s'"),
                       port);
        return NULL;
    }

    /* Now check if source sent a list of disks to prealloc. We might be
     * talking to an older server, so it's not an error if the list is
     * missing. */
    if ((n = virXPathNodeSet("./nbd/disk", ctxt, &disks)) > 0) {
        ret->disks = g_new0(struct qemuMigrationCookieNBDDisk, n);
        ret->ndisks = n;

        for (i = 0; i < n; i++) {
            g_autofree char *capacity = NULL;

            ctxt->node = disks[i];

            if (!(ret->disks[i].target = virXPathString("string(./@target)", ctxt))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Malformed disk target"));
                return NULL;
            }

            capacity = virXPathString("string(./@capacity)", ctxt);
            if (!capacity ||
                virStrToLong_ull(capacity, NULL, 10,
                                 &ret->disks[i].capacity) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Malformed disk capacity: '%s'"),
                               NULLSTR(capacity));
                return NULL;
            }
        }
    }

    return g_steal_pointer(&ret);
}


static qemuDomainJobInfoPtr
qemuMigrationCookieStatisticsXMLParse(xmlXPathContextPtr ctxt)
{
    qemuDomainJobInfoPtr jobInfo = NULL;
    qemuMonitorMigrationStats *stats;
    VIR_XPATH_NODE_AUTORESTORE(ctxt)

    if (!(ctxt->node = virXPathNode("./statistics", ctxt)))
        return NULL;

    jobInfo = g_new0(qemuDomainJobInfo, 1);

    stats = &jobInfo->stats.mig;
    jobInfo->status = QEMU_DOMAIN_JOB_STATUS_COMPLETED;

    virXPathULongLong("string(./started[1])", ctxt, &jobInfo->started);
    virXPathULongLong("string(./stopped[1])", ctxt, &jobInfo->stopped);
    virXPathULongLong("string(./sent[1])", ctxt, &jobInfo->sent);
    if (virXPathLongLong("string(./delta[1])", ctxt, &jobInfo->timeDelta) == 0)
        jobInfo->timeDeltaSet = true;

    virXPathULongLong("string(./" VIR_DOMAIN_JOB_TIME_ELAPSED "[1])",
                      ctxt, &jobInfo->timeElapsed);

    if (virXPathULongLong("string(./" VIR_DOMAIN_JOB_DOWNTIME "[1])",
                          ctxt, &stats->downtime) == 0)
        stats->downtime_set = true;
    if (virXPathULongLong("string(./" VIR_DOMAIN_JOB_SETUP_TIME "[1])",
                          ctxt, &stats->setup_time) == 0)
        stats->setup_time_set = true;

    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_TOTAL "[1])",
                      ctxt, &stats->ram_total);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_PROCESSED "[1])",
                      ctxt, &stats->ram_transferred);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_REMAINING "[1])",
                      ctxt, &stats->ram_remaining);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_BPS "[1])",
                      ctxt, &stats->ram_bps);

    if (virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_CONSTANT "[1])",
                          ctxt, &stats->ram_duplicate) == 0)
        stats->ram_duplicate_set = true;
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_NORMAL "[1])",
                      ctxt, &stats->ram_normal);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_NORMAL_BYTES "[1])",
                      ctxt, &stats->ram_normal_bytes);

    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_DIRTY_RATE "[1])",
                      ctxt, &stats->ram_dirty_rate);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_ITERATION "[1])",
                      ctxt, &stats->ram_iteration);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_POSTCOPY_REQS "[1])",
                      ctxt, &stats->ram_postcopy_reqs);

    virXPathULongLong("string(./" VIR_DOMAIN_JOB_MEMORY_PAGE_SIZE "[1])",
                      ctxt, &stats->ram_page_size);

    virXPathULongLong("string(./" VIR_DOMAIN_JOB_DISK_TOTAL "[1])",
                      ctxt, &stats->disk_total);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_DISK_PROCESSED "[1])",
                      ctxt, &stats->disk_transferred);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_DISK_REMAINING "[1])",
                      ctxt, &stats->disk_remaining);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_DISK_BPS "[1])",
                      ctxt, &stats->disk_bps);

    if (virXPathULongLong("string(./" VIR_DOMAIN_JOB_COMPRESSION_CACHE "[1])",
                          ctxt, &stats->xbzrle_cache_size) == 0)
        stats->xbzrle_set = true;
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_COMPRESSION_BYTES "[1])",
                      ctxt, &stats->xbzrle_bytes);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_COMPRESSION_PAGES "[1])",
                      ctxt, &stats->xbzrle_pages);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_COMPRESSION_CACHE_MISSES "[1])",
                      ctxt, &stats->xbzrle_cache_miss);
    virXPathULongLong("string(./" VIR_DOMAIN_JOB_COMPRESSION_OVERFLOW "[1])",
                      ctxt, &stats->xbzrle_overflow);

    virXPathInt("string(./" VIR_DOMAIN_JOB_AUTO_CONVERGE_THROTTLE "[1])",
                ctxt, &stats->cpu_throttle_percentage);

    return jobInfo;
}


static qemuMigrationCookieCapsPtr
qemuMigrationCookieCapsXMLParse(xmlXPathContextPtr ctxt)
{
    g_autoptr(qemuMigrationCookieCaps) caps = g_new0(qemuMigrationCookieCaps, 1);
    g_autofree xmlNodePtr *nodes = NULL;
    size_t i;
    int n;

    caps->supported = virBitmapNew(QEMU_MIGRATION_CAP_LAST);
    caps->automatic = virBitmapNew(QEMU_MIGRATION_CAP_LAST);

    if ((n = virXPathNodeSet("./capabilities[1]/cap", ctxt, &nodes)) < 0)
        return NULL;

    for (i = 0; i < n; i++) {
        g_autofree char *name = NULL;
        g_autofree char *automatic = NULL;
        int cap;

        if (!(name = virXMLPropString(nodes[i], "name"))) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("missing migration capability name"));
            return NULL;
        }

        if ((cap = qemuMigrationCapabilityTypeFromString(name)) < 0)
            VIR_DEBUG("unknown migration capability '%s'", name);
        else
            ignore_value(virBitmapSetBit(caps->supported, cap));

        if ((automatic = virXMLPropString(nodes[i], "auto")) &&
            STREQ(automatic, "yes"))
            ignore_value(virBitmapSetBit(caps->automatic, cap));
    }

    return g_steal_pointer(&caps);
}


/**
 * qemuMigrationCookieXMLParseMandatoryFeatures:
 *
 * Check to ensure all mandatory features from XML are also present in 'flags'.
 */
static int
qemuMigrationCookieXMLParseMandatoryFeatures(xmlXPathContextPtr ctxt,
                                             unsigned int flags)
{
    g_autofree xmlNodePtr *nodes = NULL;
    size_t i;
    ssize_t n;

    if ((n = virXPathNodeSet("./feature", ctxt, &nodes)) < 0)
        return -1;

    for (i = 0; i < n; i++) {
        int val;
        g_autofree char *str = virXMLPropString(nodes[i], "name");

        if (!str) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           "%s", _("missing feature name"));
            return -1;
        }

        if ((val = qemuMigrationCookieFlagTypeFromString(str)) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unknown migration cookie feature %s"), str);
            return -1;
        }

        if ((flags & (1 << val)) == 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Unsupported migration cookie feature %s"), str);
            return -1;
        }
    }

    return 0;
}


static int
qemuMigrationCookieXMLParse(qemuMigrationCookiePtr mig,
                            virQEMUDriverPtr driver,
                            virQEMUCapsPtr qemuCaps,
                            xmlDocPtr doc,
                            xmlXPathContextPtr ctxt,
                            unsigned int flags)
{
    g_autofree char *name = NULL;
    g_autofree char *uuid = NULL;
    g_autofree char *hostuuid = NULL;
    char localdomuuid[VIR_UUID_STRING_BUFLEN];

    /* We don't store the uuid, name, hostname, or hostuuid
     * values. We just compare them to local data to do some
     * sanity checking on migration operation
     */

    /* Extract domain name */
    if (!(name = virXPathString("string(./name[1])", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing name element in migration data"));
        return -1;
    }
    if (STRNEQ(name, mig->name)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Incoming cookie data had unexpected name %s vs %s"),
                       name, mig->name);
        return -1;
    }

    /* Extract domain uuid */
    if (!(uuid = virXPathString("string(./uuid[1])", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing uuid element in migration data"));
        return -1;
    }
    virUUIDFormat(mig->uuid, localdomuuid);
    if (STRNEQ(uuid, localdomuuid)) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Incoming cookie data had unexpected UUID %s vs %s"),
                       uuid, localdomuuid);
        return -1;
    }

    if (!(mig->remoteHostname = virXPathString("string(./hostname[1])", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing hostname element in migration data"));
        return -1;
    }
    /* Historically, this is the place where we checked whether remoteHostname
     * and localHostname are the same. But even if they were, it doesn't mean
     * the domain is migrating onto the same host. Rely on UUID which can tell
     * for sure. */

    /* Check & forbid localhost migration */
    if (!(hostuuid = virXPathString("string(./hostuuid[1])", ctxt))) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("missing hostuuid element in migration data"));
        return -1;
    }
    if (virUUIDParse(hostuuid, mig->remoteHostuuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       "%s", _("malformed hostuuid element in migration data"));
        return -1;
    }
    if (memcmp(mig->remoteHostuuid, mig->localHostuuid, VIR_UUID_BUFLEN) == 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Attempt to migrate guest to the same host %s"),
                       hostuuid);
        return -1;
    }

    if (qemuMigrationCookieXMLParseMandatoryFeatures(ctxt, flags) < 0)
        return -1;

    if ((flags & QEMU_MIGRATION_COOKIE_GRAPHICS) &&
        virXPathBoolean("count(./graphics) > 0", ctxt) &&
        (!(mig->graphics = qemuMigrationCookieGraphicsXMLParse(ctxt))))
        return -1;

    if ((flags & QEMU_MIGRATION_COOKIE_LOCKSTATE) &&
        virXPathBoolean("count(./lockstate) > 0", ctxt)) {
        g_autofree char *lockState = NULL;

        mig->lockDriver = virXPathString("string(./lockstate[1]/@driver)", ctxt);
        if (!mig->lockDriver) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Missing lock driver name in migration cookie"));
            return -1;
        }

        lockState = virXPathString("string(./lockstate[1]/leases[1])", ctxt);
        if (STRNEQ_NULLABLE(lockState, ""))
            mig->lockState = g_steal_pointer(&lockState);
    }

    if ((flags & QEMU_MIGRATION_COOKIE_PERSISTENT) &&
        virXPathBoolean("count(./domain) > 0", ctxt)) {
        g_autofree xmlNodePtr *nodes = NULL;

        if ((virXPathNodeSet("./domain", ctxt, &nodes)) != 1) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Too many domain elements in migration cookie"));
            return -1;
        }
        mig->persistent = virDomainDefParseNode(doc, nodes[0],
                                                driver->xmlopt, qemuCaps,
                                                VIR_DOMAIN_DEF_PARSE_INACTIVE |
                                                VIR_DOMAIN_DEF_PARSE_ABI_UPDATE_MIGRATION |
                                                VIR_DOMAIN_DEF_PARSE_SKIP_VALIDATE);
        if (!mig->persistent)
            return -1;
    }

    if ((flags & QEMU_MIGRATION_COOKIE_NETWORK) &&
        virXPathBoolean("count(./network) > 0", ctxt) &&
        (!(mig->network = qemuMigrationCookieNetworkXMLParse(ctxt))))
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_NBD &&
        virXPathBoolean("boolean(./nbd)", ctxt) &&
        (!(mig->nbd = qemuMigrationCookieNBDXMLParse(ctxt))))
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_STATS &&
        virXPathBoolean("boolean(./statistics)", ctxt) &&
        (!(mig->jobInfo = qemuMigrationCookieStatisticsXMLParse(ctxt))))
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_CPU &&
        virCPUDefParseXML(ctxt, "./cpu[1]", VIR_CPU_TYPE_GUEST, &mig->cpu,
                          false) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_ALLOW_REBOOT &&
        qemuDomainObjPrivateXMLParseAllowReboot(ctxt, &mig->allowReboot) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_CAPS &&
        !(mig->caps = qemuMigrationCookieCapsXMLParse(ctxt)))
        return -1;

    return 0;
}


static int
qemuMigrationCookieXMLParseStr(qemuMigrationCookiePtr mig,
                               virQEMUDriverPtr driver,
                               virQEMUCapsPtr qemuCaps,
                               const char *xml,
                               unsigned int flags)
{
    xmlDocPtr doc = NULL;
    xmlXPathContextPtr ctxt = NULL;
    int ret = -1;

    VIR_DEBUG("xml=%s", NULLSTR(xml));

    if (!(doc = virXMLParseStringCtxt(xml, _("(qemu_migration_cookie)"), &ctxt)))
        goto cleanup;

    ret = qemuMigrationCookieXMLParse(mig, driver, qemuCaps, doc, ctxt, flags);

 cleanup:
    xmlXPathFreeContext(ctxt);
    xmlFreeDoc(doc);

    return ret;
}


int
qemuMigrationCookieFormat(qemuMigrationCookiePtr mig,
                          virQEMUDriverPtr driver,
                          virDomainObjPtr dom,
                          qemuMigrationParty party,
                          char **cookieout,
                          int *cookieoutlen,
                          unsigned int flags)
{
    qemuDomainObjPrivatePtr priv = dom->privateData;
    g_auto(virBuffer) buf = VIR_BUFFER_INITIALIZER;

    if (!cookieout || !cookieoutlen)
        return 0;

    *cookieoutlen = 0;

    if (flags & QEMU_MIGRATION_COOKIE_GRAPHICS &&
        qemuMigrationCookieAddGraphics(mig, driver, dom) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_LOCKSTATE &&
        qemuMigrationCookieAddLockstate(mig, driver, dom) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_NETWORK &&
        qemuMigrationCookieAddNetwork(mig, driver, dom) < 0) {
        return -1;
    }

    if ((flags & QEMU_MIGRATION_COOKIE_NBD) &&
        qemuMigrationCookieAddNBD(mig, driver, dom) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_STATS &&
        qemuMigrationCookieAddStatistics(mig, dom) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_MEMORY_HOTPLUG)
        mig->flagsMandatory |= QEMU_MIGRATION_COOKIE_MEMORY_HOTPLUG;

    if (flags & QEMU_MIGRATION_COOKIE_CPU_HOTPLUG)
        mig->flagsMandatory |= QEMU_MIGRATION_COOKIE_CPU_HOTPLUG;

    if (flags & QEMU_MIGRATION_COOKIE_CPU &&
        qemuMigrationCookieAddCPU(mig, dom) < 0)
        return -1;

    if (flags & QEMU_MIGRATION_COOKIE_ALLOW_REBOOT)
        qemuMigrationCookieAddAllowReboot(mig, dom);

    if (flags & QEMU_MIGRATION_COOKIE_CAPS &&
        qemuMigrationCookieAddCaps(mig, dom, party) < 0)
        return -1;

    if (qemuMigrationCookieXMLFormat(driver, priv->qemuCaps, &buf, mig) < 0)
        return -1;

    *cookieoutlen = virBufferUse(&buf) + 1;
    *cookieout = virBufferContentAndReset(&buf);

    VIR_DEBUG("cookielen=%d cookie=%s", *cookieoutlen, *cookieout);

    return 0;
}


qemuMigrationCookiePtr
qemuMigrationCookieParse(virQEMUDriverPtr driver,
                         const virDomainDef *def,
                         const char *origname,
                         qemuDomainObjPrivatePtr priv,
                         const char *cookiein,
                         int cookieinlen,
                         unsigned int flags)
{
    g_autoptr(qemuMigrationCookie) mig = NULL;

    /* Parse & validate incoming cookie (if any) */
    if (cookiein && cookieinlen &&
        cookiein[cookieinlen-1] != '\0') {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Migration cookie was not NULL terminated"));
        return NULL;
    }

    VIR_DEBUG("cookielen=%d cookie='%s'", cookieinlen, NULLSTR(cookiein));

    if (!(mig = qemuMigrationCookieNew(def, origname)))
        return NULL;

    if (cookiein && cookieinlen &&
        qemuMigrationCookieXMLParseStr(mig,
                                       driver,
                                       priv ? priv->qemuCaps : NULL,
                                       cookiein,
                                       flags) < 0)
        return NULL;

    if (flags & QEMU_MIGRATION_COOKIE_PERSISTENT &&
        mig->persistent &&
        STRNEQ(def->name, mig->persistent->name)) {
        g_free(mig->persistent->name);
        mig->persistent->name = g_strdup(def->name);
    }

    if (mig->flags & QEMU_MIGRATION_COOKIE_LOCKSTATE) {
        if (!mig->lockDriver) {
            if (virLockManagerPluginUsesState(driver->lockManager)) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Missing %s lock state for migration cookie"),
                               virLockManagerPluginGetName(driver->lockManager));
                return NULL;
            }
        } else if (STRNEQ(mig->lockDriver,
                          virLockManagerPluginGetName(driver->lockManager))) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Source host lock driver %s different from target %s"),
                           mig->lockDriver,
                           virLockManagerPluginGetName(driver->lockManager));
            return NULL;
        }
    }

    if (flags & QEMU_MIGRATION_COOKIE_STATS && mig->jobInfo && priv->job.current)
        mig->jobInfo->operation = priv->job.current->operation;

    return g_steal_pointer(&mig);
}
