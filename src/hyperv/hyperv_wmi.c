/*
 * hyperv_wmi.c: general WMI over WSMAN related functions and structures for
 *               managing Microsoft Hyper-V hosts
 *
 * Copyright (C) 2017 Datto Inc
 * Copyright (C) 2014 Red Hat, Inc.
 * Copyright (C) 2011 Matthias Bolte <matthias.bolte@googlemail.com>
 * Copyright (C) 2009 Michael Sievers <msievers83@googlemail.com>
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
 *
 */

#include <config.h>

#include <wsman-soap.h>
#include <wsman-xml.h>
#include <wsman-xml-binding.h>

#include "internal.h"
#include "virerror.h"
#include "datatypes.h"
#include "viralloc.h"
#include "viruuid.h"
#include "virbuffer.h"
#include "hyperv_private.h"
#include "hyperv_wmi.h"
#include "virstring.h"
#include "virlog.h"
#include "virxml.h"

#define VIR_FROM_THIS VIR_FROM_HYPERV

#define HYPERV_JOB_TIMEOUT_MS 300000

VIR_LOG_INIT("hyperv.hyperv_wmi");

int
hypervGetWmiClassList(hypervPrivate *priv, hypervWmiClassInfoPtr wmiInfo,
                      virBufferPtr query, hypervObject **wmiClass)
{
    hypervWqlQuery wqlQuery = HYPERV_WQL_QUERY_INITIALIZER;

    wqlQuery.info = wmiInfo;
    wqlQuery.query = query;

    return hypervEnumAndPull(priv, &wqlQuery, wmiClass);
}


int
hypervVerifyResponse(WsManClient *client, WsXmlDocH response,
                     const char *detail)
{
    int lastError = wsmc_get_last_error(client);
    int responseCode = wsmc_get_response_code(client);
    WsManFault *fault;

    if (lastError != WS_LASTERR_OK) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Transport error during %s: %s (%d)"),
                       detail, wsman_transport_get_last_error_string(lastError),
                       lastError);
        return -1;
    }

    /* Check the HTTP response code and report an error if it's not 200 (OK),
     * 400 (Bad Request) or 500 (Internal Server Error) */
    if (responseCode != 200 && responseCode != 400 && responseCode != 500) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Unexpected HTTP response during %s: %d"),
                       detail, responseCode);
        return -1;
    }

    if (response == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Empty response during %s"), detail);
        return -1;
    }

    if (wsmc_check_for_fault(response)) {
        fault = wsmc_fault_new();

        if (fault == NULL) {
            virReportOOMError();
            return -1;
        }

        wsmc_get_fault_data(response, fault);

        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("SOAP fault during %s: code '%s', subcode '%s', "
                         "reason '%s', detail '%s'"),
                       detail, NULLSTR(fault->code), NULLSTR(fault->subcode),
                       NULLSTR(fault->reason), NULLSTR(fault->fault_detail));

        wsmc_fault_destroy(fault);
        return -1;
    }

    return 0;
}


/*
 * Methods to work with method invocation parameters
 */

/*
 * hypervCreateInvokeParamsList:
 * @method: The name of the method you are calling
 * @selector: The selector for the object you are invoking the method on
 * @obj: The WmiInfo of the object class you are invoking the method on.
 *
 * Create a new InvokeParamsList object for the method call.
 *
 * Returns a pointer to the newly instantiated object on success, which should
 * be freed by hypervInvokeMethod. Otherwise returns NULL.
 */
hypervInvokeParamsListPtr
hypervCreateInvokeParamsList(const char *method,
                             const char *selector,
                             hypervWmiClassInfoPtr info)
{
    hypervInvokeParamsListPtr params = NULL;

    params = g_new0(hypervInvokeParamsList, 1);

    params->params = g_new0(hypervParam, HYPERV_DEFAULT_PARAM_COUNT);

    params->method = method;
    params->ns = info->rootUri;
    params->resourceUri = info->resourceUri;
    params->selector = selector;
    params->nbParams = 0;
    params->nbAvailParams = HYPERV_DEFAULT_PARAM_COUNT;

    return params;
}


/*
 * hypervFreeInvokeParams:
 * @params: Params object to be freed
 *
 */
void
hypervFreeInvokeParams(hypervInvokeParamsListPtr params)
{
    hypervParamPtr p = NULL;
    size_t i = 0;

    if (params == NULL)
        return;

    for (i = 0; i < params->nbParams; i++) {
        p = &(params->params[i]);

        switch (p->type) {
        case HYPERV_SIMPLE_PARAM:
            break;
        case HYPERV_EPR_PARAM:
            virBufferFreeAndReset(p->epr.query);
            break;
        case HYPERV_EMBEDDED_PARAM:
            hypervFreeEmbeddedParam(p->embedded.table);
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Invalid parameter type passed to free"));
        }
    }

    VIR_DISPOSE_N(params->params, params->nbAvailParams);
    VIR_FREE(params);
}


static inline int
hypervCheckParams(hypervInvokeParamsListPtr params)
{
    if (params->nbParams + 1 > params->nbAvailParams) {
        if (VIR_EXPAND_N(params->params, params->nbAvailParams, 5) < 0)
            return -1;
    }

    return 0;
}


/*
 * hypervAddSimpleParam:
 * @params: Params object to add to
 * @name: Name of the parameter
 * @value: Value of the parameter
 *
 * Add a param of type HYPERV_SIMPLE_PARAM, which is essentially a serialized
 * key/value pair.
 *
 * Returns -1 on failure, 0 on success.
 */
int
hypervAddSimpleParam(hypervInvokeParamsListPtr params, const char *name,
                     const char *value)
{
    hypervParamPtr p = NULL;

    if (hypervCheckParams(params) < 0)
        return -1;

    p = &params->params[params->nbParams];
    p->type = HYPERV_SIMPLE_PARAM;

    p->simple.name = name;
    p->simple.value = value;

    params->nbParams++;

    return 0;
}


/*
 * hypervAddEprParam:
 * @params: Params object to add to
 * @name: Parameter name
 * @priv: hypervPrivate object associated with the connection
 * @query: WQL filter
 * @eprInfo: WmiInfo of the object being filtered
 *
 * Adds an EPR param to the params list. Returns -1 on failure, 0 on success.
 */
int
hypervAddEprParam(hypervInvokeParamsListPtr params,
                  const char *name,
                  virBufferPtr query,
                  hypervWmiClassInfoPtr classInfo)
{
    hypervParamPtr p = NULL;

    if (hypervCheckParams(params) < 0)
        return -1;

    p = &params->params[params->nbParams];
    p->type = HYPERV_EPR_PARAM;
    p->epr.name = name;
    p->epr.query = query;
    p->epr.info = classInfo;
    params->nbParams++;

    return 0;
}


/*
 * hypervCreateEmbeddedParam:
 * @info: WmiInfo of the object type to serialize
 *
 * Instantiates a GHashTable pre-filled with all the properties pre-added
 * a key/value pairs set to NULL. The user then sets only those properties that
 * they wish to serialize, and passes the table via hypervAddEmbeddedParam.
 *
 * Returns a pointer to the GHashTable on success, otherwise NULL.
 */
GHashTable *
hypervCreateEmbeddedParam(hypervWmiClassInfoPtr classInfo)
{
    size_t i;
    size_t count;
    g_autoptr(GHashTable) table = NULL;
    XmlSerializerInfo *typeinfo = NULL;

    typeinfo = classInfo->serializerInfo;

    /* loop through the items to find out how many fields there are */
    for (count = 0; typeinfo[count].name != NULL; count++)
        ;

    table = virHashNew(NULL);
    if (table == NULL)
        return NULL;

    for (i = 0; typeinfo[i].name != NULL; i++) {
        XmlSerializerInfo *item = &typeinfo[i];

        if (virHashAddEntry(table, item->name, NULL) < 0)
            return NULL;
    }

    return g_steal_pointer(&table);
}


/**
 * hypervSetEmbeddedProperty:
 * @table: hash table allocated earlier by hypervCreateEmbeddedParam()
 * @name: name of the property
 * @value: value of the property
 *
 * For given table of properties, set property of @name to @value.
 * Please note, that the hash table does NOT become owner of the @value and
 * thus caller must ensure the pointer validity.
 *
 * Returns: 0 on success,
 *         -1 otherwise.
 */
int
hypervSetEmbeddedProperty(GHashTable *table,
                          const char *name,
                          const char *value)
{
    return virHashUpdateEntry(table, name, (void*) value);
}


/*
 * hypervAddEmbeddedParam:
 * @params: Params list to add to
 * @name: Name of the parameter
 * @table: pointer to table of properties to add
 * @info: WmiInfo of the object to serialize
 *
 * Add a GHashTable containing object properties as an embedded param to
 * an invocation list.
 *
 * Upon successfull return the @table is consumed and the pointer is cleared out.
 *
 * Returns -1 on failure, 0 on success.
 */
int
hypervAddEmbeddedParam(hypervInvokeParamsListPtr params,
                       const char *name,
                       GHashTable **table,
                       hypervWmiClassInfoPtr classInfo)
{
    hypervParamPtr p = NULL;

    if (hypervCheckParams(params) < 0)
        return -1;

    p = &params->params[params->nbParams];
    p->type = HYPERV_EMBEDDED_PARAM;
    p->embedded.name = name;
    p->embedded.table = g_steal_pointer(table);
    p->embedded.info = classInfo;
    params->nbParams++;

    return 0;
}


/*
 * hypervFreeEmbeddedParam:
 * @param: Pointer to embedded param to free
 *
 * Free the embedded param hash table.
 */
void
hypervFreeEmbeddedParam(GHashTable *p)
{
    virHashFree(p);
}


/*
 * Serializing parameters to XML and invoking methods
 */
static int
hypervGetCimTypeInfo(hypervCimTypePtr typemap, const char *name,
                     hypervCimTypePtr *property)
{
    size_t i = 0;
    while (typemap[i].name[0] != '\0') {
        if (STREQ(typemap[i].name, name)) {
            *property = &typemap[i];
            return 0;
        }
        i++;
    }

    return -1;
}


static int
hypervCreateInvokeXmlDoc(hypervInvokeParamsListPtr params, WsXmlDocH *docRoot)
{
    int result = -1;
    char *method = NULL;
    WsXmlNodeH xmlNodeMethod = NULL;

    method = g_strdup_printf("%s_INPUT", params->method);

    *docRoot = ws_xml_create_doc(NULL, method);
    if (*docRoot == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not instantiate XML document"));
        goto cleanup;
    }

    xmlNodeMethod = xml_parser_get_root(*docRoot);
    if (xmlNodeMethod == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not get root node of XML document"));
        goto cleanup;
    }

    /* add resource URI as namespace */
    ws_xml_set_ns(xmlNodeMethod, params->resourceUri, "p");

    result = 0;

 cleanup:
    if (result < 0 && *docRoot != NULL) {
        ws_xml_destroy_doc(*docRoot);
        *docRoot = NULL;
    }
    VIR_FREE(method);
    return result;
}


static int
hypervSerializeSimpleParam(hypervParamPtr p, const char *resourceUri,
                           WsXmlNodeH *methodNode)
{
    WsXmlNodeH xmlNodeParam = NULL;

    xmlNodeParam = ws_xml_add_child(*methodNode, resourceUri,
                                    p->simple.name, p->simple.value);
    if (xmlNodeParam == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create simple param"));
        return -1;
    }

    return 0;
}


static int
hypervSerializeEprParam(hypervParamPtr p, hypervPrivate *priv,
                        const char *resourceUri, WsXmlNodeH *methodNode)
{
    int result = -1;
    WsXmlNodeH xmlNodeParam = NULL,
               xmlNodeTemp = NULL,
               xmlNodeAddr = NULL,
               xmlNodeRef = NULL;
    WsXmlDocH xmlDocResponse = NULL;
    WsXmlNsH ns = NULL;
    client_opt_t *options = NULL;
    filter_t *filter = NULL;
    char *enumContext = NULL;
    char *query_string = NULL;

    /* init and set up options */
    options = wsmc_options_init();
    if (!options) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not init options"));
        goto cleanup;
    }
    wsmc_set_action_option(options, FLAG_ENUMERATION_ENUM_EPR);

    query_string = virBufferContentAndReset(p->epr.query);

    filter = filter_create_simple(WSM_WQL_FILTER_DIALECT, query_string);
    if (!filter) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not create WQL filter"));
        goto cleanup;
    }

    /* enumerate based on the filter from this query */
    xmlDocResponse = wsmc_action_enumerate(priv->client, p->epr.info->rootUri,
                                           options, filter);
    if (hypervVerifyResponse(priv->client, xmlDocResponse, "enumeration") < 0)
        goto cleanup;

    /* Get context */
    enumContext = wsmc_get_enum_context(xmlDocResponse);
    ws_xml_destroy_doc(xmlDocResponse);

    /* Pull using filter and enum context */
    xmlDocResponse = wsmc_action_pull(priv->client, resourceUri, options,
                                      filter, enumContext);

    if (hypervVerifyResponse(priv->client, xmlDocResponse, "pull") < 0)
        goto cleanup;

    /* drill down and extract EPR node children */
    if (!(xmlNodeTemp = ws_xml_get_soap_body(xmlDocResponse))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get SOAP body"));
        goto cleanup;
    }

    if (!(xmlNodeTemp = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ENUMERATION,
                                         WSENUM_PULL_RESP))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get response"));
        goto cleanup;
    }

    if (!(xmlNodeTemp = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ENUMERATION, WSENUM_ITEMS))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get response items"));
        goto cleanup;
    }

    if (!(xmlNodeTemp = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ADDRESSING, WSA_EPR))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get EPR items"));
        goto cleanup;
    }

    if (!(xmlNodeAddr = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ADDRESSING,
                                         WSA_ADDRESS))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not get EPR address"));
        goto cleanup;
    }

    if (!(xmlNodeRef = ws_xml_get_child(xmlNodeTemp, 0, XML_NS_ADDRESSING,
                                        WSA_REFERENCE_PARAMETERS))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not lookup EPR item reference parameters"));
        goto cleanup;
    }

    /* now build a new xml doc with the EPR node children */
    if (!(xmlNodeParam = ws_xml_add_child(*methodNode, resourceUri,
                                          p->epr.name, NULL))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add child node to methodNode"));
        goto cleanup;
    }

    if (!(ns = ws_xml_ns_add(xmlNodeParam,
                             "http://schemas.xmlsoap.org/ws/2004/08/addressing", "a"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not set namespace address for xmlNodeParam"));
        goto cleanup;
    }

    if (!(ns = ws_xml_ns_add(xmlNodeParam,
                             "http://schemas.dmtf.org/wbem/wsman/1/wsman.xsd", "w"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not set wsman namespace address for xmlNodeParam"));
        goto cleanup;
    }

    ws_xml_duplicate_tree(xmlNodeParam, xmlNodeAddr);
    ws_xml_duplicate_tree(xmlNodeParam, xmlNodeRef);

    /* we did it! */
    result = 0;

 cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);
    if (filter != NULL)
        filter_destroy(filter);
    ws_xml_destroy_doc(xmlDocResponse);
    VIR_FREE(enumContext);
    VIR_FREE(query_string);
    return result;
}


static int
hypervSerializeEmbeddedParam(hypervParamPtr p, const char *resourceUri,
                             WsXmlNodeH *methodNode)
{
    int result = -1;
    WsXmlNodeH xmlNodeInstance = NULL,
               xmlNodeProperty = NULL,
               xmlNodeParam = NULL,
               xmlNodeArray = NULL;
    WsXmlDocH xmlDocTemp = NULL,
              xmlDocCdata = NULL;
    char *cdataContent = NULL;
    xmlNodePtr xmlNodeCdata = NULL;
    hypervWmiClassInfoPtr classInfo = p->embedded.info;
    virHashKeyValuePairPtr items = NULL;
    hypervCimTypePtr property = NULL;
    ssize_t numKeys = -1;
    int len = 0, i = 0;

    if (!(xmlNodeParam = ws_xml_add_child(*methodNode, resourceUri, p->embedded.name,
                                          NULL))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Could not add child node %s"),
                       p->embedded.name);
        goto cleanup;
    }

    /* create the temp xml doc */

    /* start with the INSTANCE node */
    if (!(xmlDocTemp = ws_xml_create_doc(NULL, "INSTANCE"))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create temporary xml doc"));
        goto cleanup;
    }

    if (!(xmlNodeInstance = xml_parser_get_root(xmlDocTemp))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not get temp xml doc root"));
        goto cleanup;
    }

    /* add CLASSNAME node to INSTANCE node */
    if (!(ws_xml_add_node_attr(xmlNodeInstance, NULL, "CLASSNAME",
                               classInfo->name))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add attribute to node"));
        goto cleanup;
    }

    /* retrieve parameters out of hash table */
    numKeys = virHashSize(p->embedded.table);
    items = virHashGetItems(p->embedded.table, NULL, false);
    if (!items) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not read embedded param hash table"));
        goto cleanup;
    }

    /* Add the parameters */
    for (i = 0; i < numKeys; i++) {
        const char *name = items[i].key;
        const char *value = items[i].value;

        if (value != NULL) {
            if (hypervGetCimTypeInfo(classInfo->propertyInfo, name,
                                     &property) < 0) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Could not read type information"));
                goto cleanup;
            }

            if (!(xmlNodeProperty = ws_xml_add_child(xmlNodeInstance, NULL,
                                                     property->isArray ? "PROPERTY.ARRAY" : "PROPERTY",
                                                     NULL))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Could not add child to XML node"));
                goto cleanup;
            }

            if (!(ws_xml_add_node_attr(xmlNodeProperty, NULL, "NAME", name))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Could not add attribute to XML node"));
                goto cleanup;
            }

            if (!(ws_xml_add_node_attr(xmlNodeProperty, NULL, "TYPE", property->type))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Could not add attribute to XML node"));
                goto cleanup;
            }

            /* If this attribute is an array, add VALUE.ARRAY node */
            if (property->isArray) {
                if (!(xmlNodeArray = ws_xml_add_child(xmlNodeProperty, NULL,
                                                      "VALUE.ARRAY", NULL))) {
                    virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                                   _("Could not add child to XML node"));
                    goto cleanup;
                }
            }

            /* add the child */
            if (!(ws_xml_add_child(property->isArray ? xmlNodeArray : xmlNodeProperty,
                                   NULL, "VALUE", value))) {
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Could not add child to XML node"));
                goto cleanup;
            }

            xmlNodeArray = NULL;
            xmlNodeProperty = NULL;
        }
    }

    /* create CDATA node */
    ws_xml_dump_memory_node_tree(xmlNodeInstance, &cdataContent, &len);

    if (!(xmlNodeCdata = xmlNewCDataBlock((xmlDocPtr) xmlDocCdata,
                                          (xmlChar *)cdataContent, len))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create CDATA element"));
        goto cleanup;
    }

    /*
     * Add CDATA node to the doc root
     *
     * FIXME: there is no openwsman wrapper for xmlNewCDataBlock, so instead
     * silence clang alignment warnings by casting to a void pointer first
     */
    if (!(xmlAddChild((xmlNodePtr)(void *)xmlNodeParam, xmlNodeCdata))) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not add CDATA to doc root"));
        goto cleanup;
    }

    /* we did it! */
    result = 0;

 cleanup:
    VIR_FREE(items);
    ws_xml_destroy_doc(xmlDocCdata);
    ws_xml_destroy_doc(xmlDocTemp);
    ws_xml_free_memory(cdataContent);
    return result;
}


/*
 * hypervInvokeMethod:
 * @priv: hypervPrivate object associated with the connection
 * @paramsPtr: pointer to object containing the all necessary information for
 *             method invocation (consumed on invocation)
 * @res: Optional out parameter to contain the response XML.
 *
 * Performs an invocation described by object at @paramsPtr, and optionally
 * returns the XML containing the result.
 *
 * Please note that, object at @paramsPtr is consumed by this function and the
 * pointer is cleared out, regardless of returning success or failure.
 *
 * Returns -1 on failure, 0 on success.
 */
int
hypervInvokeMethod(hypervPrivate *priv,
                   hypervInvokeParamsListPtr *paramsPtr,
                   WsXmlDocH *res)
{
    g_autoptr(hypervInvokeParamsList) params = *paramsPtr;
    int result = -1;
    size_t i = 0;
    int returnCode;
    WsXmlDocH paramsDocRoot = NULL;
    client_opt_t *options = NULL;
    WsXmlDocH response = NULL;
    WsXmlNodeH methodNode = NULL;
    char *returnValue_xpath = NULL;
    char *jobcode_instance_xpath = NULL;
    char *returnValue = NULL;
    char *instanceID = NULL;
    bool completed = false;
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;
    Msvm_ConcreteJob *job = NULL;
    int jobState = -1;
    hypervParamPtr p = NULL;
    int timeout = HYPERV_JOB_TIMEOUT_MS;

    if (hypervCreateInvokeXmlDoc(params, &paramsDocRoot) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create XML document"));
        goto cleanup;
    }

    methodNode = xml_parser_get_root(paramsDocRoot);
    if (!methodNode) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not get root of XML document"));
        goto cleanup;
    }

    /* Serialize parameters */
    for (i = 0; i < params->nbParams; i++) {
        p = &(params->params[i]);

        switch (p->type) {
        case HYPERV_SIMPLE_PARAM:
            if (hypervSerializeSimpleParam(p, params->resourceUri,
                                           &methodNode) < 0)
                goto cleanup;
            break;
        case HYPERV_EPR_PARAM:
            if (hypervSerializeEprParam(p, priv, params->resourceUri,
                                        &methodNode) < 0)
                goto cleanup;
            break;
        case HYPERV_EMBEDDED_PARAM:
            if (hypervSerializeEmbeddedParam(p, params->resourceUri,
                                             &methodNode) < 0)
                goto cleanup;
            break;
        default:
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Unknown parameter type"));
            goto cleanup;
        }
    }

    /* Invoke the method and get the response */

    options = wsmc_options_init();
    if (!options) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Could not init options"));
        goto cleanup;
    }
    wsmc_add_selectors_from_str(options, params->selector);

    /* do the invoke */
    response = wsmc_action_invoke(priv->client, params->resourceUri, options,
                                  params->method, paramsDocRoot);

    /* check return code of invocation */
    returnValue_xpath = g_strdup_printf("/s:Envelope/s:Body/p:%s_OUTPUT/p:ReturnValue",
                                        params->method);

    returnValue = ws_xml_get_xpath_value(response, returnValue_xpath);
    if (!returnValue) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not get return value for %s invocation"),
                       params->method);
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0)
        goto cleanup;

    if (returnCode == CIM_RETURNCODE_TRANSITION_STARTED) {
        jobcode_instance_xpath = g_strdup_printf("/s:Envelope/s:Body/p:%s_OUTPUT/p:Job/a:ReferenceParameters/"
                                                 "w:SelectorSet/w:Selector[@Name='InstanceID']",
                                                 params->method);

        instanceID = ws_xml_get_xpath_value(response, jobcode_instance_xpath);
        if (!instanceID) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not get instance ID for %s invocation"),
                           params->method);
            goto cleanup;
        }

        /*
         * Poll Hyper-V about the job until either the job completes or fails,
         * or 5 minutes have elapsed.
         *
         * Windows has its own timeout on running WMI method calls (it calls
         * these "jobs"), by default set to 1 minute. The administrator can
         * change this to whatever they want, however, so we can't rely on it.
         *
         * Therefore, to avoid waiting in this loop for a very long-running job
         * to complete, we instead bail after 5 minutes no matter what. NOTE that
         * this does not mean that the remote job has terminated on the Windows
         * side! That is up to Windows to control, we don't do anything about it.
         */
        while (!completed && timeout >= 0) {
            virBufferEscapeSQL(&query,
                               MSVM_CONCRETEJOB_WQL_SELECT
                               "WHERE InstanceID = '%s'", instanceID);

            if (hypervGetWmiClass(Msvm_ConcreteJob, &job) < 0 || !job)
                goto cleanup;

            jobState = job->data->JobState;
            switch (jobState) {
            case MSVM_CONCRETEJOB_JOBSTATE_NEW:
            case MSVM_CONCRETEJOB_JOBSTATE_STARTING:
            case MSVM_CONCRETEJOB_JOBSTATE_RUNNING:
            case MSVM_CONCRETEJOB_JOBSTATE_SHUTTING_DOWN:
                hypervFreeObject(priv, (hypervObject *)job);
                job = NULL;
                g_usleep(100 * 1000); /* sleep 100 ms */
                timeout -= 100;
                continue;
            case MSVM_CONCRETEJOB_JOBSTATE_COMPLETED:
                completed = true;
                break;
            case MSVM_CONCRETEJOB_JOBSTATE_TERMINATED:
            case MSVM_CONCRETEJOB_JOBSTATE_KILLED:
            case MSVM_CONCRETEJOB_JOBSTATE_EXCEPTION:
            case MSVM_CONCRETEJOB_JOBSTATE_SERVICE:
                goto cleanup;
            default:
                virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                               _("Unknown invocation state"));
                goto cleanup;
            }
        }
        if (!completed && timeout < 0) {
            virReportError(VIR_ERR_OPERATION_TIMEOUT,
                           _("Timeout waiting for %s invocation"), params->method);
            goto cleanup;
        }
    } else if (returnCode != CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR) {
        virReportError(VIR_ERR_INTERNAL_ERROR, _("Invocation of %s returned an error: %s (%d)"),
                       params->method, hypervReturnCodeToString(returnCode),
                       returnCode);
        goto cleanup;
    }

    if (res)
        *res = response;

    result = 0;

 cleanup:
    if (options)
        wsmc_options_destroy(options);
    if (response && (!res))
        ws_xml_destroy_doc(response);
    if (paramsDocRoot)
        ws_xml_destroy_doc(paramsDocRoot);
    VIR_FREE(returnValue_xpath);
    VIR_FREE(jobcode_instance_xpath);
    VIR_FREE(returnValue);
    VIR_FREE(instanceID);
    hypervFreeObject(priv, (hypervObject *)job);
    *paramsPtr = NULL;
    return result;
}

/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Object
 */

/* This function guarantees that wqlQuery->query is reset, even on failure */
int
hypervEnumAndPull(hypervPrivate *priv, hypervWqlQueryPtr wqlQuery,
                  hypervObject **list)
{
    int result = -1;
    WsSerializerContextH serializerContext;
    client_opt_t *options = NULL;
    char *query_string = NULL;
    hypervWmiClassInfoPtr wmiInfo = wqlQuery->info;
    filter_t *filter = NULL;
    WsXmlDocH response = NULL;
    char *enumContext = NULL;
    hypervObject *head = NULL;
    hypervObject *tail = NULL;
    WsXmlNodeH node = NULL;
    hypervObject *object;

    query_string = virBufferContentAndReset(wqlQuery->query);

    if (list == NULL || *list != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        VIR_FREE(query_string);
        return -1;
    }

    serializerContext = wsmc_get_serialization_context(priv->client);

    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    filter = filter_create_simple(WSM_WQL_FILTER_DIALECT, query_string);

    if (filter == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not create filter"));
        goto cleanup;
    }

    response = wsmc_action_enumerate(priv->client, wmiInfo->rootUri, options,
                                     filter);

    if (hypervVerifyResponse(priv->client, response, "enumeration") < 0)
        goto cleanup;

    enumContext = wsmc_get_enum_context(response);

    ws_xml_destroy_doc(response);
    response = NULL;

    while (enumContext != NULL && *enumContext != '\0') {
        XML_TYPE_PTR data = NULL;

        response = wsmc_action_pull(priv->client, wmiInfo->resourceUri, options,
                                    filter, enumContext);

        if (hypervVerifyResponse(priv->client, response, "pull") < 0)
            goto cleanup;

        node = ws_xml_get_soap_body(response);

        if (node == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not lookup SOAP body"));
            goto cleanup;
        }

        node = ws_xml_get_child(node, 0, XML_NS_ENUMERATION, WSENUM_PULL_RESP);

        if (node == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not lookup pull response"));
            goto cleanup;
        }

        node = ws_xml_get_child(node, 0, XML_NS_ENUMERATION, WSENUM_ITEMS);

        if (node == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not lookup pull response items"));
            goto cleanup;
        }

        if (ws_xml_get_child(node, 0, wmiInfo->resourceUri,
                             wmiInfo->name) == NULL)
            break;

        data = ws_deserialize(serializerContext, node, wmiInfo->serializerInfo,
                              wmiInfo->name, wmiInfo->resourceUri, NULL, 0, 0);

        if (data == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                           _("Could not deserialize pull response item"));
            goto cleanup;
        }

        object = g_new0(hypervObject, 1);
        object->info = wmiInfo;
        object->data = data;

        if (head == NULL) {
            head = object;
        } else {
            tail->next = object;
        }

        tail = object;

        VIR_FREE(enumContext);
        enumContext = wsmc_get_enum_context(response);

        ws_xml_destroy_doc(response);
        response = NULL;
    }

    *list = head;
    head = NULL;

    result = 0;

 cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);

    if (filter != NULL)
        filter_destroy(filter);

    VIR_FREE(query_string);
    ws_xml_destroy_doc(response);
    VIR_FREE(enumContext);
    hypervFreeObject(priv, head);

    return result;
}


void
hypervFreeObject(hypervPrivate *priv G_GNUC_UNUSED, hypervObject *object)
{
    hypervObject *next;
    WsSerializerContextH serializerContext;

    if (object == NULL)
        return;

    serializerContext = wsmc_get_serialization_context(priv->client);

    while (object != NULL) {
        next = object->next;

        if (ws_serializer_free_mem(serializerContext, object->data,
                                   object->info->serializerInfo) < 0) {
            VIR_ERROR(_("Could not free deserialized data"));
        }

        VIR_FREE(object);

        object = next;
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * CIM/Msvm_ReturnCode
 */

const char *
hypervReturnCodeToString(int returnCode)
{
    switch (returnCode) {
    case CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR:
        return _("Completed with no error");

    case CIM_RETURNCODE_NOT_SUPPORTED:
        return _("Not supported");

    case CIM_RETURNCODE_UNKNOWN_ERROR:
        return _("Unknown error");

    case CIM_RETURNCODE_CANNOT_COMPLETE_WITHIN_TIMEOUT_PERIOD:
        return _("Cannot complete within timeout period");

    case CIM_RETURNCODE_FAILED:
        return _("Failed");

    case CIM_RETURNCODE_INVALID_PARAMETER:
        return _("Invalid parameter");

    case CIM_RETURNCODE_IN_USE:
        return _("In use");

    case CIM_RETURNCODE_TRANSITION_STARTED:
        return _("Transition started");

    case CIM_RETURNCODE_INVALID_STATE_TRANSITION:
        return _("Invalid state transition");

    case CIM_RETURNCODE_TIMEOUT_PARAMETER_NOT_SUPPORTED:
        return _("Timeout parameter not supported");

    case CIM_RETURNCODE_BUSY:
        return _("Busy");

    case MSVM_RETURNCODE_FAILED:
        return _("Failed");

    case MSVM_RETURNCODE_ACCESS_DENIED:
        return _("Access denied");

    case MSVM_RETURNCODE_NOT_SUPPORTED:
        return _("Not supported");

    case MSVM_RETURNCODE_STATUS_IS_UNKNOWN:
        return _("Status is unknown");

    case MSVM_RETURNCODE_TIMEOUT:
        return _("Timeout");

    case MSVM_RETURNCODE_INVALID_PARAMETER:
        return _("Invalid parameter");

    case MSVM_RETURNCODE_SYSTEM_IS_IN_USE:
        return _("System is in use");

    case MSVM_RETURNCODE_INVALID_STATE_FOR_THIS_OPERATION:
        return _("Invalid state for this operation");

    case MSVM_RETURNCODE_INCORRECT_DATA_TYPE:
        return _("Incorrect data type");

    case MSVM_RETURNCODE_SYSTEM_IS_NOT_AVAILABLE:
        return _("System is not available");

    case MSVM_RETURNCODE_OUT_OF_MEMORY:
        return _("Out of memory");

    default:
        return _("Unknown return code");
    }
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_ComputerSystem
 */

int
hypervInvokeMsvmComputerSystemRequestStateChange(virDomainPtr domain,
                                                 int requestedState)
{
    int result = -1;
    hypervPrivate *priv = domain->conn->privateData;
    char uuid_string[VIR_UUID_STRING_BUFLEN];
    WsXmlDocH response = NULL;
    client_opt_t *options = NULL;
    char *selector = NULL;
    char *properties = NULL;
    char *returnValue = NULL;
    int returnCode;
    char *instanceID = NULL;
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;
    Msvm_ConcreteJob *concreteJob = NULL;
    bool completed = false;

    virUUIDFormat(domain->uuid, uuid_string);

    selector = g_strdup_printf("Name=%s&CreationClassName=Msvm_ComputerSystem", uuid_string);
    properties = g_strdup_printf("RequestedState=%d", requestedState);

    options = wsmc_options_init();

    if (options == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s",
                       _("Could not initialize options"));
        goto cleanup;
    }

    wsmc_add_selectors_from_str(options, selector);
    wsmc_add_prop_from_str(options, properties);

    /* Invoke method */
    response = wsmc_action_invoke(priv->client, MSVM_COMPUTERSYSTEM_RESOURCE_URI,
                                  options, "RequestStateChange", NULL);

    if (hypervVerifyResponse(priv->client, response, "invocation") < 0)
        goto cleanup;

    /* Check return value */
    returnValue = ws_xml_get_xpath_value(response, (char *)"/s:Envelope/s:Body/p:RequestStateChange_OUTPUT/p:ReturnValue");

    if (returnValue == NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not lookup %s for %s invocation"),
                       "ReturnValue", "RequestStateChange");
        goto cleanup;
    }

    if (virStrToLong_i(returnValue, NULL, 10, &returnCode) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse return code from '%s'"), returnValue);
        goto cleanup;
    }

    if (returnCode == CIM_RETURNCODE_TRANSITION_STARTED) {
        /* Get concrete job object */
        instanceID = ws_xml_get_xpath_value(response, (char *)"/s:Envelope/s:Body/p:RequestStateChange_OUTPUT/p:Job/a:ReferenceParameters/w:SelectorSet/w:Selector[@Name='InstanceID']");

        if (instanceID == NULL) {
            virReportError(VIR_ERR_INTERNAL_ERROR,
                           _("Could not lookup %s for %s invocation"),
                           "InstanceID", "RequestStateChange");
            goto cleanup;
        }

        /* FIXME: Poll every 100ms until the job completes or fails. There
         *        seems to be no other way than polling. */
        while (!completed) {
            virBufferAsprintf(&query,
                              MSVM_CONCRETEJOB_WQL_SELECT
                              "WHERE InstanceID = '%s'", instanceID);

            if (hypervGetWmiClass(Msvm_ConcreteJob, &concreteJob) < 0)
                goto cleanup;

            if (concreteJob == NULL) {
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Could not lookup %s for %s invocation"),
                               "Msvm_ConcreteJob", "RequestStateChange");
                goto cleanup;
            }

            switch (concreteJob->data->JobState) {
            case MSVM_CONCRETEJOB_JOBSTATE_NEW:
            case MSVM_CONCRETEJOB_JOBSTATE_STARTING:
            case MSVM_CONCRETEJOB_JOBSTATE_RUNNING:
            case MSVM_CONCRETEJOB_JOBSTATE_SHUTTING_DOWN:
                hypervFreeObject(priv, (hypervObject *)concreteJob);
                concreteJob = NULL;

                g_usleep(100 * 1000);
                continue;

            case MSVM_CONCRETEJOB_JOBSTATE_COMPLETED:
                completed = true;
                break;

            case MSVM_CONCRETEJOB_JOBSTATE_TERMINATED:
            case MSVM_CONCRETEJOB_JOBSTATE_KILLED:
            case MSVM_CONCRETEJOB_JOBSTATE_EXCEPTION:
            case MSVM_CONCRETEJOB_JOBSTATE_SERVICE:
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Concrete job for %s invocation is in error state"),
                               "RequestStateChange");
                goto cleanup;

            default:
                virReportError(VIR_ERR_INTERNAL_ERROR,
                               _("Concrete job for %s invocation is in unknown state"),
                               "RequestStateChange");
                goto cleanup;
            }
        }
    } else if (returnCode != CIM_RETURNCODE_COMPLETED_WITH_NO_ERROR) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Invocation of %s returned an error: %s (%d)"),
                       "RequestStateChange", hypervReturnCodeToString(returnCode),
                       returnCode);
        goto cleanup;
    }

    result = 0;

 cleanup:
    if (options != NULL)
        wsmc_options_destroy(options);

    ws_xml_destroy_doc(response);
    VIR_FREE(selector);
    VIR_FREE(properties);
    VIR_FREE(returnValue);
    VIR_FREE(instanceID);
    hypervFreeObject(priv, (hypervObject *)concreteJob);

    return result;
}


int
hypervMsvmComputerSystemEnabledStateToDomainState
(Msvm_ComputerSystem *computerSystem)
{
    switch (computerSystem->data->EnabledState) {
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_UNKNOWN:
        return VIR_DOMAIN_NOSTATE;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_ENABLED:
        return VIR_DOMAIN_RUNNING;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_DISABLED:
        return VIR_DOMAIN_SHUTOFF;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSED:
        return VIR_DOMAIN_PAUSED;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SUSPENDED: /* managed save */
        return VIR_DOMAIN_SHUTOFF;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STARTING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SNAPSHOTTING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SAVING:
        return VIR_DOMAIN_RUNNING;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STOPPING:
        return VIR_DOMAIN_SHUTDOWN;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_RESUMING:
        return VIR_DOMAIN_RUNNING;

    default:
        return VIR_DOMAIN_NOSTATE;
    }
}


bool
hypervIsMsvmComputerSystemActive(Msvm_ComputerSystem *computerSystem,
                                 bool *in_transition)
{
    if (in_transition != NULL)
        *in_transition = false;

    switch (computerSystem->data->EnabledState) {
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_UNKNOWN:
        return false;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_ENABLED:
        return true;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_DISABLED:
        return false;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSED:
        return true;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SUSPENDED: /* managed save */
        return false;

    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STARTING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SNAPSHOTTING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_SAVING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_STOPPING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_PAUSING:
    case MSVM_COMPUTERSYSTEM_ENABLEDSTATE_RESUMING:
        if (in_transition != NULL)
            *in_transition = true;

        return true;

    default:
        return false;
    }
}


int
hypervMsvmComputerSystemToDomain(virConnectPtr conn,
                                 Msvm_ComputerSystem *computerSystem,
                                 virDomainPtr *domain)
{
    unsigned char uuid[VIR_UUID_BUFLEN];
    int id = -1;

    if (domain == NULL || *domain != NULL) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    if (virUUIDParse(computerSystem->data->Name, uuid) < 0) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not parse UUID from string '%s'"),
                       computerSystem->data->Name);
        return -1;
    }

    if (hypervIsMsvmComputerSystemActive(computerSystem, NULL))
        id = computerSystem->data->ProcessID;

    *domain = virGetDomain(conn, computerSystem->data->ElementName, uuid, id);

    return *domain ? 0 : -1;
}


int
hypervMsvmComputerSystemFromUUID(hypervPrivate *priv, const char *uuid,
                                 Msvm_ComputerSystem **computerSystem)
{
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;

    if (!computerSystem || *computerSystem) {
        virReportError(VIR_ERR_INTERNAL_ERROR, "%s", _("Invalid argument"));
        return -1;
    }

    virBufferEscapeSQL(&query,
                       MSVM_COMPUTERSYSTEM_WQL_SELECT
                       "WHERE " MSVM_COMPUTERSYSTEM_WQL_VIRTUAL
                       "AND Name = '%s'", uuid);

    if (hypervGetWmiClass(Msvm_ComputerSystem, computerSystem) < 0)
        return -1;

    if (!*computerSystem) {
        virReportError(VIR_ERR_NO_DOMAIN, _("No domain with UUID %s"), uuid);
        return -1;
    }

    return 0;
}


int
hypervMsvmComputerSystemFromDomain(virDomainPtr domain,
                                   Msvm_ComputerSystem **computerSystem)
{
    hypervPrivate *priv = domain->conn->privateData;
    char uuidString[VIR_UUID_STRING_BUFLEN];

    virUUIDFormat(domain->uuid, uuidString);

    return hypervMsvmComputerSystemFromUUID(priv, uuidString, computerSystem);
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Generic "Get WMI class list" helpers
 */

int
hypervGetMsvmVirtualSystemSettingDataFromUUID(hypervPrivate *priv,
                                              const char *uuid_string,
                                              Msvm_VirtualSystemSettingData **list)
{
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&query,
                      "ASSOCIATORS OF {Msvm_ComputerSystem.CreationClassName='Msvm_ComputerSystem',Name='%s'} "
                      "WHERE AssocClass = Msvm_SettingsDefineState "
                      "ResultClass = Msvm_VirtualSystemSettingData",
                      uuid_string);

    if (hypervGetWmiClass(Msvm_VirtualSystemSettingData, list) < 0 || !*list)
        return -1;

    return 0;
}


int
hypervGetResourceAllocationSD(hypervPrivate *priv,
                              const char *id,
                              Msvm_ResourceAllocationSettingData **data)
{
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;
    virBufferEscapeSQL(&query,
                       "ASSOCIATORS OF {Msvm_VirtualSystemSettingData.InstanceID='%s'} "
                       "WHERE AssocClass = Msvm_VirtualSystemSettingDataComponent "
                       "ResultClass = Msvm_ResourceAllocationSettingData",
                       id);

    if (hypervGetWmiClass(Msvm_ResourceAllocationSettingData, data) < 0)
        return -1;

    if (!*data) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not look up resource allocation setting data with virtual system instance ID '%s'"),
                       id);
        return -1;
    }

    return 0;
}


int
hypervGetProcessorSD(hypervPrivate *priv,
                     const char *id,
                     Msvm_ProcessorSettingData **data)
{
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;
    virBufferEscapeSQL(&query,
                       "ASSOCIATORS OF {Msvm_VirtualSystemSettingData.InstanceID='%s'} "
                       "WHERE AssocClass = Msvm_VirtualSystemSettingDataComponent "
                       "ResultClass = Msvm_ProcessorSettingData",
                       id);

    if (hypervGetWmiClass(Msvm_ProcessorSettingData, data) < 0)
        return -1;

    if (!*data) {
        virReportError(VIR_ERR_INTERNAL_ERROR,
                       _("Could not look up processor setting data with virtual system instance ID '%s'"),
                       id);
        return -1;
    }

    return 0;
}


int
hypervGetMemorySD(hypervPrivate *priv,
                  const char *vssd_instanceid,
                  Msvm_MemorySettingData **list)
{
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;

    virBufferAsprintf(&query,
                      "ASSOCIATORS OF {Msvm_VirtualSystemSettingData.InstanceID='%s'} "
                      "WHERE AssocClass = Msvm_VirtualSystemSettingDataComponent "
                      "ResultClass = Msvm_MemorySettingData",
                      vssd_instanceid);

    if (hypervGetWmiClass(Msvm_MemorySettingData, list) < 0 || !*list)
        return -1;

    return 0;
}


int
hypervGetStorageAllocationSD(hypervPrivate *priv,
                             const char *id,
                             Msvm_StorageAllocationSettingData **data)
{
    g_auto(virBuffer) query = VIR_BUFFER_INITIALIZER;
    virBufferEscapeSQL(&query,
                       "ASSOCIATORS OF {Msvm_VirtualSystemSettingData.InstanceID='%s'} "
                       "WHERE AssocClass = Msvm_VirtualSystemSettingDataComponent "
                       "ResultClass = Msvm_StorageAllocationSettingData",
                       id);

    if (hypervGetWmiClass(Msvm_StorageAllocationSettingData, data) < 0)
        return -1;

    return 0;
}


/* * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * * *
 * Msvm_VirtualSystemManagementService
 */

int
hypervMsvmVSMSModifyResourceSettings(hypervPrivate *priv,
                                     GHashTable **resourceSettingsPtr,
                                     hypervWmiClassInfoPtr wmiInfo)
{
    int result = -1;
    GHashTable *resourceSettings = *resourceSettingsPtr;
    g_autoptr(hypervInvokeParamsList) params = NULL;

    params = hypervCreateInvokeParamsList("ModifyResourceSettings",
                                          MSVM_VIRTUALSYSTEMMANAGEMENTSERVICE_SELECTOR,
                                          Msvm_VirtualSystemManagementService_WmiInfo);

    if (!params)
        goto cleanup;

    if (hypervAddEmbeddedParam(params, "ResourceSettings", &resourceSettings, wmiInfo) < 0) {
        hypervFreeEmbeddedParam(resourceSettings);
        goto cleanup;
    }

    if (hypervInvokeMethod(priv, &params, NULL) < 0)
        goto cleanup;

    result = 0;

 cleanup:
    *resourceSettingsPtr = NULL;

    return result;
}
