/*
 * qemu_security.c: QEMU security management
 *
 * Copyright (C) 2016 Red Hat, Inc.
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

#include "qemu_domain.h"
#include "qemu_namespace.h"
#include "qemu_security.h"
#include "virlog.h"

#define VIR_FROM_THIS VIR_FROM_QEMU

VIR_LOG_INIT("qemu.qemu_security");


int
qemuSecuritySetAllLabel(virQEMUDriver *driver,
                        virDomainObj *vm,
                        const char *incomingPath,
                        bool migrated)
{
    int ret = -1;
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetAllLabel(driver->securityManager,
                                      vm->def,
                                      incomingPath,
                                      priv->chardevStdioLogd,
                                      migrated) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


void
qemuSecurityRestoreAllLabel(virQEMUDriver *driver,
                            virDomainObj *vm,
                            bool migrated)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    bool transactionStarted = false;

    /* In contrast to qemuSecuritySetAllLabel, do not use vm->pid
     * here. This function is called from qemuProcessStop() which
     * is meant to do cleanup after qemu process died. The
     * domain's namespace is gone as qemu was the only process
     * running there. We would not succeed in entering the
     * namespace then. */
    if (virSecurityManagerTransactionStart(driver->securityManager) >= 0)
        transactionStarted = true;

    virSecurityManagerRestoreAllLabel(driver->securityManager,
                                      vm->def,
                                      migrated,
                                      priv->chardevStdioLogd);

    if (transactionStarted &&
        virSecurityManagerTransactionCommit(driver->securityManager,
                                            -1, priv->rememberOwner) < 0)
        VIR_WARN("Unable to run security manager transaction");

    virSecurityManagerTransactionAbort(driver->securityManager);
}


int
qemuSecuritySetImageLabel(virQEMUDriver *driver,
                          virDomainObj *vm,
                          virStorageSource *src,
                          bool backingChain,
                          bool chainTop)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;
    virSecurityDomainImageLabelFlags labelFlags = 0;

    if (backingChain)
        labelFlags |= VIR_SECURITY_DOMAIN_IMAGE_LABEL_BACKING_CHAIN;

    if (chainTop)
        labelFlags |= VIR_SECURITY_DOMAIN_IMAGE_PARENT_CHAIN_TOP;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetImageLabel(driver->securityManager,
                                        vm->def, src, labelFlags) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityRestoreImageLabel(virQEMUDriver *driver,
                              virDomainObj *vm,
                              virStorageSource *src,
                              bool backingChain)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;
    virSecurityDomainImageLabelFlags labelFlags = 0;

    if (backingChain)
        labelFlags |= VIR_SECURITY_DOMAIN_IMAGE_LABEL_BACKING_CHAIN;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreImageLabel(driver->securityManager,
                                            vm->def, src, labelFlags) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityMoveImageMetadata(virQEMUDriver *driver,
                              virDomainObj *vm,
                              virStorageSource *src,
                              virStorageSource *dst)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;

    if (!priv->rememberOwner)
        return 0;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    return virSecurityManagerMoveImageMetadata(driver->securityManager, pid, src, dst);
}


int
qemuSecuritySetHostdevLabel(virQEMUDriver *driver,
                            virDomainObj *vm,
                            virDomainHostdevDef *hostdev)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetHostdevLabel(driver->securityManager,
                                          vm->def,
                                          hostdev,
                                          NULL) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityRestoreHostdevLabel(virQEMUDriver *driver,
                                virDomainObj *vm,
                                virDomainHostdevDef *hostdev)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreHostdevLabel(driver->securityManager,
                                              vm->def,
                                              hostdev,
                                              NULL) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecuritySetMemoryLabel(virQEMUDriver *driver,
                           virDomainObj *vm,
                           virDomainMemoryDef *mem)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetMemoryLabel(driver->securityManager,
                                         vm->def,
                                         mem) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityRestoreMemoryLabel(virQEMUDriver *driver,
                               virDomainObj *vm,
                               virDomainMemoryDef *mem)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreMemoryLabel(driver->securityManager,
                                             vm->def,
                                             mem) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecuritySetInputLabel(virDomainObj *vm,
                          virDomainInputDef *input)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    virQEMUDriver *driver = priv->driver;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetInputLabel(driver->securityManager,
                                        vm->def,
                                        input) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityRestoreInputLabel(virDomainObj *vm,
                              virDomainInputDef *input)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    virQEMUDriver *driver = priv->driver;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreInputLabel(driver->securityManager,
                                            vm->def,
                                            input) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecuritySetChardevLabel(virQEMUDriver *driver,
                            virDomainObj *vm,
                            virDomainChrDef *chr)
{
    int ret = -1;
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetChardevLabel(driver->securityManager,
                                          vm->def,
                                          chr->source,
                                          priv->chardevStdioLogd) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityRestoreChardevLabel(virQEMUDriver *driver,
                                virDomainObj *vm,
                                virDomainChrDef *chr)
{
    int ret = -1;
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreChardevLabel(driver->securityManager,
                                              vm->def,
                                              chr->source,
                                              priv->chardevStdioLogd) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}

int
qemuSecuritySetNetdevLabel(virQEMUDriver *driver,
                           virDomainObj *vm,
                           virDomainNetDef *net)
{
    int ret = -1;
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerSetNetdevLabel(driver->securityManager,
                                         vm->def, net) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityRestoreNetdevLabel(virQEMUDriver *driver,
                               virDomainObj *vm,
                               virDomainNetDef *net)
{
    int ret = -1;
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerRestoreNetdevLabel(driver->securityManager,
                                             vm->def, net) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


/*
 * qemuSecurityStartVhostUserGPU:
 *
 * @driver: the QEMU driver
 * @vm: the domain object
 * @cmd: the command to run
 * @existstatus: pointer to int returning exit status of process
 * @cmdret: pointer to int returning result of virCommandRun
 *
 * Start the vhost-user-gpu process with appropriate labels.
 * This function returns -1 on security setup error, 0 if all the
 * setup was done properly. In case the virCommand failed to run
 * 0 is returned but cmdret is set appropriately with the process
 * exitstatus also set.
 */
int
qemuSecurityStartVhostUserGPU(virQEMUDriver *driver,
                              virDomainObj *vm,
                              virCommand *cmd,
                              int *exitstatus,
                              int *cmdret)
{
    if (virSecurityManagerSetChildProcessLabel(driver->securityManager,
                                               vm->def, cmd) < 0)
        return -1;

    if (virSecurityManagerPreFork(driver->securityManager) < 0)
        return -1;

    *cmdret = virCommandRun(cmd, exitstatus);

    virSecurityManagerPostFork(driver->securityManager);

    if (*cmdret < 0)
        return -1;

    return 0;
}


/*
 * qemuSecurityStartTPMEmulator:
 *
 * @driver: the QEMU driver
 * @vm: the domain object
 * @cmd: the command to run
 * @uid: the uid to run the emulator
 * @gid: the gid to run the emulator
 * @existstatus: pointer to int returning exit status of process
 * @cmdret: pointer to int returning result of virCommandRun
 *
 * Start the TPM emulator with appropriate labels. Apply security
 * labels to files first.
 * This function returns -1 on security setup error, 0 if all the
 * setup was done properly. In case the virCommand failed to run
 * 0 is returned but cmdret is set appropriately with the process
 * exitstatus also set.
 */
int
qemuSecurityStartTPMEmulator(virQEMUDriver *driver,
                             virDomainObj *vm,
                             virCommand *cmd,
                             uid_t uid,
                             gid_t gid,
                             int *exitstatus,
                             int *cmdret)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    int ret = -1;
    bool transactionStarted = false;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        return -1;
    transactionStarted = true;

    if (virSecurityManagerSetTPMLabels(driver->securityManager,
                                       vm->def) < 0) {
        virSecurityManagerTransactionAbort(driver->securityManager);
        return -1;
    }

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            -1, priv->rememberOwner) < 0)
        goto cleanup_abort;
    transactionStarted = false;

    if (qemuSecurityCommandRun(driver, vm, cmd, uid, gid, exitstatus, cmdret) < 0)
        goto cleanup;

    ret = 0;

    if (*cmdret < 0)
        goto cleanup;

    return 0;

 cleanup:
    if (!transactionStarted &&
        virSecurityManagerTransactionStart(driver->securityManager) >= 0)
        transactionStarted = true;

    virSecurityManagerRestoreTPMLabels(driver->securityManager, vm->def);

    if (transactionStarted &&
        virSecurityManagerTransactionCommit(driver->securityManager,
                                            -1, priv->rememberOwner) < 0)
        VIR_WARN("Unable to run security manager transaction");

 cleanup_abort:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


void
qemuSecurityCleanupTPMEmulator(virQEMUDriver *driver,
                               virDomainObj *vm)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    bool transactionStarted = false;

    if (virSecurityManagerTransactionStart(driver->securityManager) >= 0)
        transactionStarted = true;

    virSecurityManagerRestoreTPMLabels(driver->securityManager, vm->def);

    if (transactionStarted &&
        virSecurityManagerTransactionCommit(driver->securityManager,
                                            -1, priv->rememberOwner) < 0)
        VIR_WARN("Unable to run security manager transaction");

    virSecurityManagerTransactionAbort(driver->securityManager);
}


int
qemuSecurityDomainSetPathLabel(virQEMUDriver *driver,
                               virDomainObj *vm,
                               const char *path,
                               bool allowSubtree)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerDomainSetPathLabel(driver->securityManager,
                                             vm->def,
                                             path,
                                             allowSubtree) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


int
qemuSecurityDomainRestorePathLabel(virQEMUDriver *driver,
                                   virDomainObj *vm,
                                   const char *path)
{
    qemuDomainObjPrivate *priv = vm->privateData;
    pid_t pid = -1;
    int ret = -1;

    if (qemuDomainNamespaceEnabled(vm, QEMU_DOMAIN_NS_MOUNT))
        pid = vm->pid;

    if (virSecurityManagerTransactionStart(driver->securityManager) < 0)
        goto cleanup;

    if (virSecurityManagerDomainRestorePathLabel(driver->securityManager,
                                                 vm->def,
                                                 path) < 0)
        goto cleanup;

    if (virSecurityManagerTransactionCommit(driver->securityManager,
                                            pid, priv->rememberOwner) < 0)
        goto cleanup;

    ret = 0;
 cleanup:
    virSecurityManagerTransactionAbort(driver->securityManager);
    return ret;
}


/**
 * qemuSecurityCommandRun:
 * @driver: the QEMU driver
 * @vm: the domain object
 * @cmd: the command to run
 * @uid: the uid to force
 * @gid: the gid to force
 * @existstatus: pointer to int returning exit status of process
 * @cmdret: pointer to int returning result of virCommandRun
 *
 * Run @cmd with seclabels set on it. If @uid and/or @gid are not
 * -1 then their value is enforced.
 *
 * Returns: 0 on success,
 *         -1 otherwise.
 */
int
qemuSecurityCommandRun(virQEMUDriver *driver,
                       virDomainObj *vm,
                       virCommand *cmd,
                       uid_t uid,
                       gid_t gid,
                       int *exitstatus,
                       int *cmdret)
{
    if (virSecurityManagerSetChildProcessLabel(driver->securityManager,
                                               vm->def, cmd) < 0)
        return -1;

    if (uid != (uid_t) -1)
        virCommandSetUID(cmd, uid);
    if (gid != (gid_t) -1)
        virCommandSetGID(cmd, gid);

    if (virSecurityManagerPreFork(driver->securityManager) < 0)
        return -1;

    *cmdret = virCommandRun(cmd, exitstatus);

    virSecurityManagerPostFork(driver->securityManager);

    return 0;
}
