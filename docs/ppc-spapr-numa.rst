
NUMA mechanics for sPAPR (pseries machines)
============================================

NUMA in sPAPR works different than the System Locality Distance
Information Table (SLIT) in ACPI. The logic is explained in the LOPAPR
1.1 chapter 15, "Non Uniform Memory Access (NUMA) Option". This
document aims to complement this specification, providing details
of the elements that impacts how QEMU views NUMA in pseries.

Associativity and ibm,associativity property
--------------------------------------------

Associativity is defined as a group of platform resources that has
similar mean performance (or in our context here, distance) relative to
everyone else outside of the group.

The format of the ibm,associativity property varies with the value of
bit 0 of byte 5 of the ibm,architecture-vec-5 property. The format with
bit 0 equal to zero is deprecated. The current format, with the bit 0
with the value of one, makes ibm,associativity property represent the
physical hierarchy of the platform, as one or more lists that starts
with the highest level grouping up to the smallest. Considering the
following topology:

::

    Mem M1 ---- Proc P1    |
    -----------------      | Socket S1  ---|
          chip C1          |               |
                                           | HW module 1 (MOD1)
    Mem M2 ---- Proc P2    |               |
    -----------------      | Socket S2  ---|
          chip C2          |

The ibm,associativity property for the processors would be:

* P1: {MOD1, S1, C1, P1}
* P2: {MOD1, S2, C2, P2}

Each allocable resource has an ibm,associativity property. The LOPAPR
specification allows multiple lists to be present in this property,
considering that the same resource can have multiple connections to the
platform.

Relative Performance Distance and ibm,associativity-reference-points
--------------------------------------------------------------------

The ibm,associativity-reference-points property is an array that is used
to define the relevant performance/distance  related boundaries, defining
the NUMA levels for the platform.

The definition of its elements also varies with the value of bit 0 of byte 5
of the ibm,architecture-vec-5 property. The format with bit 0 equal to zero
is also deprecated. With the current format, each integer of the
ibm,associativity-reference-points represents an 1 based ordinal index (i.e.
the first element is 1) of the ibm,associativity array. The first
boundary is the most significant to application performance, followed by
less significant boundaries. Allocated resources that belongs to the
same performance boundaries are expected to have relative NUMA distance
that matches the relevancy of the boundary itself. Resources that belongs
to the same first boundary will have the shortest distance from each
other. Subsequent boundaries represents greater distances and degraded
performance.

Using the previous example, the following setting reference points defines
three NUMA levels:

* ibm,associativity-reference-points = {0x3, 0x2, 0x1}

The first NUMA level (0x3) is interpreted as the third element of each
ibm,associativity array, the second level is the second element and
the third level is the first element. Let's also consider that elements
belonging to the first NUMA level have distance equal to 10 from each
other, and each NUMA level doubles the distance from the previous. This
means that the second would be 20 and the third level 40. For the P1 and
P2 processors, we would have the following NUMA levels:

::

  * ibm,associativity-reference-points = {0x3, 0x2, 0x1}

  * P1: associativity{MOD1, S1, C1, P1}

  First NUMA level (0x3) => associativity[2] = C1
  Second NUMA level (0x2) => associativity[1] = S1
  Third NUMA level (0x1) => associativity[0] = MOD1

  * P2: associativity{MOD1, S2, C2, P2}

  First NUMA level (0x3) => associativity[2] = C2
  Second NUMA level (0x2) => associativity[1] = S2
  Third NUMA level (0x1) => associativity[0] = MOD1

  P1 and P2 have the same third NUMA level, MOD1: Distance between them = 40

Changing the ibm,associativity-reference-points array changes the performance
distance attributes for the same associativity arrays, as the following
example illustrates:

::

  * ibm,associativity-reference-points = {0x2}

  * P1: associativity{MOD1, S1, C1, P1}

  First NUMA level (0x2) => associativity[1] = S1

  * P2: associativity{MOD1, S2, C2, P2}

  First NUMA level (0x2) => associativity[1] = S2

  P1 and P2 does not have a common performance boundary. Since this is a one level
  NUMA configuration, distance between them is one boundary above the first
  level, 20.


In a hypothetical platform where all resources inside the same hardware module
is considered to be on the same performance boundary:

::

  * ibm,associativity-reference-points = {0x1}

  * P1: associativity{MOD1, S1, C1, P1}

  First NUMA level (0x1) => associativity[0] = MOD0

  * P2: associativity{MOD1, S2, C2, P2}

  First NUMA level (0x1) => associativity[0] = MOD0

  P1 and P2 belongs to the same first order boundary. The distance between then
  is 10.




As an example, let's consider a memory region M right next to a processor P,
and the distance of M to every other resource in the platform being D. Due to
the close proximity of M and P, the mean distance of P to everyone else is also
D. One implication is that any operation between M and P will present the best
performance possible due to the high associativity between the resources. The
other implication is that both M and P can be seen as a group G that has the
a distance D to every other resource. This is called an associativity domain.

Multiple levels of associativity domains are allowed. A common example would
be processors that belongs to the same socket. Consider a similar grouping
between a memory region M2 and a processor P2, grouped together as a group G2,
in the same socket as the previous group G. The distance 'd', e.g. 20, from
G2 to G is less than the distance D to both G and G2 to the other resources,
meaning that the socket itself can be defined as another associativity
domain, G3. A task running a in P2 will get better performance accessing the
memory with the highest associativity possible, meaning that M2 would be the
best, followed by M, and then all other memory regions.


For the previous examplewe would have the following values of ibm,associativity:

* Mem M and Proc P: G3, G
* Mem M2 and Proc P2: G3, G2


