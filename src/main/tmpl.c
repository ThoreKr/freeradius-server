/*
 *   This program is free software; you can redistribute it and/or modify
 *   it under the terms of the GNU General Public License as published by
 *   the Free Software Foundation; either version 2 of the License, or
 *   (at your option) any later version.
 *
 *   This program is distributed in the hope that it will be useful,
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 *   GNU General Public License for more details.
 *
 *   You should have received a copy of the GNU General Public License
 *   along with this program; if not, write to the Free Software
 *   Foundation, Inc., 51 Franklin St, Fifth Floor, Boston, MA 02110-1301, USA
 */

/**
 * $Id$
 *
 * @brief #VALUE_PAIR template functions
 * @file main/tmpl.c
 *
 * @ingroup AVP
 *
 * @copyright 2014-2015 The FreeRADIUS server project
 */
RCSID("$Id$")

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>

#include <ctype.h>

/** Map #tmpl_type_t values to descriptive strings
 */
FR_NAME_NUMBER const tmpl_names[] = {
	{ "literal",		TMPL_TYPE_UNPARSED 	},
	{ "xlat",		TMPL_TYPE_XLAT		},
	{ "attr",		TMPL_TYPE_ATTR		},
	{ "unknown attr",	TMPL_TYPE_ATTR_UNDEFINED	},
	{ "list",		TMPL_TYPE_LIST		},
	{ "regex",		TMPL_TYPE_REGEX		},
	{ "exec",		TMPL_TYPE_EXEC		},
	{ "data",		TMPL_TYPE_DATA		},
	{ "parsed xlat",	TMPL_TYPE_XLAT_STRUCT	},
	{ "parsed regex",	TMPL_TYPE_REGEX_STRUCT	},
	{ "null",		TMPL_TYPE_NULL		},
	{ NULL, 0 }
};

/** Map keywords to #pair_lists_t values
 */
const FR_NAME_NUMBER pair_lists[] = {
	{ "request",		PAIR_LIST_REQUEST },
	{ "reply",		PAIR_LIST_REPLY },
	{ "control",		PAIR_LIST_CONTROL },		/* New name should have priority */
	{ "config",		PAIR_LIST_CONTROL },
	{ "session-state",	PAIR_LIST_STATE },
#ifdef WITH_PROXY
	{ "proxy-request",	PAIR_LIST_PROXY_REQUEST },
	{ "proxy-reply",	PAIR_LIST_PROXY_REPLY },
#endif
#ifdef WITH_COA
	{ "coa",		PAIR_LIST_COA },
	{ "coa-reply",		PAIR_LIST_COA_REPLY },
	{ "disconnect",		PAIR_LIST_DM },
	{ "disconnect-reply",	PAIR_LIST_DM_REPLY },
#endif
	{  NULL , -1 }
};

/** Map keywords to #request_refs_t values
 */
const FR_NAME_NUMBER request_refs[] = {
	{ "outer",		REQUEST_OUTER },
	{ "current",		REQUEST_CURRENT },
	{ "parent",		REQUEST_PARENT },
	{ "proxy",		REQUEST_PROXY },
	{  NULL , -1 }
};

/** @name Parse list and request qualifiers to #pair_lists_t and #request_refs_t values
 *
 * These functions also resolve #pair_lists_t and #request_refs_t values to #REQUEST
 * structs and the head of #VALUE_PAIR lists in those structs.
 *
 * For adding new #VALUE_PAIR to the lists, the #radius_list_ctx function can be used
 * to obtain the appropriate TALLOC_CTX pointer.
 *
 * @note These don't really have much to do with #vp_tmpl_t. They're in the same
 *	file as they're used almost exclusively by the tmpl_* functions.
 * @{
 */

/** Resolve attribute name to a #pair_lists_t value.
 *
 * Check the name string for #pair_lists qualifiers and write a #pair_lists_t value
 * for that list to out. This value may be passed to #radius_list, along with the current
 * #REQUEST, to get a pointer to the actual list in the #REQUEST.
 *
 * If we're sure we've definitely found a list qualifier token delimiter (``:``) but the
 * string doesn't match a #radius_list qualifier, return 0 and write #PAIR_LIST_UNKNOWN
 * to out.
 *
 * If we can't find a string that looks like a request qualifier, set out to def, and
 * return 0.
 *
 * @note #radius_list_name should be called before passing a name string that may
 *	contain qualifiers to #fr_dict_attr_by_name.
 *
 * @param[out] out Where to write the list qualifier.
 * @param[in] name String containing list qualifiers to parse.
 * @param[in] def the list to return if no qualifiers were found.
 * @return 0 if no valid list qualifier could be found, else the number of bytes consumed.
 *	The caller may then advanced the name pointer by the value returned, to get the
 *	start of the attribute name (if any).
 *
 * @see pair_list
 * @see radius_list
 */
size_t radius_list_name(pair_lists_t *out, char const *name, pair_lists_t def)
{
	char const *p = name;
	char const *q;

	/* This should never be a NULL pointer */
	rad_assert(name);

	/*
	 *	Try and determine the end of the token
	 */
	for (q = p; fr_dict_attr_allowed_chars[(uint8_t) *q]; q++);

	switch (*q) {
	/*
	 *	It's a bareword made up entirely of dictionary chars
	 *	check and see if it's a list qualifier, and if it's
	 *	not, return the def and say we couldn't parse
	 *	anything.
	 */
	case '\0':
		*out = fr_substr2int(pair_lists, p, PAIR_LIST_UNKNOWN, (q - p));
		if (*out != PAIR_LIST_UNKNOWN) return q - p;
		*out = def;
		return 0;

	/*
	 *	It may be a list qualifier delimiter. Because of tags
	 *	We need to check that it doesn't look like a tag suffix.
	 *	We do this by looking at the chars between ':' and the
	 *	next token delimiter, and seeing if they're all digits.
	 */
	case ':':
	{
		char const *d = q + 1;

		if (isdigit((int) *d)) {
			while (isdigit((int) *d)) d++;

			/*
			 *	Char after the number string
			 *	was a token delimiter, so this is a
			 *	tag, not a list qualifier.
			 */
			if (!fr_dict_attr_allowed_chars[(uint8_t) *d]) {
				*out = def;
				return 0;
			}
		}

		*out = fr_substr2int(pair_lists, p, PAIR_LIST_UNKNOWN, (q - p));
		if (*out == PAIR_LIST_UNKNOWN) return 0;

		return (q + 1) - name; /* Consume the list and delimiter */
	}

	default:
		*out = def;
		return 0;
	}
}

/** Resolve attribute #pair_lists_t value to an attribute list.
 *
 * The value returned is a pointer to the pointer of the HEAD of a #VALUE_PAIR list in the
 * #REQUEST. If the head of the list changes, the pointer will still be valid.
 *
 * @param[in] request containing the target lists.
 * @param[in] list #pair_lists_t value to resolve to #VALUE_PAIR list. Will be NULL if list
 *	name couldn't be resolved.
 * @return a pointer to the HEAD of a list in the #REQUEST.
 *
 * @see tmpl_cursor_init
 * @see fr_cursor_init
 */
VALUE_PAIR **radius_list(REQUEST *request, pair_lists_t list)
{
	if (!request) return NULL;

	switch (list) {
	/* Don't add default */
	case PAIR_LIST_UNKNOWN:
		break;

	case PAIR_LIST_REQUEST:
		if (!request->packet) return NULL;
		return &request->packet->vps;

	case PAIR_LIST_REPLY:
		if (!request->reply) return NULL;
		return &request->reply->vps;

	case PAIR_LIST_CONTROL:
		return &request->control;

	case PAIR_LIST_STATE:
		return &request->state;

#ifdef WITH_PROXY
	case PAIR_LIST_PROXY_REQUEST:
		if (!request->proxy || !request->proxy->packet) break;
		return &request->proxy->packet->vps;

	case PAIR_LIST_PROXY_REPLY:
		if (!request->proxy || !request->proxy->reply) break;
		return &request->proxy->reply->vps;
#endif
#ifdef WITH_COA
	case PAIR_LIST_COA:
		if (request->coa &&
		    (request->coa->proxy->packet->code == PW_CODE_COA_REQUEST)) {
			return &request->coa->proxy->packet->vps;
		}
		break;

	case PAIR_LIST_COA_REPLY:
		if (request->coa && /* match reply with request */
		    (request->coa->proxy->packet->code == PW_CODE_COA_REQUEST) &&
		    request->coa->proxy->reply) {
			return &request->coa->proxy->reply->vps;
		}
		break;

	case PAIR_LIST_DM:
		if (request->coa &&
		    (request->coa->proxy->packet->code == PW_CODE_DISCONNECT_REQUEST)) {
			return &request->coa->proxy->packet->vps;
		}
		break;

	case PAIR_LIST_DM_REPLY:
		if (request->coa && /* match reply with request */
		    (request->coa->proxy->packet->code == PW_CODE_DISCONNECT_REQUEST) &&
		    request->coa->proxy->reply) {
			return &request->coa->proxy->reply->vps;
		}
		break;
#endif
	}

	RWDEBUG2("List \"%s\" is not available",
		fr_int2str(pair_lists, list, "<INVALID>"));

	return NULL;
}

/** Resolve a list to the #RADIUS_PACKET holding the HEAD pointer for a #VALUE_PAIR list
 *
 * Returns a pointer to the #RADIUS_PACKET that holds the HEAD pointer of a given list,
 * for the current #REQUEST.
 *
 * @param[in] request To resolve list in.
 * @param[in] list #pair_lists_t value to resolve to #RADIUS_PACKET.
 * @return
 *	- #RADIUS_PACKET on success.
 *	- NULL on failure.
 *
 * @see radius_list
 */
RADIUS_PACKET *radius_packet(REQUEST *request, pair_lists_t list)
{
	switch (list) {
	/* Don't add default */
	case PAIR_LIST_STATE:
	case PAIR_LIST_CONTROL:
	case PAIR_LIST_UNKNOWN:
		return NULL;

	case PAIR_LIST_REQUEST:
		return request->packet;

	case PAIR_LIST_REPLY:
		return request->reply;

#ifdef WITH_PROXY
	case PAIR_LIST_PROXY_REQUEST:
		if (!request->proxy) return NULL;
		return request->proxy->packet;

	case PAIR_LIST_PROXY_REPLY:
		if (!request->proxy) return NULL;
		return request->proxy->reply;
#endif

#ifdef WITH_COA
	case PAIR_LIST_COA:
	case PAIR_LIST_DM:
		if (!request->coa) return NULL;
		return request->coa->proxy->packet;

	case PAIR_LIST_COA_REPLY:
	case PAIR_LIST_DM_REPLY:
		if (!request->coa) return NULL;
		return request->coa->proxy->reply;
#endif
	}

	return NULL;
}

/** Return the correct TALLOC_CTX to alloc #VALUE_PAIR in, for a list
 *
 * Allocating new #VALUE_PAIR in the context of a #REQUEST is usually wrong.
 * #VALUE_PAIR should be allocated in the context of a #RADIUS_PACKET, so that if the
 * #RADIUS_PACKET is freed before the #REQUEST, the associated #VALUE_PAIR lists are
 * freed too.
 *
 * @param[in] request containing the target lists.
 * @param[in] list #pair_lists_t value to resolve to TALLOC_CTX.
 * @return
 *	- TALLOC_CTX on success.
 *	- NULL on failure.
 *
 * @see radius_list
 */
TALLOC_CTX *radius_list_ctx(REQUEST *request, pair_lists_t list)
{
	if (!request) return NULL;

	switch (list) {
	case PAIR_LIST_REQUEST:
		return request->packet;

	case PAIR_LIST_REPLY:
		return request->reply;

	case PAIR_LIST_CONTROL:
		return request;

	case PAIR_LIST_STATE:
		return request->state_ctx;

#ifdef WITH_PROXY
	case PAIR_LIST_PROXY_REQUEST:
		if (!request->proxy) return NULL;
		return request->proxy->packet;

	case PAIR_LIST_PROXY_REPLY:
		if (!request->proxy) return NULL;
		return request->proxy->reply;
#endif

#ifdef WITH_COA
	case PAIR_LIST_COA:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->packet->code != PW_CODE_COA_REQUEST) return NULL;
		return request->coa->proxy->packet;

	case PAIR_LIST_COA_REPLY:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->packet->code != PW_CODE_COA_REQUEST) return NULL;
		return request->coa->proxy->reply;

	case PAIR_LIST_DM:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->packet->code != PW_CODE_DISCONNECT_REQUEST) return NULL;
		return request->coa->proxy->packet;

	case PAIR_LIST_DM_REPLY:
		if (!request->coa) return NULL;
		rad_assert(request->coa->proxy != NULL);
		if (request->coa->proxy->packet->code != PW_CODE_DISCONNECT_REQUEST) return NULL;
		return request->coa->proxy->reply;
#endif
	/* Don't add default */
	case PAIR_LIST_UNKNOWN:
		break;
	}

	return NULL;
}

/** Resolve attribute name to a #request_refs_t value.
 *
 * Check the name string for qualifiers that reference a parent #REQUEST.
 *
 * If we find a string that matches a #request_refs qualifier, return the number of chars
 * we consumed.
 *
 * If we're sure we've definitely found a list qualifier token delimiter (``*``) but the
 * qualifier doesn't match one of the #request_refs qualifiers, return 0 and set out to
 * #REQUEST_UNKNOWN.
 *
 * If we can't find a string that looks like a request qualifier, set out to def, and
 * return 0.
 *
 * @param[out] out The #request_refs_t value the name resolved to (or #REQUEST_UNKNOWN).
 * @param[in] name of attribute.
 * @param[in] def default request ref to return if no request qualifier is present.
 * @return 0 if no valid request qualifier could be found, else the number of bytes consumed.
 *	The caller may then advanced the name pointer by the value returned, to get the
 *	start of the attribute list or attribute name(if any).
 *
 * @see radius_list_name
 * @see request_refs
 */
size_t radius_request_name(request_refs_t *out, char const *name, request_refs_t def)
{
	char const *p, *q;

	p = name;
	/*
	 *	Try and determine the end of the token
	 */
	for (q = p; fr_dict_attr_allowed_chars[(uint8_t) *q] && (*q != '.') && (*q != '-'); q++);

	/*
	 *	First token delimiter wasn't a '.'
	 */
	if (*q != '.') {
		*out = def;
		return 0;
	}

	*out = fr_substr2int(request_refs, name, REQUEST_UNKNOWN, q - p);
	if (*out == REQUEST_UNKNOWN) return 0;

	return (q + 1) - p;
}

/** Resolve a #request_refs_t to a #REQUEST.
 *
 * Sometimes #REQUEST structs may be chained to each other, as is the case
 * when internally proxying EAP. This function resolves a #request_refs_t
 * to a #REQUEST higher in the chain than the current #REQUEST.
 *
 * @see radius_list
 * @param[in,out] context #REQUEST to start resolving from, and where to write
 *	a pointer to the resolved #REQUEST back to.
 * @param[in] name (request) to resolve.
 * @return
 *	- 0 if request is valid in this context.
 *	- -1 if request is not valid in this context.
 */
int radius_request(REQUEST **context, request_refs_t name)
{
	REQUEST *request = *context;

	switch (name) {
	case REQUEST_CURRENT:
		return 0;

	case REQUEST_PARENT:	/* for future use in request chaining */
	case REQUEST_OUTER:
		if (!request->parent) {
			return -1;
		}
		*context = request->parent;
		break;

	case REQUEST_PROXY:
		if (!request->proxy) {
			return -1;
		}
		*context = request->proxy;
		break;

	case REQUEST_UNKNOWN:
	default:
		rad_assert(0);
		return -1;
	}

	return 0;
}
/** @} */

/** @name Alloc or initialise #vp_tmpl_t
 *
 * @note Should not usually be called outside of tmpl_* functions, use one of
 *	the tmpl_*from_* functions instead.
 * @{
 */

/** Initialise stack allocated #vp_tmpl_t
 *
 * @note Name is not talloc_strdup'd or memcpy'd so must be available, and must not change
 *	for the lifetime of the #vp_tmpl_t.
 *
 * @param[out] vpt to initialise.
 * @param[in] type to set in the #vp_tmpl_t.
 * @param[in] name of the #vp_tmpl_t.
 * @param[in] len The length of the buffer (or a substring of the buffer) pointed to by name.
 *	If < 0 strlen will be used to determine the length.
 * @param[in] quote The type of quoting around the template name.
 * @return a pointer to the initialised #vp_tmpl_t. The same value as vpt.
 */
vp_tmpl_t *tmpl_init(vp_tmpl_t *vpt, tmpl_type_t type, char const *name, ssize_t len, FR_TOKEN quote)
{
	rad_assert(vpt);
	rad_assert(type != TMPL_TYPE_UNKNOWN);
	rad_assert(type <= TMPL_TYPE_NULL);

	memset(vpt, 0, sizeof(vp_tmpl_t));
	vpt->type = type;

	if (name) {
		vpt->name = name;
		vpt->len = len < 0 ? strlen(name) :
				     (size_t) len;
		vpt->quote = quote;
	}
	return vpt;
}

/** Create a new heap allocated #vp_tmpl_t
 *
 * @param[in,out] ctx to allocate in.
 * @param[in] type to set in the #vp_tmpl_t.
 * @param[in] name of the #vp_tmpl_t (will be copied to a new talloc buffer parented
 *	by the #vp_tmpl_t).
 * @param[in] len The length of the buffer (or a substring of the buffer) pointed to by name.
 *	If < 0 strlen will be used to determine the length.
 * @param[in] quote The type of quoting around the template name.
 * @return the newly allocated #vp_tmpl_t.
 */
vp_tmpl_t *tmpl_alloc(TALLOC_CTX *ctx, tmpl_type_t type, char const *name, ssize_t len, FR_TOKEN quote)
{
	vp_tmpl_t *vpt;

	rad_assert(type != TMPL_TYPE_UNKNOWN);
	rad_assert(type <= TMPL_TYPE_NULL);

	vpt = talloc_zero(ctx, vp_tmpl_t);
	if (!vpt) return NULL;
	vpt->type = type;
	if (name) {
		vpt->name = talloc_bstrndup(vpt, name, len < 0 ? strlen(name) : (size_t)len);
		vpt->len = talloc_array_length(vpt->name) - 1;
		vpt->quote = quote;
	}

	return vpt;
}
/* @} **/

/** @name Create new #vp_tmpl_t from a string
 *
 * @{
 */

/** Initialise a #vp_tmpl_t to search for, or create attributes
 *
 * @param vpt to initialise.
 * @param da of #VALUE_PAIR type to operate on.
 * @param tag Must be one of:
 *	- A positive integer specifying a specific tag.
 *	- #TAG_ANY - Attribute with no specific tag value.
 *	- #TAG_NONE - No tag.
 * @param num Specific instance, or all instances. Must be one of:
 *	- A positive integer specifying an instance.
 *	- #NUM_ALL - All instances.
 *	- #NUM_ANY - The first instance found.
 *	- #NUM_LAST - The last instance found.
 * @param request to operate on.
 * @param list to operate on.
 */
void tmpl_from_da(vp_tmpl_t *vpt, fr_dict_attr_t const *da, int8_t tag, int num,
		  request_refs_t request, pair_lists_t list)
{
	static char const name[] = "internal";

	rad_assert(da);

	tmpl_init(vpt, TMPL_TYPE_ATTR, name, sizeof(name), T_BARE_WORD);
	vpt->tmpl_da = da;

	vpt->tmpl_request = request;
	vpt->tmpl_list = list;
	vpt->tmpl_tag = tag;
	vpt->tmpl_num = num;
}

/** Create a #vp_tmpl_t from a #value_data_t
 *
 * @param[in,out] ctx to allocate #vp_tmpl_t in.
 * @param[out] out Where to write pointer to new #vp_tmpl_t.
 * @param[in] data to convert.
 * @param[in] type of data.
 * @param[in] enumv Used to convert integers to string types for printing. May be NULL.
 * @param[in] steal If true, any buffers are moved to the new ctx instead of being duplicated.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int tmpl_afrom_value_data(TALLOC_CTX *ctx, vp_tmpl_t **out, value_data_t *data,
			  PW_TYPE type, fr_dict_attr_t const *enumv, bool steal)
{
	char const *name;
	vp_tmpl_t *vpt;

	vpt = talloc(ctx, vp_tmpl_t);
	name = value_data_asprint(vpt, type, enumv, data, '\0');
	tmpl_init(vpt, TMPL_TYPE_DATA, name, talloc_array_length(name),
		  (type == PW_TYPE_STRING) ? T_DOUBLE_QUOTED_STRING : T_BARE_WORD);

	if (steal) {
		if (value_data_steal(vpt, &vpt->tmpl_data_value, type, data) < 0) {
			talloc_free(vpt);
			return -1;
		}
		vpt->tmpl_data_type = type;
	} else {
		if (value_data_copy(vpt, &vpt->tmpl_data_value, type, data) < 0) {
			talloc_free(vpt);
			return -1;
		}
		vpt->tmpl_data_type = type;
	}
	*out = vpt;

	return 0;
}

/** Parse a string into a TMPL_TYPE_ATTR_* or #TMPL_TYPE_LIST type #vp_tmpl_t
 *
 * @note The name field is just a copy of the input pointer, if you know that string might be
 *	freed before you're done with the #vp_tmpl_t use #tmpl_afrom_attr_str
 *	instead.
 *
 * @param[out] vpt to modify.
 * @param[in] name of attribute including #request_refs and #pair_lists qualifiers.
 *	If only #request_refs and #pair_lists qualifiers are found, a #TMPL_TYPE_LIST
 *	#vp_tmpl_t will be produced.
 * @param[in] request_def The default #REQUEST to set if no #request_refs qualifiers are
 *	found in name.
 * @param[in] list_def The default list to set if no #pair_lists qualifiers are found in
 *	name.
 * @param[in] allow_unknown If true attributes in the format accepted by
 *	#fr_dict_unknown_from_suboid will be allowed, even if they're not in the main
 *	dictionaries.
 *	If an unknown attribute is found a #TMPL_TYPE_ATTR #vp_tmpl_t will be
 *	produced with the unknown #fr_dict_attr_t stored in the ``unknown.da`` buffer.
 *	This #fr_dict_attr_t will have its ``flags.is_unknown`` field set to true.
 *	If #tmpl_from_attr_substr is being called on startup, the #vp_tmpl_t may be
 *	passed to #tmpl_define_unknown_attr to add the unknown attribute to the main
 *	dictionary.
 *	If the unknown attribute is not added to the main dictionary the #vp_tmpl_t
 *	cannot be used to search for a #VALUE_PAIR in a #REQUEST.
 * @param[in] allow_undefined If true, we don't generate a parse error on unknown attributes.
 *	If an unknown attribute is found a #TMPL_TYPE_ATTR_UNDEFINED #vp_tmpl_t
 *	will be produced.  A #vp_tmpl_t of this type can be passed to
 *	#tmpl_define_undefined_attr which will add the attribute to the global dictionary,
 *	and fixup the #vp_tmpl_t, changing it to a #TMPL_TYPE_ATTR with a pointer to the
 *	new #fr_dict_attr_t.
 * @return
 *	- <= 0 on error (parse failure offset as negative integer).
 *	- > 0 on success (number of bytes parsed).
 *
 * @see REMARKER to produce pretty error markers from the return value.
 */
ssize_t tmpl_from_attr_substr(vp_tmpl_t *vpt, char const *name,
			      request_refs_t request_def, pair_lists_t list_def,
			      bool allow_unknown, bool allow_undefined)
{
	char const *p;
	long num;
	char *q;
	tmpl_type_t type = TMPL_TYPE_ATTR;

	value_pair_tmpl_attr_t attr;	/* So we don't fill the tmpl with junk and then error out */

	memset(vpt, 0, sizeof(*vpt));
	memset(&attr, 0, sizeof(attr));

	p = name;

	if (*p == '&') p++;

	p += radius_request_name(&attr.request, p, request_def);
	if (attr.request == REQUEST_UNKNOWN) {
		fr_strerror_printf("Invalid request qualifier");
		return -(p - name);
	}

	/*
	 *	Finding a list qualifier is optional
	 */
	p += radius_list_name(&attr.list, p, list_def);
	if (attr.list == PAIR_LIST_UNKNOWN) {
		fr_strerror_printf("Invalid list qualifier");
		return -(p - name);
	}

	attr.tag = TAG_ANY;
	attr.num = NUM_ANY;

	/*
	 *	This may be just a bare list, but it can still
	 *	have instance selectors and tag selectors.
	 */
	switch (*p) {
	case '\0':
		type = TMPL_TYPE_LIST;
		goto finish;

	case '[':
		type = TMPL_TYPE_LIST;
		goto do_num;

	default:
		break;
	}

	attr.da = fr_dict_attr_by_name_substr(NULL, &p);
	if (!attr.da) {
		char const *a;

		/*
		 *	Record start of attribute in case we need to error out.
		 */
		a = p;

		fr_strerror();	/* Clear out any existing errors */

		/*
		 *	Attr-1.2.3.4 is OK.
		 */
		if (fr_dict_unknown_from_suboid(NULL, (fr_dict_attr_t *)&attr.unknown.vendor,
						(fr_dict_attr_t *)&attr.unknown.da, fr_dict_root(fr_dict_internal),
						&p) == 0) {
			/*
			 *	Check what we just parsed really hasn't been defined
			 *	in the main dictionaries.
			 *
			 *	If it has, parsing is the same as if the attribute
			 *	name had been used instead of its OID.
			 */
			attr.da = fr_dict_attr_child_by_num(((fr_dict_attr_t *)&attr.unknown.da)->parent,
							    ((fr_dict_attr_t *)&attr.unknown.da)->attr);
			if (attr.da) {
				vpt->auto_converted = true;
				goto do_num;
			}

			if (!allow_unknown) {
				fr_strerror_printf("Unknown attribute");
				return -(a - name);
			}

			/*
			 *	Unknown attributes can't be encoded, as we don't
			 *	know how to encode them!
			 */
			((fr_dict_attr_t *)attr.unknown.da)->flags.internal = 1;
			attr.da = (fr_dict_attr_t *)&attr.unknown.da;

			goto do_num; /* unknown attributes can't have tags */
		}

		/*
		 *	Can't parse it as an attribute, might be a literal string
		 *	let the caller decide.
		 *
		 *	Don't alter the fr_strerror buffer, should contain the parse
		 *	error from fr_dict_unknown_from_suboid.
		 */
		if (!allow_undefined) return -(a - name);

		/*
		 *	Copy the name to a field for later resolution
		 */
		type = TMPL_TYPE_ATTR_UNDEFINED;
		for (q = attr.unknown.name; fr_dict_attr_allowed_chars[(int) *p]; *q++ = *p++) {
			if (q >= (attr.unknown.name + sizeof(attr.unknown.name) - 1)) {
				fr_strerror_printf("Attribute name is too long");
				return -(p - name);
			}
		}
		*q = '\0';

		goto do_num;
	}

	/*
	 *	The string MIGHT have a tag.
	 */
	if (*p == ':') {
		if (attr.da && !attr.da->flags.has_tag) { /* Lists don't have a da */
			fr_strerror_printf("Attribute '%s' cannot have a tag", attr.da->name);
			return -(p - name);
		}

		num = strtol(p + 1, &q, 10);
		if ((num > 0x1f) || (num < 0)) {
			fr_strerror_printf("Invalid tag value '%li' (should be between 0-31)", num);
			return -((p + 1)- name);
		}

		attr.tag = num;
		p = q;
	}

do_num:
	if (*p == '\0') goto finish;

	if (*p == '[') {
		p++;

		switch (*p) {
		case '#':
			attr.num = NUM_COUNT;
			p++;
			break;

		case '*':
			attr.num = NUM_ALL;
			p++;
			break;

		case 'n':
			attr.num = NUM_LAST;
			p++;
			break;

		default:
			num = strtol(p, &q, 10);
			if (p == q) {
				fr_strerror_printf("Array index is not an integer");
				return -(p - name);
			}

			if ((num > 1000) || (num < 0)) {
				fr_strerror_printf("Invalid array reference '%li' (should be between 0-1000)", num);
				return -(p - name);
			}
			attr.num = num;
			p = q;
			break;
		}

		if (*p != ']') {
			fr_strerror_printf("No closing ']' for array index");
			return -(p - name);
		}
		p++;
	}

finish:
	vpt->type = type;
	vpt->name = name;
	vpt->len = p - name;
	vpt->quote = T_BARE_WORD;

	/*
	 *	Copy over the attribute definition, now we're
	 *	sure what we were passed is valid.
	 */
	memcpy(&vpt->data.attribute, &attr, sizeof(vpt->data.attribute));
	if ((vpt->type == TMPL_TYPE_ATTR) && attr.da->flags.is_unknown) {
		vpt->tmpl_da = (fr_dict_attr_t *)&vpt->data.attribute.unknown.da;
	}

	VERIFY_TMPL(vpt);	/* Because we want to ensure we produced something sane */

	return vpt->len;
}

/** Parse a string into a TMPL_TYPE_ATTR_* or #TMPL_TYPE_LIST type #vp_tmpl_t
 *
 * @note Unlike #tmpl_from_attr_substr this function will error out if the entire
 *	name string isn't parsed.
 *
 * @copydetails tmpl_from_attr_substr
 */
ssize_t tmpl_from_attr_str(vp_tmpl_t *vpt, char const *name,
			   request_refs_t request_def, pair_lists_t list_def,
			   bool allow_unknown, bool allow_undefined)
{
	ssize_t slen;

	slen = tmpl_from_attr_substr(vpt, name, request_def, list_def, allow_unknown, allow_undefined);
	if (slen <= 0) return slen;
	if (name[slen] != '\0') {
		/* This looks wrong, but it produces meaningful errors for unknown attrs with tags */
		fr_strerror_printf("Unexpected text after %s", fr_int2str(tmpl_names, vpt->type, "<INVALID>"));
		return -slen;
	}

	VERIFY_TMPL(vpt);

	return slen;
}

/** Parse a string into a TMPL_TYPE_ATTR_* or #TMPL_TYPE_LIST type #vp_tmpl_t
 *
 * @param[in,out] ctx to allocate #vp_tmpl_t in.
 * @param[out] out Where to write pointer to new #vp_tmpl_t.
 * @param[in] name of attribute including #request_refs and #pair_lists qualifiers.
 *	If only #request_refs #pair_lists qualifiers are found, a #TMPL_TYPE_LIST
 *	#vp_tmpl_t will be produced.
 * @param[in] request_def The default #REQUEST to set if no #request_refs qualifiers are
 *	found in name.
 * @param[in] list_def The default list to set if no #pair_lists qualifiers are found in
 *	name.
 * @param[in] allow_unknown If true attributes in the format accepted by
 *	#fr_dict_unknown_from_suboid will be allowed, even if they're not in the main
 *	dictionaries.
 *	If an unknown attribute is found a #TMPL_TYPE_ATTR #vp_tmpl_t will be
 *	produced with the unknown #fr_dict_attr_t stored in the ``unknown.da`` buffer.
 *	This #fr_dict_attr_t will have its ``flags.is_unknown`` field set to true.
 *	If #tmpl_from_attr_substr is being called on startup, the #vp_tmpl_t may be
 *	passed to #tmpl_define_unknown_attr to add the unknown attribute to the main
 *	dictionary.
 *	If the unknown attribute is not added to the main dictionary the #vp_tmpl_t
 *	cannot be used to search for a #VALUE_PAIR in a #REQUEST.
 * @param[in] allow_undefined If true, we don't generate a parse error on unknown attributes.
 *	If an unknown attribute is found a #TMPL_TYPE_ATTR_UNDEFINED #vp_tmpl_t
 *	will be produced.
 * @return <= 0 on error (offset as negative integer), > 0 on success
 *	(number of bytes parsed).
 *
 * @see REMARKER to produce pretty error markers from the return value.
 */
ssize_t tmpl_afrom_attr_substr(TALLOC_CTX *ctx, vp_tmpl_t **out, char const *name,
			       request_refs_t request_def, pair_lists_t list_def,
			       bool allow_unknown, bool allow_undefined)
{
	ssize_t slen;
	vp_tmpl_t *vpt;

	MEM(vpt = talloc(ctx, vp_tmpl_t)); /* tmpl_from_attr_substr zeros it */

	slen = tmpl_from_attr_substr(vpt, name, request_def, list_def, allow_unknown, allow_undefined);
	if (slen <= 0) {
		TALLOC_FREE(vpt);
		return slen;
	}
	vpt->name = talloc_strndup(vpt, vpt->name, slen);

	VERIFY_TMPL(vpt);

	*out = vpt;

	return slen;
}

/** Parse a string into a TMPL_TYPE_ATTR_* or #TMPL_TYPE_LIST type #vp_tmpl_t
 *
 * @note Unlike #tmpl_afrom_attr_substr this function will error out if the entire
 *	name string isn't parsed.
 *
 * @copydetails tmpl_afrom_attr_substr
 */
ssize_t tmpl_afrom_attr_str(TALLOC_CTX *ctx, vp_tmpl_t **out, char const *name,
			    request_refs_t request_def, pair_lists_t list_def,
			    bool allow_unknown, bool allow_undefined)
{
	ssize_t slen;
	vp_tmpl_t *vpt;

	MEM(vpt = talloc(ctx, vp_tmpl_t)); /* tmpl_from_attr_substr zeros it */

	slen = tmpl_from_attr_substr(vpt, name, request_def, list_def, allow_unknown, allow_undefined);
	if (slen <= 0) {
		TALLOC_FREE(vpt);
		return slen;
	}
	if (name[slen] != '\0') {
		/* This looks wrong, but it produces meaningful errors for unknown attrs with tags */
		fr_strerror_printf("Unexpected text after %s", fr_int2str(tmpl_names, vpt->type, "<INVALID>"));
		TALLOC_FREE(vpt);
		return -slen;
	}
	vpt->name = talloc_strndup(vpt, vpt->name, vpt->len);

	VERIFY_TMPL(vpt);

	*out = vpt;

	return slen;
}

/** Convert an arbitrary string into a #vp_tmpl_t
 *
 * @note Unlike #tmpl_afrom_attr_str return code 0 doesn't necessarily indicate failure,
 *	may just mean a 0 length string was parsed.
 *
 * @note xlats and regexes are left uncompiled.  This is to support the two pass parsing
 *	done by the modcall code.  Compilation on pass1 of that code could fail, as
 *	attributes or xlat functions registered by modules may not be available (yet).
 *
 * @note For details of attribute parsing see #tmpl_from_attr_substr.
 *
 * @param[in,out] ctx To allocate #vp_tmpl_t in.
 * @param[out] out Where to write the pointer to the new #vp_tmpl_t.
 * @param[in] in String to convert to a #vp_tmpl_t.
 * @param[in] inlen length of string to convert.
 * @param[in] type of quoting around value. May be one of:
 *	- #T_BARE_WORD - If string begins with ``&`` produces #TMPL_TYPE_ATTR,
 *	  #TMPL_TYPE_ATTR_UNDEFINED, #TMPL_TYPE_LIST or error.
 *	  If string does not begin with ``&`` produces #TMPL_TYPE_UNPARSED,
 *	  #TMPL_TYPE_ATTR or #TMPL_TYPE_LIST.
 *	- #T_SINGLE_QUOTED_STRING - Produces #TMPL_TYPE_UNPARSED
 *	- #T_DOUBLE_QUOTED_STRING - Produces #TMPL_TYPE_XLAT or #TMPL_TYPE_UNPARSED (if
 *	  string doesn't contain ``%``).
 *	- #T_BACK_QUOTED_STRING - Produces #TMPL_TYPE_EXEC
 *	- #T_OP_REG_EQ - Produces #TMPL_TYPE_REGEX
 * @param[in] request_def The default #REQUEST to set if no #request_refs qualifiers are
 *	found in name.
 * @param[in] list_def The default list to set if no #pair_lists qualifiers are found in
 *	name.
 * @param[in] do_unescape whether or not we should do unescaping. Should be false if the
 *	caller already did it.
 * @return <= 0 on error (offset as negative integer), > 0 on success
 *	(number of bytes parsed).
 *	@see REMARKER to produce pretty error markers from the return value.
 *
 * @see tmpl_from_attr_substr
 */
ssize_t tmpl_afrom_str(TALLOC_CTX *ctx, vp_tmpl_t **out, char const *in, size_t inlen, FR_TOKEN type,
		       request_refs_t request_def, pair_lists_t list_def, bool do_unescape)
{
	bool do_xlat;
	char quote;
	char const *p;
	ssize_t slen;
	PW_TYPE data_type = PW_TYPE_STRING;
	vp_tmpl_t *vpt = NULL;
	value_data_t data;

	switch (type) {
	case T_BARE_WORD:
		/*
		 *  No attribute names start with 0x, and if they did, the user
		 *  can just use the explicit & prefix.
		 */
		if ((in[0] == '0') && (tolower(in[1]) == 'x')) {
			size_t binlen, len;

			/*
			 *  Hex strings must contain even number of characters
			 */
			if (inlen & 0x01) {
				fr_strerror_printf("Hex string not even length");
				return -inlen;
			}

			if (inlen <= 2) {
				fr_strerror_printf("Zero length hex string is invalid");
				return -inlen;
			}

			binlen = (inlen - 2) / 2;

			vpt = tmpl_alloc(ctx, TMPL_TYPE_DATA, in, inlen, type);
			vpt->tmpl_data_value.ptr = talloc_array(vpt, uint8_t, binlen);
			vpt->tmpl_data_length = binlen;
			vpt->tmpl_data_type = PW_TYPE_OCTETS;

			len = fr_hex2bin(vpt->tmpl_data_value.ptr, binlen, in + 2, inlen - 2);
			if (len != binlen) {
				fr_strerror_printf("Hex string contains none hex char");
				talloc_free(vpt);
				return -(len + 2);
			}
			slen = len;
			break;
		}

		/*
		 *	If we can parse it as an attribute, it's an attribute.
		 *	Otherwise, treat it as a literal.
		 */
		quote = '\0';

		slen = tmpl_afrom_attr_str(ctx, &vpt, in, request_def, list_def, true, (in[0] == '&'));
		if ((in[0] == '&') && (slen <= 0)) return slen;
		if (slen > 0) break;
		goto parse;

	case T_SINGLE_QUOTED_STRING:
		quote = '\'';

	parse:
		if (do_unescape) {
			if (value_data_from_str(ctx, &data, &data_type, NULL, in, inlen, quote) < 0) return 0;

			vpt = tmpl_alloc(ctx, TMPL_TYPE_UNPARSED, data.strvalue, talloc_array_length(data.strvalue) - 1, type);
			talloc_free(data.ptr);
		} else {
			vpt = tmpl_alloc(ctx, TMPL_TYPE_UNPARSED, in, inlen, type);
		}
		slen = vpt->len;
		break;

	case T_DOUBLE_QUOTED_STRING:
		do_xlat = false;

		p = in;
		while (*p) {
			if (do_unescape) { /* otherwise \ is just another character */
				if (*p == '\\') {
					if (!p[1]) break;
					p += 2;
					continue;
				}
			}

			if (*p == '%') {
				do_xlat = true;
				break;
			}

			p++;
		}

		/*
		 *	If the double quoted string needs to be
		 *	expanded at run time, make it an xlat
		 *	expansion.  Otherwise, convert it to be a
		 *	literal.
		 */
		if (do_unescape) {
			if (value_data_from_str(ctx, &data, &data_type, NULL, in,
						inlen, fr_token_quote[type]) < 0) return -1;
			if (do_xlat) {
				vpt = tmpl_alloc(ctx, TMPL_TYPE_XLAT, data.strvalue,
						 talloc_array_length(data.strvalue) - 1, type);
			} else {
				vpt = tmpl_alloc(ctx, TMPL_TYPE_UNPARSED, data.strvalue,
						 talloc_array_length(data.strvalue) - 1, type);
				vpt->quote = T_DOUBLE_QUOTED_STRING;
			}
			talloc_free(data.ptr);
		} else {
			if (do_xlat) {
				vpt = tmpl_alloc(ctx, TMPL_TYPE_XLAT, in, inlen, type);
			} else {
				vpt = tmpl_alloc(ctx, TMPL_TYPE_UNPARSED, in, inlen, type);
				vpt->quote = T_DOUBLE_QUOTED_STRING;
			}
		}
		slen = vpt->len;
		break;

	case T_BACK_QUOTED_STRING:
		if (do_unescape) {
			if (value_data_from_str(ctx, &data, &data_type, NULL, in,
						inlen, fr_token_quote[type]) < 0) return -1;

			vpt = tmpl_alloc(ctx, TMPL_TYPE_EXEC, data.strvalue, talloc_array_length(data.strvalue) - 1, type);
			talloc_free(data.ptr);
		} else {
			vpt = tmpl_alloc(ctx, TMPL_TYPE_EXEC, in, inlen, type);
		}
		slen = vpt->len;
		break;

	case T_OP_REG_EQ: /* hack */
		vpt = tmpl_alloc(ctx, TMPL_TYPE_REGEX, in, inlen, T_BARE_WORD);
		slen = vpt->len;
		break;

	default:
		rad_assert(0);
		return 0;	/* 0 is an error here too */
	}

	if (!vpt) return 0;

	vpt->quote = type;

	rad_assert(slen >= 0);

	VERIFY_TMPL(vpt);
	*out = vpt;

	return slen;
}
/* @} **/

/** @name Cast or convert #vp_tmpl_t
 *
 * #tmpl_cast_in_place can be used to convert #TMPL_TYPE_UNPARSED to a #TMPL_TYPE_DATA of a
 *  specified #PW_TYPE.
 *
 * #tmpl_cast_in_place_str does the same as #tmpl_cast_in_place, but will always convert to
 * #PW_TYPE #PW_TYPE_STRING.
 *
 * #tmpl_cast_to_vp does the same as #tmpl_cast_in_place, but outputs a #VALUE_PAIR.
 *
 * #tmpl_define_unknown_attr converts a #TMPL_TYPE_ATTR with an unknown #fr_dict_attr_t to a
 * #TMPL_TYPE_ATTR with a known #fr_dict_attr_t, by adding the unknown #fr_dict_attr_t to the main
 * dictionary, and updating the ``tmpl_da`` pointer.
 * @{
 */

/** Convert #vp_tmpl_t of type #TMPL_TYPE_UNPARSED or #TMPL_TYPE_DATA to #TMPL_TYPE_DATA of type specified
 *
 * @note Conversion is done in place.
 * @note Irrespective of whether the #vp_tmpl_t was #TMPL_TYPE_UNPARSED or #TMPL_TYPE_DATA,
 *	on successful cast it will be #TMPL_TYPE_DATA.
 *
 * @param[in,out] vpt The template to modify. Must be of type #TMPL_TYPE_UNPARSED
 *	or #TMPL_TYPE_DATA.
 * @param[in] type to cast to.
 * @param[in] enumv Enumerated dictionary values associated with a #fr_dict_attr_t.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int tmpl_cast_in_place(vp_tmpl_t *vpt, PW_TYPE type, fr_dict_attr_t const *enumv)
{
	VERIFY_TMPL(vpt);

	rad_assert(vpt != NULL);
	rad_assert((vpt->type == TMPL_TYPE_UNPARSED) || (vpt->type == TMPL_TYPE_DATA));

	switch (vpt->type) {
	case TMPL_TYPE_UNPARSED:
		vpt->tmpl_data_type = type;

		/*
		 *	Why do we pass a pointer to the tmpl type? Goddamn WiMAX.
		 */
		if (value_data_from_str(vpt, &vpt->tmpl_data_value, &vpt->tmpl_data_type,
					enumv, vpt->name, vpt->len, '\0') < 0) return -1;
		vpt->type = TMPL_TYPE_DATA;
		break;

	case TMPL_TYPE_DATA:
	{
		value_data_t new;

		if (type == vpt->tmpl_data_type) return 0;	/* noop */

		if (value_data_cast(vpt, &new, type, enumv, vpt->tmpl_data_type,
				    NULL, &vpt->tmpl_data_value) < 0) return -1;

		/*
		 *	Free old value buffers
		 */
		switch (vpt->tmpl_data_type) {
		case PW_TYPE_STRING:
		case PW_TYPE_OCTETS:
			talloc_free(vpt->tmpl_data_value.ptr);
			break;

		default:
			break;
		}

		memcpy(&vpt->tmpl_data_value, &new, sizeof(vpt->tmpl_data_value));
		vpt->tmpl_data_type = type;
	}
		break;

	default:
		rad_assert(0);
	}

	VERIFY_TMPL(vpt);

	return 0;
}

/** Convert #vp_tmpl_t of type #TMPL_TYPE_UNPARSED to #TMPL_TYPE_DATA of type #PW_TYPE_STRING
 *
 * @note Conversion is done in place.
 *
 * @param[in,out] vpt The template to modify. Must be of type #TMPL_TYPE_UNPARSED.
 */
void tmpl_cast_in_place_str(vp_tmpl_t *vpt)
{
	rad_assert(vpt != NULL);
	rad_assert(vpt->type == TMPL_TYPE_UNPARSED);

	vpt->tmpl_data.vp_strvalue = talloc_typed_strdup(vpt, vpt->name);
	rad_assert(vpt->tmpl_data.vp_strvalue != NULL);

	vpt->type = TMPL_TYPE_DATA;
	vpt->tmpl_data_type = PW_TYPE_STRING;
	vpt->tmpl_data_length = talloc_array_length(vpt->tmpl_data.vp_strvalue) - 1;
}

/** Expand a #vp_tmpl_t to a string, parse it as an attribute of type cast, create a #VALUE_PAIR from the result
 *
 * @note Like #tmpl_expand, but produces a #VALUE_PAIR.
 *
 * @param out Where to write pointer to the new #VALUE_PAIR.
 * @param request The current #REQUEST.
 * @param vpt to cast. Must be one of the following types:
 *	- #TMPL_TYPE_UNPARSED
 *	- #TMPL_TYPE_EXEC
 *	- #TMPL_TYPE_XLAT
 *	- #TMPL_TYPE_XLAT_STRUCT
 *	- #TMPL_TYPE_ATTR
 *	- #TMPL_TYPE_DATA
 * @param cast type of #VALUE_PAIR to create.
 * @return
 *	- 0 on success.
 *	- -1 on failure.
 */
int tmpl_cast_to_vp(VALUE_PAIR **out, REQUEST *request,
		    vp_tmpl_t const *vpt, fr_dict_attr_t const *cast)
{
	int rcode;
	VALUE_PAIR *vp;
	value_data_t data;
	char *p;

	VERIFY_TMPL(vpt);

	*out = NULL;

	vp = fr_pair_afrom_da(request, cast);
	if (!vp) return -1;

	if (vpt->type == TMPL_TYPE_DATA) {
		VERIFY_VP(vp);
		rad_assert(vp->da->type == vpt->tmpl_data_type);

		value_data_copy(vp, &vp->data, vpt->tmpl_data_type, &vpt->tmpl_data_value);
		*out = vp;
		return 0;
	}

	rcode = tmpl_aexpand(vp, &p, request, vpt, NULL, NULL);
	if (rcode < 0) {
		fr_pair_list_free(&vp);
		return rcode;
	}
	data.strvalue = p;

	/*
	 *	New escapes: strings are in binary form.
	 */
	if (vp->da->type == PW_TYPE_STRING) {
		vp->data.ptr = talloc_steal(vp, data.ptr);
		vp->vp_length = rcode;
	} else if (fr_pair_value_from_str(vp, data.strvalue, rcode) < 0) {
		talloc_free(data.ptr);
		fr_pair_list_free(&vp);
		return -1;
	}

	*out = vp;
	return 0;
}

/** Add an unknown #fr_dict_attr_t specified by a #vp_tmpl_t to the main dictionary
 *
 * @param vpt to add. ``tmpl_da`` pointer will be updated to point to the
 *	#fr_dict_attr_t inserted into the dictionary.
 * @return
 *	- 1 noop (did nothing) - Not possible to convert tmpl.
 *	- 0 on success.
 *	- -1 on failure.
 */
int tmpl_define_unknown_attr(vp_tmpl_t *vpt)
{
	fr_dict_attr_t const *da;

	if (!vpt) return 1;

	VERIFY_TMPL(vpt);

	if (vpt->type != TMPL_TYPE_ATTR) return 1;

	if (!vpt->tmpl_da->flags.is_unknown) return 1;

	da = fr_dict_unknown_add(NULL, vpt->tmpl_da);
	if (!da) return -1;
	vpt->tmpl_da = da;

	return 0;
}

/** Add an undefined #fr_dict_attr_t specified by a #vp_tmpl_t to the main dictionary
 *
 * @note fr_dict_attr_add will not return an error if the attribute already exists
 *	meaning that multiple #vp_tmpl_t specifying the same attribute can be
 *	passed to this function to be fixed up, so long as the type and flags
 *	are identical.
 *
 * @param vpt specifying undefined attribute to add. ``tmpl_da`` pointer will be
 *	updated to point to the #fr_dict_attr_t inserted into the dictionary.
 *	Lists and requests will be preserved.
 * @param type to define undefined attribute as.
 * @param flags to define undefined attribute with.
 * @return
 *	- 1 noop (did nothing) - Not possible to convert tmpl.
 *	- 0 on success.
 *	- -1 on failure.
 */
int tmpl_define_undefined_attr(vp_tmpl_t *vpt, PW_TYPE type, fr_dict_attr_flags_t const *flags)
{
	fr_dict_attr_t const *da;

	if (!vpt) return -1;

	VERIFY_TMPL(vpt);

	if (vpt->type != TMPL_TYPE_ATTR_UNDEFINED) return 1;

	if (fr_dict_attr_add(NULL, fr_dict_root(fr_dict_internal), vpt->tmpl_unknown_name, -1, type, *flags) < 0) {
		return -1;
	}
	da = fr_dict_attr_by_name(NULL, vpt->tmpl_unknown_name);
	if (!da) return -1;

	if (type != da->type) {
		fr_strerror_printf("Attribute %s of type %s already defined with type %s",
				   da->name, fr_int2str(dict_attr_types, type, "<UNKNOWN>"),
				   fr_int2str(dict_attr_types, da->type, "<UNKNOWN>"));
		return -1;
	}

	if (memcmp(flags, &da->flags, sizeof(*flags)) != 0) {
		fr_strerror_printf("Attribute %s already defined with different flags", da->name);
		return -1;
	}

#ifndef NDEBUG
	/*
	 *	Clear existing data (so we don't trip TMPL_VERIFY);
	 */
	memset(&vpt->data.attribute.unknown, 0, sizeof(vpt->data.attribute.unknown));
#endif

	vpt->tmpl_da = da;
	vpt->type = TMPL_TYPE_ATTR;

	return 0;
}
/* @} **/

/** @name Resolve a #vp_tmpl_t outputting the result in various formats
 *
 * @{
 */

/** Expand a #vp_tmpl_t to a string writing the result to a buffer
 *
 * The intended use of #tmpl_expand and #tmpl_aexpand is for modules to easily convert a #vp_tmpl_t
 * provided by the conf parser, into a usable value.
 * The value returned should be raw and undoctored for #PW_TYPE_STRING and #PW_TYPE_OCTETS types,
 * and the printable (string) version of the data for all others.
 *
 * Depending what arguments are passed, either copies the value to buff, or writes a pointer
 * to a string buffer to out. This allows the most efficient access to the value resolved by
 * the #vp_tmpl_t, avoiding unecessary string copies.
 *
 * @note This function is used where raw string values are needed, which may mean the string
 *	returned may be binary data or contain unprintable chars. #fr_snprint or #fr_asprint should
 *	be used before using these values in debug statements. #is_printable can be used to check
 *	if the string only contains printable chars.
 *
 * @param out Where to write a pointer to the string buffer. On return may point to buff if
 *	buff was used to store the value. Otherwise will point to a #value_data_t buffer,
 *	or the name of the template. To force copying to buff, out should be NULL.
 * @param buff Expansion buffer, may be NULL if out is not NULL, and processing #TMPL_TYPE_UNPARSED
 *	or string types.
 * @param bufflen Length of expansion buffer.
 * @param request Current request.
 * @param vpt to expand. Must be one of the following types:
 *	- #TMPL_TYPE_UNPARSED
 *	- #TMPL_TYPE_EXEC
 *	- #TMPL_TYPE_XLAT
 *	- #TMPL_TYPE_XLAT_STRUCT
 *	- #TMPL_TYPE_ATTR
 *	- #TMPL_TYPE_DATA
 * @param escape xlat escape function (only used for xlat types).
 * @param escape_ctx xlat escape function data.
 * @return
 *	- -1 on failure.
 *	- The length of data written to buff, or pointed to by out.
 */
ssize_t tmpl_expand(char const **out, char *buff, size_t bufflen, REQUEST *request,
		    vp_tmpl_t const *vpt, xlat_escape_t escape, void *escape_ctx)
{
	VALUE_PAIR *vp;
	ssize_t slen = -1;	/* quiet compiler */

	VERIFY_TMPL(vpt);

	rad_assert(vpt->type != TMPL_TYPE_LIST);

	if (out) *out = NULL;

	switch (vpt->type) {
	case TMPL_TYPE_UNPARSED:
		RDEBUG4("EXPAND TMPL LITERAL");

		if (!out) {
			rad_assert(buff);
			memcpy(buff, vpt->name, vpt->len >= bufflen ? bufflen : vpt->len + 1);
		} else {
			*out = vpt->name;
		}
		return vpt->len;

	case TMPL_TYPE_EXEC:
	{
		RDEBUG4("EXPAND TMPL EXEC");
		rad_assert(buff);
		if (radius_exec_program(request, buff, bufflen, NULL, request, vpt->name, NULL,
					true, false, EXEC_TIMEOUT) != 0) {
			return -1;
		}
		slen = strlen(buff);
		if (out) *out = buff;
	}
		break;

	case TMPL_TYPE_XLAT:
		RDEBUG4("EXPAND TMPL XLAT");
		rad_assert(buff);
		/* Error in expansion, this is distinct from zero length expansion */
		slen = radius_xlat(buff, bufflen, request, vpt->name, escape, escape_ctx);
		if (slen < 0) return slen;
		if (out) *out = buff;
		break;

	case TMPL_TYPE_XLAT_STRUCT:
		RDEBUG4("EXPAND TMPL XLAT STRUCT");
		rad_assert(buff);
		/* Error in expansion, this is distinct from zero length expansion */
		slen = radius_xlat_struct(buff, bufflen, request, vpt->tmpl_xlat, escape, escape_ctx);
		if (slen < 0) {
			return slen;
		}
		slen = strlen(buff);
		if (out) *out = buff;
		break;

	case TMPL_TYPE_ATTR:
	{
		int ret;

		RDEBUG4("EXPAND TMPL ATTR");
		rad_assert(buff);
		ret = tmpl_find_vp(&vp, request, vpt);
		if (ret < 0) return -2;

		if (out && ((vp->da->type == PW_TYPE_STRING) || (vp->da->type == PW_TYPE_OCTETS))) {
			*out = vp->data.ptr;
			slen = vp->vp_length;
		} else {
			if (out) *out = buff;
			slen = fr_pair_value_snprint(buff, bufflen, vp, '\0');
		}
	}
		break;

	case TMPL_TYPE_DATA:
	{
		RDEBUG4("EXPAND TMPL DATA");

		if (out && ((vpt->tmpl_data_type == PW_TYPE_STRING) || (vpt->tmpl_data_type == PW_TYPE_OCTETS))) {
			*out = vpt->tmpl_data_value.ptr;
			slen = vpt->tmpl_data_length;
		} else {
			if (out) *out = buff;
			/**
			 *  @todo tmpl_expand should accept an enumv da from the lhs of the map.
			 */
			slen = value_data_snprint(buff, bufflen, vpt->tmpl_data_type, NULL, &vpt->tmpl_data_value, '\0');
		}
	}
		break;

	/*
	 *	We should never be expanding these.
	 */
	case TMPL_TYPE_UNKNOWN:
	case TMPL_TYPE_NULL:
	case TMPL_TYPE_LIST:
	case TMPL_TYPE_REGEX:
	case TMPL_TYPE_ATTR_UNDEFINED:
	case TMPL_TYPE_REGEX_STRUCT:
		rad_assert(0 == 1);
		slen = -1;
		break;
	}

	if (slen < 0) return slen;


#if 0
	/*
	 *	If we're doing correct escapes, we may have to re-parse the string.
	 *	If the string is from another expansion, it needs re-parsing.
	 *	Or, if it's from a "string" attribute, it needs re-parsing.
	 *	Integers, IP addresses, etc. don't need re-parsing.
	 */
	if (vpt->type != TMPL_TYPE_ATTR) {
	     	value_data_t	vd;
	     	int		ret;

		PW_TYPE type = PW_TYPE_STRING;

		ret = value_data_from_str(ctx, &vd, &type, NULL, *out, slen, '"');
		talloc_free(*out);	/* free the old value */
		if (ret < 0) return -1;
		*out = vd.ptr;
		slen = vd.length;
	}
#endif

	if (vpt->type == TMPL_TYPE_XLAT_STRUCT) {
		RDEBUG2("EXPAND %s", vpt->name); /* xlat_struct doesn't do this */
		RDEBUG2("   --> %s", buff);
	}

	return slen;
}

/** Expand a template to a string, allocing a new buffer to hold the string
 *
 * The intended use of #tmpl_expand and #tmpl_aexpand is for modules to easily convert a #vp_tmpl_t
 * provided by the conf parser, into a usable value.
 * The value returned should be raw and undoctored for #PW_TYPE_STRING and #PW_TYPE_OCTETS types,
 * and the printable (string) version of the data for all others.
 *
 * This function will always duplicate values, whereas #tmpl_expand may return a pointer to an
 * existing buffer.
 *
 * @note This function is used where raw string values are needed, which may mean the string
 *	returned may be binary data or contain unprintable chars. #fr_snprint or #fr_asprint should
 *	be used before using these values in debug statements. #is_printable can be used to check
 *	if the string only contains printable chars.
 *
 * @note The type (char or uint8_t) can be obtained with talloc_get_type, and may be used as a
 *	hint as to how to process or print the data.
 *
 * @param ctx		to allocate new buffer in.
 * @param out		Where to write pointer to the new buffer.
 * @param request	Current request.
 * @param vpt		to expand. Must be one of the following types:
 *			- #TMPL_TYPE_UNPARSED
 *			- #TMPL_TYPE_EXEC
 *			- #TMPL_TYPE_XLAT
 *			- #TMPL_TYPE_XLAT_STRUCT
 *			- #TMPL_TYPE_ATTR
 *			- #TMPL_TYPE_DATA
 * @param escape xlat	escape function (only used for TMPL_TYPE_XLAT_* types).
 * @param escape_ctx	xlat escape function data (only used for TMPL_TYPE_XLAT_* types).
 * @param dst_type	- PW_TYPE_* matching out pointer.  @see tmpl_aexpand.
 * @return
 *	- -1 on failure.
 *	- The length of data written to buff, or pointed to by out.
 */
ssize_t _tmpl_to_atype(TALLOC_CTX *ctx, void *out,
		       REQUEST *request,
		       vp_tmpl_t const *vpt,
		       xlat_escape_t escape, void *escape_ctx,
		       PW_TYPE dst_type)
{
	value_data_t const	*to_cast = NULL;
	value_data_t		from_cast;

	VALUE_PAIR		*vp = NULL;
	value_data_t		vd;
	PW_TYPE			src_type = PW_TYPE_STRING;
	bool			needs_dup = false;

	ssize_t			slen = -1;	/* quiet compiler */
	int			ret;

	VERIFY_TMPL(vpt);

	memset(&vd, 0, sizeof(vd));

	switch (vpt->type) {
	case TMPL_TYPE_UNPARSED:
		RDEBUG4("EXPAND TMPL UNPARSED");

		vd.length = vpt->len;
		vd.strvalue = vpt->name;
		to_cast = &vd;
		break;

	case TMPL_TYPE_EXEC:
		RDEBUG4("EXPAND TMPL EXEC");

		MEM(vd.strvalue = talloc_array(ctx, char, 1024));
		if (radius_exec_program(request, (char *)vd.ptr, 1024, NULL, request, vpt->name, NULL,
					true, false, EXEC_TIMEOUT) != 0) {
			TALLOC_FREE(vd.ptr);
			return -1;
		}
		vd.length = strlen(vd.strvalue);
		MEM(vd.strvalue = talloc_realloc(ctx, vd.ptr, char, vd.length + 1));	/* Trim */
		rad_assert(vd.strvalue[vd.length] == '\0');
		to_cast = &vd;
		break;

	case TMPL_TYPE_XLAT:
	{
		value_data_t	tmp;

		RDEBUG4("EXPAND TMPL XLAT");

		/* Error in expansion, this is distinct from zero length expansion */
		slen = radius_axlat((char **)&vd.ptr, request, vpt->name, escape, escape_ctx);
		if (slen < 0) return slen;
		vd.length = slen;

		/*
		 *	Undo any of the escaping that was done by the
		 *	xlat expansion function.
		 *
		 *	@fixme We need a way of signalling xlat not to escape things.
		 */
		ret = value_data_from_str(ctx, &tmp, &src_type, NULL, vd.strvalue, vd.length, '"');
		talloc_free(vd.ptr);	/* free the old value */
		if (ret < 0) return -1;

		vd.strvalue = tmp.strvalue;
		vd.length = tmp.length;
		to_cast = &vd;
	}
		break;

	case TMPL_TYPE_XLAT_STRUCT:
	{
		value_data_t	tmp;

		RDEBUG4("EXPAND TMPL XLAT STRUCT");
		RDEBUG2("EXPAND %s", vpt->name); /* xlat_struct doesn't do this */

		/* Error in expansion, this is distinct from zero length expansion */
		slen = radius_axlat_struct((char **)&vd.ptr, request, vpt->tmpl_xlat, escape, escape_ctx);
		if (slen < 0) return slen;

		vd.length = slen;

		/*
		 *	Undo any of the escaping that was done by the
		 *	xlat expansion function.
		 *
		 *	@fixme We need a way of signalling xlat not to escape things.
		 */
		ret = value_data_from_str(ctx, &tmp, &src_type, NULL, vd.strvalue, vd.length, '"');
		talloc_free(vd.ptr);	/* free the old value */
		if (ret < 0) return -1;

		vd.strvalue = tmp.strvalue;
		vd.length = tmp.length;
		to_cast = &vd;

		RDEBUG2("   --> %s", vd.strvalue);	/* Print post-unescaping */
	}
		break;

	case TMPL_TYPE_ATTR:
		RDEBUG4("EXPAND TMPL ATTR");

		ret = tmpl_find_vp(&vp, request, vpt);
		if (ret < 0) return -2;

		rad_assert(vp);

		to_cast = &vp->data;
		src_type = vp->da->type;

		switch (src_type) {
		case PW_TYPE_STRING:
		case PW_TYPE_OCTETS:
			rad_assert(to_cast->ptr);
			needs_dup = true;
			break;

		default:
			break;
		}
		break;

	case TMPL_TYPE_DATA:
	{
		RDEBUG4("EXPAND TMPL DATA");

		to_cast = &vpt->tmpl_data_value;
		src_type = vpt->tmpl_data_type;

		switch (src_type) {
		case PW_TYPE_STRING:
		case PW_TYPE_OCTETS:
			rad_assert(to_cast->ptr);
			needs_dup = true;
			break;

		default:
			break;
		}
	}
		break;

	/*
	 *	We should never be expanding these.
	 */
	case TMPL_TYPE_UNKNOWN:
	case TMPL_TYPE_NULL:
	case TMPL_TYPE_LIST:
	case TMPL_TYPE_REGEX:
	case TMPL_TYPE_ATTR_UNDEFINED:
	case TMPL_TYPE_REGEX_STRUCT:
		rad_assert(0);
		talloc_free(vd.ptr);
		return -1;
	}

	/*
	 *	Don't dup the buffers unless we need to.
	 */
	if ((src_type != dst_type) || needs_dup) {
		ret = value_data_cast(ctx, &from_cast, dst_type, NULL, src_type, vp ? vp->da : NULL, to_cast);

		switch (src_type) {
		case PW_TYPE_OCTETS:
		case PW_TYPE_STRING:
			talloc_free(vd.ptr);
			break;

		default:
			break;
		}

		if (ret < 0) return -1;
	} else {
		memcpy(&from_cast, to_cast, sizeof(from_cast));
	}

	RDEBUG4("Copying %zu bytes to %p from offset %zu",
		value_data_field_sizes[src_type], *((void **)out), value_data_offsets[src_type]);

	memcpy(out, &from_cast + value_data_offsets[src_type], value_data_field_sizes[src_type]);

	return from_cast.length;
}

/** Print a #vp_tmpl_t to a string
 *
 * @param[out] out Where to write the presentation format #vp_tmpl_t string.
 * @param[in] outlen Size of output buffer.
 * @param[in] vpt to print.
 * @param[in] values Used for #TMPL_TYPE_DATA only. #fr_dict_attr_t to use when mapping integer
 *	values to strings.
 * @return
 *	- The number of bytes written to the out buffer.
 *	- A number >= outlen if truncation has occurred.
 */
size_t tmpl_snprint(char *out, size_t outlen, vp_tmpl_t const *vpt, fr_dict_attr_t const *values)
{
	size_t		len;
	char const	*p;
	char		c;
	char		*out_p = out, *end = out_p + outlen;

	if (!vpt || (outlen < 3)) {
	empty:
		*out = '\0';
		return 0;
	}
	VERIFY_TMPL(vpt);

	out[outlen - 1] = '\0';	/* Always terminate for safety */

	switch (vpt->type) {
	case TMPL_TYPE_LIST:
		*out_p++ = '&';

		/*
		 *	Don't add &current.
		 */
		if (vpt->tmpl_request == REQUEST_CURRENT) {
			len = snprintf(out_p, end - out_p, "%s:", fr_int2str(pair_lists, vpt->tmpl_list, ""));
			RETURN_IF_TRUNCATED(out_p, len, end - out_p);
			goto inst_and_tag;
		}

		len = snprintf(out_p, end - out_p, "%s.%s:", fr_int2str(request_refs, vpt->tmpl_request, ""),
			       fr_int2str(pair_lists, vpt->tmpl_list, ""));
		RETURN_IF_TRUNCATED(out_p, len, end - out_p);
		goto inst_and_tag;

	case TMPL_TYPE_ATTR_UNDEFINED:
	case TMPL_TYPE_ATTR:
		*out_p++ = '&';

		p = vpt->type == TMPL_TYPE_ATTR ? vpt->tmpl_da->name : vpt->tmpl_unknown_name;

		/*
		 *	Don't add &current.
		 */
		if (vpt->tmpl_request == REQUEST_CURRENT) {
			if (vpt->tmpl_list == PAIR_LIST_REQUEST) {
				len = strlcpy(out_p, p, end - out_p);
				RETURN_IF_TRUNCATED(out_p, len, end - out_p);
				goto inst_and_tag;
			}

			/*
			 *	Don't add &request:
			 */
			len = snprintf(out_p, end - out_p, "%s:%s",
				       fr_int2str(pair_lists, vpt->tmpl_list, ""), p);
			RETURN_IF_TRUNCATED(out_p, len, end - out_p);
			goto inst_and_tag;
		}

		len = snprintf(out_p, end - out_p, "%s.%s:%s", fr_int2str(request_refs, vpt->tmpl_request, ""),
			       fr_int2str(pair_lists, vpt->tmpl_list, ""), p);
		RETURN_IF_TRUNCATED(out_p, len, end - out_p);

	inst_and_tag:
		if (vpt->tmpl_tag != TAG_ANY) {
			len = snprintf(out_p, end - out_p, ":%d", vpt->tmpl_tag);
			RETURN_IF_TRUNCATED(out_p, len, end - out_p);
		}

		switch (vpt->tmpl_num) {
		case NUM_ANY:
			goto finish;

		case NUM_ALL:
			len = snprintf(out_p, end - out_p, "[*]");
			break;

		case NUM_COUNT:
			len = snprintf(out_p, end - out_p, "[#]");
			break;

		case NUM_LAST:
			len = snprintf(out_p, end - out_p, "[n]");
			break;

		default:
			len = snprintf(out_p, end - out_p, "[%i]", vpt->tmpl_num);
			break;
		}
		RETURN_IF_TRUNCATED(out_p, len, end - out_p);
		goto finish;

	/*
	 *	Regexes have their own set of escaping rules
	 */
	case TMPL_TYPE_REGEX:
	case TMPL_TYPE_REGEX_STRUCT:
		if (outlen < 4) goto empty;	/* / + <c> + / + \0 */
		*out_p++ = '/';
		len = fr_snprint(out_p, (end - out_p) - 1, vpt->name, vpt->len, '\0');
		RETURN_IF_TRUNCATED(out_p, len, (end - out_p) - 1);
		*out_p++ = '/';
		goto finish;

	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_XLAT_STRUCT:
		c = '"';
		goto do_literal;

	case TMPL_TYPE_EXEC:
		c = '`';
		goto do_literal;

	case TMPL_TYPE_UNPARSED:
		/*
		 *	Nasty nasty hack that needs to be fixed.
		 *
		 *	Determines what quoting to use around strings based on their content.
		 *	Should use vpt->quote, but that's not always set correctly
		 *	at the moment.
		 */
		for (p = vpt->name; *p != '\0'; p++) {
			if (*p == ' ') break;
			if (*p == '\'') break;
			if (!fr_dict_attr_allowed_chars[(int) *p]) break;
		}
		c = *p ? '"' : '\0';

do_literal:
		if (outlen < 4) goto empty;	/* / + <c> + / + \0 */
		if (c != '\0') *out_p++ = c;
		len = fr_snprint(out_p, (end - out_p) - ((c == '\0') ? 0 : 1), vpt->name, vpt->len, c);
		RETURN_IF_TRUNCATED(out_p, len, (end - out_p) - ((c == '\0') ? 0 : 1));
		if (c != '\0') *out_p++ = c;
		break;

	case TMPL_TYPE_DATA:
		return value_data_snprint(out, outlen, vpt->tmpl_data_type, values, &vpt->tmpl_data_value,
					 fr_token_quote[vpt->quote]);

	default:
		goto empty;
	}

finish:
	*out_p = '\0';
	return (out_p - out);
}

/** Initialise a #vp_cursor_t to the #VALUE_PAIR specified by a #vp_tmpl_t
 *
 * This makes iterating over the one or more #VALUE_PAIR specified by a #vp_tmpl_t
 * significantly easier.
 *
 * @param err May be NULL if no error code is required. Will be set to:
 *	- 0 on success.
 *	- -1 if no matching #VALUE_PAIR could be found.
 *	- -2 if list could not be found (doesn't exist in current #REQUEST).
 *	- -3 if context could not be found (no parent #REQUEST available).
 * @param cursor to store iterator state.
 * @param request The current #REQUEST.
 * @param vpt specifying the #VALUE_PAIR type/tag or list to iterate over.
 * @return
 *	- First #VALUE_PAIR specified by the #vp_tmpl_t.
 *	- NULL if no matching #VALUE_PAIR found, and NULL on error.
 *
 * @see tmpl_cursor_next
 */
VALUE_PAIR *tmpl_cursor_init(int *err, vp_cursor_t *cursor, REQUEST *request, vp_tmpl_t const *vpt)
{
	VALUE_PAIR **vps, *vp = NULL;
	int num;

	VERIFY_TMPL(vpt);

	rad_assert((vpt->type == TMPL_TYPE_ATTR) || (vpt->type == TMPL_TYPE_LIST));

	if (err) *err = 0;

	if (radius_request(&request, vpt->tmpl_request) < 0) {
		if (err) *err = -3;
		return NULL;
	}
	vps = radius_list(request, vpt->tmpl_list);
	if (!vps) {
		if (err) *err = -2;
		return NULL;
	}
	(void) fr_cursor_init(cursor, vps);

	switch (vpt->type) {
	/*
	 *	May not may not be found, but it *is* a known name.
	 */
	case TMPL_TYPE_ATTR:
		switch (vpt->tmpl_num) {
		case NUM_ANY:
			vp = fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag);
			if (!vp) {
				if (err) *err = -1;
				return NULL;
			}
			VERIFY_VP(vp);
			return vp;

		/*
		 *	Get the last instance of a VALUE_PAIR.
		 */
		case NUM_LAST:
		{
			VALUE_PAIR *last = NULL;

			while ((vp = fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag))) {
				VERIFY_VP(vp);
				last = vp;
			}
			VERIFY_VP(last);
			if (!last) break;
			return last;
		}

		/*
		 *	Callers expect NUM_COUNT to setup the cursor to point
		 *	to the first attribute in the list we're meant to be
		 *	counting.
		 *
		 *	It does not produce a virtual attribute containing the
		 *	total number of attributes.
		 */
		case NUM_COUNT:
			return fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag);

		default:
			num = vpt->tmpl_num;
			while ((vp = fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag))) {
				VERIFY_VP(vp);
				if (num-- <= 0) return vp;
			}
			break;
		}

		if (err) *err = -1;
		return NULL;

	case TMPL_TYPE_LIST:
		switch (vpt->tmpl_num) {
		case NUM_COUNT:
		case NUM_ANY:
		case NUM_ALL:
			vp = fr_cursor_init(cursor, vps);
			if (!vp) {
				if (err) *err = -1;
				return NULL;
			}
			VERIFY_VP(vp);
			return vp;

		/*
		 *	Get the last instance of a VALUE_PAIR.
		 */
		case NUM_LAST:
		{
			VALUE_PAIR *last = NULL;

			for (vp = fr_cursor_init(cursor, vps);
			     vp;
			     vp = fr_cursor_next(cursor)) {
				VERIFY_VP(vp);
				last = vp;
			}
			if (!last) break;
			VERIFY_VP(last);
			return last;
		}

		default:
			num = vpt->tmpl_num;
			for (vp = fr_cursor_init(cursor, vps);
			     vp;
			     vp = fr_cursor_next(cursor)) {
				VERIFY_VP(vp);
				if (num-- <= 0) return vp;
			}
			break;
		}

		break;

	default:
		rad_assert(0);
	}

	return vp;
}

/** Returns the next #VALUE_PAIR specified by vpt
 *
 * @param cursor initialised with #tmpl_cursor_init.
 * @param vpt specifying the #VALUE_PAIR type/tag to iterate over.
 *	Must be one of the following types:
 *	- #TMPL_TYPE_LIST
 *	- #TMPL_TYPE_ATTR
 * @return
 *	- The next #VALUE_PAIR matching the #vp_tmpl_t.
 *	- NULL if no more matching #VALUE_PAIR of the specified type/tag are found.
 */
VALUE_PAIR *tmpl_cursor_next(vp_cursor_t *cursor, vp_tmpl_t const *vpt)
{
	rad_assert((vpt->type == TMPL_TYPE_ATTR) || (vpt->type == TMPL_TYPE_LIST));

	VERIFY_TMPL(vpt);

	switch (vpt->type) {
	/*
	 *	May not may not be found, but it *is* a known name.
	 */
	case TMPL_TYPE_ATTR:
		switch (vpt->tmpl_num) {
		default:
			return NULL;

		case NUM_ALL:
		case NUM_COUNT:	/* This cursor is being used to count matching attrs */
			break;
		}
		return fr_cursor_next_by_da(cursor, vpt->tmpl_da, vpt->tmpl_tag);

	case TMPL_TYPE_LIST:
		switch (vpt->tmpl_num) {
		default:
			return NULL;

		case NUM_ALL:
		case NUM_COUNT:	/* This cursor is being used to count matching attrs */
			break;
		}
		return fr_cursor_next(cursor);

	default:
		rad_assert(0);
		return NULL;	/* Older versions of GCC flag the lack of return as an error */
	}
}

/** Copy pairs matching a #vp_tmpl_t in the current #REQUEST
 *
 * @param ctx to allocate new #VALUE_PAIR in.
 * @param out Where to write the copied #VALUE_PAIR (s).
 * @param request The current #REQUEST.
 * @param vpt specifying the #VALUE_PAIR type/tag or list to copy.
 *	Must be one of the following types:
 *	- #TMPL_TYPE_LIST
 *	- #TMPL_TYPE_ATTR
 * @return
 *	- -1 if no matching #VALUE_PAIR could be found.
 *	- -2 if list could not be found (doesn't exist in current #REQUEST).
 *	- -3 if context could not be found (no parent #REQUEST available).
 *	- -4 on memory allocation error.
 */
int tmpl_copy_vps(TALLOC_CTX *ctx, VALUE_PAIR **out, REQUEST *request, vp_tmpl_t const *vpt)
{
	VALUE_PAIR *vp;
	vp_cursor_t from, to;

	VERIFY_TMPL(vpt);

	int err;

	rad_assert((vpt->type == TMPL_TYPE_ATTR) || (vpt->type == TMPL_TYPE_LIST));

	*out = NULL;

	fr_cursor_init(&to, out);

	for (vp = tmpl_cursor_init(&err, &from, request, vpt);
	     vp;
	     vp = tmpl_cursor_next(&from, vpt)) {
		vp = fr_pair_copy(ctx, vp);
		if (!vp) {
			fr_pair_list_free(out);
			return -4;
		}
		fr_cursor_append(&to, vp);
	}

	return err;
}

/** Returns the first VP matching a #vp_tmpl_t
 *
 * @param[out] out where to write the retrieved vp.
 * @param[in] request The current #REQUEST.
 * @param[in] vpt specifying the #VALUE_PAIR type/tag to find.
 *	Must be one of the following types:
 *	- #TMPL_TYPE_LIST
 *	- #TMPL_TYPE_ATTR
 * @return
 *	- 0 on success (found matching #VALUE_PAIR).
 *	- -1 if no matching #VALUE_PAIR could be found.
 *	- -2 if list could not be found (doesn't exist in current #REQUEST).
 *	- -3 if context could not be found (no parent #REQUEST available).
 */
int tmpl_find_vp(VALUE_PAIR **out, REQUEST *request, vp_tmpl_t const *vpt)
{
	vp_cursor_t cursor;
	VALUE_PAIR *vp;

	VERIFY_TMPL(vpt);

	int err;

	vp = tmpl_cursor_init(&err, &cursor, request, vpt);
	if (out) *out = vp;

	return err;
}

/** Returns the first VP matching a #vp_tmpl_t, or if no VPs match, creates a new one.
 *
 * @param[out] out where to write the retrieved or created vp.
 * @param[in] request The current #REQUEST.
 * @param[in] vpt specifying the #VALUE_PAIR type/tag to retrieve or create.  Must be #TMPL_TYPE_ATTR.
 * @return
 *	- 1 on success a pair was created.
 *	- 0 on success a pair was found.
 *	- -1 if a new #VALUE_PAIR couldn't be found or created.
 *	- -2 if list could not be found (doesn't exist in current #REQUEST).
 *	- -3 if context could not be found (no parent #REQUEST available).
 */
int tmpl_find_or_add_vp(VALUE_PAIR **out, REQUEST *request, vp_tmpl_t const *vpt)
{
	vp_cursor_t	cursor;
	VALUE_PAIR	*vp;
	int		err;

	VERIFY_TMPL(vpt);
	rad_assert(vpt->type == TMPL_TYPE_ATTR);

	*out = NULL;

	vp = tmpl_cursor_init(&err, &cursor, request, vpt);
	switch (err) {
	case 0:
		*out = vp;
		return 0;

	case -1:
	{
		TALLOC_CTX	*ctx;
		VALUE_PAIR	**head;

		RADIUS_LIST_AND_CTX(ctx, head, request, vpt->tmpl_request, vpt->tmpl_list);

		vp = fr_pair_afrom_da(ctx, vpt->tmpl_da);
		if (!vp) {
			REDEBUG("Failed allocating attribute %s", vpt->tmpl_da->name);
			return -1;
		}
		*out = vp;
	}
		return 0;

	default:
		return err;
	}
}
/* @} **/

#ifdef WITH_VERIFY_PTR
/** Used to check whether areas of a vp_tmpl_t are zeroed out
 *
 * @param ptr Offset to begin checking at.
 * @param len How many bytes to check.
 * @return
 *	- Pointer to the first non-zero byte.
 *	- NULL if all bytes were zero.
 */
static uint8_t const *not_zeroed(uint8_t const *ptr, size_t len)
{
	size_t i;

	for (i = 0; i < len; i++) {
		if (ptr[i] != 0x00) return ptr + i;
	}

	return NULL;
}
#define CHECK_ZEROED(_x) not_zeroed((uint8_t const *)&_x + sizeof(_x), sizeof(vpt->data) - sizeof(_x))

/** Verify fields of a vp_tmpl_t make sense
 *
 * @note If the #vp_tmpl_t is invalid, causes the server to exit.
 *
 * @param file obtained with __FILE__.
 * @param line obtained with __LINE__.
 * @param vpt to check.
 */
void tmpl_verify(char const *file, int line, vp_tmpl_t const *vpt)
{
	rad_assert(vpt);

	if (vpt->type == TMPL_TYPE_UNKNOWN) {
		FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: vp_tmpl_t type was "
			     "TMPL_TYPE_UNKNOWN (uninitialised)", file, line);
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	if (vpt->type > TMPL_TYPE_NULL) {
		FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: vp_tmpl_t type was %i "
			     "(outside range of tmpl_names)", file, line, vpt->type);
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	if (!vpt->name && (vpt->quote != T_INVALID)) {
		char quote = vpt->quote > T_TOKEN_LAST ? '?' : fr_token_quote[vpt->quote];

		FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: Quote type '%c' (%i) was set for NULL name",
			     file, line, quote, vpt->quote);
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	if (vpt->name && (vpt->quote == T_INVALID)) {
		FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: No quoting type was set for name \"%.*s\"",
			     file, line, (int)vpt->len, vpt->name);
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}

	/*
	 *  Do a memcmp of the bytes after where the space allocated for
	 *  the union member should have ended and the end of the union.
	 *  These should always be zero if the union has been initialised
	 *  properly.
	 *
	 *  If they're still all zero, do TMPL_TYPE specific checks.
	 */
	switch (vpt->type) {
	case TMPL_TYPE_NULL:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_NULL "
				     "has non-zero bytes in its data union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_UNPARSED:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_UNPARSED "
				     "has non-zero bytes in its data union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_XLAT:
	case TMPL_TYPE_XLAT_STRUCT:
		break;

/* @todo When regexes get converted to xlat the flags field of the regex union is used
	case TMPL_TYPE_XLAT:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_XLAT "
				     "has non-zero bytes in its data union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_XLAT_STRUCT:
		if (CHECK_ZEROED(vpt->data.xlat)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_XLAT_STRUCT "
				     "has non-zero bytes after the data.xlat pointer in the union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;
*/

	case TMPL_TYPE_EXEC:
		if (not_zeroed((uint8_t const *)&vpt->data, sizeof(vpt->data))) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_EXEC "
				     "has non-zero bytes in its data union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_ATTR_UNDEFINED:
		rad_assert(vpt->tmpl_da == NULL);
		break;

	case TMPL_TYPE_ATTR:
		if (CHECK_ZEROED(vpt->data.attribute)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
				     "has non-zero bytes after the data.attribute struct in the union",
				     file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if (vpt->tmpl_da->flags.is_unknown) {
			if (vpt->tmpl_da != (fr_dict_attr_t const *)&vpt->data.attribute.unknown.da) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
					     "da is marked as unknown, but does not point to the template's "
					     "unknown da buffer", file, line);
				if (!fr_cond_assert(0)) fr_exit_now(1);
			}

		} else {
			fr_dict_attr_t const *da;

			/*
			 *	Attribute may be present with multiple names
			 */
			da = fr_dict_attr_by_name(NULL, vpt->tmpl_da->name);
			if (!da) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
					     "attribute \"%s\" (%s) not found in global dictionary",
					     file, line, vpt->tmpl_da->name,
					     fr_int2str(dict_attr_types, vpt->tmpl_da->type, "<INVALID>"));
				if (!fr_cond_assert(0)) fr_exit_now(1);
			}

			if ((da->type == PW_TYPE_COMBO_IP_ADDR) && (da->type != vpt->tmpl_da->type)) {
				da = fr_dict_attr_by_type(NULL, vpt->tmpl_da->vendor,
							  vpt->tmpl_da->attr, vpt->tmpl_da->type);
				if (!da) {
					FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
						     "attribute \"%s\" variant (%s) not found in global dictionary",
						     file, line, vpt->tmpl_da->name,
						     fr_int2str(dict_attr_types, vpt->tmpl_da->type, "<INVALID>"));
					if (!fr_cond_assert(0)) fr_exit_now(1);
				}
			}

			if (da != vpt->tmpl_da) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_ATTR "
					     "dictionary pointer %p \"%s\" (%s) "
					     "and global dictionary pointer %p \"%s\" (%s) differ",
					     file, line,
					     vpt->tmpl_da, vpt->tmpl_da->name,
					     fr_int2str(dict_attr_types, vpt->tmpl_da->type, "<INVALID>"),
					     da, da->name,
					     fr_int2str(dict_attr_types, da->type, "<INVALID>"));
				if (!fr_cond_assert(0)) fr_exit_now(1);
			}
		}
		break;

	case TMPL_TYPE_LIST:
		if (CHECK_ZEROED(vpt->data.attribute)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_LIST"
				     "has non-zero bytes after the data.attribute struct in the union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if (vpt->tmpl_da != NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_LIST da pointer was NULL", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_DATA:
		if (CHECK_ZEROED(vpt->data.literal)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA "
				     "has non-zero bytes after the data.literal struct in the union",
				     file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if (vpt->tmpl_data_type == PW_TYPE_INVALID) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA type was "
				     "PW_TYPE_INVALID (uninitialised)", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if (vpt->tmpl_data_type >= PW_TYPE_MAX) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA type was "
				     "%i (outside the range of PW_TYPEs)", file, line, vpt->tmpl_data_type);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		/*
		 *	Unlike VALUE_PAIRs we can't guarantee that VALUE_PAIR_TMPL buffers will
		 *	be talloced. They may be allocated on the stack or in global variables.
		 */
		switch (vpt->tmpl_data_type) {
		case PW_TYPE_STRING:
			if (vpt->tmpl_data.vp_strvalue[vpt->tmpl_data_length] != '\0') {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA char buffer not \\0 "
					     "terminated", file, line);
				if (!fr_cond_assert(0)) fr_exit_now(1);
			}
			break;

		case PW_TYPE_TLV:
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA is of type TLV",
				     file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);

		case PW_TYPE_OCTETS:
			break;

		default:
			if (vpt->tmpl_data_length == 0) {
				FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_DATA data pointer not NULL "
				             "but len field is zero", file, line);
				if (!fr_cond_assert(0)) fr_exit_now(1);
			}
		}

		break;

	case TMPL_TYPE_REGEX:
		/*
		 *	iflag field is used for non compiled regexes too.
		 */
		if (CHECK_ZEROED(vpt->data.preg)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "has non-zero bytes after the data.preg struct in the union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if (vpt->tmpl_preg != NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "preg field was not NULL", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if ((vpt->tmpl_iflag != true) && (vpt->tmpl_iflag != false)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "iflag field was neither true or false", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if ((vpt->tmpl_mflag != true) && (vpt->tmpl_mflag != false)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "mflag field was neither true or false", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		break;

	case TMPL_TYPE_REGEX_STRUCT:
		if (CHECK_ZEROED(vpt->data.preg)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX_STRUCT "
				     "has non-zero bytes after the data.preg struct in the union", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if (vpt->tmpl_preg == NULL) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX_STRUCT "
				     "comp field was NULL", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if ((vpt->tmpl_iflag != true) && (vpt->tmpl_iflag != false)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX_STRUCT "
				     "iflag field was neither true or false", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}

		if ((vpt->tmpl_mflag != true) && (vpt->tmpl_mflag != false)) {
			FR_FAULT_LOG("CONSISTENCY CHECK FAILED %s[%u]: TMPL_TYPE_REGEX "
				     "mflag field was neither true or false", file, line);
			if (!fr_cond_assert(0)) fr_exit_now(1);
		}
		break;

	case TMPL_TYPE_UNKNOWN:
		if (!fr_cond_assert(0)) fr_exit_now(1);
	}
}
#endif
