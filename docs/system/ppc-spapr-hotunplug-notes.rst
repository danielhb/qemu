======================================
sPAPR (pseries) device hotunplug notes
======================================

For the most part, device hotunplug in the pseries machine works like other
QEMU machines. This document aims to provide the user with details on where
the hotunplug process will differ from the norm in pseries, allowing the user
to set expectations on what works and what doesn't.


General hotunplug baseline: be mindful of current esource usage
===============================================================

Device hotunplug is a complex operation by itself. All devices are prone to
hotunplug failure depending on guest load. Although the pseries machine doesn't
forbid any hotunplug operation based on guest resource usage, the user is
*strongly* encouraged to not attempt hotunplug operations on resources that
are being used heavily, e.g. hotunplugging a disk in the middle of intense
I/O operations or hotunplugging memory modules while the guest is running
memory-intensive workloads.


Hotunplug in pseries - the guest knows it all, QEMU doesn't
===========================================================

The pseries machine follows the LOPAR specification [1]. This specification
does not determine any way for the guest operational system to report hotunplug
failure events back to the platform (in our case, QEMU). This means that the
hotunplug result is uncertain from the QEMU side until the device is released
from the guest and removed from QEMU. If any error occurs on the guest side,
QEMU will not be aware.

Let's demonstrate it with a quick example. Here is a case of a CPU hotplug and
hotunplug in a pseries machine running kernel 4.18, which booted with a single
CPU:

::

    (qemu) device_add host-spapr-cpu-core,core-id=1,id=core1
    (qemu)
    ( ---- back in the guest ---- )
    [root@guest ~]# lscpu
    Architecture:        ppc64le
    Byte Order:          Little Endian
    CPU(s):              2
    On-line CPU(s) list: 0,1
    Thread(s) per core:  1
    Core(s) per socket:  2
    Socket(s):           1
    NUMA node(s):        1
    (...)

    [root@guest ~]# (qemu)
    (qemu) device_del core1
    (qemu)
    [root@guest ~]# lscpu
    Architecture:        ppc64le
    Byte Order:          Little Endian
    CPU(s):              1
    On-line CPU(s) list: 0
    Thread(s) per core:  1
    Core(s) per socket:  1
    Socket(s):           1
    NUMA node(s):        1
    (...)

When issuing `device_del core1` what happens is:

1. QEMU marks the CPU 'core1' as pending unplug
2. a hotplug event is sent to the guest to handle. QEMU waits for the guest to
   signal the release of the CPU
3. the guest releases CPU 'core1'. QEMU is notified  of the removal
4. the device 'core1' is removed from QEMU

This time, let's make the CPU hotunplug fail by offlining all CPUs of the guest but
the hotplugged CPU. The guest kernel will refuse to release the CPU, and QEMU will
be unaware of it:

::

    (qemu) device_add host-spapr-cpu-core,core-id=1,id=core1
    (qemu)
    [root@guest ~]# lscpu
    Architecture:        ppc64le
    Byte Order:          Little Endian
    CPU(s):              2
    On-line CPU(s) list: 0,1
    (...)

    (---- disable vcpu 0 ----)

    [root@guest ~]# chcpu -d 0
    CPU 0 disabled
    [root@guest ~]# lscpu
    Architecture:         ppc64le
    Byte Order:           Little Endian
    CPU(s):               2
    On-line CPU(s) list:  1
    Off-line CPU(s) list: 0
    (...)

    (---- hotunplug 'core1' fails because it is now the last online CPU ----)

    [root@guest ~]# (qemu)
    (qemu) device_del core1
    (qemu)

    [root@newhostname-qga5 ~]# lscpu
    Architecture:         ppc64le
    Byte Order:           Little Endian
    CPU(s):               2
    On-line CPU(s) list:  1
    Off-line CPU(s) list: 0
    (...)

    (---- enabling vcpu0 and hotunplugging 'core1' does nothing, because
          QEMU thinks the first hotunplug of 'core1' is still ongoing ----)

    [root@guest ~]# chcpu -e 0
    CPU 0 enabled
    [root@guest ~]# lscpu
    Architecture:        ppc64le
    Byte Order:          Little Endian
    CPU(s):              2
    On-line CPU(s) list: 0,1
    (...)
    [root@guest ~]# (qemu)
    (qemu) device_del core1
    (qemu)
    [root@guest ~]# lscpu
    Architecture:         ppc64le
    Byte Order:           Little Endian
    CPU(s):               2
    On-line CPU(s) list: 0,1

This behavior was reported in https://bugzilla.redhat.com/1911414 and it's a scenario where the
CPU hotunplug fails in 100% of the time. The relevant events that happened:

1. A CPU was hotplugged to the guest
2. The guest, now with 2 CPUs, offlines CPU 0 (the boot CPU). 'core1' is now the
   last online CPU of the guest
3. 'device_del core1' will mark CPU 'core1' as pending unplug
4. The kernel will not release its last online CPU, but QEMU will keep waiting for
   the unplug
5. At this point,  CPU 'core1' will still be in the 'pending unplug' state in QEMU, until
   the guest is either rebooted or shutdown. It doesn't matter if the guest is now able
   to hotunplug 'core1'

All pseries machine prior to pseries-6.0 is affected by this behavior. pseries-6.0 solved
this issue by adding an hotunplug timeout for CPUs.

