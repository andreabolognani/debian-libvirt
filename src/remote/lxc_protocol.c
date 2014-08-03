#include <config.h>
/*
 * Please do not edit this file.
 * It was generated using rpcgen.
 */

#include "lxc_protocol.h"
#include "internal.h"
#include "remote_protocol.h"

bool_t
xdr_lxc_domain_open_namespace_args (XDR *xdrs, lxc_domain_open_namespace_args *objp)
{

         if (!xdr_remote_nonnull_domain (xdrs, &objp->dom))
                 return FALSE;
         if (!xdr_u_int (xdrs, &objp->flags))
                 return FALSE;
        return TRUE;
}

bool_t
xdr_lxc_procedure (XDR *xdrs, lxc_procedure *objp)
{

         if (!xdr_enum (xdrs, (enum_t *) objp))
                 return FALSE;
        return TRUE;
}