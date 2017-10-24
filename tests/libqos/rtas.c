/*
 * This work is licensed under the terms of the GNU GPL, version 2 or later.
 * See the COPYING file in the top-level directory.
 */

#include "qemu/osdep.h"
#include "libqtest.h"
#include "libqos/rtas.h"

static void qrtas_copy_args(uint64_t target_args, uint32_t nargs,
                            uint32_t *args)
{
    int i;

    for (i = 0; i < nargs; i++) {
        writel(target_args + i * sizeof(uint32_t), args[i]);
    }
}

static void qrtas_copy_ret(uint64_t target_ret, uint32_t nret, uint32_t *ret)
{
    int i;

    for (i = 0; i < nret; i++) {
        ret[i] = readl(target_ret + i * sizeof(uint32_t));
    }
}

static uint64_t qrtas_call(QGuestAllocator *alloc, const char *name,
                           uint32_t nargs, uint32_t *args,
                           uint32_t nret, uint32_t *ret)
{
    uint64_t res;
    uint64_t target_args, target_ret;

    target_args = guest_alloc(alloc, nargs * sizeof(uint32_t));
    target_ret = guest_alloc(alloc, nret * sizeof(uint32_t));

    qrtas_copy_args(target_args, nargs, args);
    res = qtest_rtas_call(global_qtest, name,
                          nargs, target_args, nret, target_ret);
    qrtas_copy_ret(target_ret, nret, ret);

    guest_free(alloc, target_ret);
    guest_free(alloc, target_args);

    return res;
}

int qrtas_get_time_of_day(QGuestAllocator *alloc, struct tm *tm, uint32_t *ns)
{
    int res;
    uint32_t ret[8];

    res = qrtas_call(alloc, "get-time-of-day", 0, NULL, 8, ret);
    if (res != 0) {
        return res;
    }

    res = ret[0];
    memset(tm, 0, sizeof(*tm));
    tm->tm_year = ret[1] - 1900;
    tm->tm_mon = ret[2] - 1;
    tm->tm_mday = ret[3];
    tm->tm_hour = ret[4];
    tm->tm_min = ret[5];
    tm->tm_sec = ret[6];
    *ns = ret[7];

    return res;
}

uint32_t qrtas_ibm_read_pci_config(QGuestAllocator *alloc, uint64_t buid,
                                   uint32_t addr, uint32_t size)
{
    int res;
    uint32_t args[4], ret[2];

    args[0] = addr;
    args[1] = buid >> 32;
    args[2] = buid & 0xffffffff;
    args[3] = size;
    res = qrtas_call(alloc, "ibm,read-pci-config", 4, args, 2, ret);
    if (res != 0) {
        return -1;
    }

    if (ret[0] != 0) {
        return -1;
    }

    return ret[1];
}

int qrtas_ibm_write_pci_config(QGuestAllocator *alloc, uint64_t buid,
                               uint32_t addr, uint32_t size, uint32_t val)
{
    int res;
    uint32_t args[5], ret[1];

    args[0] = addr;
    args[1] = buid >> 32;
    args[2] = buid & 0xffffffff;
    args[3] = size;
    args[4] = val;
    res = qrtas_call(alloc, "ibm,write-pci-config", 5, args, 1, ret);
    if (res != 0) {
        return -1;
    }

    if (ret[0] != 0) {
        return -1;
    }

    return 0;
}

/*
 * check_exception as defined by PAPR 2.7+, 7.3.3.2
 *
 * nargs = 7 (with Extended Information)
 * nrets = 1
 *
 * arg[2] = mask of event classes to process
 * arg[4] = real address of error log
 * arg[5] = length of error log
 *
 * arg[0] (Vector Offset), arg[1] and arg[6] (Additional information)
 * and arg[3] (Critical) aren't used in the logic of check_exception
 * in hw/ppc/spapr_events.c and can be ignored.
 *
 * If there is an event that matches the given mask, check-exception writes
 * it in buf_addr up to a max of buf_len bytes.
 *
 */
int qrtas_check_exception(QGuestAllocator *alloc, uint32_t mask,
                          uint32_t buf_addr, uint32_t buf_len)
{
    uint32_t args[7], ret[1];
    int res;

    args[0] = args[1] = args[3] = args[6] = 0;
    args[2] = mask;
    args[4] = buf_addr;
    args[5] = buf_len;

    res = qrtas_call(alloc, "check-exception", 7, args, 1, ret);
    if (res != 0) {
        return -1;
    }

    return ret[0];
}

/*
 * set_indicator as defined by PAPR 2.7+, 7.3.5.4
 *
 * nargs = 3
 * nrets = 1
 *
 * arg[0] = the type of the indicator
 * arg[1] = index of the specific indicator
 * arg[2] = desired new state
 *
 * Depending on the input, set_indicator will call set_isolation_state,
 * set_allocation_state or set_dr_indicator in hw/ppc/spapr_drc.c.
 * These functions allows the guest to control the state of hotplugged
 * and hot unplugged devices.
 */
int qrtas_set_indicator(QGuestAllocator *alloc, uint32_t type, uint32_t idx,
                        uint32_t new_state)
{
    uint32_t args[3], ret[1];
    int res;

    args[0] = type;
    args[1] = idx;
    args[2] = new_state;

    res = qrtas_call(alloc, "set-indicator", 3, args, 1, ret);
    if (res != 0) {
        return -1;
    }

    return ret[0];
}
