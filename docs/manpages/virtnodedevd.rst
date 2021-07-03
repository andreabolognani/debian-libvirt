============
virtnodedevd
============

-------------------------------------
libvirt host device management daemon
-------------------------------------

:Manual section: 8
:Manual group: Virtualization Support

.. contents::

SYNOPSIS
========

``virtnodedevd`` [*OPTION*]...


DESCRIPTION
===========

The ``virtnodedevd`` program is a server side daemon component of the libvirt
virtualization management system.

It is one of a collection of modular daemons that replace functionality
previously provided by the monolithic ``libvirtd`` daemon.

This daemon runs on virtualization hosts to provide management for host devices.

The ``virtnodedevd`` daemon only listens for requests on a local Unix domain
socket. Remote off-host access and backwards compatibility with legacy
clients expecting ``libvirtd`` is provided by the ``virtproxy`` daemon.

Restarting ``virtnodedevd`` does not interrupt running guests. Guests continue to
operate and changes in their state will generally be picked up automatically
during startup. None the less it is recommended to avoid restarting with
running guests whenever practical.


SYSTEM SOCKET ACTIVATION
========================

The ``virtnodedevd`` daemon is capable of starting in two modes.

In the traditional mode, it will create and listen on UNIX sockets itself.

In socket activation mode, it will rely on systemd to create and listen
on the UNIX sockets and pass them as pre-opened file descriptors. In this
mode most of the socket related config options in
``/etc/libvirt/virtnodedevd.conf`` will no longer have any effect.

Socket activation mode is generally the default when running on a host
OS that uses systemd. To revert to the traditional mode, all the socket
unit files must be masked:

::

   $ systemctl mask virtnodedevd.socket virtnodedevd-ro.socket \
      virtnodedevd-admin.socket


OPTIONS
=======

``-h``, ``--help``

Display command line help usage then exit.

``-d``, ``--daemon``

Run as a daemon & write PID file.

``-f``, ``--config *FILE*``

Use this configuration file, overriding the default value.

``-p``, ``--pid-file *FILE*``

Use this name for the PID file, overriding the default value.

``-t``, ``--timeout *SECONDS*``

Exit after timeout period (in seconds), provided there are neither any client
connections nor any running domains.

``-v``, ``--verbose``

Enable output of verbose messages.

``--version``

Display version information then exit.


SIGNALS
=======

On receipt of ``SIGHUP`` ``virtnodedevd`` will reload its configuration.


FILES
=====

When run as *root*
------------------

* ``@SYSCONFDIR@/libvirt/virtnodedevd.conf``

The default configuration file used by ``virtnodedevd``, unless overridden on the
command line using the ``-f`` | ``--config`` option.

* ``@RUNSTATEDIR@/libvirt/virtnodedevd-sock``
* ``@RUNSTATEDIR@/libvirt/virtnodedevd-sock-ro``
* ``@RUNSTATEDIR@/libvirt/virtnodedevd-admin-sock``

The sockets ``virtnodedevd`` will use.

The TLS **Server** private key ``virtnodedevd`` will use.

* ``@RUNSTATEDIR@/virtnodedevd.pid``

The PID file to use, unless overridden by the ``-p`` | ``--pid-file`` option.


When run as *non-root*
----------------------

* ``$XDG_CONFIG_HOME/libvirt/virtnodedevd.conf``

The default configuration file used by ``virtnodedevd``, unless overridden on the
command line using the ``-f``|``--config`` option.

* ``$XDG_RUNTIME_DIR/libvirt/virtnodedevd-sock``
* ``$XDG_RUNTIME_DIR/libvirt/virtnodedevd-admin-sock``

The sockets ``virtnodedevd`` will use.

* ``$XDG_RUNTIME_DIR/libvirt/virtnodedevd.pid``

The PID file to use, unless overridden by the ``-p``|``--pid-file`` option.


If ``$XDG_CONFIG_HOME`` is not set in your environment, ``virtnodedevd`` will use
``$HOME/.config``

If ``$XDG_RUNTIME_DIR`` is not set in your environment, ``virtnodedevd`` will use
``$HOME/.cache``


EXAMPLES
========

To retrieve the version of ``virtnodedevd``:

::

  # virtnodedevd --version
  virtnodedevd (libvirt) @VERSION@


To start ``virtnodedevd``, instructing it to daemonize and create a PID file:

::

  # virtnodedevd -d
  # ls -la @RUNSTATEDIR@/virtnodedevd.pid
  -rw-r--r-- 1 root root 6 Jul  9 02:40 @RUNSTATEDIR@/virtnodedevd.pid


BUGS
====

Please report all bugs you discover.  This should be done via either:

#. the mailing list

   `https://libvirt.org/contact.html <https://libvirt.org/contact.html>`_

#. the bug tracker

   `https://libvirt.org/bugs.html <https://libvirt.org/bugs.html>`_

Alternatively, you may report bugs to your software distributor / vendor.


AUTHORS
=======

Please refer to the AUTHORS file distributed with libvirt.


COPYRIGHT
=========

Copyright (C) 2006-2020 Red Hat, Inc., and the authors listed in the
libvirt AUTHORS file.


LICENSE
=======

``virtnodedevd`` is distributed under the terms of the GNU LGPL v2.1+.
This is free software; see the source for copying conditions. There
is NO warranty; not even for MERCHANTABILITY or FITNESS FOR A PARTICULAR
PURPOSE


SEE ALSO
========

virsh(1), libvirtd(8),
`https://www.libvirt.org/daemons.html <https://www.libvirt.org/daemons.html>`_,
`https://www.libvirt.org/drvnodedev.html <https://www.libvirt.org/drvnodedev.html>`_
