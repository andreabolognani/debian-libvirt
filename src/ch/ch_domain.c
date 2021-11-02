/*
 * Copyright Intel Corp. 2020-2021
 *
 * ch_domain.c: Domain manager functions for Cloud-Hypervisor driver
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

#include "ch_domain.h"
#include "viralloc.h"
#include "virchrdev.h"
#include "virlog.h"
#include "virtime.h"

#define VIR_FROM_THIS VIR_FROM_CH

VIR_ENUM_IMPL(virCHDomainJob,
              CH_JOB_LAST,
              "none",
              "query",
              "destroy",
              "modify",
);

VIR_LOG_INIT("ch.ch_domain");

static int
virCHDomainObjInitJob(virCHDomainObjPrivate *priv)
{
    memset(&priv->job, 0, sizeof(priv->job));

    if (virCondInit(&priv->job.cond) < 0)
        return -1;

    return 0;
}

static void
virCHDomainObjResetJob(virCHDomainObjPrivate *priv)
{
    struct virCHDomainJobObj *job = &priv->job;

    job->active = CH_JOB_NONE;
    job->owner = 0;
}

static void
virCHDomainObjFreeJob(virCHDomainObjPrivate *priv)
{
    ignore_value(virCondDestroy(&priv->job.cond));
}

/*
 * obj must be locked before calling, virCHDriver must NOT be locked
 *
 * This must be called by anything that will change the VM state
 * in any way
 *
 * Upon successful return, the object will have its ref count increased.
 * Successful calls must be followed by EndJob eventually.
 */
int
virCHDomainObjBeginJob(virDomainObj *obj, enum virCHDomainJob job)
{
    virCHDomainObjPrivate *priv = obj->privateData;
    unsigned long long now;
    unsigned long long then;

    if (virTimeMillisNow(&now) < 0)
        return -1;
    then = now + CH_JOB_WAIT_TIME;

    while (priv->job.active) {
        VIR_DEBUG("Wait normal job condition for starting job: %s",
                  virCHDomainJobTypeToString(job));
        if (virCondWaitUntil(&priv->job.cond, &obj->parent.lock, then) < 0)
            goto error;
    }

    virCHDomainObjResetJob(priv);

    VIR_DEBUG("Starting job: %s", virCHDomainJobTypeToString(job));
    priv->job.active = job;
    priv->job.owner = virThreadSelfID();

    return 0;

 error:
    VIR_WARN("Cannot start job (%s) for domain %s;"
             " current job is (%s) owned by (%d)",
             virCHDomainJobTypeToString(job),
             obj->def->name,
             virCHDomainJobTypeToString(priv->job.active),
             priv->job.owner);

    if (errno == ETIMEDOUT)
        virReportError(VIR_ERR_OPERATION_TIMEOUT,
                       "%s", _("cannot acquire state change lock"));
    else
        virReportSystemError(errno,
                             "%s", _("cannot acquire job mutex"));
    return -1;
}

/*
 * obj must be locked and have a reference before calling
 *
 * To be called after completing the work associated with the
 * earlier virCHDomainBeginJob() call
 */
void
virCHDomainObjEndJob(virDomainObj *obj)
{
    virCHDomainObjPrivate *priv = obj->privateData;
    enum virCHDomainJob job = priv->job.active;

    VIR_DEBUG("Stopping job: %s",
              virCHDomainJobTypeToString(job));

    virCHDomainObjResetJob(priv);
    virCondSignal(&priv->job.cond);
}

static void *
virCHDomainObjPrivateAlloc(void *opaque G_GNUC_UNUSED)
{
    virCHDomainObjPrivate *priv;

    priv = g_new0(virCHDomainObjPrivate, 1);

    if (virCHDomainObjInitJob(priv) < 0) {
        g_free(priv);
        return NULL;
    }

    if (!(priv->chrdevs = virChrdevAlloc())) {
        virCHDomainObjFreeJob(priv);
        g_free(priv);
        return NULL;
    }

    return priv;
}

static void
virCHDomainObjPrivateFree(void *data)
{
    virCHDomainObjPrivate *priv = data;

    virChrdevFree(priv->chrdevs);
    virCHDomainObjFreeJob(priv);
    g_free(priv);
}

virDomainXMLPrivateDataCallbacks virCHDriverPrivateDataCallbacks = {
    .alloc = virCHDomainObjPrivateAlloc,
    .free = virCHDomainObjPrivateFree,
};

static int
virCHDomainDefPostParseBasic(virDomainDef *def,
                             void *opaque G_GNUC_UNUSED)
{
    /* check for emulator and create a default one if needed */
    if (!def->emulator) {
        if (!(def->emulator = g_find_program_in_path(CH_CMD))) {
            virReportError(VIR_ERR_CONFIG_UNSUPPORTED, "%s",
                           _("No emulator found for cloud-hypervisor"));
            return 1;
        }
    }

    return 0;
}

static int
virCHDomainDefPostParse(virDomainDef *def,
                        unsigned int parseFlags G_GNUC_UNUSED,
                        void *opaque,
                        void *parseOpaque G_GNUC_UNUSED)
{
    virCHDriver *driver = opaque;
    g_autoptr(virCaps) caps = virCHDriverGetCapabilities(driver, false);
    if (!caps)
        return -1;
    if (!virCapabilitiesDomainSupported(caps, def->os.type,
                                        def->os.arch,
                                        def->virtType))
        return -1;

    return 0;
}

static int
chValidateDomainDeviceDef(const virDomainDeviceDef *dev,
                          const virDomainDef *def G_GNUC_UNUSED,
                          void *opaque G_GNUC_UNUSED,
                          void *parseOpaque G_GNUC_UNUSED)
{
    switch ((virDomainDeviceType)dev->type) {
    case VIR_DOMAIN_DEVICE_DISK:
    case VIR_DOMAIN_DEVICE_NET:
    case VIR_DOMAIN_DEVICE_MEMORY:
    case VIR_DOMAIN_DEVICE_VSOCK:
    case VIR_DOMAIN_DEVICE_CONTROLLER:
    case VIR_DOMAIN_DEVICE_CHR:
        break;

    case VIR_DOMAIN_DEVICE_LEASE:
    case VIR_DOMAIN_DEVICE_FS:
    case VIR_DOMAIN_DEVICE_INPUT:
    case VIR_DOMAIN_DEVICE_SOUND:
    case VIR_DOMAIN_DEVICE_VIDEO:
    case VIR_DOMAIN_DEVICE_HOSTDEV:
    case VIR_DOMAIN_DEVICE_WATCHDOG:
    case VIR_DOMAIN_DEVICE_GRAPHICS:
    case VIR_DOMAIN_DEVICE_HUB:
    case VIR_DOMAIN_DEVICE_REDIRDEV:
    case VIR_DOMAIN_DEVICE_SMARTCARD:
    case VIR_DOMAIN_DEVICE_MEMBALLOON:
    case VIR_DOMAIN_DEVICE_NVRAM:
    case VIR_DOMAIN_DEVICE_RNG:
    case VIR_DOMAIN_DEVICE_SHMEM:
    case VIR_DOMAIN_DEVICE_TPM:
    case VIR_DOMAIN_DEVICE_PANIC:
    case VIR_DOMAIN_DEVICE_IOMMU:
    case VIR_DOMAIN_DEVICE_AUDIO:
        virReportError(VIR_ERR_CONFIG_UNSUPPORTED,
                       _("Cloud-Hypervisor doesn't support '%s' device"),
                       virDomainDeviceTypeToString(dev->type));
        return -1;

    case VIR_DOMAIN_DEVICE_NONE:
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("unexpected VIR_DOMAIN_DEVICE_NONE"));
        return -1;

    case VIR_DOMAIN_DEVICE_LAST:
    default:
        virReportEnumRangeError(virDomainDeviceType, dev->type);
        return -1;
    }

    if ((def->nconsoles &&
         def->consoles[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)
        && (def->nserials &&
            def->serials[0]->source->type == VIR_DOMAIN_CHR_TYPE_PTY)) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Only a single console or serial can be configured for this domain"));
        return -1;
    } else if (def->nconsoles > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Only a single console can be configured for this domain"));
        return -1;
    } else if (def->nserials > 1) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Only a single serial can be configured for this domain"));
        return -1;
    }

    if (def->nconsoles && def->consoles[0]->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Console can only be enabled for a PTY"));
        return -1;
    }

    if (def->nserials && def->serials[0]->source->type != VIR_DOMAIN_CHR_TYPE_PTY) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Serial can only be enabled for a PTY"));
        return -1;
    }

    return 0;
}

virDomainDefParserConfig virCHDriverDomainDefParserConfig = {
    .domainPostParseBasicCallback = virCHDomainDefPostParseBasic,
    .domainPostParseCallback = virCHDomainDefPostParse,
    .deviceValidateCallback = chValidateDomainDeviceDef,
};
