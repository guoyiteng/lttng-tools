/*
 * Copyright (C) 2011 - David Goulet <david.goulet@polymtl.ca>
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License, version 2 only,
 * as published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License along
 * with this program; if not, write to the Free Software Foundation, Inc.,
 * 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _LGPL_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <inttypes.h>

#include <common/common.h>
#include <common/kernel-ctl/kernel-ctl.h>
#include <common/kernel-ctl/kernel-ioctl.h>
#include <common/sessiond-comm/sessiond-comm.h>

#include "consumer.h"
#include "kernel.h"
#include "kernel-consumer.h"
#include "kern-modules.h"
#include "utils.h"
#include "rotate.h"

/*
 * Key used to reference a channel between the sessiond and the consumer. This
 * is only read and updated with the session_list lock held.
 */
static uint64_t next_kernel_channel_key;

#include <lttng/userspace-probe.h>
#include <lttng/userspace-probe-internal.h>
/*
 * Add context on a kernel channel.
 *
 * Assumes the ownership of ctx.
 */
int kernel_add_channel_context(struct ltt_kernel_channel *chan,
		struct ltt_kernel_context *ctx)
{
	int ret;

	assert(chan);
	assert(ctx);

	DBG("Adding context to channel %s", chan->channel->name);
	ret = kernctl_add_context(chan->fd, &ctx->ctx);
	if (ret < 0) {
		switch (-ret) {
		case ENOSYS:
			/* Exists but not available for this kernel */
			ret = LTTNG_ERR_KERN_CONTEXT_UNAVAILABLE;
			goto error;
		case EEXIST:
			/* If EEXIST, we just ignore the error */
			ret = 0;
			goto end;
		default:
			PERROR("add context ioctl");
			ret = LTTNG_ERR_KERN_CONTEXT_FAIL;
			goto error;
		}
	}
	ret = 0;

end:
	cds_list_add_tail(&ctx->list, &chan->ctx_list);
	ctx->in_list = true;
	ctx = NULL;
error:
	if (ctx) {
		trace_kernel_destroy_context(ctx);
	}
	return ret;
}

/*
 * Create a new kernel session, register it to the kernel tracer and add it to
 * the session daemon session.
 */
int kernel_create_session(struct ltt_session *session, int tracer_fd)
{
	int ret;
	struct ltt_kernel_session *lks;

	assert(session);

	/* Allocate data structure */
	lks = trace_kernel_create_session();
	if (lks == NULL) {
		ret = -1;
		goto error;
	}

	/* Kernel tracer session creation */
	ret = kernctl_create_session(tracer_fd);
	if (ret < 0) {
		PERROR("ioctl kernel create session");
		goto error;
	}

	lks->fd = ret;
	/* Prevent fd duplication after execlp() */
	ret = fcntl(lks->fd, F_SETFD, FD_CLOEXEC);
	if (ret < 0) {
		PERROR("fcntl session fd");
	}

	lks->id = session->id;
	lks->consumer_fds_sent = 0;
	session->kernel_session = lks;

	DBG("Kernel session created (fd: %d)", lks->fd);

	return 0;

error:
	if (lks) {
		trace_kernel_destroy_session(lks);
	}
	return ret;
}

/*
 * Create a kernel channel, register it to the kernel tracer and add it to the
 * kernel session.
 */
int kernel_create_channel(struct ltt_kernel_session *session,
		struct lttng_channel *chan)
{
	int ret;
	struct ltt_kernel_channel *lkc;

	assert(session);
	assert(chan);

	/* Allocate kernel channel */
	lkc = trace_kernel_create_channel(chan);
	if (lkc == NULL) {
		goto error;
	}

	DBG3("Kernel create channel %s with attr: %d, %" PRIu64 ", %" PRIu64 ", %u, %u, %d, %d",
			chan->name, lkc->channel->attr.overwrite,
			lkc->channel->attr.subbuf_size, lkc->channel->attr.num_subbuf,
			lkc->channel->attr.switch_timer_interval, lkc->channel->attr.read_timer_interval,
			lkc->channel->attr.live_timer_interval, lkc->channel->attr.output);

	/* Kernel tracer channel creation */
	ret = kernctl_create_channel(session->fd, &lkc->channel->attr);
	if (ret < 0) {
		PERROR("ioctl kernel create channel");
		goto error;
	}

	/* Setup the channel fd */
	lkc->fd = ret;
	/* Prevent fd duplication after execlp() */
	ret = fcntl(lkc->fd, F_SETFD, FD_CLOEXEC);
	if (ret < 0) {
		PERROR("fcntl session fd");
	}

	/* Add channel to session */
	cds_list_add(&lkc->list, &session->channel_list.head);
	session->channel_count++;
	lkc->session = session;
	lkc->key = ++next_kernel_channel_key;

	DBG("Kernel channel %s created (fd: %d, key: %" PRIu64 ")",
			lkc->channel->name, lkc->fd, lkc->key);

	return 0;

error:
	if (lkc) {
		free(lkc->channel);
		free(lkc);
	}
	return -1;
}

/*
 * Compute the offset of the instrumentation byte in the binary based on the
 * function probe location using the ELF lookup method.
 *
 * Returns 0 on success and set the offset out parameter to the offset of the
 * elf symbol
 * Returns -1 on error
 */
static
int extract_userspace_probe_offset_function_elf(
		const struct lttng_userspace_probe_location *probe_location,
		struct ltt_kernel_session *session, uint64_t *offset)
{
	int fd;
	int ret = 0;
	const char *symbol = NULL;
	const struct lttng_userspace_probe_location_lookup_method *lookup = NULL;
	enum lttng_userspace_probe_location_lookup_method_type lookup_method_type;

	assert(lttng_userspace_probe_location_get_type(probe_location) ==
			LTTNG_USERSPACE_PROBE_LOCATION_TYPE_FUNCTION);

	lookup = lttng_userspace_probe_location_get_lookup_method(
			probe_location);
	if (!lookup) {
		ret = -1;
		goto end;
	}

	lookup_method_type =
			lttng_userspace_probe_location_lookup_method_get_type(lookup);

	assert(lookup_method_type ==
			LTTNG_USERSPACE_PROBE_LOCATION_LOOKUP_METHOD_TYPE_FUNCTION_ELF);

	symbol = lttng_userspace_probe_location_function_get_function_name(
			probe_location);
	if (!symbol) {
		ret = -1;
		goto end;
	}

	fd = lttng_userspace_probe_location_function_get_binary_fd(probe_location);
	if (fd < 0) {
		ret = -1;
		goto end;
	}

	ret = run_as_extract_elf_symbol_offset(fd, symbol, session->uid,
			session->gid, offset);
	if (ret < 0) {
		DBG("userspace probe offset calculation failed for "
				"function %s", symbol);
		goto end;
	}

	DBG("userspace probe elf offset for %s is 0x%jd", symbol, (intmax_t)(*offset));
end:
	return ret;
}

/*
 * Compute the offsets of the instrumentation bytes in the binary based on the
 * tracepoint probe location using the SDT lookup method. This function
 * allocates the offsets buffer, the caller must free it.
 *
 * Returns 0 on success and set the offset out parameter to the offsets of the
 * SDT tracepoint.
 * Returns -1 on error.
 */
static
int extract_userspace_probe_offset_tracepoint_sdt(
		const struct lttng_userspace_probe_location *probe_location,
		struct ltt_kernel_session *session, uint64_t **offsets,
		uint32_t *offsets_count)
{
	enum lttng_userspace_probe_location_lookup_method_type lookup_method_type;
	const struct lttng_userspace_probe_location_lookup_method *lookup = NULL;
	const char *probe_name = NULL, *provider_name = NULL;
	int ret = 0;
	int fd, i;

	assert(lttng_userspace_probe_location_get_type(probe_location) ==
			LTTNG_USERSPACE_PROBE_LOCATION_TYPE_TRACEPOINT);

	lookup = lttng_userspace_probe_location_get_lookup_method(probe_location);
	if (!lookup) {
		ret = -1;
		goto end;
	}

	lookup_method_type =
			lttng_userspace_probe_location_lookup_method_get_type(lookup);

	assert(lookup_method_type ==
			LTTNG_USERSPACE_PROBE_LOCATION_LOOKUP_METHOD_TYPE_TRACEPOINT_SDT);


	probe_name = lttng_userspace_probe_location_tracepoint_get_probe_name(
			probe_location);
	if (!probe_name) {
		ret = -1;
		goto end;
	}

	provider_name = lttng_userspace_probe_location_tracepoint_get_provider_name(
			probe_location);
	if (!provider_name) {
		ret = -1;
		goto end;
	}

	fd = lttng_userspace_probe_location_tracepoint_get_binary_fd(probe_location);
	if (fd < 0) {
		ret = -1;
		goto end;
	}

	ret = run_as_extract_sdt_probe_offsets(fd, provider_name, probe_name,
			session->uid, session->gid, offsets, offsets_count);
	if (ret < 0) {
		DBG("userspace probe offset calculation failed for sdt "
				"probe %s:%s", provider_name, probe_name);
		goto end;
	}

	if (*offsets_count == 0) {
		DBG("no userspace probe offset found");
		goto end;
	}

	DBG("%u userspace probe SDT offsets found for %s:%s at:",
			*offsets_count, provider_name, probe_name);
	for (i = 0; i < *offsets_count; i++) {
		DBG("\t0x%jd", (intmax_t)((*offsets)[i]));
	}
end:
	return ret;
}

/*
 * Extract the offsets of the instrumentation point for the different lookup
 * methods.
 */
static
int userspace_probe_add_callsites(struct lttng_event *ev,
			struct ltt_kernel_session *session, int fd)
{
	const struct lttng_userspace_probe_location_lookup_method *lookup_method = NULL;
	enum lttng_userspace_probe_location_lookup_method_type type;
	const struct lttng_userspace_probe_location *location = NULL;
	int ret;

	assert(ev);
	assert(ev->type == LTTNG_EVENT_USERSPACE_PROBE);

	location = lttng_event_get_userspace_probe_location(ev);
	if (!location) {
		ret = -1;
		goto end;
	}
	lookup_method =
			lttng_userspace_probe_location_get_lookup_method(location);
	if (!lookup_method) {
		ret = -1;
		goto end;
	}

	type = lttng_userspace_probe_location_lookup_method_get_type(lookup_method);
	switch (type) {
	case LTTNG_USERSPACE_PROBE_LOCATION_LOOKUP_METHOD_TYPE_FUNCTION_ELF:
	{
		struct lttng_kernel_event_callsite callsite;
		uint64_t offset;

		ret = extract_userspace_probe_offset_function_elf(location, session, &offset);
		if (ret) {
			ret = LTTNG_ERR_PROBE_LOCATION_INVAL;
			goto end;
		}

		callsite.u.uprobe.offset = offset;
		ret = kernctl_add_callsite(fd, &callsite);
		if (ret) {
			WARN("Adding callsite to userspace probe "
					"event %s failed.", ev->name);
			ret = LTTNG_ERR_KERN_ENABLE_FAIL;
			goto end;
		}
		break;
	}
	case LTTNG_USERSPACE_PROBE_LOCATION_LOOKUP_METHOD_TYPE_TRACEPOINT_SDT:
	{
		int i;
		uint64_t *offsets = NULL;
		uint32_t offsets_count;
		struct lttng_kernel_event_callsite callsite;

		/*
		 * This call allocates the offsets buffer. This buffer must be freed
		 * by the caller
		 */
		ret = extract_userspace_probe_offset_tracepoint_sdt(location, session,
				&offsets, &offsets_count);
		if (ret) {
			ret = LTTNG_ERR_PROBE_LOCATION_INVAL;
			goto end;
		}
		for (i = 0; i < offsets_count; i++) {
			callsite.u.uprobe.offset = offsets[i];
			ret = kernctl_add_callsite(fd, &callsite);
			if (ret) {
				WARN("Adding callsite to userspace probe "
						"event %s failed.", ev->name);
				ret = LTTNG_ERR_KERN_ENABLE_FAIL;
				free(offsets);
				goto end;
			}
		}
		free(offsets);
		break;
	}
	default:
		ret = LTTNG_ERR_PROBE_LOCATION_INVAL;
		goto end;
	}
end:
	return ret;
}

/*
 * Create a kernel event, enable it to the kernel tracer and add it to the
 * channel event list of the kernel session.
 * We own filter_expression and filter.
 */
int kernel_create_event(struct lttng_event *ev,
		struct ltt_kernel_channel *channel,
		char *filter_expression,
		struct lttng_filter_bytecode *filter)
{
	int err, fd;
	enum lttng_error_code ret;
	struct ltt_kernel_event *event;

	assert(ev);
	assert(channel);

	/* We pass ownership of filter_expression and filter */
	ret = trace_kernel_create_event(ev, filter_expression,
			filter, &event);
	if (ret != LTTNG_OK) {
		goto error;
	}

	fd = kernctl_create_event(channel->fd, event->event);
	if (fd < 0) {
		switch (-fd) {
		case EEXIST:
			ret = LTTNG_ERR_KERN_EVENT_EXIST;
			break;
		case ENOSYS:
			WARN("Event type not implemented");
			ret = LTTNG_ERR_KERN_EVENT_ENOSYS;
			break;
		case ENOENT:
			WARN("Event %s not found!", ev->name);
			ret = LTTNG_ERR_KERN_ENABLE_FAIL;
			break;
		default:
			ret = LTTNG_ERR_KERN_ENABLE_FAIL;
			PERROR("create event ioctl");
		}
		goto free_event;
	}

	event->type = ev->type;
	event->fd = fd;
	/* Prevent fd duplication after execlp() */
	err = fcntl(event->fd, F_SETFD, FD_CLOEXEC);
	if (err < 0) {
		PERROR("fcntl session fd");
	}

	if (filter) {
		err = kernctl_filter(event->fd, filter);
		if (err < 0) {
			switch (-err) {
			case ENOMEM:
				ret = LTTNG_ERR_FILTER_NOMEM;
				break;
			default:
				ret = LTTNG_ERR_FILTER_INVAL;
				break;
			}
			goto filter_error;
		}
	}

	if (ev->type == LTTNG_EVENT_USERSPACE_PROBE) {
		ret = userspace_probe_add_callsites(ev, channel->session, event->fd);
		if (ret) {
			goto add_callsite_error;
		}
	}

	err = kernctl_enable(event->fd);
	if (err < 0) {
		switch (-err) {
		case EEXIST:
			ret = LTTNG_ERR_KERN_EVENT_EXIST;
			break;
		default:
			PERROR("enable kernel event");
			ret = LTTNG_ERR_KERN_ENABLE_FAIL;
			break;
		}
		goto enable_error;
	}

	/* Add event to event list */
	cds_list_add(&event->list, &channel->events_list.head);
	channel->event_count++;

	DBG("Event %s created (fd: %d)", ev->name, event->fd);

	return 0;

add_callsite_error:
enable_error:
filter_error:
	{
		int closeret;

		closeret = close(event->fd);
		if (closeret) {
			PERROR("close event fd");
		}
	}
free_event:
	free(event);
error:
	return ret;
}

/*
 * Disable a kernel channel.
 */
int kernel_disable_channel(struct ltt_kernel_channel *chan)
{
	int ret;

	assert(chan);

	ret = kernctl_disable(chan->fd);
	if (ret < 0) {
		PERROR("disable chan ioctl");
		goto error;
	}

	chan->enabled = 0;
	DBG("Kernel channel %s disabled (fd: %d, key: %" PRIu64 ")",
			chan->channel->name, chan->fd, chan->key);

	return 0;

error:
	return ret;
}

/*
 * Enable a kernel channel.
 */
int kernel_enable_channel(struct ltt_kernel_channel *chan)
{
	int ret;

	assert(chan);

	ret = kernctl_enable(chan->fd);
	if (ret < 0 && ret != -EEXIST) {
		PERROR("Enable kernel chan");
		goto error;
	}

	chan->enabled = 1;
	DBG("Kernel channel %s enabled (fd: %d, key: %" PRIu64 ")",
			chan->channel->name, chan->fd, chan->key);

	return 0;

error:
	return ret;
}

/*
 * Enable a kernel event.
 */
int kernel_enable_event(struct ltt_kernel_event *event)
{
	int ret;

	assert(event);

	ret = kernctl_enable(event->fd);
	if (ret < 0) {
		switch (-ret) {
		case EEXIST:
			ret = LTTNG_ERR_KERN_EVENT_EXIST;
			break;
		default:
			PERROR("enable kernel event");
			break;
		}
		goto error;
	}

	event->enabled = 1;
	DBG("Kernel event %s enabled (fd: %d)", event->event->name, event->fd);

	return 0;

error:
	return ret;
}

/*
 * Disable a kernel event.
 */
int kernel_disable_event(struct ltt_kernel_event *event)
{
	int ret;

	assert(event);

	ret = kernctl_disable(event->fd);
	if (ret < 0) {
		switch (-ret) {
		case EEXIST:
			ret = LTTNG_ERR_KERN_EVENT_EXIST;
			break;
		default:
			PERROR("disable kernel event");
			break;
		}
		goto error;
	}

	event->enabled = 0;
	DBG("Kernel event %s disabled (fd: %d)", event->event->name, event->fd);

	return 0;

error:
	return ret;
}


int kernel_track_pid(struct ltt_kernel_session *session, int pid)
{
	int ret;

	DBG("Kernel track PID %d for session id %" PRIu64 ".",
			pid, session->id);
	ret = kernctl_track_pid(session->fd, pid);
	if (!ret) {
		return LTTNG_OK;
	}
	switch (-ret) {
	case EINVAL:
		return LTTNG_ERR_INVALID;
	case ENOMEM:
		return LTTNG_ERR_NOMEM;
	case EEXIST:
		return LTTNG_ERR_PID_TRACKED;
	default:
		return LTTNG_ERR_UNK;
	}
}

int kernel_untrack_pid(struct ltt_kernel_session *session, int pid)
{
	int ret;

	DBG("Kernel untrack PID %d for session id %" PRIu64 ".",
			pid, session->id);
	ret = kernctl_untrack_pid(session->fd, pid);
	if (!ret) {
		return LTTNG_OK;
	}
	switch (-ret) {
	case EINVAL:
		return LTTNG_ERR_INVALID;
	case ENOMEM:
		return LTTNG_ERR_NOMEM;
	case ENOENT:
		return LTTNG_ERR_PID_NOT_TRACKED;
	default:
		return LTTNG_ERR_UNK;
	}
}

ssize_t kernel_list_tracker_pids(struct ltt_kernel_session *session,
		int **_pids)
{
	int fd, ret;
	int pid;
	ssize_t nbmem, count = 0;
	FILE *fp;
	int *pids;

	fd = kernctl_list_tracker_pids(session->fd);
	if (fd < 0) {
		PERROR("kernel tracker pids list");
		goto error;
	}

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		PERROR("kernel tracker pids list fdopen");
		goto error_fp;
	}

	nbmem = KERNEL_TRACKER_PIDS_INIT_LIST_SIZE;
	pids = zmalloc(sizeof(*pids) * nbmem);
	if (pids == NULL) {
		PERROR("alloc list pids");
		count = -ENOMEM;
		goto end;
	}

	while (fscanf(fp, "process { pid = %u; };\n", &pid) == 1) {
		if (count >= nbmem) {
			int *new_pids;
			size_t new_nbmem;

			new_nbmem = nbmem << 1;
			DBG("Reallocating pids list from %zu to %zu entries",
					nbmem, new_nbmem);
			new_pids = realloc(pids, new_nbmem * sizeof(*new_pids));
			if (new_pids == NULL) {
				PERROR("realloc list events");
				free(pids);
				count = -ENOMEM;
				goto end;
			}
			/* Zero the new memory */
			memset(new_pids + nbmem, 0,
				(new_nbmem - nbmem) * sizeof(*new_pids));
			nbmem = new_nbmem;
			pids = new_pids;
		}
		pids[count++] = pid;
	}

	*_pids = pids;
	DBG("Kernel list tracker pids done (%zd pids)", count);
end:
	ret = fclose(fp);	/* closes both fp and fd */
	if (ret) {
		PERROR("fclose");
	}
	return count;

error_fp:
	ret = close(fd);
	if (ret) {
		PERROR("close");
	}
error:
	return -1;
}

/*
 * Create kernel metadata, open from the kernel tracer and add it to the
 * kernel session.
 */
int kernel_open_metadata(struct ltt_kernel_session *session)
{
	int ret;
	struct ltt_kernel_metadata *lkm = NULL;

	assert(session);

	/* Allocate kernel metadata */
	lkm = trace_kernel_create_metadata();
	if (lkm == NULL) {
		goto error;
	}

	/* Kernel tracer metadata creation */
	ret = kernctl_open_metadata(session->fd, &lkm->conf->attr);
	if (ret < 0) {
		goto error_open;
	}

	lkm->fd = ret;
	lkm->key = ++next_kernel_channel_key;
	/* Prevent fd duplication after execlp() */
	ret = fcntl(lkm->fd, F_SETFD, FD_CLOEXEC);
	if (ret < 0) {
		PERROR("fcntl session fd");
	}

	session->metadata = lkm;

	DBG("Kernel metadata opened (fd: %d)", lkm->fd);

	return 0;

error_open:
	trace_kernel_destroy_metadata(lkm);
error:
	return -1;
}

/*
 * Start tracing session.
 */
int kernel_start_session(struct ltt_kernel_session *session)
{
	int ret;

	assert(session);

	ret = kernctl_start_session(session->fd);
	if (ret < 0) {
		PERROR("ioctl start session");
		goto error;
	}

	DBG("Kernel session started");

	return 0;

error:
	return ret;
}

/*
 * Make a kernel wait to make sure in-flight probe have completed.
 */
void kernel_wait_quiescent(int fd)
{
	int ret;

	DBG("Kernel quiescent wait on %d", fd);

	ret = kernctl_wait_quiescent(fd);
	if (ret < 0) {
		PERROR("wait quiescent ioctl");
		ERR("Kernel quiescent wait failed");
	}
}

/*
 *  Force flush buffer of metadata.
 */
int kernel_metadata_flush_buffer(int fd)
{
	int ret;

	DBG("Kernel flushing metadata buffer on fd %d", fd);

	ret = kernctl_buffer_flush(fd);
	if (ret < 0) {
		ERR("Fail to flush metadata buffers %d (ret: %d)", fd, ret);
	}

	return 0;
}

/*
 * Force flush buffer for channel.
 */
int kernel_flush_buffer(struct ltt_kernel_channel *channel)
{
	int ret;
	struct ltt_kernel_stream *stream;

	assert(channel);

	DBG("Flush buffer for channel %s", channel->channel->name);

	cds_list_for_each_entry(stream, &channel->stream_list.head, list) {
		DBG("Flushing channel stream %d", stream->fd);
		ret = kernctl_buffer_flush(stream->fd);
		if (ret < 0) {
			PERROR("ioctl");
			ERR("Fail to flush buffer for stream %d (ret: %d)",
					stream->fd, ret);
		}
	}

	return 0;
}

/*
 * Stop tracing session.
 */
int kernel_stop_session(struct ltt_kernel_session *session)
{
	int ret;

	assert(session);

	ret = kernctl_stop_session(session->fd);
	if (ret < 0) {
		goto error;
	}

	DBG("Kernel session stopped");

	return 0;

error:
	return ret;
}

/*
 * Open stream of channel, register it to the kernel tracer and add it
 * to the stream list of the channel.
 *
 * Note: given that the streams may appear in random order wrt CPU
 * number (e.g. cpu hotplug), the index value of the stream number in
 * the stream name is not necessarily linked to the CPU number.
 *
 * Return the number of created stream. Else, a negative value.
 */
int kernel_open_channel_stream(struct ltt_kernel_channel *channel)
{
	int ret;
	struct ltt_kernel_stream *lks;

	assert(channel);

	while ((ret = kernctl_create_stream(channel->fd)) >= 0) {
		lks = trace_kernel_create_stream(channel->channel->name,
				channel->stream_count);
		if (lks == NULL) {
			ret = close(ret);
			if (ret) {
				PERROR("close");
			}
			goto error;
		}

		lks->fd = ret;
		/* Prevent fd duplication after execlp() */
		ret = fcntl(lks->fd, F_SETFD, FD_CLOEXEC);
		if (ret < 0) {
			PERROR("fcntl session fd");
		}

		lks->tracefile_size = channel->channel->attr.tracefile_size;
		lks->tracefile_count = channel->channel->attr.tracefile_count;

		/* Add stream to channel stream list */
		cds_list_add(&lks->list, &channel->stream_list.head);
		channel->stream_count++;

		DBG("Kernel stream %s created (fd: %d, state: %d)", lks->name, lks->fd,
				lks->state);
	}

	return channel->stream_count;

error:
	return -1;
}

/*
 * Open the metadata stream and set it to the kernel session.
 */
int kernel_open_metadata_stream(struct ltt_kernel_session *session)
{
	int ret;

	assert(session);

	ret = kernctl_create_stream(session->metadata->fd);
	if (ret < 0) {
		PERROR("kernel create metadata stream");
		goto error;
	}

	DBG("Kernel metadata stream created (fd: %d)", ret);
	session->metadata_stream_fd = ret;
	/* Prevent fd duplication after execlp() */
	ret = fcntl(session->metadata_stream_fd, F_SETFD, FD_CLOEXEC);
	if (ret < 0) {
		PERROR("fcntl session fd");
	}

	return 0;

error:
	return -1;
}

/*
 * Get the event list from the kernel tracer and return the number of elements.
 */
ssize_t kernel_list_events(int tracer_fd, struct lttng_event **events)
{
	int fd, ret;
	char *event;
	size_t nbmem, count = 0;
	FILE *fp;
	struct lttng_event *elist;

	assert(events);

	fd = kernctl_tracepoint_list(tracer_fd);
	if (fd < 0) {
		PERROR("kernel tracepoint list");
		goto error;
	}

	fp = fdopen(fd, "r");
	if (fp == NULL) {
		PERROR("kernel tracepoint list fdopen");
		goto error_fp;
	}

	/*
	 * Init memory size counter
	 * See kernel-ctl.h for explanation of this value
	 */
	nbmem = KERNEL_EVENT_INIT_LIST_SIZE;
	elist = zmalloc(sizeof(struct lttng_event) * nbmem);
	if (elist == NULL) {
		PERROR("alloc list events");
		count = -ENOMEM;
		goto end;
	}

	while (fscanf(fp, "event { name = %m[^;]; };\n", &event) == 1) {
		if (count >= nbmem) {
			struct lttng_event *new_elist;
			size_t new_nbmem;

			new_nbmem = nbmem << 1;
			DBG("Reallocating event list from %zu to %zu bytes",
					nbmem, new_nbmem);
			new_elist = realloc(elist, new_nbmem * sizeof(struct lttng_event));
			if (new_elist == NULL) {
				PERROR("realloc list events");
				free(event);
				free(elist);
				count = -ENOMEM;
				goto end;
			}
			/* Zero the new memory */
			memset(new_elist + nbmem, 0,
				(new_nbmem - nbmem) * sizeof(struct lttng_event));
			nbmem = new_nbmem;
			elist = new_elist;
		}
		strncpy(elist[count].name, event, LTTNG_SYMBOL_NAME_LEN);
		elist[count].name[LTTNG_SYMBOL_NAME_LEN - 1] = '\0';
		elist[count].enabled = -1;
		count++;
		free(event);
	}

	*events = elist;
	DBG("Kernel list events done (%zu events)", count);
end:
	ret = fclose(fp);	/* closes both fp and fd */
	if (ret) {
		PERROR("fclose");
	}
	return count;

error_fp:
	ret = close(fd);
	if (ret) {
		PERROR("close");
	}
error:
	return -1;
}

/*
 * Get kernel version and validate it.
 */
int kernel_validate_version(int tracer_fd,
		struct lttng_kernel_tracer_version *version,
		struct lttng_kernel_tracer_abi_version *abi_version)
{
	int ret;

	ret = kernctl_tracer_version(tracer_fd, version);
	if (ret < 0) {
		ERR("Failed to retrieve the lttng-modules version");
		goto error;
	}

	/* Validate version */
	if (version->major != VERSION_MAJOR) {
		ERR("Kernel tracer major version (%d) is not compatible with lttng-tools major version (%d)",
			version->major, VERSION_MAJOR);
		goto error_version;
	}
	ret = kernctl_tracer_abi_version(tracer_fd, abi_version);
	if (ret < 0) {
		ERR("Failed to retrieve lttng-modules ABI version");
		goto error;
	}
	if (abi_version->major != LTTNG_MODULES_ABI_MAJOR_VERSION) {
		ERR("Kernel tracer ABI version (%d.%d) does not match the expected ABI major version (%d.*)",
			abi_version->major, abi_version->minor,
			LTTNG_MODULES_ABI_MAJOR_VERSION);
		goto error;
	}
	DBG2("Kernel tracer version validated (%d.%d, ABI %d.%d)",
			version->major, version->minor,
			abi_version->major, abi_version->minor);
	return 0;

error_version:
	ret = -1;

error:
	ERR("Kernel tracer version check failed; kernel tracing will not be available");
	return ret;
}

/*
 * Kernel work-arounds called at the start of sessiond main().
 */
int init_kernel_workarounds(void)
{
	int ret;
	FILE *fp;

	/*
	 * boot_id needs to be read once before being used concurrently
	 * to deal with a Linux kernel race. A fix is proposed for
	 * upstream, but the work-around is needed for older kernels.
	 */
	fp = fopen("/proc/sys/kernel/random/boot_id", "r");
	if (!fp) {
		goto end_boot_id;
	}
	while (!feof(fp)) {
		char buf[37] = "";

		ret = fread(buf, 1, sizeof(buf), fp);
		if (ret < 0) {
			/* Ignore error, we don't really care */
		}
	}
	ret = fclose(fp);
	if (ret) {
		PERROR("fclose");
	}
end_boot_id:
	return 0;
}

/*
 * Complete teardown of a kernel session.
 */
void kernel_destroy_session(struct ltt_kernel_session *ksess)
{
	if (ksess == NULL) {
		DBG3("No kernel session when tearing down session");
		return;
	}

	DBG("Tearing down kernel session");

	/*
	 * Destroy channels on the consumer if at least one FD has been sent and we
	 * are in no output mode because the streams are in *no* monitor mode so we
	 * have to send a command to clean them up or else they leaked.
	 */
	if (!ksess->output_traces && ksess->consumer_fds_sent) {
		int ret;
		struct consumer_socket *socket;
		struct lttng_ht_iter iter;

		/* For each consumer socket. */
		rcu_read_lock();
		cds_lfht_for_each_entry(ksess->consumer->socks->ht, &iter.iter,
				socket, node.node) {
			struct ltt_kernel_channel *chan;

			/* For each channel, ask the consumer to destroy it. */
			cds_list_for_each_entry(chan, &ksess->channel_list.head, list) {
				ret = kernel_consumer_destroy_channel(socket, chan);
				if (ret < 0) {
					/* Consumer is probably dead. Use next socket. */
					continue;
				}
			}
		}
		rcu_read_unlock();
	}

	/* Close any relayd session */
	consumer_output_send_destroy_relayd(ksess->consumer);

	trace_kernel_destroy_session(ksess);
}

/*
 * Destroy a kernel channel object. It does not do anything on the tracer side.
 */
void kernel_destroy_channel(struct ltt_kernel_channel *kchan)
{
	struct ltt_kernel_session *ksess = NULL;

	assert(kchan);
	assert(kchan->channel);

	DBG3("Kernel destroy channel %s", kchan->channel->name);

	/* Update channel count of associated session. */
	if (kchan->session) {
		/* Keep pointer reference so we can update it after the destroy. */
		ksess = kchan->session;
	}

	trace_kernel_destroy_channel(kchan);

	/*
	 * At this point the kernel channel is not visible anymore. This is safe
	 * since in order to work on a visible kernel session, the tracing session
	 * lock (ltt_session.lock) MUST be acquired.
	 */
	if (ksess) {
		ksess->channel_count--;
	}
}

/*
 * Take a snapshot for a given kernel session.
 *
 * Return LTTNG_OK on success or else return a LTTNG_ERR code.
 */
enum lttng_error_code kernel_snapshot_record(struct ltt_kernel_session *ksess,
		struct snapshot_output *output, int wait,
		uint64_t nb_packets_per_stream)
{
	int err, ret, saved_metadata_fd;
	enum lttng_error_code status = LTTNG_OK;
	struct consumer_socket *socket;
	struct lttng_ht_iter iter;
	struct ltt_kernel_metadata *saved_metadata;
	struct ltt_session *session = NULL;
	uint64_t trace_archive_id;

	assert(ksess);
	assert(ksess->consumer);
	assert(output);

	DBG("Kernel snapshot record started");

	session = session_find_by_id(ksess->id);
	assert(session);
	assert(pthread_mutex_trylock(&session->lock));
	assert(session_trylock_list());
	trace_archive_id = session->current_archive_id;

	/* Save current metadata since the following calls will change it. */
	saved_metadata = ksess->metadata;
	saved_metadata_fd = ksess->metadata_stream_fd;

	rcu_read_lock();

	ret = kernel_open_metadata(ksess);
	if (ret < 0) {
		status = LTTNG_ERR_KERN_META_FAIL;
		goto error;
	}

	ret = kernel_open_metadata_stream(ksess);
	if (ret < 0) {
		status = LTTNG_ERR_KERN_META_FAIL;
		goto error_open_stream;
	}

	/* Send metadata to consumer and snapshot everything. */
	cds_lfht_for_each_entry(ksess->consumer->socks->ht, &iter.iter,
			socket, node.node) {
		struct consumer_output *saved_output;
		struct ltt_kernel_channel *chan;

		/*
		 * Temporarly switch consumer output for our snapshot output. As long
		 * as the session lock is taken, this is safe.
		 */
		saved_output = ksess->consumer;
		ksess->consumer = output->consumer;

		pthread_mutex_lock(socket->lock);
		/* This stream must not be monitored by the consumer. */
		ret = kernel_consumer_add_metadata(socket, ksess, 0);
		pthread_mutex_unlock(socket->lock);
		/* Put back the saved consumer output into the session. */
		ksess->consumer = saved_output;
		if (ret < 0) {
			status = LTTNG_ERR_KERN_META_FAIL;
			goto error_consumer;
		}

		/* For each channel, ask the consumer to snapshot it. */
		cds_list_for_each_entry(chan, &ksess->channel_list.head, list) {
			status = consumer_snapshot_channel(socket, chan->key, output, 0,
					ksess->uid, ksess->gid,
					DEFAULT_KERNEL_TRACE_DIR, wait,
					nb_packets_per_stream,
					trace_archive_id);
			if (status != LTTNG_OK) {
				(void) kernel_consumer_destroy_metadata(socket,
						ksess->metadata);
				goto error_consumer;
			}
		}

		/* Snapshot metadata, */
		status = consumer_snapshot_channel(socket, ksess->metadata->key, output,
				1, ksess->uid, ksess->gid,
				DEFAULT_KERNEL_TRACE_DIR, wait, 0,
				trace_archive_id);
		if (status != LTTNG_OK) {
			goto error_consumer;
		}

		/*
		 * The metadata snapshot is done, ask the consumer to destroy it since
		 * it's not monitored on the consumer side.
		 */
		(void) kernel_consumer_destroy_metadata(socket, ksess->metadata);
	}

error_consumer:
	/* Close newly opened metadata stream. It's now on the consumer side. */
	err = close(ksess->metadata_stream_fd);
	if (err < 0) {
		PERROR("close snapshot kernel");
	}

error_open_stream:
	trace_kernel_destroy_metadata(ksess->metadata);
error:
	/* Restore metadata state.*/
	ksess->metadata = saved_metadata;
	ksess->metadata_stream_fd = saved_metadata_fd;
	if (session) {
		session_put(session);
	}
	rcu_read_unlock();
	return status;
}

/*
 * Get the syscall mask array from the kernel tracer.
 *
 * Return 0 on success else a negative value. In both case, syscall_mask should
 * be freed.
 */
int kernel_syscall_mask(int chan_fd, char **syscall_mask, uint32_t *nr_bits)
{
	assert(syscall_mask);
	assert(nr_bits);

	return kernctl_syscall_mask(chan_fd, syscall_mask, nr_bits);
}

/*
 * Check for the support of the RING_BUFFER_SNAPSHOT_SAMPLE_POSITIONS via abi
 * version number.
 *
 * Return 1 on success, 0 when feature is not supported, negative value in case
 * of errors.
 */
int kernel_supports_ring_buffer_snapshot_sample_positions(int tracer_fd)
{
	int ret = 0; // Not supported by default
	struct lttng_kernel_tracer_abi_version abi;

	ret = kernctl_tracer_abi_version(tracer_fd, &abi);
	if (ret < 0) {
		ERR("Failed to retrieve lttng-modules ABI version");
		goto error;
	}

	/*
	 * RING_BUFFER_SNAPSHOT_SAMPLE_POSITIONS was introduced in 2.3
	 */
	if (abi.major >= 2 && abi.minor >= 3) {
		/* Supported */
		ret = 1;
	} else {
		/* Not supported */
		ret = 0;
	}
error:
	return ret;
}

/*
 * Rotate a kernel session.
 *
 * Return LTTNG_OK on success or else an LTTng error code.
 */
enum lttng_error_code kernel_rotate_session(struct ltt_session *session)
{
	int ret;
	enum lttng_error_code status = LTTNG_OK;
	struct consumer_socket *socket;
	struct lttng_ht_iter iter;
	struct ltt_kernel_session *ksess = session->kernel_session;

	assert(ksess);
	assert(ksess->consumer);

	DBG("Rotate kernel session %s started (session %" PRIu64 ")",
			session->name, session->id);

	rcu_read_lock();

	/*
	 * Note that this loop will end after one iteration given that there is
	 * only one kernel consumer.
	 */
	cds_lfht_for_each_entry(ksess->consumer->socks->ht, &iter.iter,
			socket, node.node) {
		struct ltt_kernel_channel *chan;

		/* For each channel, ask the consumer to rotate it. */
		cds_list_for_each_entry(chan, &ksess->channel_list.head, list) {
			DBG("Rotate kernel channel %" PRIu64 ", session %s",
					chan->key, session->name);
			ret = consumer_rotate_channel(socket, chan->key,
					ksess->uid, ksess->gid, ksess->consumer,
					ksess->consumer->domain_subdir,
					/* is_metadata_channel */ false,
					session->current_archive_id);
			if (ret < 0) {
				status = LTTNG_ERR_KERN_CONSUMER_FAIL;
				goto error;
			}
		}

		/*
		 * Rotate the metadata channel.
		 */
		ret = consumer_rotate_channel(socket, ksess->metadata->key,
				ksess->uid, ksess->gid, ksess->consumer,
				ksess->consumer->domain_subdir,
				/* is_metadata_channel */ true,
				session->current_archive_id);
		if (ret < 0) {
			status = LTTNG_ERR_KERN_CONSUMER_FAIL;
			goto error;
		}
	}

error:
	rcu_read_unlock();
	return status;
}
