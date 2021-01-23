/*
 * qemu_tpm.c: QEMU TPM support
 *
 * Copyright (C) 2018 IBM Corporation
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

#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <fcntl.h>

#include "qemu_extdevice.h"
#include "qemu_domain.h"
#include "qemu_security.h"

#include "conf/domain_conf.h"
#include "vircommand.h"
#include "viralloc.h"
#include "virkmod.h"
#include "virlog.h"
#include "virutil.h"
#include "viruuid.h"
#include "virfile.h"
#include "virstring.h"
#include "virpidfile.h"
#include "configmake.h"
#include "qemu_tpm.h"
#include "virtpm.h"
#include "virsecret.h"

#define VIR_FROM_THIS VIR_FROM_NONE

VIR_LOG_INIT("qemu.tpm");

/*
 * qemuTPMCreateEmulatorStoragePath
 *
 * @swtpmStorageDir: directory for swtpm persistent state
 * @uuid: The UUID of the VM for which to create the storage
 * @tpmversion: version of the TPM
 *
 * Create the swtpm's storage path
 */
static char *
qemuTPMCreateEmulatorStoragePath(const char *swtpmStorageDir,
                                 const char *uuidstr,
                                 virDomainTPMVersion tpmversion)
{
    char *path = NULL;
    const char *dir = "";

    switch (tpmversion) {
    case VIR_DOMAIN_TPM_VERSION_1_2:
        dir = "tpm1.2";
        break;
    case VIR_DOMAIN_TPM_VERSION_2_0:
        dir = "tpm2";
        break;
    case VIR_DOMAIN_TPM_VERSION_DEFAULT:
    case VIR_DOMAIN_TPM_VERSION_LAST:
        return NULL;
    }

    path = g_strdup_printf("%s/%s/%s", swtpmStorageDir, uuidstr, dir);
    return path;
}


/*
 * qemuTPMEmulatorInitStorage
 *
 * Initialize the TPM Emulator storage by creating its root directory,
 * which is typically found in /var/lib/libvirt/tpm.
 *
 */
static int
qemuTPMEmulatorInitStorage(const char *swtpmStorageDir)
{
    int rc = 0;

    /* allow others to cd into this dir */
    if (virFileMakePathWithMode(swtpmStorageDir, 0711) < 0) {
        virReportSystemError(errno,
                             _("Could not create TPM directory %s"),
                             swtpmStorageDir);
        rc = -1;
    }

    return rc;
}


/*
 * qemuTPMCreateEmulatorStorage
 *
 * @storagepath: directory for swtpm's persistent state
 * @created: a pointer to a bool that will be set to true if the
 *           storage was created because it did not exist yet
 * @swtpm_user: The uid that needs to be able to access the directory
 * @swtpm_group: The gid that needs to be able to access the directory
 *
 * Unless the storage path for the swtpm for the given VM
 * already exists, create it and make it accessible for the given userid.
 * Adapt ownership of the directory and all swtpm's state files there.
 */
static int
qemuTPMCreateEmulatorStorage(const char *storagepath,
                             bool *created,
                             uid_t swtpm_user,
                             gid_t swtpm_group)
{
    g_autofree char *swtpmStorageDir = g_path_get_dirname(storagepath);

    if (qemuTPMEmulatorInitStorage(swtpmStorageDir) < 0)
        return -1;

    *created = false;

    if (!virFileExists(storagepath))
        *created = true;

    if (virDirCreate(storagepath, 0700, swtpm_user, swtpm_group,
                     VIR_DIR_CREATE_ALLOW_EXIST) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not create directory %s as %u:%d"),
                       storagepath, swtpm_user, swtpm_group);
        return -1;
    }

    if (virFileChownFiles(storagepath, swtpm_user, swtpm_group) < 0)
        return -1;

    return 0;
}


static void
qemuTPMDeleteEmulatorStorage(virDomainTPMDefPtr tpm)
{
    g_autofree char *path =  g_path_get_dirname(tpm->data.emulator.storagepath);

    ignore_value(virFileDeleteTree(path));
}


/*
 * qemuTPMCreateEmulatorSocket:
 *
 * @swtpmStateDir: the directory where to create the socket in
 * @shortName: short and unique name of the domain
 *
 * Create the Unix socket path from the given parameters
 */
static char *
qemuTPMCreateEmulatorSocket(const char *swtpmStateDir,
                            const char *shortName)
{
    return g_strdup_printf("%s/%s-swtpm.sock", swtpmStateDir, shortName);
}


/*
 * qemuTPMEmulatorInitPaths:
 *
 * @tpm: TPM definition for an emulator type
 * @swtpmStorageDir: the general swtpm storage dir which is used as a base
 *                   directory for creating VM specific directories
 * @uuid: the UUID of the VM
 */
static int
qemuTPMEmulatorInitPaths(virDomainTPMDefPtr tpm,
                         const char *swtpmStorageDir,
                         const unsigned char *uuid)
{
    char uuidstr[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(uuid, uuidstr);

    if (!tpm->data.emulator.storagepath &&
        !(tpm->data.emulator.storagepath =
            qemuTPMCreateEmulatorStoragePath(swtpmStorageDir, uuidstr,
                                             tpm->version)))
        return -1;

    return 0;
}


/*
 * qemuTPMCreatePidFilename
 */
static char *
qemuTPMEmulatorCreatePidFilename(const char *swtpmStateDir,
                                 const char *shortName)
{
    g_autofree char *devicename = NULL;

    devicename = g_strdup_printf("%s-swtpm", shortName);

    return virPidFileBuildPath(swtpmStateDir, devicename);
}


/*
 * qemuTPMEmulatorGetPid
 *
 * @swtpmStateDir: the directory where swtpm writes the pidfile into
 * @shortName: short name of the domain
 * @pid: pointer to pid
 *
 * Return -1 upon error, or zero on successful reading of the pidfile.
 * If the PID was not still alive, zero will be returned, and @pid will be
 * set to -1;
 */
static int
qemuTPMEmulatorGetPid(const char *swtpmStateDir,
                      const char *shortName,
                      pid_t *pid)
{
    g_autofree char *swtpm = virTPMGetSwtpm();
    g_autofree char *pidfile = qemuTPMEmulatorCreatePidFilename(swtpmStateDir,
                                                                shortName);
    if (!pidfile)
        return -1;

    if (virPidFileReadPathIfAlive(pidfile, pid, swtpm) < 0)
        return -1;

    return 0;
}


/*
 * qemuTPMEmulatorPrepareHost:
 *
 * @tpm: tpm definition
 * @logDir: directory where swtpm writes its logs into
 * @vmname: name of the VM
 * @swtpm_user: uid to run the swtpm with
 * @swtpm_group: gid to run the swtpm with
 * @swtpmStateDir: directory for swtpm's persistent state
 * @qemu_user: uid that qemu will run with; we share the socket file with it
 * @shortName: short and unique name of the domain
 *
 * Prepare the log directory for the swtpm and adjust ownership of it and the
 * log file we will be using. Prepare the state directory where we will share
 * the socket between tss and qemu users.
 */
static int
qemuTPMEmulatorPrepareHost(virDomainTPMDefPtr tpm,
                           const char *logDir,
                           const char *vmname,
                           uid_t swtpm_user,
                           gid_t swtpm_group,
                           const char *swtpmStateDir,
                           uid_t qemu_user,
                           const char *shortName)
{
    if (virTPMEmulatorInit() < 0)
        return -1;

    /* create log dir ... allow 'tss' user to cd into it */
    if (virFileMakePathWithMode(logDir, 0711) < 0)
        return -1;

    /* ... and adjust ownership */
    if (virDirCreate(logDir, 0730, swtpm_user, swtpm_group,
                     VIR_DIR_CREATE_ALLOW_EXIST) < 0)
        return -1;

    /* create logfile name ... */
    if (!tpm->data.emulator.logfile)
        tpm->data.emulator.logfile = g_strdup_printf("%s/%s-swtpm.log", logDir, vmname);

    if (!virFileExists(tpm->data.emulator.logfile) &&
        virFileTouch(tpm->data.emulator.logfile, 0644) < 0) {
        return -1;
    }

    /* ... and make sure it can be accessed by swtpm_user */
    if (chown(tpm->data.emulator.logfile, swtpm_user, swtpm_group) < 0) {
        virReportSystemError(errno,
                             _("Could not chown on swtpm logfile %s"),
                             tpm->data.emulator.logfile);
        return -1;
    }

    /*
      create our swtpm state dir ...
      - QEMU user needs to be able to access the socket there
      - swtpm group needs to be able to create files there
      - in privileged mode 0570 would be enough, for non-privileged mode
        we need 0770
    */
    if (virDirCreate(swtpmStateDir, 0770, qemu_user, swtpm_group,
                     VIR_DIR_CREATE_ALLOW_EXIST) < 0)
        return -1;

    /* create the socket filename */
    if (!tpm->data.emulator.source.data.nix.path &&
        !(tpm->data.emulator.source.data.nix.path =
          qemuTPMCreateEmulatorSocket(swtpmStateDir, shortName)))
        return -1;
    tpm->data.emulator.source.type = VIR_DOMAIN_CHR_TYPE_UNIX;

    return 0;
}

/*
 * qemuTPMSetupEncryption
 *
 * @secretuuid: The UUID with the secret holding passphrase
 * @cmd: the virCommand to transfer the secret to
 *
 * Returns file descriptor representing the read-end of a pipe.
 * The passphrase can be read from this pipe. Returns < 0 in case
 * of error.
 *
 * This function reads the passphrase and writes it into the
 * write-end of a pipe so that the read-end of the pipe can be
 * passed to the emulator for reading the passphrase from.
 */
static int
qemuTPMSetupEncryption(const unsigned char *secretuuid,
                       virCommandPtr cmd)
{
    int ret = -1;
    int pipefd[2] = { -1, -1 };
    virConnectPtr conn;
    g_autofree uint8_t *secret = NULL;
    size_t secret_len;
    virSecretLookupTypeDef seclookupdef = {
         .type = VIR_SECRET_LOOKUP_TYPE_UUID,
    };

    conn = virGetConnectSecret();
    if (!conn)
        return -1;

    memcpy(seclookupdef.u.uuid, secretuuid, sizeof(seclookupdef.u.uuid));
    if (virSecretGetSecretString(conn, &seclookupdef,
                                 VIR_SECRET_USAGE_TYPE_VTPM,
                                 &secret, &secret_len) < 0)
        goto error;

    if (virPipe(pipefd) < 0)
        goto error;

    if (virCommandSetSendBuffer(cmd, pipefd[1], secret, secret_len) < 0)
        goto error;

    secret = NULL;
    ret = pipefd[0];

 cleanup:
    virObjectUnref(conn);

    return ret;

 error:
    VIR_FORCE_CLOSE(pipefd[1]);
    VIR_FORCE_CLOSE(pipefd[0]);

    goto cleanup;
}

/*
 * qemuTPMEmulatorRunSetup
 *
 * @storagepath: path to the directory for TPM state
 * @vmname: the name of the VM
 * @vmuuid: the UUID of the VM
 * @privileged: whether we are running in privileged mode
 * @swtpm_user: The userid to switch to when setting up the TPM;
 *              typically this should be the uid of 'tss' or 'root'
 * @swtpm_group: The group id to switch to
 * @logfile: The file to write the log into; it must be writable
 *           for the user given by userid or 'tss'
 * @tpmversion: The version of the TPM, either a TPM 1.2 or TPM 2
 * @encryption: pointer to virStorageEncryption holding secret
 * @incomingMigration: whether we have an incoming migration
 *
 * Setup the external swtpm by creating endorsement key and
 * certificates for it.
 */
static int
qemuTPMEmulatorRunSetup(const char *storagepath,
                        const char *vmname,
                        const unsigned char *vmuuid,
                        bool privileged,
                        uid_t swtpm_user,
                        gid_t swtpm_group,
                        const char *logfile,
                        const virDomainTPMVersion tpmversion,
                        const unsigned char *secretuuid,
                        bool incomingMigration)
{
    g_autoptr(virCommand) cmd = NULL;
    int exitstatus;
    char uuid[VIR_UUID_STRING_BUFLEN];
    g_autofree char *vmid = NULL;
    g_autofree char *swtpm_setup = virTPMGetSwtpmSetup();
    VIR_AUTOCLOSE pwdfile_fd = -1;

    if (!swtpm_setup)
        return -1;

    if (!privileged && tpmversion == VIR_DOMAIN_TPM_VERSION_1_2)
        return virFileWriteStr(logfile,
                               _("Did not create EK and certificates since "
                                 "this requires privileged mode for a "
                                 "TPM 1.2\n"), 0600);

    cmd = virCommandNew(swtpm_setup);
    if (!cmd)
        return -1;

    virUUIDFormat(vmuuid, uuid);
    vmid = g_strdup_printf("%s:%s", vmname, uuid);

    virCommandSetUID(cmd, swtpm_user);
    virCommandSetGID(cmd, swtpm_group);

    switch (tpmversion) {
    case VIR_DOMAIN_TPM_VERSION_1_2:
        break;
    case VIR_DOMAIN_TPM_VERSION_2_0:
        virCommandAddArgList(cmd, "--tpm2", NULL);
        break;
    case VIR_DOMAIN_TPM_VERSION_DEFAULT:
    case VIR_DOMAIN_TPM_VERSION_LAST:
        break;
    }

    if (secretuuid) {
        if (!virTPMSwtpmSetupCapsGet(
                VIR_TPM_SWTPM_SETUP_FEATURE_CMDARG_PWDFILE_FD)) {
            virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                _("%s does not support passing a passphrase using a file "
                  "descriptor"), swtpm_setup);
            return -1;
        }
        if ((pwdfile_fd = qemuTPMSetupEncryption(secretuuid, cmd)) < 0)
            return -1;

        virCommandAddArg(cmd, "--pwdfile-fd");
        virCommandAddArgFormat(cmd, "%d", pwdfile_fd);
        virCommandAddArgList(cmd, "--cipher", "aes-256-cbc", NULL);
        virCommandPassFD(cmd, pwdfile_fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        pwdfile_fd = -1;
    }

    if (!incomingMigration) {
        virCommandAddArgList(cmd,
                             "--tpm-state", storagepath,
                             "--vmid", vmid,
                             "--logfile", logfile,
                             "--createek",
                             "--create-ek-cert",
                             "--create-platform-cert",
                             "--lock-nvram",
                             "--not-overwrite",
                             NULL);
    } else {
        virCommandAddArgList(cmd,
                             "--tpm-state", storagepath,
                             "--overwrite",
                             NULL);
    }

    virCommandClearCaps(cmd);

    if (virCommandRun(cmd, &exitstatus) < 0 || exitstatus != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not run '%s'. exitstatus: %d; "
                         "Check error log '%s' for details."),
                          swtpm_setup, exitstatus, logfile);
        return -1;
    }

    return 0;
}


/*
 * qemuTPMEmulatorBuildCommand:
 *
 * @tpm: TPM definition
 * @vmname: The name of the VM
 * @vmuuid: The UUID of the VM
 * @privileged: whether we are running in privileged mode
 * @swtpm_user: The uid for the swtpm to run as (drop privileges to from root)
 * @swtpm_group: The gid for the swtpm to run as
 * @swtpmStateDir: the directory where swtpm writes the pid file and creates the
 *                 Unix socket
 * @shortName: the short name of the VM
 * @incomingMigration: whether we have an incoming migration
 *
 * Create the virCommand use for starting the emulator
 * Do some initializations on the way, such as creation of storage
 * and emulator setup.
 */
static virCommandPtr
qemuTPMEmulatorBuildCommand(virDomainTPMDefPtr tpm,
                            const char *vmname,
                            const unsigned char *vmuuid,
                            bool privileged,
                            uid_t swtpm_user,
                            gid_t swtpm_group,
                            const char *swtpmStateDir,
                            const char *shortName,
                            bool incomingMigration)
{
    g_autoptr(virCommand) cmd = NULL;
    bool created = false;
    g_autofree char *pidfile = NULL;
    g_autofree char *swtpm = virTPMGetSwtpm();
    VIR_AUTOCLOSE pwdfile_fd = -1;
    VIR_AUTOCLOSE migpwdfile_fd = -1;
    const unsigned char *secretuuid = NULL;

    if (!swtpm)
        return NULL;

    if (qemuTPMCreateEmulatorStorage(tpm->data.emulator.storagepath,
                                     &created, swtpm_user, swtpm_group) < 0)
        return NULL;

    if (tpm->data.emulator.hassecretuuid)
        secretuuid = tpm->data.emulator.secretuuid;

    if (created &&
        qemuTPMEmulatorRunSetup(tpm->data.emulator.storagepath, vmname, vmuuid,
                                privileged, swtpm_user, swtpm_group,
                                tpm->data.emulator.logfile, tpm->version,
                                secretuuid, incomingMigration) < 0)
        goto error;

    unlink(tpm->data.emulator.source.data.nix.path);

    cmd = virCommandNew(swtpm);
    if (!cmd)
        goto error;

    virCommandClearCaps(cmd);

    virCommandAddArgList(cmd, "socket", "--daemon", "--ctrl", NULL);
    virCommandAddArgFormat(cmd, "type=unixio,path=%s,mode=0600",
                           tpm->data.emulator.source.data.nix.path);

    virCommandAddArg(cmd, "--tpmstate");
    virCommandAddArgFormat(cmd, "dir=%s,mode=0600",
                           tpm->data.emulator.storagepath);

    virCommandAddArg(cmd, "--log");
    virCommandAddArgFormat(cmd, "file=%s", tpm->data.emulator.logfile);

    virCommandSetUID(cmd, swtpm_user);
    virCommandSetGID(cmd, swtpm_group);

    switch (tpm->version) {
    case VIR_DOMAIN_TPM_VERSION_1_2:
        break;
    case VIR_DOMAIN_TPM_VERSION_2_0:
        virCommandAddArg(cmd, "--tpm2");
        break;
    case VIR_DOMAIN_TPM_VERSION_DEFAULT:
    case VIR_DOMAIN_TPM_VERSION_LAST:
        break;
    }

    if (!(pidfile = qemuTPMEmulatorCreatePidFilename(swtpmStateDir, shortName)))
        goto error;

    virCommandAddArg(cmd, "--pid");
    virCommandAddArgFormat(cmd, "file=%s", pidfile);

    if (tpm->data.emulator.hassecretuuid) {
        if (!virTPMSwtpmCapsGet(VIR_TPM_SWTPM_FEATURE_CMDARG_PWD_FD)) {
            virReportError(VIR_ERR_ARGUMENT_UNSUPPORTED,
                  _("%s does not support passing passphrase via file descriptor"),
                  swtpm);
            goto error;
        }

        pwdfile_fd = qemuTPMSetupEncryption(tpm->data.emulator.secretuuid, cmd);
        if (pwdfile_fd)
        migpwdfile_fd = qemuTPMSetupEncryption(tpm->data.emulator.secretuuid,
                                               cmd);
        if (pwdfile_fd < 0 || migpwdfile_fd < 0)
            goto error;

        virCommandAddArg(cmd, "--key");
        virCommandAddArgFormat(cmd, "pwdfd=%d,mode=aes-256-cbc",
                               pwdfile_fd);
        virCommandPassFD(cmd, pwdfile_fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        pwdfile_fd = -1;

        virCommandAddArg(cmd, "--migration-key");
        virCommandAddArgFormat(cmd, "pwdfd=%d,mode=aes-256-cbc",
                               migpwdfile_fd);
        virCommandPassFD(cmd, migpwdfile_fd, VIR_COMMAND_PASS_FD_CLOSE_PARENT);
        migpwdfile_fd = -1;
    }

    return g_steal_pointer(&cmd);

 error:
    if (created)
        qemuTPMDeleteEmulatorStorage(tpm);

    return NULL;
}


/*
 * qemuTPMEmulatorStop
 * @swtpmStateDir: A directory where the socket is located
 * @shortName: short and unique name of the domain
 *
 * Gracefully stop the swptm
 */
static void
qemuTPMEmulatorStop(const char *swtpmStateDir,
                    const char *shortName)
{
    virCommandPtr cmd;
    g_autofree char *pathname = NULL;
    g_autofree char *errbuf = NULL;
    g_autofree char *swtpm_ioctl = virTPMGetSwtpmIoctl();

    if (!swtpm_ioctl)
        return;

    if (virTPMEmulatorInit() < 0)
        return;

    if (!(pathname = qemuTPMCreateEmulatorSocket(swtpmStateDir, shortName)))
        return;

    if (!virFileExists(pathname))
        return;

    cmd = virCommandNew(swtpm_ioctl);
    if (!cmd)
        return;

    virCommandAddArgList(cmd, "--unix", pathname, "-s", NULL);

    virCommandSetErrorBuffer(cmd, &errbuf);

    ignore_value(virCommandRun(cmd, NULL));

    virCommandFree(cmd);

    /* clean up the socket */
    unlink(pathname);
}


int
qemuExtTPMInitPaths(virQEMUDriverPtr driver,
                    virDomainDefPtr def)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    size_t i;

    for (i = 0; i < def->ntpms; i++) {
        if (def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        return qemuTPMEmulatorInitPaths(def->tpms[i], cfg->swtpmStorageDir,
                                        def->uuid);
    }

    return 0;
}


int
qemuExtTPMPrepareHost(virQEMUDriverPtr driver,
                      virDomainDefPtr def)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    g_autofree char *shortName = NULL;
    size_t i;

    for (i = 0; i < def->ntpms; i++) {
        if (def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        shortName = virDomainDefGetShortName(def);
        if (!shortName)
            return -1;

        return qemuTPMEmulatorPrepareHost(def->tpms[i], cfg->swtpmLogDir,
                                          def->name, cfg->swtpm_user,
                                          cfg->swtpm_group,
                                          cfg->swtpmStateDir, cfg->user,
                                          shortName);
    }

    return 0;
}


void
qemuExtTPMCleanupHost(virDomainDefPtr def)
{
    size_t i;

    for (i = 0; i < def->ntpms; i++) {
        if (def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        if (!def->tpms[i]->data.emulator.persistent_state)
            qemuTPMDeleteEmulatorStorage(def->tpms[i]);
    }
}


/*
 * qemuExtTPMStartEmulator:
 *
 * @driver: QEMU driver
 * @vm: the domain object
 * @incomingMigration: whether we have an incoming migration
 *
 * Start the external TPM Emulator:
 * - have the command line built
 * - start the external TPM Emulator and sync with it before QEMU start
 */
static int
qemuExtTPMStartEmulator(virQEMUDriverPtr driver,
                        virDomainObjPtr vm,
                        virDomainTPMDefPtr tpm,
                        bool incomingMigration)
{
    g_autoptr(virCommand) cmd = NULL;
    int exitstatus = 0;
    g_autofree char *errbuf = NULL;
    g_autoptr(virQEMUDriverConfig) cfg = NULL;
    g_autofree char *shortName = virDomainDefGetShortName(vm->def);
    int cmdret = 0, timeout, rc;
    pid_t pid;

    if (!shortName)
        return -1;

    cfg = virQEMUDriverGetConfig(driver);

    /* stop any left-over TPM emulator for this VM */
    qemuTPMEmulatorStop(cfg->swtpmStateDir, shortName);

    if (!(cmd = qemuTPMEmulatorBuildCommand(tpm, vm->def->name, vm->def->uuid,
                                            driver->privileged,
                                            cfg->swtpm_user,
                                            cfg->swtpm_group,
                                            cfg->swtpmStateDir, shortName,
                                            incomingMigration)))
        return -1;

    if (qemuExtDeviceLogCommand(driver, vm, cmd, "TPM Emulator") < 0)
        return -1;

    virCommandSetErrorBuffer(cmd, &errbuf);

    if (qemuSecurityStartTPMEmulator(driver, vm, cmd,
                                     cfg->swtpm_user, cfg->swtpm_group,
                                     &exitstatus, &cmdret) < 0)
        return -1;

    if (cmdret < 0 || exitstatus != 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not start 'swtpm'. exitstatus: %d, "
                         "error: %s"), exitstatus, errbuf);
        return -1;
    }

    /* check that the swtpm has written its pid into the file */
    timeout = 1000; /* ms */
    while (timeout > 0) {
        rc = qemuTPMEmulatorGetPid(cfg->swtpmStateDir, shortName, &pid);
        if (rc < 0) {
            timeout -= 50;
            g_usleep(50 * 1000);
            continue;
        }
        if (rc == 0 && pid == (pid_t)-1)
            goto error;
        break;
    }
    if (timeout <= 0)
        goto error;

    return 0;

 error:
    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                   _("swtpm failed to start"));
    return -1;
}


int
qemuExtTPMStart(virQEMUDriverPtr driver,
                virDomainObjPtr vm,
                bool incomingMigration)
{
    size_t i;

    for (i = 0; i < vm->def->ntpms; i++) {
        if (vm->def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        return qemuExtTPMStartEmulator(driver, vm, vm->def->tpms[i],
                                       incomingMigration);
    }

    return 0;
}


void
qemuExtTPMStop(virQEMUDriverPtr driver,
               virDomainObjPtr vm)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    size_t i;

    for (i = 0; i < vm->def->ntpms; i++) {
        g_autofree char *shortName = NULL;

        if (vm->def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        shortName = virDomainDefGetShortName(vm->def);
        if (!shortName)
            return;

        qemuTPMEmulatorStop(cfg->swtpmStateDir, shortName);
        qemuSecurityCleanupTPMEmulator(driver, vm);
    }

    return;
}


int
qemuExtTPMSetupCgroup(virQEMUDriverPtr driver,
                      virDomainDefPtr def,
                      virCgroupPtr cgroup)
{
    g_autoptr(virQEMUDriverConfig) cfg = virQEMUDriverGetConfig(driver);
    int rc;
    pid_t pid;
    size_t i;

    for (i = 0; i < def->ntpms; i++) {
        g_autofree char *shortName = NULL;

        if (def->tpms[i]->type != VIR_DOMAIN_TPM_TYPE_EMULATOR)
            continue;

        shortName = virDomainDefGetShortName(def);
        if (!shortName)
            return -1;
        rc = qemuTPMEmulatorGetPid(cfg->swtpmStateDir, shortName, &pid);
        if (rc < 0 || (rc == 0 && pid == (pid_t)-1)) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not get process id of swtpm"));
            return -1;
        }
        if (virCgroupAddProcess(cgroup, pid) < 0)
            return -1;
    }

    return 0;
}
