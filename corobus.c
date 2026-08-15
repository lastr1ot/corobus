#include "corobus.h"

#include "libcoro.h"
#include "rlist.h"

#include <assert.h>
#include <stdlib.h>
#include <string.h>

struct data_vector {
	unsigned *data;
	size_t size;
	size_t capacity;
};

#if 1 /* Uncomment this if want to use */

/** Append @a count messages in @a data to the end of the vector. */
static void
data_vector_append_many(struct data_vector *vector,
	const unsigned *data, size_t count)
{
	if (vector->size + count > vector->capacity) {
		if (vector->capacity == 0)
			vector->capacity = 4;
		else
			vector->capacity *= 2;
		if (vector->capacity < vector->size + count)
			vector->capacity = vector->size + count;
		vector->data = realloc(vector->data,
			sizeof(vector->data[0]) * vector->capacity);
	}
	memcpy(&vector->data[vector->size], data, sizeof(data[0]) * count);
	vector->size += count;
}

/** Append a single message to the vector. */
static void
data_vector_append(struct data_vector *vector, unsigned data)
{
	data_vector_append_many(vector, &data, 1);
}

/** Pop @a count of messages into @a data from the head of the vector. */
static void
data_vector_pop_first_many(struct data_vector *vector, unsigned *data, size_t count)
{
	assert(count <= vector->size);
	memcpy(data, vector->data, sizeof(data[0]) * count);
	vector->size -= count;
	memmove(vector->data, &vector->data[count], vector->size * sizeof(vector->data[0]));
}

/** Pop a single message from the head of the vector. */
static unsigned
data_vector_pop_first(struct data_vector *vector)
{
	unsigned data = 0;
	data_vector_pop_first_many(vector, &data, 1);
	return data;
}

#endif

/**
 * One coroutine waiting to be woken up in a list of other
 * suspended coros.
 */
struct wakeup_entry {
	struct rlist base;
	struct coro *coro;
	bool is_woken;
};

/** A queue of suspended coros waiting to be woken up. */
struct wakeup_queue {
	struct rlist coros;
};

#if 1 /* Uncomment this if want to use */

/** Suspend the current coroutine until it is woken up. */
static void
wakeup_queue_suspend_this(struct wakeup_queue *queue)
{
	struct wakeup_entry entry;
	entry.coro = coro_this();
	entry.is_woken = false;
	rlist_add_tail_entry(&queue->coros, &entry, base);
	coro_suspend();
	if (!entry.is_woken)
		rlist_del_entry(&entry, base);
}

/** Wakeup the first coroutine in the queue. */
static void
wakeup_queue_wakeup_first(struct wakeup_queue *queue)
{
	if (rlist_empty(&queue->coros))
		return;
	struct wakeup_entry *entry = rlist_first_entry(&queue->coros,
		struct wakeup_entry, base);
	entry->is_woken = true;
	coro_wakeup(entry->coro);
	rlist_del_entry(entry, base);
}

static void
wakeup_queue_wakeup_n(struct wakeup_queue *queue, size_t count)
{
	size_t woken = 0;

	while(woken < count && !rlist_empty(&queue->coros))
	{
		struct wakeup_entry *entry = rlist_first_entry(&queue->coros, struct wakeup_entry, base);
		entry->is_woken = true;
		coro_wakeup(entry->coro);

		rlist_del_entry(entry, base);

		woken++;
	}
}

#endif

struct coro_bus_channel {
	/** Channel max capacity. */
	size_t size_limit;
	/** Coroutines waiting until the channel is not full. */
	struct wakeup_queue send_queue;
	/** Coroutines waiting until the channel is not empty. */
	struct wakeup_queue recv_queue;
	/** Message queue. */
	struct data_vector data;
};

struct coro_bus {
	struct coro_bus_channel **channels;
	int channel_count;
};

static enum coro_bus_error_code global_error = CORO_BUS_ERR_NONE;

enum coro_bus_error_code
coro_bus_errno(void)
{
	return global_error;
}

void
coro_bus_errno_set(enum coro_bus_error_code err)
{
	global_error = err;
}

struct coro_bus *
coro_bus_new(void)
{
	struct coro_bus *bus = calloc(1, sizeof(struct coro_bus));
	return bus;
}

void
coro_bus_delete(struct coro_bus *bus)
{
	if (!bus) return;

	for (int i = 0; i < bus->channel_count; i++)
	{
		if (bus->channels[i] != NULL)
		{
			coro_bus_channel_close(bus, i);
		}
	}
	free(bus->channels);
	free(bus);
}

int
coro_bus_channel_open(struct coro_bus *bus, size_t size_limit)
{
	int idx = -1;
	for (int i = 0; i < bus->channel_count; i++)
	{
		if (bus->channels[i] == NULL)
		{
			idx = i;
			break;
		}
	}

	if (idx == -1)
	{
		idx = bus->channel_count;
		bus->channel_count++;
		bus->channels = realloc(bus->channels, sizeof(struct coro_bus_channel *) * bus->channel_count);
	}

	struct coro_bus_channel *ch = calloc(1, sizeof(struct coro_bus_channel));
	ch->size_limit = size_limit;
	rlist_create(&ch->send_queue.coros);
	rlist_create(&ch->recv_queue.coros);

	bus->channels[idx] = ch;
	return idx;
}

void
coro_bus_channel_close(struct coro_bus *bus, int channel)
{
	if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
		return;
	
	struct coro_bus_channel *ch = bus->channels[channel];

	coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);

	while (!rlist_empty(&ch->send_queue.coros))
	{
		struct wakeup_entry *entry = rlist_shift_entry(&ch->send_queue.coros, struct wakeup_entry, base);
		entry->is_woken = true;
		coro_wakeup(entry->coro);
	}

	while (!rlist_empty(&ch->recv_queue.coros))
	{
		struct wakeup_entry *entry = rlist_shift_entry(&ch->recv_queue.coros, struct wakeup_entry, base);
		entry->is_woken = true;
		coro_wakeup(entry->coro);
	}

	free(ch->data.data);
	free(ch);
	bus->channels[channel] = NULL;
}

int
coro_bus_send(struct coro_bus *bus, int channel, unsigned data)
{
	while(1)
	{
		int res = coro_bus_try_send(bus, channel, data);
		if (res == 0)
			return 0;

		if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK)
			return -1;

		wakeup_queue_suspend_this(&bus->channels[channel]->send_queue);
	}
}

int
coro_bus_try_send(struct coro_bus *bus, int channel, unsigned data)
{
	if(channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *ch = bus->channels[channel];

	if(ch->data.size >= ch->size_limit)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	data_vector_append(&ch->data, data);
	wakeup_queue_wakeup_first(&ch->recv_queue);

	return 0;
}

int
coro_bus_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	while(1)
	{
		int res = coro_bus_try_recv(bus, channel, data);
		if (res == 0)
			return 0;

		if (coro_bus_errno() != CORO_BUS_ERR_WOULD_BLOCK)
			return -1;

		wakeup_queue_suspend_this(&bus->channels[channel]->recv_queue);
	}
}

int
coro_bus_try_recv(struct coro_bus *bus, int channel, unsigned *data)
{
	if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *ch = bus->channels[channel];

	if (ch->data.size ==0)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	*data = data_vector_pop_first(&ch->data);
	wakeup_queue_wakeup_first(&ch->send_queue);

	return 0;
}


#if NEED_BROADCAST

int
coro_bus_broadcast(struct coro_bus *bus, unsigned data)
{
	if (bus == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

    int has_any_channel = 0;
    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            has_any_channel = 1;
            break;
        }
    }
    
    if (!has_any_channel) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

	while(1)
	{
		int first_full_channel = -1;
		for (int i = 0; i < bus->channel_count; i++)
		{
			if (bus->channels[i] != NULL)
			{
				if (bus->channels[i]->data.size >= bus->channels[i]->size_limit)
				{
					first_full_channel = i;
					break;
				}
			}
		}

		if (first_full_channel == -1)
		{
			for (int i = 0; i < bus->channel_count; i++)
			{
				if (bus->channels[i] != NULL)
				{
					data_vector_append(&bus->channels[i]->data, data);
					wakeup_queue_wakeup_first(&bus->channels[i]->recv_queue);
				}
			}
			return 0;
		}

		if (bus->channels[first_full_channel] == NULL)
			continue;

		wakeup_queue_suspend_this(&bus->channels[first_full_channel]->send_queue);
	}
}

int
coro_bus_try_broadcast(struct coro_bus *bus, unsigned data)
{
	if (bus == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

    int has_any_channel = 0;
    for (int i = 0; i < bus->channel_count; i++) {
        if (bus->channels[i] != NULL) {
            has_any_channel = 1;
            break;
        }
    }
    
    if (!has_any_channel) {
        coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
        return -1;
    }

	for (int i = 0; i < bus->channel_count; i++)
	{
		if (bus->channels[i] != NULL)
		{
			if (bus->channels[i]->data.size >= bus->channels[i]->size_limit)
			{
				coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
				return -1;
			}
		}
	}

	for (int i = 0; i < bus->channel_count; i++)
	{
		if (bus->channels[i] != NULL)
		{
			data_vector_append(&bus->channels[i]->data, data);
			wakeup_queue_wakeup_first(&bus->channels[i]->recv_queue);
		}
	}

	return 0;
}

#endif

#if NEED_BATCH

int
coro_bus_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}
	
	struct coro_bus_channel *ch = bus->channels[channel];

	if (count == 0)
		return 0;

	while(1)
	{
		if(ch->data.size < ch->size_limit)
		{
			size_t avail_space = ch->size_limit - ch->data.size;
			unsigned to_send = (unsigned)(avail_space < count ? avail_space: count);

			for (unsigned i = 0; i < to_send; i++)
			{
				data_vector_append(&ch->data, data[i]);
			}

			if (to_send > 0)
				wakeup_queue_wakeup_first(&ch->recv_queue);
			
			return (int)to_send;
		}

		wakeup_queue_suspend_this(&ch->send_queue);
	}
}

int
coro_bus_try_send_v(struct coro_bus *bus, int channel, const unsigned *data, unsigned count)
{
	if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *ch = bus->channels[channel];

	if (count == 0)
		return 0;
	
	if (ch->data.size >= ch->size_limit)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	size_t avail_space = ch->size_limit - ch->data.size;
	unsigned to_send = (unsigned)(avail_space < count ? avail_space : count);

	for (unsigned i = 0; i < to_send; i++)
	{
		data_vector_append(&ch->data, data[i]);
	}

	if (to_send > 0)
		wakeup_queue_wakeup_first(&ch->recv_queue);

	return (int)to_send;
}

int
coro_bus_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	if (capacity == 0)
		return 0;

	struct coro_bus_channel *ch = bus->channels[channel];

	while(1)
	{
		if (ch->data.size > 0)
		{
			size_t avail_msg = ch->data.size;
			unsigned to_recv = (unsigned)(avail_msg < capacity ? avail_msg: capacity);

			for (unsigned i = 0; i < to_recv; i++)
				data[i] = data_vector_pop_first(&ch->data);

			if (to_recv > 0)
				wakeup_queue_wakeup_n(&ch->send_queue, to_recv);

			return (int)to_recv;
		}

		wakeup_queue_suspend_this(&ch->recv_queue);
	}
}

int
coro_bus_try_recv_v(struct coro_bus *bus, int channel, unsigned *data, unsigned capacity)
{
	if (channel < 0 || channel >= bus->channel_count || bus->channels[channel] == NULL)
	{
		coro_bus_errno_set(CORO_BUS_ERR_NO_CHANNEL);
		return -1;
	}

	struct coro_bus_channel *ch = bus->channels[channel];

	if (capacity == 0)
		return 0;

	if (ch->data.size == 0)
	{
		coro_bus_errno_set(CORO_BUS_ERR_WOULD_BLOCK);
		return -1;
	}

	size_t avail_msg = ch->data.size;
	unsigned to_recv = (unsigned)(avail_msg < capacity ? avail_msg: capacity);

	for (unsigned i = 0; i < to_recv; i++)
		data[i] = data_vector_pop_first(&ch->data);
	
	if (to_recv > 0)
		wakeup_queue_wakeup_n(&ch->send_queue, to_recv);

	return (int)to_recv;
}

#endif
