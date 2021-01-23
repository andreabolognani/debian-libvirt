/*
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

#include "testutils.h"
#include "virnetdevopenvswitch.h"

#define VIR_FROM_THIS VIR_FROM_NONE

typedef struct _InterfaceParseStatsData InterfaceParseStatsData;
struct _InterfaceParseStatsData {
    const char *filename;
    const virDomainInterfaceStatsStruct stats;
};


static int
testInterfaceParseStats(const void *opaque)
{
    const InterfaceParseStatsData *data = opaque;
    g_autofree char *filename = NULL;
    g_autofree char *buf = NULL;
    virDomainInterfaceStatsStruct actual;

    filename = g_strdup_printf("%s/virnetdevopenvswitchdata/%s", abs_srcdir,
                               data->filename);

    if (virFileReadAll(filename, 1024, &buf) < 0)
        return -1;

    if (virNetDevOpenvswitchInterfaceParseStats(buf, &actual) < 0)
        return -1;

    if (memcmp(&actual, &data->stats, sizeof(actual)) != 0) {
        fprintf(stderr,
                "Expected stats: %lld %lld %lld %lld %lld %lld %lld %lld\n"
                "Actual stats: %lld %lld %lld %lld %lld %lld %lld %lld",
                data->stats.rx_bytes,
                data->stats.rx_packets,
                data->stats.rx_errs,
                data->stats.rx_drop,
                data->stats.tx_bytes,
                data->stats.tx_packets,
                data->stats.tx_errs,
                data->stats.tx_drop,
                actual.rx_bytes,
                actual.rx_packets,
                actual.rx_errs,
                actual.rx_drop,
                actual.tx_bytes,
                actual.tx_packets,
                actual.tx_errs,
                actual.tx_drop);

        return -1;
    }

    return 0;
}


typedef struct _escapeData escapeData;
struct _escapeData {
    const char *input;
    const char *expect;
};


static int
testNameEscape(const void *opaque)
{
    const escapeData *data = opaque;
    g_autofree char *reply = g_strdup(data->input);
    int rv;

    rv = virNetDevOpenvswitchMaybeUnescapeReply(reply);

    if (data->expect) {
        if (rv < 0 || STRNEQ(reply, data->expect)) {
            fprintf(stderr,
                    "Unexpected failure, expected: %s for input %s got %s\n",
                    data->expect, data->input, reply);
            return -1;
        }
    } else {
        if (rv >= 0) {
            fprintf(stderr,
                    "Unexpected success, input %s got %s\n",
                    data->input, reply);
            return -1;
        }
    }

    return 0;
}


static int
mymain(void)
{
    int ret = 0;

#define TEST_INTERFACE_STATS(file, \
                             rxBytes, rxPackets, rxErrs, rxDrop, \
                             txBytes, txPackets, txErrs, txDrop) \
    do { \
        const InterfaceParseStatsData data = {.filename = file, .stats = { \
                             rxBytes, rxPackets, rxErrs, rxDrop, \
                             txBytes, txPackets, txErrs, txDrop}}; \
        if (virTestRun("Interface stats " file, testInterfaceParseStats, &data) < 0) \
            ret = -1; \
    } while (0)

    TEST_INTERFACE_STATS("stats1.json", 9, 12, 11, 10, 2, 8, 5, 4);
    TEST_INTERFACE_STATS("stats2.json", 12406, 173, 0, 0, 0, 0, 0, 0);

#define TEST_NAME_ESCAPE(str, fail) \
    do { \
        const escapeData data = {str, fail};\
        if (virTestRun("Name escape " str, testNameEscape, &data) < 0) \
            ret = -1; \
    } while (0)

    TEST_NAME_ESCAPE("", "");
    TEST_NAME_ESCAPE("\"\"", "");
    TEST_NAME_ESCAPE("vhost-user1", "vhost-user1");
    TEST_NAME_ESCAPE("\"vhost-user1\"", "vhost-user1");
    TEST_NAME_ESCAPE("\"vhost_user-name.to.escape1", NULL);
    TEST_NAME_ESCAPE("\"vhost_user-name.to\\\"escape1\"", "vhost_user-name.to\"escape1");
    TEST_NAME_ESCAPE("\"vhost\"user1\"", NULL);
    TEST_NAME_ESCAPE("\"\\\\", NULL);

    return ret == 0 ? EXIT_SUCCESS : EXIT_FAILURE;
}

VIR_TEST_MAIN(mymain);
