/*
 * networkcommon_conf.c: network XML handling
 *
 * Copyright (C) 2006-2014 Red Hat, Inc.
 * Copyright (C) 2006-2008 Daniel P. Berrange
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

#include "virerror.h"
#include "datatypes.h"
#include "networkcommon_conf.h"
#include "viralloc.h"
#include "virstring.h"
#include "virxml.h"

#define VIR_FROM_THIS VIR_FROM_NETWORK

virNetDevIPRoute *
virNetDevIPRouteCreate(const char *errorDetail,
                       const char *family,
                       const char *address,
                       const char *netmask,
                       const char *gateway,
                       unsigned int prefix,
                       bool hasPrefix,
                       unsigned int metric,
                       bool hasMetric)
{
    g_autoptr(virNetDevIPRoute) def = NULL;
    virSocketAddr testAddr;

    def = g_new0(virNetDevIPRoute, 1);

    def->family = g_strdup(family);

    def->prefix = prefix;
    def->has_prefix = hasPrefix;
    def->metric = metric;
    def->has_metric = hasMetric;

    /* Note: both network and gateway addresses must be specified */

    if (!address) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Missing required address attribute "
                         "in route definition"),
                       errorDetail);
        return NULL;
    }

    if (!gateway) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Missing required gateway attribute "
                         "in route definition"),
                       errorDetail);
        return NULL;
    }

    if (virSocketAddrParse(&def->address, address, AF_UNSPEC) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Bad network address '%s' "
                         "in route definition"),
                       errorDetail, address);
        return NULL;
    }

    if (virSocketAddrParse(&def->gateway, gateway, AF_UNSPEC) < 0) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Bad gateway address '%s' "
                         "in route definition"),
                       errorDetail, gateway);
        return NULL;
    }

    /* validate network address, etc. for each family */
    if ((def->family == NULL) || (STREQ(def->family, "ipv4"))) {
        if (!(VIR_SOCKET_ADDR_IS_FAMILY(&def->address, AF_INET) ||
              VIR_SOCKET_ADDR_IS_FAMILY(&def->address, AF_UNSPEC))) {
            virReportError(VIR_ERR_XML_ERROR,
                           def->family == NULL ?
                           _("%s: No family specified for non-IPv4 address '%s' "
                             "in route definition") :
                           _("%s: IPv4 family specified for non-IPv4 address '%s' "
                             "in route definition"),
                           errorDetail, address);
            return NULL;
        }
        if (!VIR_SOCKET_ADDR_IS_FAMILY(&def->gateway, AF_INET)) {
            virReportError(VIR_ERR_XML_ERROR,
                           def->family == NULL ?
                           _("%s: No family specified for non-IPv4 gateway '%s' "
                             "in route definition") :
                           _("%s: IPv4 family specified for non-IPv4 gateway '%s' "
                             "in route definition"),
                           errorDetail, address);
            return NULL;
        }
        if (netmask) {
            if (virSocketAddrParse(&def->netmask, netmask, AF_UNSPEC) < 0) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("%s: Bad netmask address '%s' "
                                 "in route definition"),
                               errorDetail, netmask);
                return NULL;
            }
            if (!VIR_SOCKET_ADDR_IS_FAMILY(&def->netmask, AF_INET)) {
                virReportError(VIR_ERR_XML_ERROR,
                               _("%s: Invalid netmask '%s' "
                                 "for address '%s' (both must be IPv4)"),
                               errorDetail, netmask, address);
                return NULL;
            }
            if (def->has_prefix) {
                /* can't have both netmask and prefix at the same time */
                virReportError(VIR_ERR_XML_ERROR,
                               _("%s: Route definition cannot have both "
                                 "a prefix and a netmask"),
                               errorDetail);
                return NULL;
            }
        }
        if (def->prefix > 32) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s: Invalid prefix %u specified "
                             "in route definition, "
                             "must be 0 - 32"),
                           errorDetail, def->prefix);
            return NULL;
        }
    } else if (STREQ(def->family, "ipv6")) {
        if (!VIR_SOCKET_ADDR_IS_FAMILY(&def->address, AF_INET6)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s: ipv6 family specified for non-IPv6 address '%s' "
                             "in route definition"),
                           errorDetail, address);
            return NULL;
        }
        if (netmask) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s: Specifying netmask invalid for IPv6 address '%s' "
                             "in route definition"),
                           errorDetail, address);
            return NULL;
        }
        if (!VIR_SOCKET_ADDR_IS_FAMILY(&def->gateway, AF_INET6)) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s: ipv6 specified for non-IPv6 gateway address '%s' "
                             "in route definition"),
                           errorDetail, gateway);
            return NULL;
        }
        if (def->prefix > 128) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s: Invalid prefix %u specified "
                             "in route definition, "
                             "must be 0 - 128"),
                           errorDetail, def->prefix);
            return NULL;
        }
    } else {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Unrecognized family '%s' "
                         "in route definition"),
                       errorDetail, def->family);
        return NULL;
    }

    /* make sure the address is a network address */
    if (netmask) {
        if (virSocketAddrMask(&def->address, &def->netmask, &testAddr) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("%s: Error converting address '%s' with netmask '%s' "
                             "to network-address "
                             "in route definition"),
                           errorDetail, address, netmask);
            return NULL;
        }
    } else {
        if (virSocketAddrMaskByPrefix(&def->address,
                                      def->prefix, &testAddr) < 0) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("%s: Error converting address '%s' with prefix %u "
                             "to network-address "
                             "in route definition"),
                           errorDetail, address, def->prefix);
            return NULL;
        }
    }
    if (!virSocketAddrEqual(&def->address, &testAddr)) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Address '%s' in route definition "
                         "is not a network address"),
                       errorDetail, address);
        return NULL;
    }

    return g_steal_pointer(&def);
}

virNetDevIPRoute *
virNetDevIPRouteParseXML(const char *errorDetail,
                         xmlNodePtr node,
                         xmlXPathContextPtr ctxt)
{
    /*
     * virNetDevIPRoute object is already allocated as part
     * of an array.  On failure clear: it out, but don't free it.
     */

    VIR_XPATH_NODE_AUTORESTORE(ctxt)
    g_autofree char *family = NULL;
    g_autofree char *address = NULL;
    g_autofree char *netmask = NULL;
    g_autofree char *gateway = NULL;
    unsigned long prefix = 0, metric = 0;
    int prefixRc, metricRc;
    bool hasPrefix = false;
    bool hasMetric = false;

    ctxt->node = node;

    /* grab raw data from XML */
    family = virXPathString("string(./@family)", ctxt);
    address = virXPathString("string(./@address)", ctxt);
    netmask = virXPathString("string(./@netmask)", ctxt);
    gateway = virXPathString("string(./@gateway)", ctxt);
    prefixRc = virXPathULong("string(./@prefix)", ctxt, &prefix);
    if (prefixRc == -2) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Invalid prefix specified "
                         "in route definition"),
                       errorDetail);
        return NULL;
    }
    hasPrefix = (prefixRc == 0);
    metricRc = virXPathULong("string(./@metric)", ctxt, &metric);
    if (metricRc == -2) {
        virReportError(VIR_ERR_XML_ERROR,
                       _("%s: Invalid metric specified "
                         "in route definition"),
                       errorDetail);
        return NULL;
    }
    if (metricRc == 0) {
        hasMetric = true;
        if (metric == 0) {
            virReportError(VIR_ERR_XML_ERROR,
                           _("%s: Invalid metric value, must be > 0 "
                             "in route definition"),
                           errorDetail);
            return NULL;
        }
    }

    return virNetDevIPRouteCreate(errorDetail, family, address, netmask,
                                  gateway, prefix, hasPrefix, metric,
                                  hasMetric);
}

int
virNetDevIPRouteFormat(virBuffer *buf,
                       const virNetDevIPRoute *def)
{
    g_autofree char *address = NULL;
    g_autofree char *netmask = NULL;
    g_autofree char *gateway = NULL;

    virBufferAddLit(buf, "<route");

    if (def->family)
        virBufferAsprintf(buf, " family='%s'", def->family);

    if (!(address = virSocketAddrFormat(&def->address)))
        return -1;
    virBufferAsprintf(buf, " address='%s'", address);

    if (VIR_SOCKET_ADDR_VALID(&def->netmask)) {
        if (!(netmask = virSocketAddrFormat(&def->netmask)))
            return -1;
        virBufferAsprintf(buf, " netmask='%s'", netmask);
    }
    if (def->has_prefix)
        virBufferAsprintf(buf, " prefix='%u'", def->prefix);

    if (!(gateway = virSocketAddrFormat(&def->gateway)))
        return -1;
    virBufferAsprintf(buf, " gateway='%s'", gateway);

    if (def->has_metric && def->metric > 0)
        virBufferAsprintf(buf, " metric='%u'", def->metric);
    virBufferAddLit(buf, "/>\n");

    return 0;
}
