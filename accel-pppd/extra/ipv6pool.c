#include <stdlib.h>
#include <stdio.h>
#include <stdint.h>
#include <unistd.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <arpa/inet.h>
#include <endian.h>

#include "ipdb.h"
#include "list.h"
#include "log.h"
#include "spinlock.h"

#include "memdebug.h"


struct ippool_item_t
{
	struct list_head entry;
	struct ipv6db_item_t it;
};

struct dppool_item_t
{
	struct list_head entry;
	struct ipv6db_prefix_t it;
};


static LIST_HEAD(ippool);
static LIST_HEAD(dppool);
static spinlock_t pool_lock = SPINLOCK_INITIALIZER;
static struct ipdb_t ipdb;

static void generate_ippool(struct in6_addr *addr, int mask, int prefix_len)
{
	struct ippool_item_t *it;
	uint64_t ip, endip, step;
	struct ipv6db_addr_t *a;

	ip = be64toh(*(uint64_t *)addr->s6_addr);
	endip = ip | ((1llu << (64 - mask)) - 1);
	step = 1 << (64 - prefix_len);
	
	for (; ip <= endip; ip += step) {
		it = malloc(sizeof(*it));
		it->it.owner = &ipdb;
		INIT_LIST_HEAD(&it->it.addr_list);
		a = malloc(sizeof(*a));
		memset(a, 0, sizeof(*a));
		*(uint64_t *)a->addr.s6_addr = htobe64(ip);
		a->prefix_len = prefix_len;
		list_add_tail(&a->entry, &it->it.addr_list);
		list_add_tail(&it->entry, &ippool);
	}
}

static void generate_dppool(struct in6_addr *addr, int mask, int prefix_len)
{
	struct dppool_item_t *it;
	uint64_t ip, endip, step;
	struct ipv6db_addr_t *a;

	ip = be64toh(*(uint64_t *)addr->s6_addr);
	endip = ip | ((1llu << (64 - mask)) - 1);
	step = 1 << (64 - prefix_len);
	
	for (; ip <= endip; ip += step) {
		it = malloc(sizeof(*it));
		it->it.owner = &ipdb;
		INIT_LIST_HEAD(&it->it.prefix_list);
		a = malloc(sizeof(*a));
		memset(a, 0, sizeof(*a));
		*(uint64_t *)a->addr.s6_addr = htobe64(ip);
		a->prefix_len = prefix_len;
		list_add_tail(&a->entry, &it->it.prefix_list);
		list_add_tail(&it->entry, &dppool);
	}
}


static void add_prefix(int type, const char *_val)
{
	char *val = _strdup(_val);
	char *ptr1, *ptr2;
	struct in6_addr addr;
	int prefix_len;
	int mask;
	
	ptr1 = strchr(val, '/');
	if (!ptr1)
		goto err;
	
	*ptr1 = 0;

	ptr2 = strchr(ptr1 + 1, ',');
	if (!ptr2)
		goto err;
	
	*ptr2 = 0;

	if (inet_pton(AF_INET6, val, &addr) == 0)
		goto err;
	
	if (sscanf(ptr1 + 1, "%i", &mask) != 1)
		goto err;
	
	if (mask < 7 || mask > 64)
		goto err;
	
	if (sscanf(ptr2 + 1, "%i", &prefix_len) != 1)
		goto err;
	
	if (prefix_len > 64  || prefix_len < mask)
		goto err;
	
	if (type)
		generate_dppool(&addr, mask, prefix_len);
	else
		generate_ippool(&addr, mask, prefix_len);

	_free(val);
	return;
	
err:
	log_error("ipv6_pool: failed to parse '%s'\n", _val);
	_free(val);
}

static struct ipv6db_item_t *get_ip(struct ap_session *ses)
{
	struct ippool_item_t *it;

	spin_lock(&pool_lock);
	if (!list_empty(&ippool)) {
		it = list_entry(ippool.next, typeof(*it), entry);
		list_del(&it->entry);
		it->it.intf_id = 0;
		it->it.peer_intf_id = 0;
	} else
		it = NULL;
	spin_unlock(&pool_lock);

	return it ? &it->it : NULL;
}

static void put_ip(struct ap_session *ses, struct ipv6db_item_t *it)
{
	struct ippool_item_t *pit = container_of(it, typeof(*pit), it);

	spin_lock(&pool_lock);
	list_add_tail(&pit->entry, &ippool);
	spin_unlock(&pool_lock);
}

static struct ipv6db_prefix_t *get_dp(struct ap_session *ses)
{
	struct dppool_item_t *it;

	spin_lock(&pool_lock);
	if (!list_empty(&dppool)) {
		it = list_entry(dppool.next, typeof(*it), entry);
		list_del(&it->entry);
	} else
		it = NULL;
	spin_unlock(&pool_lock);

	return it ? &it->it : NULL;
}

static void put_dp(struct ap_session *ses, struct ipv6db_prefix_t *it)
{
	struct dppool_item_t *pit = container_of(it, typeof(*pit), it);

	spin_lock(&pool_lock);
	list_add_tail(&pit->entry, &dppool);
	spin_unlock(&pool_lock);
}

static struct ipdb_t ipdb = {
	.get_ipv6 = get_ip,
	.put_ipv6 = put_ip,
	.get_ipv6_prefix = get_dp,
	.put_ipv6_prefix = put_dp,
};

static void ippool_init(void)
{
	struct conf_sect_t *s = conf_get_section("ipv6-pool");
	struct conf_option_t *opt;
	
	if (!s)
		return;
	
	list_for_each_entry(opt, &s->items, entry) {
		if (!strcmp(opt->name, "delegate"))
			add_prefix(1, opt->val);
		else
			add_prefix(0, opt->name);
	}

	ipdb_register(&ipdb);
}

DEFINE_INIT(51, ippool_init);

