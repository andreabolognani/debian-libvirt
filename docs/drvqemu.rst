.. role:: since
.. role:: removed

==========================
KVM/QEMU hypervisor driver
==========================

The libvirt KVM/QEMU driver can manage any QEMU emulator from version 2.11.0 or
later.

.. contents::

Project Links
-------------

-  The `KVM <https://www.linux-kvm.org/>`__ Linux hypervisor
-  The `QEMU <https://wiki.qemu.org/Index.html>`__ emulator

Deployment pre-requisites
-------------------------

-  **QEMU emulators**: The driver will probe ``/usr/bin`` for the presence of
   ``qemu``, ``qemu-system-x86_64``, ``qemu-system-microblaze``,
   ``qemu-system-microblazeel``, ``qemu-system-mips``,\ ``qemu-system-mipsel``,
   ``qemu-system-sparc``,\ ``qemu-system-ppc``. The results of this can be seen
   from the capabilities XML output.
-  **KVM hypervisor**: The driver will probe ``/usr/bin`` for the presence of
   ``qemu-kvm`` and ``/dev/kvm`` device node. If both are found, then KVM fully
   virtualized, hardware accelerated guests will be available.

Connections to QEMU driver
--------------------------

The libvirt QEMU driver is a multi-instance driver, providing a single system
wide privileged driver (the "system" instance), and per-user unprivileged
drivers (the "session" instance). The URI driver protocol is "qemu". Some
example connection URIs for the libvirt driver are:

::

   qemu:///session                      (local access to per-user instance)
   qemu+unix:///session                 (local access to per-user instance)

   qemu:///system                       (local access to system instance)
   qemu+unix:///system                  (local access to system instance)
   qemu://example.com/system            (remote access, TLS/x509)
   qemu+tcp://example.com/system        (remote access, SASl/Kerberos)
   qemu+ssh://root@example.com/system   (remote access, SSH tunnelled)

Embedded driver
~~~~~~~~~~~~~~~

Since 6.1.0 the QEMU driver has experimental support for operating in an
embedded mode. In this scenario, rather than connecting to the libvirtd daemon,
the QEMU driver runs in the client application process directly. To use this the
client application must have registered & be running an instance of the event
loop. To open the driver in embedded mode the app use the new URI path and
specify a virtual root directory under which the driver will create content. The
path to the root directory must be absolute. Passing a relative path results in
an error.

::

         qemu:///embed?root=/some/dir

Broadly speaking the range of functionality is intended to be on a par with that
seen when using the traditional system or session libvirt connections to QEMU.
The features will of course differ depending on whether the application using
the embedded driver is running privileged or unprivileged. For example PCI
device assignment or TAP based networking are only available when running
privileged. While the embedded mode is still classed as experimental some
features may change their default settings between releases.

By default if the application uses any APIs associated with secondary drivers,
these will result in a connection being opened to the corresponding driver in
libvirtd. For example, this allows a virtual machine from the embedded QEMU to
connect its NIC to a virtual network or connect its disk to a storage volume.
Some of the secondary drivers will also be able to support running in embedded
mode. Currently this is supported by the secrets driver, to allow for use of VMs
with encrypted disks

Directory tree
^^^^^^^^^^^^^^

Under the specified root directory the following locations will be used

::

   /some/dir
     |
     +- log
     |   |
     |   +- qemu
     |   +- swtpm
     |
     +- etc
     |   |
     |   +- qemu
     |   +- pki
     |       |
     |       +- qemu
     |
     +- run
     |   |
     |   +- qemu
     |   +- swtpm
     |
     +- cache
     |   |
     |   +- qemu
     |
     +- lib
         |
         +- qemu
         +- swtpm

Note that UNIX domain sockets used for QEMU virtual machines had a maximum
filename length of 108 characters. Bear this in mind when picking a root
directory to avoid risk of exhausting the filename space. The application is
responsible for recursively purging the contents of this directory tree once
they no longer require a connection, though it can also be left intact for reuse
when opening a future connection.

API usage with event loop
^^^^^^^^^^^^^^^^^^^^^^^^^

To use the QEMU driver in embedded mode the application must register an event
loop with libvirt. Many of the QEMU driver API calls will rely on the event loop
processing data. With this in mind, applications must **NEVER** invoke API calls
from the event loop thread itself, only other threads. Not following this rule
will lead to deadlocks in the API. This restriction was lifted starting from
6.2.0 release, when QMP processing moved to a dedicated thread. However, it is
important to let the event loop run after each API call, even the ones made from
the event loop thread itself.

Location of configuration files
-------------------------------

The QEMU driver comes with sane default values. However, during its
initialization it reads a configuration file which offers system administrator
or an user to override some of that default. The location of the file depends on
the connection URI, as follows:

=================== ======================================
``qemu:///system``  ``/etc/libvirt/qemu.conf``
``qemu:///session`` ``$XDG_CONFIG_HOME/libvirt/qemu.conf``
``qemu:///embed``   ``$rootdir/etc/qemu.conf``
=================== ======================================

If ``$XDG_CONFIG_HOME`` is not set in the environment, it defaults to
``$HOME/.config``. For the embed URI the ``$rootdir`` represents the specified
root directory from the connection URI.

Please note, that it is very likely that the only qemu.conf file that will exist
after installing libvirt is the ``/etc/libvirt/qemu.conf``, if users of the
session daemon or the embed driver want to override a built in value, then they
need to create the file before connecting to the respective URI.

Driver security architecture
----------------------------

There are multiple layers to security in the QEMU driver, allowing for
flexibility in the use of QEMU based virtual machines.

Driver instances
~~~~~~~~~~~~~~~~

As explained above there are two ways to access the QEMU driver in libvirt. The
"qemu:///session" family of URIs connect to a libvirtd instance running as the
same user/group ID as the client application. Thus the QEMU instances spawned
from this driver will share the same privileges as the client application. The
intended use case for this driver is desktop virtualization, with virtual
machines storing their disk images in the user's home directory and being
managed from the local desktop login session.

The "qemu:///system" family of URIs connect to a libvirtd instance running as
the privileged system account 'root'. Thus the QEMU instances spawned from this
driver may have much higher privileges than the client application managing
them. The intended use case for this driver is server virtualization, where the
virtual machines may need to be connected to host resources (block, PCI, USB,
network devices) whose access requires elevated privileges.

POSIX users/groups
~~~~~~~~~~~~~~~~~~

In the "session" instance, the POSIX users/groups model restricts QEMU virtual
machines (and libvirtd in general) to only have access to resources with the
same user/group ID as the client application. There is no finer level of
configuration possible for the "session" instances.

In the "system" instance, libvirt releases from 0.7.0 onwards allow control over
the user/group that the QEMU virtual machines are run as. A build of libvirt
with no configuration parameters set will still run QEMU processes as root:root.
It is possible to change this default by using the --with-qemu-user=$USERNAME
and --with-qemu-group=$GROUPNAME arguments to 'configure' during build. It is
strongly recommended that vendors build with both of these arguments set to
'qemu'. Regardless of this build time default, administrators can set a per-host
default setting in the ``/etc/libvirt/qemu.conf`` configuration file via the
``user=$USERNAME`` and ``group=$GROUPNAME`` parameters. When a non-root user or
group is configured, the libvirt QEMU driver will change uid/gid to match
immediately before executing the QEMU binary for a virtual machine.

If QEMU virtual machines from the "system" instance are being run as non-root,
there will be greater restrictions on what host resources the QEMU process will
be able to access. The libvirtd daemon will attempt to manage permissions on
resources to minimise the likelihood of unintentional security denials, but the
administrator / application developer must be aware of some of the consequences
/ restrictions.

-  The directories ``/var/run/libvirt/qemu/``, ``/var/lib/libvirt/qemu/`` and
   ``/var/cache/libvirt/qemu/`` must all have their ownership set to match the
   user / group ID that QEMU guests will be run as. If the vendor has set a
   non-root user/group for the QEMU driver at build time, the permissions should
   be set automatically at install time. If a host administrator customizes
   user/group in ``/etc/libvirt/qemu.conf``, they will need to manually set the
   ownership on these directories.

-  When attaching USB and PCI devices to a QEMU guest, QEMU will need to access
   files in ``/dev/bus/usb`` and ``/sys/bus/pci/devices`` respectively. The
   libvirtd daemon will automatically set the ownership on specific devices that
   are assigned to a guest at start time. There should not be any need for
   administrator changes in this respect.

-  Any files/devices used as guest disk images must be accessible to the
   user/group ID that QEMU guests are configured to run as. The libvirtd daemon
   will automatically set the ownership of the file/device path to the correct
   user/group ID. Applications / administrators must be aware though that the
   parent directory permissions may still deny access. The directories
   containing disk images must either have their ownership set to match the
   user/group configured for QEMU, or their UNIX file permissions must have the
   'execute/search' bit enabled for 'others'.

   The simplest option is the latter one, of just enabling the 'execute/search'
   bit. For any directory to be used for storing disk images, this can be
   achieved by running the following command on the directory itself, and any
   parent directories

   ::

      chmod o+x /path/to/directory

   In particular note that if using the "system" instance and attempting to
   store disk images in a user home directory, the default permissions on $HOME
   are typically too restrictive to allow access.

The libvirt maintainers **strongly recommend against** running QEMU as the root
user/group. This should not be required in most supported usage scenarios, as
libvirt will generally do the right thing to grant QEMU access to files it is
permitted to use when it is running non-root.

Linux process capabilities
~~~~~~~~~~~~~~~~~~~~~~~~~~

In versions of libvirt prior to 6.0.0, even if QEMU was configured to run as the
root user / group, libvirt would strip all process capabilities. This meant that
QEMU could only read/write files owned by root, or with open permissions. In
reality, stripping capabilities did not have any security benefit, as it was
trivial to get commands to run in another context with full capabilities, for
example, by creating a cronjob.

Thus since 6.0.0, if QEMU is running as root, it will keep all process
capabilities. Behaviour when QEMU is running non-root is unchanged, it still has
no capabilities.

SELinux basic confinement
~~~~~~~~~~~~~~~~~~~~~~~~~

The basic SELinux protection for QEMU virtual machines is intended to protect
the host OS from a compromised virtual machine process. There is no protection
between guests.

In the basic model, all QEMU virtual machines run under the confined domain
``root:system_r:qemu_t``. It is required that any disk image assigned to a QEMU
virtual machine is labelled with ``system_u:object_r:virt_image_t``. In a
default deployment, package vendors/distributor will typically ensure that the
directory ``/var/lib/libvirt/images`` has this label, such that any disk images
created in this directory will automatically inherit the correct labelling. If
attempting to use disk images in another location, the user/administrator must
ensure the directory has be given this requisite label. Likewise physical block
devices must be labelled ``system_u:object_r:virt_image_t``.

Not all filesystems allow for labelling of individual files. In particular NFS,
VFat and NTFS have no support for labelling. In these cases administrators must
use the 'context' option when mounting the filesystem to set the default label
to ``system_u:object_r:virt_image_t``. In the case of NFS, there is an
alternative option, of enabling the ``virt_use_nfs`` SELinux boolean.

SELinux sVirt confinement
~~~~~~~~~~~~~~~~~~~~~~~~~

The SELinux sVirt protection for QEMU virtual machines builds to the basic level
of protection, to also allow individual guests to be protected from each other.

In the sVirt model, each QEMU virtual machine runs under its own confined
domain, which is based on ``system_u:system_r:svirt_t:s0`` with a unique
category appended, eg, ``system_u:system_r:svirt_t:s0:c34,c44``. The rules are
setup such that a domain can only access files which are labelled with the
matching category level, eg ``system_u:object_r:svirt_image_t:s0:c34,c44``. This
prevents one QEMU process accessing any file resources that are prevent to
another QEMU process.

There are two ways of assigning labels to virtual machines under sVirt. In the
default setup, if sVirt is enabled, guests will get an automatically assigned
unique label each time they are booted. The libvirtd daemon will also
automatically relabel exclusive access disk images to match this label. Disks
that are marked as <shared> will get a generic label
``system_u:system_r:svirt_image_t:s0`` allowing all guests read/write access
them, while disks marked as <readonly> will get a generic label
``system_u:system_r:svirt_content_t:s0`` which allows all guests read-only
access.

With statically assigned labels, the application should include the desired
guest and file labels in the XML at time of creating the guest with libvirt. In
this scenario the application is responsible for ensuring the disk images &
similar resources are suitably labelled to match, libvirtd will not attempt any
relabelling.

If the sVirt security model is active, then the node capabilities XML will
include its details. If a virtual machine is currently protected by the security
model, then the guest XML will include its assigned labels. If enabled at
compile time, the sVirt security model will always be activated if SELinux is
available on the host OS. To disable sVirt, and revert to the basic level of
SELinux protection (host protection only), the ``/etc/libvirt/qemu.conf`` file
can be used to change the setting to ``security_driver="none"``

AppArmor sVirt confinement
~~~~~~~~~~~~~~~~~~~~~~~~~~

When using basic AppArmor protection for the libvirtd daemon and QEMU virtual
machines, the intention is to protect the host OS from a compromised virtual
machine process. There is no protection between guests.

The AppArmor sVirt protection for QEMU virtual machines builds on this basic
level of protection, to also allow individual guests to be protected from each
other.

In the sVirt model, if a profile is loaded for the libvirtd daemon, then each
``qemu:///system`` QEMU virtual machine will have a profile created for it when
the virtual machine is started if one does not already exist. This generated
profile uses a profile name based on the UUID of the QEMU virtual machine and
contains rules allowing access to only the files it needs to run, such as its
disks, pid file and log files. Just before the QEMU virtual machine is started,
the libvirtd daemon will change into this unique profile, preventing the QEMU
process from accessing any file resources that are present in another QEMU
process or the host machine.

The AppArmor sVirt implementation is flexible in that it allows an administrator
to customize the template file in ``/etc/apparmor.d/libvirt/TEMPLATE`` for
site-specific access for all newly created QEMU virtual machines. Also, when a
new profile is generated, two files are created:
``/etc/apparmor.d/libvirt/libvirt-<uuid>`` and
``/etc/apparmor.d/libvirt/libvirt-<uuid>.files``. The former can be fine-tuned
by the administrator to allow custom access for this particular QEMU virtual
machine, and the latter will be updated appropriately when required file access
changes, such as when a disk is added. This flexibility allows for situations
such as having one virtual machine in complain mode with all others in enforce
mode.

While users can define their own AppArmor profile scheme, a typical
configuration will include a profile for ``/usr/sbin/libvirtd``,
``/usr/lib/libvirt/virt-aa-helper`` or ``/usr/libexec/virt-aa-helper``\ (a
helper program which the libvirtd daemon uses instead of manipulating AppArmor
directly), and an abstraction to be included by
``/etc/apparmor.d/libvirt/TEMPLATE`` (typically
``/etc/apparmor.d/abstractions/libvirt-qemu``). An example profile scheme can be
found in the examples/apparmor directory of the source distribution.

If the sVirt security model is active, then the node capabilities XML will
include its details. If a virtual machine is currently protected by the security
model, then the guest XML will include its assigned profile name. If enabled at
compile time, the sVirt security model will be activated if AppArmor is
available on the host OS and a profile for the libvirtd daemon is loaded when
libvirtd is started. To disable sVirt, and revert to the basic level of AppArmor
protection (host protection only), the ``/etc/libvirt/qemu.conf`` file can be
used to change the setting to ``security_driver="none"``.

Cgroups device ACLs
~~~~~~~~~~~~~~~~~~~

Linux kernels have a capability known as "cgroups" which is used for resource
management. It is implemented via a number of "controllers", each controller
covering a specific task/functional area. One of the available controllers is
the "devices" controller, which is able to setup access control lists of
block/character devices that a cgroup should be allowed to access. If the
"devices" controller is mounted on a host, then libvirt will automatically
create a dedicated cgroup for each QEMU virtual machine and setup the device
access control list so that the QEMU process can only access shared devices, and
explicitly assigned disks images backed by block devices.

The list of shared devices a guest is allowed access to is

::

   /dev/null, /dev/full, /dev/zero,
   /dev/random, /dev/urandom,
   /dev/ptmx, /dev/kvm,

In the event of unanticipated needs arising, this can be customized via the
``/etc/libvirt/qemu.conf`` file. To mount the cgroups device controller, the
following command should be run as root, prior to starting libvirtd

::

   mkdir /dev/cgroup
   mount -t cgroup none /dev/cgroup -o devices

libvirt will then place each virtual machine in a cgroup at
``/dev/cgroup/libvirt/qemu/$VMNAME/``

Import and export of libvirt domain XML configs
-----------------------------------------------

The QEMU driver currently supports a single native config format known as
``qemu-argv``. The data for this format is expected to be a single line first a
list of environment variables, then the QEMu binary name, finally followed by
the QEMU command line arguments

Converting from QEMU args to domain XML
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

**Note:** this operation is :removed:`deleted as of 5.5.0` and will return an
error.

The ``virsh domxml-from-native`` provides a way to convert an existing set of
QEMU args into a guest description using libvirt Domain XML that can then be
used by libvirt. Please note that this command is intended to be used to convert
existing qemu guests previously started from the command line to be managed
through libvirt. It should not be used a method of creating new guests from
scratch. New guests should be created using an application calling the libvirt
APIs (see the `libvirt applications page <apps.html>`__ for some examples) or by
manually crafting XML to pass to virsh.

Converting from domain XML to QEMU args
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

The ``virsh domxml-to-native`` provides a way to convert a guest description
using libvirt Domain XML, into a set of QEMU args that can be run manually. Note
that currently the command line formatted by libvirt is no longer suited for
manually running qemu as the configuration expects various resources and open
file descriptors passed to the process which are usually prepared by libvirtd.

Pass-through of arbitrary qemu commands
---------------------------------------

Libvirt provides an XML namespace and an optional library ``libvirt-qemu.so``
for dealing specifically with qemu. When used correctly, these extensions allow
testing specific qemu features that have not yet been ported to the generic
libvirt XML and API interfaces. However, they are **unsupported**, in that the
library is not guaranteed to have a stable API, abusing the library or XML may
result in inconsistent state the crashes libvirtd, and upgrading either qemu-kvm
or libvirtd may break behavior of a domain that was relying on a qemu-specific
pass-through. If you find yourself needing to use them to access a particular
qemu feature, then please post an RFE to the libvirt mailing list to get that
feature incorporated into the stable libvirt XML and API interfaces.

The library provides two API: ``virDomainQemuMonitorCommand``, for sending an
arbitrary monitor command (in either HMP or QMP format) to a qemu guest (
:since:`Since 0.8.3` ), and ``virDomainQemuAttach``, for registering a qemu
domain that was manually started so that it can then be managed by libvirtd (
:since:`Since 0.9.4` , :removed:`removed as of 5.5.0` ).

Additionally, the following XML additions allow fine-tuning of the command line
given to qemu when starting a domain ( :since:`Since 0.8.3` ). In order to use
the XML additions, it is necessary to issue an XML namespace request (the
special ``xmlns:name`` attribute) that pulls in
``http://libvirt.org/schemas/domain/qemu/1.0``; typically, the namespace is
given the name of ``qemu``. With the namespace in place, it is then possible to
add an element ``<qemu:commandline>`` under ``domain``, with the following
sub-elements repeated as often as needed:

``qemu:arg``
   Add an additional command-line argument to the qemu process when starting the
   domain, given by the value of the attribute ``value``.
``qemu:env``
   Add an additional environment variable to the qemu process when starting the
   domain, given with the name-value pair recorded in the attributes ``name``
   and optional ``value``.

Example:

::

   <domain type='qemu' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
     <name>QEMU-fedora-i686</name>
     <memory>219200</memory>
     <os>
       <type arch='i686' machine='pc'>hvm</type>
     </os>
     <devices>
       <emulator>/usr/bin/qemu-system-x86_64</emulator>
     </devices>
     <qemu:commandline>
       <qemu:arg value='-newarg'/>
       <qemu:env name='QEMU_ENV' value='VAL'/>
     </qemu:commandline>
   </domain>

QEMU feature configuration for testing
--------------------------------------

In some cases e.g. when developing a new feature or for testing it may be
required to control a given qemu feature (or qemu capability) to test it before
it's complete or disable it for debugging purposes. :since:`Since 5.5.0` it's
possible to use the same special qemu namespace as above
(``http://libvirt.org/schemas/domain/qemu/1.0``) and use ``<qemu:capabilities>``
element to add (``<qemu:add capability="capname"/>``) or remove
(``<qemu:del capability="capname"/>``) capability bits. The naming of the
feature bits is the same libvirt uses in the status XML. Note that this feature
is meant for experiments only and should _not_ be used in production.

Example:

::

   <domain type='qemu' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
     <name>testvm</name>

      [...]

     <qemu:capabilities>
       <qemu:add capability='blockdev'/>
       <qemu:del capability='drive'/>
     </qemu:capabilities>
   </domain>

Control of QEMU deprecation warnings
------------------------------------

The following knob controls how QEMU behaves towards deprecated commands and
arguments used by libvirt:

::

   <domain type='qemu' xmlns:qemu='http://libvirt.org/schemas/domain/qemu/1.0'>
     <name>testvm</name>

      [...]

     <qemu:deprecation behavior='crash'/>

This setting is meant for developers and CI efforts to make it obvious when
libvirt relies on fields which are deprecated so that it can be fixes as soon
as possible.

Possible options are:

``none``
   (default) qemu is supposed to accept and output deprecated fields and commands

``omit``
    qemu is instructed to omit deprecated fields on output, behaviour towards
    fields and commands from libvirtd is not changed

``reject``
    qemu is instructed to report an error if a deprecated command or field is
    used by libvirtd

``crash``
    qemu crashes when an deprecated command or field is used by libvirtd

For both "reject" and "crash" qemu is instructed to omit any deprecated fields
on output.

The "reject" option is less harsh towards the VMs but some code paths ignore
errors reported by qemu and thus it may not be obvious that a deprecated
command/field was used, thus it's suggested to use the "crash" option instead.

In cases when qemu doesn't support configuring the behaviour this setting is
silently ignored to allow testing older qemu versions without having to
reconfigure libvirtd.

*DO NOT* use in production.

Example domain XML config
-------------------------

QEMU emulated guest on x86_64
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

   <domain type='qemu'>
     <name>QEMU-fedora-i686</name>
     <uuid>c7a5fdbd-cdaf-9455-926a-d65c16db1809</uuid>
     <memory>219200</memory>
     <currentMemory>219200</currentMemory>
     <vcpu>2</vcpu>
     <os>
       <type arch='i686' machine='pc'>hvm</type>
       <boot dev='cdrom'/>
     </os>
     <devices>
       <emulator>/usr/bin/qemu-system-x86_64</emulator>
       <disk type='file' device='cdrom'>
         <source file='/home/user/boot.iso'/>
         <target dev='hdc'/>
         <readonly/>
       </disk>
       <disk type='file' device='disk'>
         <source file='/home/user/fedora.img'/>
         <target dev='hda'/>
       </disk>
       <interface type='network'>
         <source network='default'/>
       </interface>
       <graphics type='vnc' port='-1'/>
     </devices>
   </domain>

KVM hardware accelerated guest on i686
~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~~

::

   <domain type='kvm'>
     <name>demo2</name>
     <uuid>4dea24b3-1d52-d8f3-2516-782e98a23fa0</uuid>
     <memory>131072</memory>
     <vcpu>1</vcpu>
     <os>
       <type arch="i686">hvm</type>
     </os>
     <clock sync="localtime"/>
     <devices>
       <emulator>/usr/bin/qemu-kvm</emulator>
       <disk type='file' device='disk'>
         <source file='/var/lib/libvirt/images/demo2.img'/>
         <target dev='hda'/>
       </disk>
       <interface type='network'>
         <source network='default'/>
         <mac address='24:42:53:21:52:45'/>
       </interface>
       <graphics type='vnc' port='-1' keymap='de'/>
     </devices>
   </domain>
