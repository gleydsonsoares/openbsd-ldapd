/*	$OpenBSD$ */

/*
 * Copyright (c) 2009, 2010 Martin Hedenfalk <martin@bzero.se>
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#include <sys/types.h>
#include <sys/queue.h>

#include <assert.h>
#include <errno.h>
#include <stdlib.h>
#include <string.h>

#include "ldapd.h"
#include "uuid.h"

int
ldap_delete(struct request *req)
{
	struct btval		 key;
	char			*dn;
	struct namespace	*ns;
	struct referrals	*refs;
	struct cursor		*cursor;

	++stats.req_mod;

	if (ber_scanf_elements(req->op, "s", &dn) != 0)
		return ldap_respond(req, LDAP_PROTOCOL_ERROR);

	normalize_dn(dn);
	log_debug("deleting entry %s", dn);

	if ((ns = namespace_for_base(dn)) == NULL) {
		refs = namespace_referrals(dn);
		if (refs == NULL)
			return ldap_respond(req, LDAP_NAMING_VIOLATION);
		else
			return ldap_refer(req, dn, NULL, refs);
	}

	if (!authorized(req->conn, ns, ACI_WRITE, dn, LDAP_SCOPE_BASE))
		return ldap_respond(req, LDAP_INSUFFICIENT_ACCESS);

	if (namespace_begin(ns) != 0) {
		if (errno == EBUSY) {
			if (namespace_queue_request(ns, req) != 0)
				return ldap_respond(req, LDAP_BUSY);
			return LDAP_BUSY;
		}
		return ldap_respond(req, LDAP_OTHER);
	}

	/* Check that this is a leaf node by getting a cursor to the DN
	 * we're about to delete. If there is a next entry with the DN
	 * as suffix (ie, a child node), the DN can't be deleted.
	 */
	if ((cursor = btree_txn_cursor_open(NULL, ns->data_txn)) == NULL)
		goto fail;

	bzero(&key, sizeof(key));
	key.data = dn;
	key.size = strlen(dn);
	if (btree_cursor_get(cursor, &key, NULL, BT_CURSOR_EXACT) != 0)
		goto fail;
	if (btree_cursor_get(cursor, &key, NULL, BT_NEXT) != 0) {
		if (errno != ENOENT)
			goto fail;
	} else if (has_suffix(&key, dn)) {
		namespace_abort(ns);
		return ldap_respond(req, LDAP_NOT_ALLOWED_ON_NONLEAF);
	}

	if (namespace_del(ns, dn) == 0) {
		namespace_commit(ns);
		return ldap_respond(req, LDAP_SUCCESS);
	}

fail:
	namespace_abort(ns);
	if (errno == ENOENT)
		return ldap_respond(req, LDAP_NO_SUCH_OBJECT);
	return ldap_respond(req, LDAP_OTHER);
}

int
ldap_add(struct request *req)
{
	char			 uuid_str[64];
	struct uuid		 uuid;
	char			*dn, *s;
	struct attr_type	*at;
	struct ber_element	*attrs, *attr, *elm, *set = NULL;
	struct namespace	*ns;
	struct referrals	*refs;
	int			 rc;

	++stats.req_mod;

	if (ber_scanf_elements(req->op, "{se", &dn, &attrs) != 0)
		return ldap_respond(req, LDAP_PROTOCOL_ERROR);

	normalize_dn(dn);
	log_debug("adding entry %s", dn);

	if (*dn == '\0')
		return ldap_respond(req, LDAP_INVALID_DN_SYNTAX);

	if ((ns = namespace_for_base(dn)) == NULL) {
		refs = namespace_referrals(dn);
		if (refs == NULL)
			return ldap_respond(req, LDAP_NAMING_VIOLATION);
		else
			return ldap_refer(req, dn, NULL, refs);
	}

	if (!authorized(req->conn, ns, ACI_WRITE, dn, LDAP_SCOPE_BASE) != 0)
		return ldap_respond(req, LDAP_INSUFFICIENT_ACCESS);

	/* Check that we're not adding immutable attributes.
	 */
	for (elm = attrs->be_sub; elm != NULL; elm = elm->be_next) {
		attr = elm->be_sub;
		if (attr == NULL || ber_get_string(attr, &s) != 0)
			return ldap_respond(req, LDAP_PROTOCOL_ERROR);
		at = lookup_attribute(conf->schema, s);
		if (at == NULL) {
			log_debug("unknown attribute type %s", s);
			return ldap_respond(req, LDAP_NO_SUCH_ATTRIBUTE);
		}
		if (at->immutable) {
			log_debug("attempt to add immutable attribute %s", s);
			return ldap_respond(req, LDAP_CONSTRAINT_VIOLATION);
		}
	}

	if (namespace_begin(ns) == -1) {
		if (errno == EBUSY) {
			if (namespace_queue_request(ns, req) != 0)
				return ldap_respond(req, LDAP_BUSY);
			return LDAP_BUSY;
		}
		return ldap_respond(req, LDAP_OTHER);
	}

	/* add operational attributes
	 */
	if ((set = ber_add_set(NULL)) == NULL)
		goto fail;
	if (ber_add_string(set, req->conn->binddn ?: "") == NULL)
		goto fail;
	if (ldap_add_attribute(attrs, "creatorsName", set) == NULL)
		goto fail;

	if ((set = ber_add_set(NULL)) == NULL)
		goto fail;
	if (ber_add_string(set, ldap_now()) == NULL)
		goto fail;
	if (ldap_add_attribute(attrs, "createTimestamp", set) == NULL)
		goto fail;

	uuid_create(&uuid);
	uuid_to_string(&uuid, uuid_str, sizeof(uuid_str));
	if ((set = ber_add_set(NULL)) == NULL)
		goto fail;
	if (ber_add_string(set, uuid_str) == NULL)
		goto fail;
	if (ldap_add_attribute(attrs, "entryUUID", set) == NULL)
		goto fail;

	if ((rc = validate_entry(dn, attrs, ns->relax)) != LDAP_SUCCESS ||
	    namespace_add(ns, dn, attrs) != 0) {
		namespace_abort(ns);
		if (rc == LDAP_SUCCESS && errno == EEXIST)
			rc = LDAP_ALREADY_EXISTS;
		else if (rc == LDAP_SUCCESS)
			rc = LDAP_OTHER;
	} else if (namespace_commit(ns) != 0)
		rc = LDAP_OTHER;

	return ldap_respond(req, rc);

fail:
	if (set != NULL)
		ber_free_elements(set);
	namespace_abort(ns);
	return ldap_respond(req, LDAP_OTHER);
}

int
ldap_modify(struct request *req)
{
	int			 rc;
	char			*dn;
	long long		 op;
	char			*attr;
	struct ber_element	*mods, *entry, *mod, *vals, *a, *set;
	struct namespace	*ns;
	struct attr_type	*at;
	struct referrals	*refs;

	++stats.req_mod;

	if (ber_scanf_elements(req->op, "{se", &dn, &mods) != 0)
		return ldap_respond(req, LDAP_PROTOCOL_ERROR);

	normalize_dn(dn);
	log_debug("modifying dn %s", dn);

	if (*dn == 0)
		return ldap_respond(req, LDAP_INVALID_DN_SYNTAX);

	if ((ns = namespace_for_base(dn)) == NULL) {
		refs = namespace_referrals(dn);
		if (refs == NULL)
			return ldap_respond(req, LDAP_NAMING_VIOLATION);
		else
			return ldap_refer(req, dn, NULL, refs);
	}

	if (!authorized(req->conn, ns, ACI_WRITE, dn, LDAP_SCOPE_BASE) != 0)
		return ldap_respond(req, LDAP_INSUFFICIENT_ACCESS);

	if (namespace_begin(ns) == -1) {
		if (errno == EBUSY) {
			if (namespace_queue_request(ns, req) != 0)
				return ldap_respond(req, LDAP_BUSY);
			return LDAP_BUSY;
		}
		return ldap_respond(req, LDAP_OTHER);
	}

	if ((entry = namespace_get(ns, dn)) == NULL) {
		rc = LDAP_NO_SUCH_OBJECT;
		goto done;
	}

	for (mod = mods->be_sub; mod; mod = mod->be_next) {
		if (ber_scanf_elements(mod, "{E{se", &op, &attr, &vals) != 0) {
			rc = LDAP_PROTOCOL_ERROR;
			goto done;
		}

		if ((at = lookup_attribute(conf->schema, attr)) == NULL &&
		    !ns->relax) {
			log_debug("unknown attribute type %s", attr);
			rc = LDAP_NO_SUCH_ATTRIBUTE;
			goto done;
		}
		if (at != NULL && at->immutable) {
			log_debug("attempt to modify immutable attribute %s",
			    attr);
			rc = LDAP_CONSTRAINT_VIOLATION;
			goto done;
		}

		a = ldap_get_attribute(entry, attr);

		switch (op) {
		case LDAP_MOD_ADD:
			if (a == NULL)
				ldap_add_attribute(entry, attr, vals);
			else
				ldap_merge_values(a, vals);
			break;
		case LDAP_MOD_DELETE:
			if (vals->be_sub &&
			    vals->be_sub->be_type == BER_TYPE_SET)
				ldap_del_values(a, vals);
			else
				ldap_del_attribute(entry, attr);
			break;
		case LDAP_MOD_REPLACE:
			if (vals->be_sub) {
				if (a == NULL)
					ldap_add_attribute(entry, attr, vals);
				else
					ldap_set_values(a, vals);
			} else if (a == NULL)
				ldap_del_attribute(entry, attr);
			break;
		}
	}

	if ((rc = validate_entry(dn, entry, ns->relax)) != LDAP_SUCCESS)
		goto done;

	set = ber_add_set(NULL);
	ber_add_string(set, req->conn->binddn ?: "");
	if ((a = ldap_get_attribute(entry, "modifiersName")) != NULL)
		ldap_set_values(a, set);
	else
		ldap_add_attribute(entry, "modifiersName", set);

	set = ber_add_set(NULL);
	ber_add_string(set, ldap_now());
	if ((a = ldap_get_attribute(entry, "modifyTimestamp")) != NULL)
		ldap_set_values(a, set);
	else
		ldap_add_attribute(entry, "modifyTimestamp", set);

	if (namespace_update(ns, dn, entry) == 0)
		rc = LDAP_SUCCESS;
	else
		rc = LDAP_OTHER;

done:
	if (rc == LDAP_SUCCESS)
		namespace_commit(ns);
	else
		namespace_abort(ns);

	if (rc >= 0)
		ldap_respond(req, rc);
	return rc;
}

