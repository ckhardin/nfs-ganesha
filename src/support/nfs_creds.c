/*
 * vim:noexpandtab:shiftwidth=8:tabstop=8:
 *
 * Copyright CEA/DAM/DIF  (2008)
 * contributeur : Philippe DENIEL   philippe.deniel@cea.fr
 *                Thomas LEIBOVICI  thomas.leibovici@cea.fr
 *
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public License
 * as published by the Free Software Foundation; either version 3 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301 USA
 *
 * ---------------------------------------
 */

/**
 * @file  nfs_export_list.c
 * @brief Routines for managing the export list.
 */

#include "config.h"
#include <stdio.h>
#include <sys/types.h>
#include <ctype.h>		/* for having isalnum */
#include <stdlib.h>		/* for having atoi */
#include <dirent.h>		/* for having MAXNAMLEN */
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <string.h>
#include <pthread.h>
#include <fcntl.h>
#include <sys/file.h>		/* for having FNDELAY */
#include <pwd.h>
#include <grp.h>
#include "log.h"
#include "ganesha_rpc.h"
#include "nfs_core.h"
#include "nfs23.h"
#include "nfs4.h"
#include "fsal.h"
#include "nfs_exports.h"
#include "nfs_creds.h"
#include "nfs_file_handle.h"
#include "idmapper.h"
#include "export_mgr.h"
#include "uid2grp.h"
#include "client_mgr.h"

void squash_setattr(export_perms_t *export_perms,
		    struct req_op_context *req_ctx,
		    struct attrlist *attr)
{
	if (attr->mask & ATTR_OWNER) {
		if (export_perms->options & EXPORT_OPTION_ALL_ANONYMOUS)
			attr->owner = export_perms->anonymous_uid;
		else if (!(export_perms->options & EXPORT_OPTION_ROOT)
			 && (attr->owner == 0)
			 && ((req_ctx->cred_flags & UID_SQUASHED) != 0))
			attr->owner = export_perms->anonymous_uid;
	}

	if (attr->mask & ATTR_GROUP) {
		/* If all squashed, then always squash the owner_group.
		 *
		 * If root squashed, then squash owner_group if
		 * caller_gid has been squashed or one of the caller's
		 * alternate groups has been squashed.
		 */
		if (export_perms->options & EXPORT_OPTION_ALL_ANONYMOUS)
			attr->group = export_perms->anonymous_gid;
		else if (!(export_perms->options & EXPORT_OPTION_ROOT)
			 && (attr->group == 0)
			 && ((req_ctx->cred_flags & (GID_SQUASHED |
						     GARRAY_SQUASHED)) != 0))
			attr->group = export_perms->anonymous_gid;
	}
}

/**
 * @brief Compares two RPC creds
 *
 * @param[in] cred1 First RPC cred
 * @param[in] cred2 Second RPC cred
 *
 * @return true if same, false otherwise
 */
bool nfs_compare_clientcred(nfs_client_cred_t *cred1,
			    nfs_client_cred_t *cred2)
{
	if (cred1 == NULL)
		return false;
	if (cred2 == NULL)
		return false;

	if (cred1->flavor != cred2->flavor)
		return false;

	switch (cred1->flavor) {
	case AUTH_UNIX:
		if (cred1->auth_union.auth_unix.aup_uid !=
		    cred2->auth_union.auth_unix.aup_uid)
			return false;
		if (cred1->auth_union.auth_unix.aup_gid !=
		    cred2->auth_union.auth_unix.aup_gid)
			return false;
		break;

	default:
		if (memcmp
		    (&cred1->auth_union, &cred2->auth_union, cred1->length))
			return false;
		break;
	}

	/* If this point is reached, structures are the same */
	return true;
}

int nfs_rpc_req2client_cred(struct svc_req *req, nfs_client_cred_t *pcred)
{
	/* Structure for managing basic AUTH_UNIX authentication */
	struct authunix_parms *aup = NULL;

	/* Stuff needed for managing RPCSEC_GSS */
#ifdef _HAVE_GSSAPI
	struct svc_rpc_gss_data *gd = NULL;
#endif

	pcred->flavor = req->rq_cred.oa_flavor;
	pcred->length = req->rq_cred.oa_length;

	switch (req->rq_cred.oa_flavor) {
	case AUTH_NONE:
		/* Do nothing... */
		break;

	case AUTH_UNIX:
		aup = (struct authunix_parms *)(req->rq_clntcred);

		pcred->auth_union.auth_unix.aup_uid = aup->aup_uid;
		pcred->auth_union.auth_unix.aup_gid = aup->aup_gid;
		pcred->auth_union.auth_unix.aup_time = aup->aup_time;

		break;

#ifdef _HAVE_GSSAPI
	case RPCSEC_GSS:
		/* Extract the information from the RPCSEC_GSS
		 * opaque structure
		 */
		gd = SVCAUTH_PRIVATE(req->rq_auth);

		pcred->auth_union.auth_gss.svc = (unsigned int)(gd->sec.svc);
		pcred->auth_union.auth_gss.qop = (unsigned int)(gd->sec.qop);
		pcred->auth_union.auth_gss.gss_context_id = gd->ctx;
		break;
#endif

	default:
		/* Unsupported authentication flavour */
		return -1;
		break;
	}

	return 1;
}

/**
 * @brief Get numeric credentials from request
 *
 * @todo This MUST be refactored to not use TI-RPC private structures.
 * Instead, export appropriate functions from lib(n)tirpc.
 *
 * @param[in]  req              Incoming request.
 * @param[out] user_credentials Filled in structure with UID and GIDs
 *
 * @return true if successful, false otherwise
 *
 */
bool get_req_creds(struct svc_req *req,
		   struct req_op_context *req_ctx,
		   export_perms_t *export_perms)
{
	unsigned int i;
	const char *auth_label = "UNKNOWN";
	gid_t **garray_copy = &req_ctx->caller_garray_copy;
#ifdef _HAVE_GSSAPI
	struct svc_rpc_gss_data *gd = NULL;
	char principal[MAXNAMLEN + 1];
#endif

	/* Make sure we clear out all the cred_flags except CREDS_LOADED and
	 * CREDS_ANON.
	 */
	req_ctx->cred_flags &= CREDS_LOADED | CREDS_ANON;

	switch (req->rq_cred.oa_flavor) {
	case AUTH_NONE:
		/* Nothing to be done here... */
		req_ctx->cred_flags |= CREDS_LOADED | CREDS_ANON;
		auth_label = "AUTH_NONE";
		break;

	case AUTH_SYS:
		if ((req_ctx->cred_flags & CREDS_LOADED) == 0) {
			struct authunix_parms *creds = NULL;

			/* We map the rq_cred to Authunix_parms */
			creds = (struct authunix_parms *) req->rq_clntcred;
			req_ctx->original_creds.caller_uid = creds->aup_uid;
			req_ctx->original_creds.caller_gid = creds->aup_gid;
			req_ctx->original_creds.caller_glen = creds->aup_len;
			req_ctx->original_creds.caller_garray = creds->aup_gids;
			req_ctx->cred_flags |= CREDS_LOADED;
		}

		/* Copy original_creds creds */
		*req_ctx->creds = req_ctx->original_creds;

		/* Do we trust AUTH_SYS creds for groups or not ? */
		if ((export_perms->options & EXPORT_OPTION_MANAGE_GIDS)
		    != 0) {
			req_ctx->cred_flags |= MANAGED_GIDS;
			garray_copy = &req_ctx->managed_garray_copy;
		}

		auth_label = "AUTH_SYS";
		break;

#ifdef _HAVE_GSSAPI
	case RPCSEC_GSS:
		if ((req_ctx->cred_flags & CREDS_LOADED) == 0) {
			/* Get the gss data to process them */
			gd = SVCAUTH_PRIVATE(req->rq_auth);

			memcpy(principal, gd->cname.value, gd->cname.length);
			principal[gd->cname.length] = 0;

			LogMidDebug(COMPONENT_DISPATCH,
				     "Mapping RPCSEC_GSS principal %s to uid/gid",
				     principal);

			/* Convert to uid */
#if _MSPAC_SUPPORT
			if (!principal2uid(principal,
					   &req_ctx->original_creds.caller_uid,
					   &req_ctx->original_creds.caller_gid,
					   gd)) {
#else
			if (!principal2uid(principal,
					   &req_ctx->original_creds.caller_uid,
					   &req_ctx->original_creds.caller_gid)) {
#endif
				LogWarn(COMPONENT_IDMAPPER,
					"Could not map principal %s to uid",
					principal);
				/* For compatibility with Linux knfsd, we set
				 * the uid/gid to anonymous when a name->uid
				 * mapping can't be found.
				 */
				req_ctx->cred_flags |= CREDS_ANON |
						       CREDS_LOADED;
				auth_label = "RPCSEC_GSS (no mapping)";
				break;
			}

			req_ctx->cred_flags |= CREDS_LOADED;
		}

		auth_label = "RPCSEC_GSS";
		req_ctx->cred_flags |= MANAGED_GIDS;
		garray_copy = &req_ctx->managed_garray_copy;

		break;
#endif				/* _USE_GSSRPC */

	default:
		LogMidDebug(COMPONENT_DISPATCH,
			     "FAILURE: Request xid=%u, has unsupported authentication %d",
			     req->rq_xid, req->rq_cred.oa_flavor);
		/* Reject the request for weak authentication and
		 * return to worker
		 */
		return false;

		break;
	}

	/****************************************************************/
	/* Mow check for anon creds or id squashing			*/
	/****************************************************************/
	if ((req_ctx->cred_flags & CREDS_ANON) != 0 ||
	    ((export_perms->options & EXPORT_OPTION_ALL_ANONYMOUS) != 0) ||
	    ((export_perms->options & EXPORT_OPTION_ROOT) == 0 &&
	      req_ctx->original_creds.caller_uid == 0)) {
		req_ctx->creds->caller_uid = export_perms->anonymous_uid;
		req_ctx->creds->caller_gid = export_perms->anonymous_gid;
		req_ctx->creds->caller_glen = 0;
		LogMidDebugAlt(COMPONENT_DISPATCH, COMPONENT_EXPORT,
			    "%s creds squashed to uid=%u, gid=%u",
			    auth_label,
			    req_ctx->creds->caller_uid,
			    req_ctx->creds->caller_gid);
		req_ctx->cred_flags |= UID_SQUASHED | GID_SQUASHED;
		return true;
	}

	/* Now we will use the original_creds uid from original credential */
	req_ctx->creds->caller_uid = req_ctx->original_creds.caller_uid;

	/****************************************************************/
	/* Now sqush group or use original_creds gid			*/
	/****************************************************************/
	if ((export_perms->options & EXPORT_OPTION_ROOT) == 0 &&
	    req_ctx->original_creds.caller_gid == 0) {
		/* Squash gid */
		req_ctx->creds->caller_gid = export_perms->anonymous_gid;
		req_ctx->cred_flags |= GID_SQUASHED;
	} else {
		/* Use original_creds gid */
		req_ctx->creds->caller_gid = req_ctx->original_creds.caller_gid;
	}

	/****************************************************************/
	/* Check if we have manage_gids.				*/
	/****************************************************************/
	if ((req_ctx->cred_flags & MANAGED_GIDS) != 0) {
		/* Fetch the group data if required */
		if (req_ctx->caller_gdata == NULL &&
		    !uid2grp(req_ctx->original_creds.caller_uid,
			     &req_ctx->caller_gdata)) {
			/** @todo: do we really want to bail here? */
			LogCrit(COMPONENT_DISPATCH,
				"Attempt to fetch managed_gids failed");
			return false;
		}

		req_ctx->creds->caller_glen = req_ctx->caller_gdata->nbgroups;
		req_ctx->creds->caller_garray = req_ctx->caller_gdata->groups;
	} else {
		/* Use the original_creds group list */
		req_ctx->creds->caller_glen   =
					req_ctx->original_creds.caller_glen;
		req_ctx->creds->caller_garray =
					req_ctx->original_creds.caller_garray;
	}

	/****************************************************************/
	/* Check the garray for gid 0 to squash				*/
	/****************************************************************/

	/* If no root squashing in caller_garray, return now */
	if ((export_perms->options & EXPORT_OPTION_ROOT) != 0 ||
	    req_ctx->creds->caller_glen == 0)
		goto out;

	for (i = 0; i < req_ctx->creds->caller_glen; i++) {
		if (req_ctx->creds->caller_garray[i] == 0) {
			/* Meed to make a copy, or use the old copy */
			if ((*garray_copy) == NULL) {
				/* Make a copy of the active garray */
				(*garray_copy) =
					gsh_malloc(req_ctx->creds->caller_glen *
						   sizeof(gid_t));

				if ((*garray_copy) == NULL) {
					LogCrit(COMPONENT_DISPATCH,
						"Attempt to sqaush caller_garray failed - no memory");
					return false;
				}

				memcpy((*garray_copy),
				       req_ctx->creds->caller_garray,
				       req_ctx->creds->caller_glen *
				       sizeof(gid_t));
			}

			/* Now squash the root id. Since the original copy is
			 * always the same, any root ids in it were still in
			 * the same place, so even if using a copy that had a
			 * different anonymous_gid, we're fine.
			 */
			(*garray_copy)[i] = export_perms->anonymous_gid;

			/* Indicate we squashed the caller_garray */
			req_ctx->cred_flags |= GARRAY_SQUASHED;
		}
	}

	/* If we squashed the caller_garray, use the squashed copy */
	if ((req_ctx->cred_flags & GARRAY_SQUASHED) != 0)
		req_ctx->creds->caller_garray = *garray_copy;

out:

	LogMidDebugAlt(COMPONENT_DISPATCH, COMPONENT_EXPORT,
		    "%s creds mapped to uid=%u, gid=%u%s, glen=%d%s",
		    auth_label,
		    req_ctx->creds->caller_uid,
		    req_ctx->creds->caller_gid,
		    (req_ctx->cred_flags & GID_SQUASHED) != 0
		    	? " (squashed)"
		    	: "",
		    req_ctx->creds->caller_glen,
		    (req_ctx->cred_flags & MANAGED_GIDS) != 0
			? ((req_ctx->cred_flags & GARRAY_SQUASHED) != 0
				? " (managed and squashed)"
				: " (managed)")
			: ((req_ctx->cred_flags & GARRAY_SQUASHED) != 0
				? " (squashed)"
				: ""));

	return true;
}

/**
 * @brief Initialize request context and credentials.
 *
 * @param[in] req_ctx The request context to initialize.
 */
void init_credentials(struct req_op_context *req_ctx)
{
	memset(req_ctx->creds, 0, sizeof(*req_ctx->creds));
	memset(&req_ctx->original_creds, 0, sizeof(req_ctx->original_creds));
	req_ctx->creds->caller_uid = (uid_t) ANON_UID;
	req_ctx->creds->caller_gid = (gid_t) ANON_GID;
	req_ctx->caller_gdata = NULL;
	req_ctx->caller_garray_copy = NULL;
	req_ctx->managed_garray_copy = NULL;
	req_ctx->cred_flags = 0;
}

/**
 * @brief Release temporary credential resources.
 *
 * @param[in] req_ctx The request context to clean up.
 */
void clean_credentials(struct req_op_context *req_ctx)
{
	/* If Manage_gids is used, unref the group list. */
	if (req_ctx->caller_gdata != NULL)
		uid2grp_unref(req_ctx->caller_gdata);

	/* Have we made a local copy of the managed_gids garray? */
	if (req_ctx->managed_garray_copy != NULL)
		gsh_free(req_ctx->managed_garray_copy);

	/* Have we made a local copy of the AUTH_SYS garray? */
	if (req_ctx->caller_garray_copy != NULL)
	    	gsh_free(req_ctx->caller_garray_copy);

	/* Prepare the request context and creds for re-use */
	init_credentials(req_ctx);
}

/**
 * @brief Validate export permissions and update compound
 *
 * @param[in] data Compound data to be used
 *
 * @return NFS4_OK if successful, NFS4ERR_ACCESS or NFS4ERR_WRONGSEC otherwise.
 *
 */
int nfs4_MakeCred(compound_data_t *data)
{
	xprt_type_t xprt_type = svc_get_xprt_type(data->req->rq_xprt);
	int port = get_port(data->req_ctx->caller_addr);

	LogMidDebugAlt(COMPONENT_NFS_V4, COMPONENT_EXPORT,
		    "nfs4_MakeCred about to call nfs_export_check_access");
	nfs_export_check_access(data->req_ctx->caller_addr, data->export,
				&data->export_perms);

	/* Check protocol version */
	if ((data->export_perms.options & EXPORT_OPTION_NFSV4) == 0) {
		LogInfoAlt(COMPONENT_NFS_V4, COMPONENT_EXPORT,
			"NFS4 not allowed on Export_Id %d %s for client %s",
			data->export->id, data->export->fullpath,
			data->req_ctx->client->hostaddr_str);
		return NFS4ERR_ACCESS;
	}

	/* Check transport type */
	if (((xprt_type == XPRT_UDP)
	     && ((data->export_perms.options & EXPORT_OPTION_UDP) == 0))
	    || ((xprt_type == XPRT_TCP)
		&& ((data->export_perms.options & EXPORT_OPTION_TCP) == 0))) {
		LogInfoAlt(COMPONENT_NFS_V4, COMPONENT_EXPORT,
			"NFS4 over %s not allowed on Export_Id %d %s for client %s",
			xprt_type_to_str(xprt_type), data->export->id,
			data->export->fullpath,
			data->req_ctx->client->hostaddr_str);
		return NFS4ERR_ACCESS;
	}

	/* Check if client is using a privileged port. */
	if (((data->export_perms.options & EXPORT_OPTION_PRIVILEGED_PORT) != 0)
	    && (port >= IPPORT_RESERVED)) {
		LogInfoAlt(COMPONENT_NFS_V4, COMPONENT_EXPORT,
			"Non-reserved Port %d is not allowed on Export_Id %d %s for client %s",
			port, data->export->id, data->export->fullpath,
			data->req_ctx->client->hostaddr_str);
		return NFS4ERR_ACCESS;
	}

	/* Test if export allows the authentication provided */
	if (nfs_export_check_security
	    (data->req, &data->export_perms, data->export) == FALSE) {
		LogInfoAlt(COMPONENT_NFS_V4, COMPONENT_EXPORT,
			"NFS4 auth not allowed on Export_Id %d %s for client %s",
			data->export->id, data->export->fullpath,
			data->req_ctx->client->hostaddr_str);
		return NFS4ERR_WRONGSEC;
	}

	/* Get creds */
	if (!get_req_creds(data->req,
			   data->req_ctx,
			   &data->export_perms))
		return NFS4ERR_ACCESS;

	return NFS4_OK;
}

/**
 * @brief Perform version independent ACCESS operation.
 *
 * This function wraps a call to cache_inode_access, determining the appropriate
 * access_mask to use to check all the requested access bits. It requests the
 * allowed and denied access so that it can respond for each requested access
 * with a single access call.
 *
 * @param[in]  entry The cache inode entry to check access for
 * @param[in]  requested_access The ACCESS3 or ACCESS4 bits requested
 * @param[out] granted_access   The bits granted
 * @param[out] supported_access The bits supported for this inode
 * @param[in]  req_ctx          The request context for the operation
 *
 * @return cache inode status
 * @retval CACHE_INODE_SUCCESS all access was granted
 * @retval CACHE_INODE_FSAL_EACCESS one or more access bits were denied
 * @retval other values indicate a cache inode failure
 *
 */

cache_inode_status_t nfs_access_op(cache_entry_t *entry,
				   uint32_t requested_access,
				   uint32_t *granted_access,
				   uint32_t *supported_access,
				   struct req_op_context *req_ctx)
{
	cache_inode_status_t status;
	fsal_accessflags_t access_mask;
	fsal_accessflags_t access_allowed;
	fsal_accessflags_t access_denied;
	uint32_t granted_mask = requested_access;

	access_mask = 0;
	*granted_access = 0;

	LogDebugAlt(COMPONENT_NFSPROTO, COMPONENT_NFS_V4_ACL,
		    "Requested ACCESS=%s,%s,%s,%s,%s,%s",
		    FSAL_TEST_MASK(requested_access,
				   ACCESS3_READ) ? "READ" : "-",
		    FSAL_TEST_MASK(requested_access,
				   ACCESS3_LOOKUP) ? "LOOKUP" : "-",
		    FSAL_TEST_MASK(requested_access,
				   ACCESS3_MODIFY) ? "MODIFY" : "-",
		    FSAL_TEST_MASK(requested_access,
				   ACCESS3_EXTEND) ? "EXTEND" : "-",
		    FSAL_TEST_MASK(requested_access,
				   ACCESS3_DELETE) ? "DELETE" : "-",
		    FSAL_TEST_MASK(requested_access,
				   ACCESS3_EXECUTE) ? "EXECUTE" : "-");

	/* Set mode for read.
	 * NOTE: FSAL_ACE_PERM_LIST_DIR and FSAL_ACE_PERM_READ_DATA have
	 *       the same bit value so we don't bother looking at file type.
	 */
	if (requested_access & ACCESS3_READ)
		access_mask |= FSAL_R_OK | FSAL_ACE_PERM_READ_DATA;

	if (requested_access & ACCESS3_LOOKUP) {
		if (entry->type == DIRECTORY)
			access_mask |= FSAL_X_OK | FSAL_ACE_PERM_EXECUTE;
		else
			granted_mask &= ~ACCESS3_LOOKUP;
	}

	if (requested_access & ACCESS3_MODIFY) {
		if (entry->type == DIRECTORY)
			access_mask |= FSAL_W_OK | FSAL_ACE_PERM_DELETE_CHILD;
		else
			access_mask |= FSAL_W_OK | FSAL_ACE_PERM_WRITE_DATA;
	}

	if (requested_access & ACCESS3_EXTEND) {
		if (entry->type == DIRECTORY)
			access_mask |=
			    FSAL_W_OK | FSAL_ACE_PERM_ADD_FILE |
			    FSAL_ACE_PERM_ADD_SUBDIRECTORY;
		else
			access_mask |= FSAL_W_OK | FSAL_ACE_PERM_APPEND_DATA;
	}

	if (requested_access & ACCESS3_DELETE) {
		if (entry->type == DIRECTORY)
			access_mask |= FSAL_W_OK | FSAL_ACE_PERM_DELETE_CHILD;
		else
			granted_mask &= ~ACCESS3_DELETE;
	}

	if (requested_access & ACCESS3_EXECUTE) {
		if (entry->type != DIRECTORY)
			access_mask |= FSAL_X_OK | FSAL_ACE_PERM_EXECUTE;
		else
			granted_mask &= ~ACCESS3_EXECUTE;
	}

	if (access_mask != 0)
		access_mask |=
		    FSAL_MODE_MASK_FLAG | FSAL_ACE4_MASK_FLAG |
		    FSAL_ACE4_PERM_CONTINUE;

	LogDebugAlt(COMPONENT_NFSPROTO, COMPONENT_NFS_V4_ACL,
		    "access_mask = mode(%c%c%c) ACL(%s,%s,%s,%s,%s)",
		    FSAL_TEST_MASK(access_mask, FSAL_R_OK) ? 'r' : '-',
		    FSAL_TEST_MASK(access_mask, FSAL_W_OK) ? 'w' : '-',
		    FSAL_TEST_MASK(access_mask, FSAL_X_OK) ? 'x' : '-',
		    FSAL_TEST_MASK(access_mask, FSAL_ACE_PERM_READ_DATA) ?
			entry->type == DIRECTORY ?
			"list_dir" : "read_data" : "-",
		    FSAL_TEST_MASK(access_mask,
				   FSAL_ACE_PERM_WRITE_DATA) ?
			entry->type == DIRECTORY ?
			"add_file" : "write_data" : "-",
		    FSAL_TEST_MASK(access_mask, FSAL_ACE_PERM_EXECUTE) ?
			"execute" : "-",
		    FSAL_TEST_MASK(access_mask,
				   FSAL_ACE_PERM_ADD_SUBDIRECTORY) ?
			"add_subdirectory" : "-",
		    FSAL_TEST_MASK(access_mask, FSAL_ACE_PERM_DELETE_CHILD) ?
			"delete_child" : "-");

	status =
	    cache_inode_access_sw(entry, access_mask, &access_allowed,
				  &access_denied, req_ctx, true);

	if (status == CACHE_INODE_SUCCESS ||
	    status == CACHE_INODE_FSAL_EACCESS) {
		/* Define granted access based on granted mode bits. */
		if (access_allowed & FSAL_R_OK)
			*granted_access |= ACCESS3_READ;

		if (access_allowed & FSAL_W_OK)
			*granted_access |=
			    ACCESS3_MODIFY | ACCESS3_EXTEND | ACCESS3_DELETE;

		if (access_allowed & FSAL_X_OK)
			*granted_access |= ACCESS3_LOOKUP | ACCESS3_EXECUTE;

		/* Define granted access based on granted ACL bits. */
		if (access_allowed & FSAL_ACE_PERM_READ_DATA)
			*granted_access |= ACCESS3_READ;

		if (entry->type == DIRECTORY) {
			if (access_allowed & FSAL_ACE_PERM_DELETE_CHILD)
				*granted_access |=
				    ACCESS3_MODIFY | ACCESS3_DELETE;

			if (access_allowed & FSAL_ACE_PERM_ADD_FILE)
				*granted_access |= ACCESS3_EXTEND;

			if (access_allowed & FSAL_ACE_PERM_ADD_SUBDIRECTORY)
				*granted_access |= ACCESS3_EXTEND;
		} else {
			if (access_allowed & FSAL_ACE_PERM_WRITE_DATA)
				*granted_access |= ACCESS3_MODIFY;

			if (access_allowed & FSAL_ACE_PERM_APPEND_DATA)
				*granted_access |= ACCESS3_EXTEND;
		}

		if (access_allowed & FSAL_ACE_PERM_EXECUTE)
			*granted_access |= ACCESS3_LOOKUP | ACCESS3_EXECUTE;

		/* Don't allow any bits that weren't set on request or
		 * allowed by the file type.
		 */
		*granted_access &= granted_mask;

		if (supported_access != NULL)
			*supported_access = granted_mask;

		LogDebugAlt(COMPONENT_NFSPROTO, COMPONENT_NFS_V4_ACL,
			    "Supported ACCESS=%s,%s,%s,%s,%s,%s",
			    FSAL_TEST_MASK(granted_mask,
					   ACCESS3_READ) ? "READ" : "-",
			    FSAL_TEST_MASK(granted_mask,
					   ACCESS3_LOOKUP) ? "LOOKUP" : "-",
			    FSAL_TEST_MASK(granted_mask,
					   ACCESS3_MODIFY) ? "MODIFY" : "-",
			    FSAL_TEST_MASK(granted_mask,
					   ACCESS3_EXTEND) ? "EXTEND" : "-",
			    FSAL_TEST_MASK(granted_mask,
					   ACCESS3_DELETE) ? "DELETE" : "-",
			    FSAL_TEST_MASK(granted_mask,
					   ACCESS3_EXECUTE) ? "EXECUTE" : "-");

		LogDebugAlt(COMPONENT_NFSPROTO, COMPONENT_NFS_V4_ACL,
			    "Granted ACCESS=%s,%s,%s,%s,%s,%s",
			    FSAL_TEST_MASK(*granted_access,
					   ACCESS3_READ) ? "READ" : "-",
			    FSAL_TEST_MASK(*granted_access,
					   ACCESS3_LOOKUP) ? "LOOKUP" : "-",
			    FSAL_TEST_MASK(*granted_access,
					   ACCESS3_MODIFY) ? "MODIFY" : "-",
			    FSAL_TEST_MASK(*granted_access,
					   ACCESS3_EXTEND) ? "EXTEND" : "-",
			    FSAL_TEST_MASK(*granted_access,
					   ACCESS3_DELETE) ? "DELETE" : "-",
			    FSAL_TEST_MASK(*granted_access,
					   ACCESS3_EXECUTE) ? "EXECUTE" : "-");
	}

	return status;
}