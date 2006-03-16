/*
 * proxy.c	Proxy stuff.
 *
 * Version:	$Id$
 *
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
 *
 * Copyright 2000  The FreeRADIUS server project
 * Copyright 2000  Miquel van Smoorenburg <miquels@cistron.nl>
 * Copyright 2000  Chris Parker <cparker@starnetusa.com>
 */

static const char rcsid[] = "$Id$";

#include <freeradius-devel/autoconf.h>

#include <sys/socket.h>

#ifdef HAVE_NETINET_IN_H
#	include <netinet/in.h>
#endif

#include <stdio.h>
#include <stdlib.h>
#include <ctype.h>
#include <string.h>

#include <freeradius-devel/radiusd.h>
#include <freeradius-devel/rad_assert.h>
#include <freeradius-devel/modules.h>
#include <freeradius-devel/request_list.h>

/*
 *	We received a response from a remote radius server.
 *	Call the post-proxy modules.
 */
int proxy_receive(REQUEST *request)
{
        int rcode;
	int post_proxy_type = 0;
	VALUE_PAIR *vp;

        /*
         *	Delete any reply we had accumulated until now.
	 */
        pairfree(&request->reply->vps);

	/*
	 *	Run the packet through the post-proxy stage,
	 *	BEFORE playing games with the attributes.
	 */
	vp = pairfind(request->config_items, PW_POST_PROXY_TYPE);
	if (vp) {
		DEBUG2("  Found Post-Proxy-Type %s", vp->vp_strvalue);
		post_proxy_type = vp->lvalue;
	}
	rcode = module_post_proxy(post_proxy_type, request);

        /*
         *	Delete the Proxy-State Attributes from the reply.
         *	These include Proxy-State attributes from us and
         *	remote server.
	 */
        pairdelete(&request->proxy_reply->vps, PW_PROXY_STATE);

	/*
	 *	Add the attributes left in the proxy reply to
	 *	the reply list.
	 */
        pairadd(&request->reply->vps, request->proxy_reply->vps);
        request->proxy_reply->vps = NULL;

        /*
	 *	Free proxy request pairs.
         */
        pairfree(&request->proxy->vps);

	/*
	 *	FIXME: If the packet is an Access-Challenge,
	 *	THEN add it to a cache, which does:
	 *
	 *	(src IP, State) -> (home server ip/port)
	 *
	 *	This allows the load-balancing code to
	 *	work for EAP...
	 *
	 *	Alternately, we can delete the State from the home
	 *	server, and use our own..  that might be better.
	 */

        return rcode;
}

/*
 *	Add a proxy-pair to the end of the request.
 */
static void proxy_addinfo(REQUEST *request)
{
	VALUE_PAIR *proxy_pair;

	proxy_pair = paircreate(PW_PROXY_STATE, PW_TYPE_STRING);
	if (proxy_pair == NULL) {
		radlog(L_ERR|L_CONS, "no memory");
		exit(1);
	}
	sprintf(proxy_pair->vp_strvalue, "%d", request->packet->id);
	proxy_pair->length = strlen(proxy_pair->vp_strvalue);

	pairadd(&request->proxy->vps, proxy_pair);
}


/*
 *	Like realm find, but does load balancing, and we don't
 *	wake up any sleeping realms.  Someone should already have
 *	done that.
 *
 *	It also does NOT do fail-over to default if the realms are dead,
 *	as that decision has already been made.
 */
static REALM *proxy_realm_ldb(REQUEST *request, const char *realm_name,
			      int accounting)
{
	REALM		*cl, *lb;
	uint32_t	count;

	/*
	 *	FIXME: If the packet contains a State attribute,
	 *	AND the realm is load-balance,
	 *	AND there is a matching
	 *	State attribute in the cached entry, THEN proxy it to
	 *	that realm.
	 */

	lb = NULL;
	count = 0;
	for (cl = mainconfig.realms; cl; cl = cl->next) {
		/*
		 *	Wake up any sleeping realm.
		 *
		 *	Note that the 'realm find' function will only
		 *	wake up the FIRST realm which matches.  We've
		 *	got to wake up ALL of the matching realms.
		 */
		if (cl->wakeup <= request->timestamp) {
			cl->active = TRUE;
		}
		if (cl->acct_wakeup <= request->timestamp) {
			cl->acct_active = TRUE;
		}

		/*
		 *	Asked for auth/acct, and the auth/acct server
		 *	is not active.  Skip it.
		 */
		if ((!accounting && !cl->active) ||
		    (accounting && !cl->acct_active)) {
			continue;
		}

		/*
		 *	The realm name doesn't match, skip it.
		 */
		if (strcasecmp(cl->realm, realm_name) != 0) {
			continue;
		}

		/*
		 *	Fail-over, pick the first one that matches.
		 */
		if ((count == 0) && /* if size > 0, we have round-robin */
		    (cl->ldflag == 0)) {
			return cl;
		}

		/*
		 *	We're doing load-balancing.  Pick a random
		 *	number, which will be used to determine which
		 *	home server is chosen.
		 */
		if (!lb) {
			lb = cl;
			count = 1;
			continue;
		}

		/*
		 *	Keep track of how many load balancing servers
		 *	we've gone through.
		 */
		count++;

		/*
		 *	See the "camel book" for why this works.
		 *
		 *	If (rand(0..n) < 1), pick the current realm.
		 *	We add a scale factor of 65536, to avoid
		 *	floating point.
		 */
		if ((count * (lrad_rand() & 0xffff)) < (uint32_t) 0x10000) {
			lb = cl;
		}
	} /* loop over the realms */

	/*
	 *	Return the load-balanced realm.
	 */
	return lb;
}

/*
 *	Relay the request to a remote server.
 *	Returns:
 *
 *      RLM_MODULE_FAIL: we don't reply, caller returns without replying
 *      RLM_MODULE_NOOP: caller falls through to normal processing
 *      RLM_MODULE_HANDLED  : we reply, caller returns without replying
 */
int proxy_send(REQUEST *request)
{
	int rcode;
	int pre_proxy_type = 0;
	VALUE_PAIR *realmpair;
	VALUE_PAIR *strippedname;
	VALUE_PAIR *vp;
	REALM *realm;
	char *realmname;

	/*
	 *	Not authentication or accounting.  Stop it.
	 */
	if ((request->packet->code != PW_AUTHENTICATION_REQUEST) &&
	    (request->packet->code != PW_ACCOUNTING_REQUEST)) {
		DEBUG2("  ERROR: Cannot proxy packets of type %d",
		       request->packet->code);
		return RLM_MODULE_FAIL;
	}

	/*
	 *	The timestamp is used below to figure the
	 *	next_try. The request needs to "hang around" until
	 *	either the other server sends a reply or the retry
	 *	count has been exceeded.  Until then, it should not
	 *	be eligible for the time-based cleanup.  --Pac. */

	realmpair = pairfind(request->config_items, PW_PROXY_TO_REALM);
	if (!realmpair) {
		/*
		 *	Not proxying, so we can exit from the proxy
		 *	code.
		 */
		return RLM_MODULE_NOOP;
	}

	/*
	 *	If the server has already decided to reject the request,
	 *	then don't try to proxy it.
	 */
	if (request->reply->code == PW_AUTHENTICATION_REJECT) {
		DEBUG2("Cancelling proxy as request was already rejected");
		return RLM_MODULE_REJECT;
	}
	if (((vp = pairfind(request->config_items, PW_AUTH_TYPE)) != NULL) &&
	    (vp->lvalue == PW_AUTHTYPE_REJECT)) {
		DEBUG2("Cancelling proxy as request was already rejected");
		return RLM_MODULE_REJECT;
	}
	/*
	 *	Length == 0 means it exists, but there's no realm.
	 *	Don't proxy it.
	 */
	if (realmpair->length == 0) {
		return RLM_MODULE_NOOP;
	}

	realmname = (char *)realmpair->vp_strvalue;

	/*
	 *	Look for the realm, using the load balancing
	 *	version of realm find.
	 */
	realm = proxy_realm_ldb(request, realmname,
				(request->packet->code == PW_ACCOUNTING_REQUEST));
	if (realm == NULL) {
		DEBUG2("  ERROR: Failed to find live home server for realm %s",
		       realmname);
		return RLM_MODULE_FAIL;
	}

	/*
	 *	Remember that we sent the request to a Realm.
	 */
	pairadd(&request->packet->vps,
		pairmake("Realm", realm->realm, T_OP_EQ));

	/*
	 *	Access-Request: look for LOCAL realm.
	 *	Accounting-Request: look for LOCAL realm.
	 */
	if (((request->packet->code == PW_AUTHENTICATION_REQUEST) &&
	     (realm->ipaddr.af == AF_INET) &&
	     (realm->ipaddr.ipaddr.ip4addr.s_addr == htonl(INADDR_NONE))) ||
	    ((request->packet->code == PW_ACCOUNTING_REQUEST) &&
	     (realm->acct_ipaddr.af == AF_INET) &&
	     (realm->acct_ipaddr.ipaddr.ip4addr.s_addr == htonl(INADDR_NONE)))) {
		DEBUG2(" WARNING: Cancelling proxy to Realm %s, as the realm is local.",
		       realm->realm);
		return RLM_MODULE_NOOP;
	}

	/*
	 *	This is mainly for radrelay.  Don't proxy packets back
	 *	to servers which sent them to us.
	 */
	if ((request->packet->code == PW_ACCOUNTING_REQUEST) &&
	    (request->listener->type == RAD_LISTEN_DETAIL) &&
	    (realm->acct_ipaddr.af == AF_INET) &&
	    (request->packet->src_ipaddr.af == AF_INET) &&
	    (realm->acct_ipaddr.ipaddr.ip4addr.s_addr == request->packet->src_ipaddr.ipaddr.ip4addr.s_addr)) {
		DEBUG2("    rlm_realm: Packet came from realm %s, proxy cancelled", realm->realm);
		return RLM_MODULE_NOOP;
	}

	/*
	 *	Allocate the proxy packet, only if it wasn't already
	 *	allocated by a module.  This check is mainly to support
	 *	the proxying of EAP-TTLS and EAP-PEAP tunneled requests.
	 *
	 *	In those cases, the EAP module creates a "fake"
	 *	request, and recursively passes it through the
	 *	authentication stage of the server.  The module then
	 *	checks if the request was supposed to be proxied, and
	 *	if so, creates a proxy packet from the TUNNELED request,
	 *	and not from the EAP request outside of the tunnel.
	 *
	 *	The proxy then works like normal, except that the response
	 *	packet is "eaten" by the EAP module, and encapsulated into
	 *	an EAP packet.
	 */
	if (!request->proxy) {
		/*
		 *	Now build a new RADIUS_PACKET.
		 *
		 *	FIXME: it could be that the id wraps around
		 *	too fast if we have a lot of requests, it
		 *	might be better to keep a seperate ID value
		 *	per remote server.
		 *
		 *	OTOH the remote radius server should be smart
		 *	enough to compare _both_ ID and vector.
		 *	Right?
		 */
		if ((request->proxy = rad_alloc(TRUE)) == NULL) {
			radlog(L_ERR|L_CONS, "no memory");
			exit(1);
		}

		/*
		 *	We now massage the attributes to be proxied...
		 */

		/*
		 *	Copy the request, then look up name and
		 *	plain-text password in the copy.
		 *
		 *	Note that the User-Name attribute is the
		 *	*original* as sent over by the client.  The
		 *	Stripped-User-Name attribute is the one hacked
		 *	through the 'hints' file.
		 */
		request->proxy->vps =  paircopy(request->packet->vps);
	}

	/*
	 *	Strip the name, if told to.
	 *
	 *	Doing it here catches the case of proxied tunneled
	 *	requests.
	 */
	if (realm->striprealm == TRUE &&
	   (strippedname = pairfind(request->proxy->vps, PW_STRIPPED_USER_NAME)) != NULL) {
		/*
		 *	If there's a Stripped-User-Name attribute in
		 *	the request, then use THAT as the User-Name
		 *	for the proxied request, instead of the
		 *	original name.
		 *
		 *	This is done by making a copy of the
		 *	Stripped-User-Name attribute, turning it into
		 *	a User-Name attribute, deleting the
		 *	Stripped-User-Name and User-Name attributes
		 *	from the vps list, and making the new
		 *	User-Name the head of the vps list.
		 */
		vp = pairfind(request->proxy->vps, PW_USER_NAME);
		if (!vp) {
			vp = paircreate(PW_USER_NAME, PW_TYPE_STRING);
			if (!vp) {
				radlog(L_ERR|L_CONS, "no memory");
				exit(1);
			}
			vp->next = request->proxy->vps;
			request->proxy->vps = vp;
		}
		memcpy(vp->vp_strvalue, strippedname->vp_strvalue,
		       sizeof(vp->vp_strvalue));
		vp->length = strippedname->length;

		/*
		 *	Do NOT delete Stripped-User-Name.
		 */
	}
	
	/*
	 *	If there is no PW_CHAP_CHALLENGE attribute but
	 *	there is a PW_CHAP_PASSWORD we need to add it
	 *	since we can't use the request authenticator
	 *	anymore - we changed it.
	 */
	if (pairfind(request->proxy->vps, PW_CHAP_PASSWORD) &&
	    pairfind(request->proxy->vps, PW_CHAP_CHALLENGE) == NULL) {
		vp = paircreate(PW_CHAP_CHALLENGE, PW_TYPE_STRING);
		if (!vp) {
			radlog(L_ERR|L_CONS, "no memory");
			exit(1);
		}
		vp->length = AUTH_VECTOR_LEN;
		memcpy(vp->vp_strvalue, request->packet->vector, AUTH_VECTOR_LEN);
		pairadd(&(request->proxy->vps), vp);
	}

	request->proxy->code = request->packet->code;
	if (request->packet->code == PW_AUTHENTICATION_REQUEST) {
		request->proxy->dst_port = realm->auth_port;
		request->proxy->dst_ipaddr = realm->ipaddr;
	} else if (request->packet->code == PW_ACCOUNTING_REQUEST) {
		request->proxy->dst_port = realm->acct_port;
		request->proxy->dst_ipaddr = realm->acct_ipaddr;
	}

	/*
	 *	Add PROXY_STATE attribute, before pre-proxy stage,
	 *	so the pre-proxy modules have access to it.
	 *
	 *	Note that, at this point, the proxied request HAS NOT
	 *	been assigned a RADIUS Id.
	 */
	proxy_addinfo(request);

	/*
	 *	Set up for sending the request.
	 */
	memcpy(request->proxysecret, realm->secret, sizeof(request->proxysecret));
	request->proxy_try_count = mainconfig.proxy_retry_count - 1;

	vp = NULL;
	if (request->packet->code == PW_ACCOUNTING_REQUEST) {
		vp = pairfind(request->proxy->vps, PW_ACCT_DELAY_TIME);
	}
	if (vp) {
		request->proxy->timestamp = request->timestamp - vp->lvalue;
	} else {
		request->proxy->timestamp = request->timestamp;
	}
	request->proxy_start_time = request->timestamp;

	/*
	 *	Do pre-proxying.
	 */
	vp = pairfind(request->config_items, PW_PRE_PROXY_TYPE);
	if (vp) {
		DEBUG2("  Found Pre-Proxy-Type %s", vp->vp_strvalue);
		pre_proxy_type = vp->lvalue;
	}
	rcode = module_pre_proxy(pre_proxy_type, request);
	switch (rcode) {
	/*
	 *	Only proxy the packet if the pre-proxy code succeeded.
	 */
	case RLM_MODULE_NOOP:
	case RLM_MODULE_OK:
	case RLM_MODULE_UPDATED:
		/*
		 *	Delay sending the proxy packet until after we've
		 *	done the work above, playing with the request.
		 *
		 *	After this point, it becomes dangerous to play with
		 *	the request data structure, as the reply MAY come in
		 *	and get processed before we're done with it here.
		 */
		request->options |= RAD_REQUEST_OPTION_PROXIED;

		/*
		 *	If it's a fake request, don't send the proxy
		 *	packet.  The outer tunnel session will take
		 *	care of doing that.
		 */
		if ((request->options & RAD_REQUEST_OPTION_FAKE_REQUEST) == 0) {
			/*
			 *	Add the proxied request to the
			 *	list of outstanding proxied
			 *	requests, BEFORE we send it, so
			 *	we have fewer problems with race
			 *	conditions when the responses come
			 *	back very quickly.
			 */
			if (!rl_add_proxy(request)) {
				DEBUG("ERROR: Failed to proxy request %d",
				      request->number);
				return RLM_MODULE_FAIL; /* caller doesn't reply */
			}

			request->proxy_listener->send(request->proxy_listener,
						      request);
		}
		rcode = RLM_MODULE_HANDLED; /* caller doesn't reply */
		break;
	/*
	 *	The module handled the request, don't reply.
	 */
	case RLM_MODULE_HANDLED:
		break;
	/*
	 *	Neither proxy, nor reply to invalid requests.
	 */
	case RLM_MODULE_FAIL:
	case RLM_MODULE_INVALID:
	case RLM_MODULE_NOTFOUND:
	case RLM_MODULE_REJECT:
	case RLM_MODULE_USERLOCK:
	default:
		rcode = RLM_MODULE_FAIL; /* caller doesn't reply */
		break;
	}

	/*
	 *	Do NOT free request->proxy->vps, the pairs are needed
	 *	for the retries!
	 */
	return rcode;
}
