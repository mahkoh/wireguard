// SPDX-License-Identifier: GPL-2.0
/*
 * Copyright (C) 2015-2018 Jason A. Donenfeld <Jason@zx2c4.com>. All Rights Reserved.
 */

#include "netlink.h"
#include "device.h"
#include "peer.h"
#include "socket.h"
#include "queueing.h"
#include "messages.h"
#include "uapi/wireguard.h"
#include <linux/if.h>
#include <net/genetlink.h>
#include <net/sock.h>

static struct genl_family genl_family;

static const struct nla_policy device_policy[WGDEVICE_A_MAX + 1] = {
	[WGDEVICE_A_IFINDEX]		= { .type = NLA_U32 },
	[WGDEVICE_A_IFNAME]		= { .type = NLA_NUL_STRING, .len = IFNAMSIZ - 1 },
	[WGDEVICE_A_PRIVATE_KEY]	= { .len = NOISE_PUBLIC_KEY_LEN },
	[WGDEVICE_A_PUBLIC_KEY]		= { .len = NOISE_PUBLIC_KEY_LEN },
	[WGDEVICE_A_FLAGS]		= { .type = NLA_U32 },
	[WGDEVICE_A_LISTEN_PORT]	= { .type = NLA_U16 },
	[WGDEVICE_A_FWMARK]		= { .type = NLA_U32 },
	[WGDEVICE_A_PEERS]		= { .type = NLA_NESTED },
	[WGDEVICE_A_DEV_NETNS_PID]	= { .type = NLA_U32 },
	[WGDEVICE_A_DEV_NETNS_FD]	= { .type = NLA_U32 },
	[WGDEVICE_A_TRANSIT_NETNS_PID]	= { .type = NLA_U32 },
	[WGDEVICE_A_TRANSIT_NETNS_FD]	= { .type = NLA_U32 },
};

static const struct nla_policy peer_policy[WGPEER_A_MAX + 1] = {
	[WGPEER_A_PUBLIC_KEY]				= { .len = NOISE_PUBLIC_KEY_LEN },
	[WGPEER_A_PRESHARED_KEY]			= { .len = NOISE_SYMMETRIC_KEY_LEN },
	[WGPEER_A_FLAGS]				= { .type = NLA_U32 },
	[WGPEER_A_ENDPOINT]				= { .len = sizeof(struct sockaddr) },
	[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]	= { .type = NLA_U16 },
	[WGPEER_A_LAST_HANDSHAKE_TIME]			= { .len = sizeof(struct timespec) },
	[WGPEER_A_RX_BYTES]				= { .type = NLA_U64 },
	[WGPEER_A_TX_BYTES]				= { .type = NLA_U64 },
	[WGPEER_A_ALLOWEDIPS]				= { .type = NLA_NESTED },
	[WGPEER_A_PROTOCOL_VERSION]			= { .type = NLA_U32 }
};

static const struct nla_policy allowedip_policy[WGALLOWEDIP_A_MAX + 1] = {
	[WGALLOWEDIP_A_FAMILY]		= { .type = NLA_U16 },
	[WGALLOWEDIP_A_IPADDR]		= { .len = sizeof(struct in_addr) },
	[WGALLOWEDIP_A_CIDR_MASK]	= { .type = NLA_U8 }
};

static struct wireguard_device *lookup_interface(struct nlattr **attrs,
						 struct net *net)
{
	struct net_device *dev = NULL;

	if (!attrs[WGDEVICE_A_IFINDEX] == !attrs[WGDEVICE_A_IFNAME])
		return ERR_PTR(-EBADR);
	if (attrs[WGDEVICE_A_IFINDEX])
		dev = dev_get_by_index(net,
				       nla_get_u32(attrs[WGDEVICE_A_IFINDEX]));
	else if (attrs[WGDEVICE_A_IFNAME])
		dev = dev_get_by_name(net,
				      nla_data(attrs[WGDEVICE_A_IFNAME]));
	if (!dev)
		return ERR_PTR(-ENODEV);
	if (!dev->rtnl_link_ops || !dev->rtnl_link_ops->kind ||
	    strcmp(dev->rtnl_link_ops->kind, KBUILD_MODNAME)) {
		dev_put(dev);
		return ERR_PTR(-EOPNOTSUPP);
	}
	return netdev_priv(dev);
}

struct allowedips_ctx {
	struct sk_buff *skb;
	unsigned int i;
};

static int get_allowedips(void *ctx, const u8 *ip, u8 cidr, int family)
{
	struct allowedips_ctx *actx = ctx;
	struct nlattr *allowedip_nest;

	allowedip_nest = nla_nest_start(actx->skb, actx->i++);
	if (!allowedip_nest)
		return -EMSGSIZE;

	if (nla_put_u8(actx->skb, WGALLOWEDIP_A_CIDR_MASK, cidr) ||
	    nla_put_u16(actx->skb, WGALLOWEDIP_A_FAMILY, family) ||
	    nla_put(actx->skb, WGALLOWEDIP_A_IPADDR, family == AF_INET6 ?
		    sizeof(struct in6_addr) : sizeof(struct in_addr), ip)) {
		nla_nest_cancel(actx->skb, allowedip_nest);
		return -EMSGSIZE;
	}

	nla_nest_end(actx->skb, allowedip_nest);
	return 0;
}

static int get_peer(struct wireguard_peer *peer, unsigned int index,
		    struct allowedips_cursor *rt_cursor, struct sk_buff *skb)
{
	struct nlattr *allowedips_nest, *peer_nest = nla_nest_start(skb, index);
	struct allowedips_ctx ctx = { .skb = skb };
	bool fail;

	if (!peer_nest)
		return -EMSGSIZE;

	down_read(&peer->handshake.lock);
	fail = nla_put(skb, WGPEER_A_PUBLIC_KEY, NOISE_PUBLIC_KEY_LEN,
		       peer->handshake.remote_static);
	up_read(&peer->handshake.lock);
	if (fail)
		goto err;

	if (!rt_cursor->seq) {
		down_read(&peer->handshake.lock);
		fail = nla_put(skb, WGPEER_A_PRESHARED_KEY,
			       NOISE_SYMMETRIC_KEY_LEN,
			       peer->handshake.preshared_key);
		up_read(&peer->handshake.lock);
		if (fail)
			goto err;

		if (nla_put(skb, WGPEER_A_LAST_HANDSHAKE_TIME,
			    sizeof(peer->walltime_last_handshake),
			    &peer->walltime_last_handshake) ||
		    nla_put_u16(skb, WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL,
				peer->persistent_keepalive_interval) ||
		    nla_put_u64_64bit(skb, WGPEER_A_TX_BYTES, peer->tx_bytes,
				      WGPEER_A_UNSPEC) ||
		    nla_put_u64_64bit(skb, WGPEER_A_RX_BYTES, peer->rx_bytes,
				      WGPEER_A_UNSPEC) ||
		    nla_put_u32(skb, WGPEER_A_PROTOCOL_VERSION, 1))
			goto err;

		read_lock_bh(&peer->endpoint_lock);
		if (peer->endpoint.addr.sa_family == AF_INET)
			fail = nla_put(skb, WGPEER_A_ENDPOINT,
				       sizeof(peer->endpoint.addr4),
				       &peer->endpoint.addr4);
		else if (peer->endpoint.addr.sa_family == AF_INET6)
			fail = nla_put(skb, WGPEER_A_ENDPOINT,
				       sizeof(peer->endpoint.addr6),
				       &peer->endpoint.addr6);
		read_unlock_bh(&peer->endpoint_lock);
		if (fail)
			goto err;
	}

	allowedips_nest = nla_nest_start(skb, WGPEER_A_ALLOWEDIPS);
	if (!allowedips_nest)
		goto err;
	if (wg_allowedips_walk_by_peer(&peer->device->peer_allowedips,
				       rt_cursor, peer, get_allowedips, &ctx,
				       &peer->device->device_update_lock)) {
		nla_nest_end(skb, allowedips_nest);
		nla_nest_end(skb, peer_nest);
		return -EMSGSIZE;
	}
	memset(rt_cursor, 0, sizeof(*rt_cursor));
	nla_nest_end(skb, allowedips_nest);
	nla_nest_end(skb, peer_nest);
	return 0;
err:
	nla_nest_cancel(skb, peer_nest);
	return -EMSGSIZE;
}

static struct net *get_attr_net(struct nlattr *net_pid, struct nlattr *net_fd)
{
	if (net_pid && net_fd)
		return ERR_PTR(-EINVAL);
	if (net_pid)
		return get_net_ns_by_pid(nla_get_u32(net_pid));
	if (net_fd)
		return get_net_ns_by_fd(nla_get_u32(net_fd));
	return NULL;
}

static int test_socket_net_capable(struct net *net)
{
	if (net != current->nsproxy->net_ns &&
			!ns_capable(net->user_ns, CAP_NET_ADMIN))
		return -EPERM;
	return 0;
}

static int get_device_start(struct netlink_callback *cb)
{
	struct nlattr **attrs = genl_family_attrbuf(&genl_family);
	struct net *owned_dev_net = NULL, *dev_net;
	struct allowedips_cursor *cursor = NULL;
	struct wireguard_device *wg;
	int ret;

	ret = nlmsg_parse(cb->nlh, GENL_HDRLEN + genl_family.hdrsize, attrs,
			  genl_family.maxattr, device_policy, NULL);
	if (ret < 0)
		return ret;

	owned_dev_net = get_attr_net(attrs[WGDEVICE_A_DEV_NETNS_PID],
			attrs[WGDEVICE_A_DEV_NETNS_FD]);
	if (IS_ERR(owned_dev_net)) {
		ret = PTR_ERR(owned_dev_net);
		owned_dev_net = NULL;
		goto out;
	}
	dev_net = owned_dev_net ? : sock_net(cb->skb->sk);
	if (!netlink_ns_capable(cb->skb, dev_net->user_ns, CAP_NET_ADMIN)) {
		ret = -EPERM;
		goto out;
	}

	cursor = kzalloc(sizeof(*cursor), GFP_KERNEL);
	if (unlikely(!cursor)) {
		ret = -ENOMEM;
		goto out;
	}

	wg = lookup_interface(attrs, dev_net);
	if (IS_ERR(wg)) {
		ret = PTR_ERR(wg);
		goto out;
	}

	cb->args[0] = (long)wg;
	cb->args[2] = (long)cursor;
	cursor = NULL;

out:
	if (cursor)
		kfree(cursor);
	if (owned_dev_net)
		put_net(owned_dev_net);
	return ret;
}

static int get_device_dump(struct sk_buff *skb, struct netlink_callback *cb)
{
	struct wireguard_peer *peer, *next_peer_cursor, *last_peer_cursor;
	struct allowedips_cursor *rt_cursor;
	struct wireguard_device *wg;
	unsigned int peer_idx = 0;
	struct nlattr *peers_nest;
	int ret = -EMSGSIZE;
	bool done = true;
	void *hdr;

	wg = (struct wireguard_device *)cb->args[0];
	next_peer_cursor = (struct wireguard_peer *)cb->args[1];
	last_peer_cursor = (struct wireguard_peer *)cb->args[1];
	rt_cursor = (struct allowedips_cursor *)cb->args[2];

	rtnl_lock();
	mutex_lock(&wg->device_update_lock);
	cb->seq = wg->device_update_gen;

	hdr = genlmsg_put(skb, NETLINK_CB(cb->skb).portid, cb->nlh->nlmsg_seq,
			  &genl_family, NLM_F_MULTI, WG_CMD_GET_DEVICE);
	if (!hdr)
		goto out;
	genl_dump_check_consistent(cb, hdr);

	if (!last_peer_cursor) {
		if (test_socket_net_capable(wg->transit_net) == 0) {
			if (nla_put_u16(skb, WGDEVICE_A_LISTEN_PORT,
						wg->incoming_port))
				goto out;
		}
		if (nla_put_u32(skb, WGDEVICE_A_FWMARK, wg->fwmark) ||
		    nla_put_u32(skb, WGDEVICE_A_IFINDEX, wg->dev->ifindex) ||
		    nla_put_string(skb, WGDEVICE_A_IFNAME, wg->dev->name))
			goto out;

		down_read(&wg->static_identity.lock);
		if (wg->static_identity.has_identity) {
			if (nla_put(skb, WGDEVICE_A_PRIVATE_KEY,
				    NOISE_PUBLIC_KEY_LEN,
				    wg->static_identity.static_private) ||
			    nla_put(skb, WGDEVICE_A_PUBLIC_KEY,
				    NOISE_PUBLIC_KEY_LEN,
				    wg->static_identity.static_public)) {
				up_read(&wg->static_identity.lock);
				goto out;
			}
		}
		up_read(&wg->static_identity.lock);
	}

	peers_nest = nla_nest_start(skb, WGDEVICE_A_PEERS);
	if (!peers_nest)
		goto out;
	ret = 0;
	/* If the last cursor was removed via list_del_init in peer_remove, then
	 * we just treat this the same as there being no more peers left. The
	 * reason is that seq_nr should indicate to userspace that this isn't a
	 * coherent dump anyway, so they'll try again.
	 */
	if (list_empty(&wg->peer_list) ||
	    (last_peer_cursor && list_empty(&last_peer_cursor->peer_list))) {
		nla_nest_cancel(skb, peers_nest);
		goto out;
	}
	lockdep_assert_held(&wg->device_update_lock);
	peer = list_prepare_entry(last_peer_cursor, &wg->peer_list, peer_list);
	list_for_each_entry_continue (peer, &wg->peer_list, peer_list) {
		if (get_peer(peer, peer_idx++, rt_cursor, skb)) {
			done = false;
			break;
		}
		next_peer_cursor = peer;
	}
	nla_nest_end(skb, peers_nest);

out:
	if (!ret && !done && next_peer_cursor)
		wg_peer_get(next_peer_cursor);
	wg_peer_put(last_peer_cursor);
	mutex_unlock(&wg->device_update_lock);
	rtnl_unlock();

	if (ret) {
		genlmsg_cancel(skb, hdr);
		return ret;
	}
	genlmsg_end(skb, hdr);
	if (done) {
		cb->args[1] = 0;
		return 0;
	}
	cb->args[1] = (long)next_peer_cursor;
	return skb->len;

	/* At this point, we can't really deal ourselves with safely zeroing out
	 * the private key material after usage. This will need an additional API
	 * in the kernel for marking skbs as zero_on_free.
	 */
}

static int get_device_done(struct netlink_callback *cb)
{
	struct wireguard_device *wg = (struct wireguard_device *)cb->args[0];
	struct wireguard_peer *peer = (struct wireguard_peer *)cb->args[1];
	struct allowedips_cursor *rt_cursor =
		(struct allowedips_cursor *)cb->args[2];

	if (wg)
		dev_put(wg->dev);
	kfree(rt_cursor);
	wg_peer_put(peer);
	return 0;
}

static int set_socket(struct wireguard_device *wg, struct nlattr **attrs)
{
	struct wireguard_peer *peer;
	struct nlattr *port_attr = attrs[WGDEVICE_A_LISTEN_PORT];
	u16 port;
	struct net *net = NULL;
	int ret = 0;

	net = get_attr_net(attrs[WGDEVICE_A_TRANSIT_NETNS_PID],
			attrs[WGDEVICE_A_TRANSIT_NETNS_FD]);
	if (IS_ERR(net))
		return PTR_ERR(net);
	if (port_attr)
		port = nla_get_u16(port_attr);
	else
		port = wg->incoming_port;

	ret = test_socket_net_capable(net ? : wg->transit_net);
	if (ret)
		goto out;

	if (wg->incoming_port == port && (!net || wg->transit_net == net))
		goto out;

	list_for_each_entry (peer, &wg->peer_list, peer_list)
		wg_socket_clear_peer_endpoint_src(peer);
	if (!netif_running(wg->dev)) {
		wg->incoming_port = port;
		if (net)
			wg_device_set_nets(wg, wg->dev_net, net);
		goto out;
	}
	ret = wg_socket_init(wg, net ? : wg->transit_net, port);

out:
	if (net)
		put_net(net);
	return ret;
}

static int set_allowedip(struct wireguard_peer *peer, struct nlattr **attrs)
{
	int ret = -EINVAL;
	u16 family;
	u8 cidr;

	if (!attrs[WGALLOWEDIP_A_FAMILY] || !attrs[WGALLOWEDIP_A_IPADDR] ||
	    !attrs[WGALLOWEDIP_A_CIDR_MASK])
		return ret;
	family = nla_get_u16(attrs[WGALLOWEDIP_A_FAMILY]);
	cidr = nla_get_u8(attrs[WGALLOWEDIP_A_CIDR_MASK]);

	if (family == AF_INET && cidr <= 32 &&
	    nla_len(attrs[WGALLOWEDIP_A_IPADDR]) == sizeof(struct in_addr))
		ret = wg_allowedips_insert_v4(
			&peer->device->peer_allowedips,
			nla_data(attrs[WGALLOWEDIP_A_IPADDR]), cidr, peer,
			&peer->device->device_update_lock);
	else if (family == AF_INET6 && cidr <= 128 &&
		 nla_len(attrs[WGALLOWEDIP_A_IPADDR]) == sizeof(struct in6_addr))
		ret = wg_allowedips_insert_v6(
			&peer->device->peer_allowedips,
			nla_data(attrs[WGALLOWEDIP_A_IPADDR]), cidr, peer,
			&peer->device->device_update_lock);

	return ret;
}

static int set_peer(struct wireguard_device *wg, struct nlattr **attrs)
{
	u8 *public_key = NULL, *preshared_key = NULL;
	struct wireguard_peer *peer = NULL;
	u32 flags = 0;
	int ret;

	ret = -EINVAL;
	if (attrs[WGPEER_A_PUBLIC_KEY] &&
	    nla_len(attrs[WGPEER_A_PUBLIC_KEY]) == NOISE_PUBLIC_KEY_LEN)
		public_key = nla_data(attrs[WGPEER_A_PUBLIC_KEY]);
	else
		goto out;
	if (attrs[WGPEER_A_PRESHARED_KEY] &&
	    nla_len(attrs[WGPEER_A_PRESHARED_KEY]) == NOISE_SYMMETRIC_KEY_LEN)
		preshared_key = nla_data(attrs[WGPEER_A_PRESHARED_KEY]);
	if (attrs[WGPEER_A_FLAGS])
		flags = nla_get_u32(attrs[WGPEER_A_FLAGS]);

	ret = -EPFNOSUPPORT;
	if (attrs[WGPEER_A_PROTOCOL_VERSION]) {
		if (nla_get_u32(attrs[WGPEER_A_PROTOCOL_VERSION]) != 1)
			goto out;
	}

	peer = wg_pubkey_hashtable_lookup(&wg->peer_hashtable,
					  nla_data(attrs[WGPEER_A_PUBLIC_KEY]));
	if (!peer) { /* Peer doesn't exist yet. Add a new one. */
		ret = -ENODEV;
		if (flags & WGPEER_F_REMOVE_ME)
			goto out; /* Tried to remove a non-existing peer. */

		down_read(&wg->static_identity.lock);
		if (wg->static_identity.has_identity &&
		    !memcmp(nla_data(attrs[WGPEER_A_PUBLIC_KEY]),
			    wg->static_identity.static_public,
			    NOISE_PUBLIC_KEY_LEN)) {
			/* We silently ignore peers that have the same public
			 * key as the device. The reason we do it silently is
			 * that we'd like for people to be able to reuse the
			 * same set of API calls across peers.
			 */
			up_read(&wg->static_identity.lock);
			ret = 0;
			goto out;
		}
		up_read(&wg->static_identity.lock);

		ret = -ENOMEM;
		peer = wg_peer_create(wg, public_key, preshared_key);
		if (!peer)
			goto out;
		/* Take additional reference, as though we've just been
		 * looked up.
		 */
		wg_peer_get(peer);
	}

	ret = 0;
	if (flags & WGPEER_F_REMOVE_ME) {
		wg_peer_remove(peer);
		goto out;
	}

	if (preshared_key) {
		down_write(&peer->handshake.lock);
		memcpy(&peer->handshake.preshared_key, preshared_key,
		       NOISE_SYMMETRIC_KEY_LEN);
		up_write(&peer->handshake.lock);
	}

	if (attrs[WGPEER_A_ENDPOINT]) {
		struct sockaddr *addr = nla_data(attrs[WGPEER_A_ENDPOINT]);
		size_t len = nla_len(attrs[WGPEER_A_ENDPOINT]);

		if ((len == sizeof(struct sockaddr_in) &&
		     addr->sa_family == AF_INET) ||
		    (len == sizeof(struct sockaddr_in6) &&
		     addr->sa_family == AF_INET6)) {
			struct endpoint endpoint = { { { 0 } } };

			memcpy(&endpoint.addr, addr, len);
			wg_socket_set_peer_endpoint(peer, &endpoint);
		}
	}

	if (flags & WGPEER_F_REPLACE_ALLOWEDIPS)
		wg_allowedips_remove_by_peer(&wg->peer_allowedips, peer,
					     &wg->device_update_lock);

	if (attrs[WGPEER_A_ALLOWEDIPS]) {
		struct nlattr *attr, *allowedip[WGALLOWEDIP_A_MAX + 1];
		int rem;

		nla_for_each_nested (attr, attrs[WGPEER_A_ALLOWEDIPS], rem) {
			ret = nla_parse_nested(allowedip, WGALLOWEDIP_A_MAX,
					       attr, allowedip_policy, NULL);
			if (ret < 0)
				goto out;
			ret = set_allowedip(peer, allowedip);
			if (ret < 0)
				goto out;
		}
	}

	if (attrs[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]) {
		const u16 persistent_keepalive_interval = nla_get_u16(
				attrs[WGPEER_A_PERSISTENT_KEEPALIVE_INTERVAL]);
		const bool send_keepalive =
			!peer->persistent_keepalive_interval &&
			persistent_keepalive_interval &&
			netif_running(wg->dev);

		peer->persistent_keepalive_interval = persistent_keepalive_interval;
		if (send_keepalive)
			wg_packet_send_keepalive(peer);
	}

	if (netif_running(wg->dev))
		wg_packet_send_staged_packets(peer);

out:
	wg_peer_put(peer);
	if (attrs[WGPEER_A_PRESHARED_KEY])
		memzero_explicit(nla_data(attrs[WGPEER_A_PRESHARED_KEY]),
				 nla_len(attrs[WGPEER_A_PRESHARED_KEY]));
	return ret;
}

static int set_device(struct sk_buff *skb, struct genl_info *info)
{
	struct net *owned_dev_net, *dev_net;
	struct wireguard_device *wg;
	int ret;

	owned_dev_net = get_attr_net(info->attrs[WGDEVICE_A_DEV_NETNS_PID],
			info->attrs[WGDEVICE_A_DEV_NETNS_FD]);
	if (IS_ERR(owned_dev_net)) {
		ret = PTR_ERR(owned_dev_net);
		goto out_nonet;
	}
	dev_net = owned_dev_net ? : sock_net(skb->sk);
	if (!netlink_ns_capable(skb, dev_net->user_ns, CAP_NET_ADMIN)) {
		ret = -EPERM;
		goto out_nodev;
	}

	wg = lookup_interface(info->attrs, dev_net);

	if (IS_ERR(wg)) {
		ret = PTR_ERR(wg);
		goto out_nodev;
	}

	rtnl_lock();
	mutex_lock(&wg->device_update_lock);
	++wg->device_update_gen;

	if (info->attrs[WGDEVICE_A_FWMARK]) {
		struct wireguard_peer *peer;

		wg->fwmark = nla_get_u32(info->attrs[WGDEVICE_A_FWMARK]);
		list_for_each_entry (peer, &wg->peer_list, peer_list)
			wg_socket_clear_peer_endpoint_src(peer);
	}

	ret = set_socket(wg, info->attrs);
	if (ret)
		goto out;

	if (info->attrs[WGDEVICE_A_FLAGS] &&
	    nla_get_u32(info->attrs[WGDEVICE_A_FLAGS]) &
		    WGDEVICE_F_REPLACE_PEERS)
		wg_peer_remove_all(wg);

	if (info->attrs[WGDEVICE_A_PRIVATE_KEY] &&
	    nla_len(info->attrs[WGDEVICE_A_PRIVATE_KEY]) ==
		    NOISE_PUBLIC_KEY_LEN) {
		u8 *private_key = nla_data(info->attrs[WGDEVICE_A_PRIVATE_KEY]);
		u8 public_key[NOISE_PUBLIC_KEY_LEN];
		struct wireguard_peer *peer, *temp;

		/* We remove before setting, to prevent race, which means doing
		 * two 25519-genpub ops.
		 */
		if (curve25519_generate_public(public_key, private_key)) {
			peer = wg_pubkey_hashtable_lookup(&wg->peer_hashtable,
							  public_key);
			if (peer) {
				wg_peer_put(peer);
				wg_peer_remove(peer);
			}
		}

		down_write(&wg->static_identity.lock);
		wg_noise_set_static_identity_private_key(&wg->static_identity,
							 private_key);
		list_for_each_entry_safe (peer, temp, &wg->peer_list,
					  peer_list) {
			if (!wg_noise_precompute_static_static(peer))
				wg_peer_remove(peer);
		}
		wg_cookie_checker_precompute_device_keys(&wg->cookie_checker);
		up_write(&wg->static_identity.lock);
	}

	if (info->attrs[WGDEVICE_A_PEERS]) {
		struct nlattr *attr, *peer[WGPEER_A_MAX + 1];
		int rem;

		nla_for_each_nested (attr, info->attrs[WGDEVICE_A_PEERS], rem) {
			ret = nla_parse_nested(peer, WGPEER_A_MAX, attr,
					       peer_policy, NULL);
			if (ret < 0)
				goto out;
			ret = set_peer(wg, peer);
			if (ret < 0)
				goto out;
		}
	}
	ret = 0;

out:
	mutex_unlock(&wg->device_update_lock);
	rtnl_unlock();
	dev_put(wg->dev);
out_nodev:
	if (owned_dev_net)
		put_net(owned_dev_net);
out_nonet:
	if (info->attrs[WGDEVICE_A_PRIVATE_KEY])
		memzero_explicit(nla_data(info->attrs[WGDEVICE_A_PRIVATE_KEY]),
				 nla_len(info->attrs[WGDEVICE_A_PRIVATE_KEY]));
	return ret;
}

#ifndef COMPAT_CANNOT_USE_CONST_GENL_OPS
static const
#else
static
#endif
struct genl_ops genl_ops[] = {
	{
		.cmd = WG_CMD_GET_DEVICE,
#ifndef COMPAT_CANNOT_USE_NETLINK_START
		.start = get_device_start,
#endif
		.dumpit = get_device_dump,
		.done = get_device_done,
		.policy = device_policy,
	}, {
		.cmd = WG_CMD_SET_DEVICE,
		.doit = set_device,
		.policy = device_policy,
	}
};

static struct genl_family genl_family
#ifndef COMPAT_CANNOT_USE_GENL_NOPS
__ro_after_init = {
	.ops = genl_ops,
	.n_ops = ARRAY_SIZE(genl_ops),
#else
= {
#endif
	.name = WG_GENL_NAME,
	.version = WG_GENL_VERSION,
	.maxattr = WGDEVICE_A_MAX,
	.module = THIS_MODULE,
	.netnsok = true
};

int __init wg_genetlink_init(void)
{
	return genl_register_family(&genl_family);
}

void __exit wg_genetlink_uninit(void)
{
	genl_unregister_family(&genl_family);
}
