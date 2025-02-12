/*
 * Copyright (C) 2012 - David Goulet <dgoulet@efficios.com>
 *               2018 - Jérémie Galarneau <jeremie.galarneau@efficios.com>
 *
 * This program is free software; you can redistribute it and/or modify it
 * under the terms of the GNU General Public License, version 2 only, as
 * published by the Free Software Foundation.
 *
 * This program is distributed in the hope that it will be useful, but WITHOUT
 * ANY WARRANTY; without even the implied warranty of MERCHANTABILITY or
 * FITNESS FOR A PARTICULAR PURPOSE.  See the GNU General Public License for
 * more details.
 *
 * You should have received a copy of the GNU General Public License along with
 * this program; if not, write to the Free Software Foundation, Inc., 51
 * Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
 */

#define _LGPL_SOURCE
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#include <inttypes.h>

#include <common/common.h>
#include <common/defaults.h>
#include <common/uri.h>
#include <common/relayd/relayd.h>

#include "consumer.h"
#include "health-sessiond.h"
#include "ust-app.h"
#include "utils.h"

/*
 * Send a data payload using a given consumer socket of size len.
 *
 * The consumer socket lock MUST be acquired before calling this since this
 * function can change the fd value.
 *
 * Return 0 on success else a negative value on error.
 */
int consumer_socket_send(struct consumer_socket *socket, void *msg, size_t len)
{
	int fd;
	ssize_t size;

	assert(socket);
	assert(socket->fd_ptr);
	assert(msg);

	/* Consumer socket is invalid. Stopping. */
	fd = *socket->fd_ptr;
	if (fd < 0) {
		goto error;
	}

	size = lttcomm_send_unix_sock(fd, msg, len);
	if (size < 0) {
		/* The above call will print a PERROR on error. */
		DBG("Error when sending data to consumer on sock %d", fd);
		/*
		 * At this point, the socket is not usable anymore thus closing it and
		 * setting the file descriptor to -1 so it is not reused.
		 */

		/* This call will PERROR on error. */
		(void) lttcomm_close_unix_sock(fd);
		*socket->fd_ptr = -1;
		goto error;
	}

	return 0;

error:
	return -1;
}

/*
 * Receive a data payload using a given consumer socket of size len.
 *
 * The consumer socket lock MUST be acquired before calling this since this
 * function can change the fd value.
 *
 * Return 0 on success else a negative value on error.
 */
int consumer_socket_recv(struct consumer_socket *socket, void *msg, size_t len)
{
	int fd;
	ssize_t size;

	assert(socket);
	assert(socket->fd_ptr);
	assert(msg);

	/* Consumer socket is invalid. Stopping. */
	fd = *socket->fd_ptr;
	if (fd < 0) {
		goto error;
	}

	size = lttcomm_recv_unix_sock(fd, msg, len);
	if (size <= 0) {
		/* The above call will print a PERROR on error. */
		DBG("Error when receiving data from the consumer socket %d", fd);
		/*
		 * At this point, the socket is not usable anymore thus closing it and
		 * setting the file descriptor to -1 so it is not reused.
		 */

		/* This call will PERROR on error. */
		(void) lttcomm_close_unix_sock(fd);
		*socket->fd_ptr = -1;
		goto error;
	}

	return 0;

error:
	return -1;
}

/*
 * Receive a reply command status message from the consumer. Consumer socket
 * lock MUST be acquired before calling this function.
 *
 * Return 0 on success, -1 on recv error or a negative lttng error code which
 * was possibly returned by the consumer.
 */
int consumer_recv_status_reply(struct consumer_socket *sock)
{
	int ret;
	struct lttcomm_consumer_status_msg reply;

	assert(sock);

	ret = consumer_socket_recv(sock, &reply, sizeof(reply));
	if (ret < 0) {
		goto end;
	}

	if (reply.ret_code == LTTCOMM_CONSUMERD_SUCCESS) {
		/* All good. */
		ret = 0;
	} else {
		ret = -reply.ret_code;
		DBG("Consumer ret code %d", ret);
	}

end:
	return ret;
}

/*
 * Once the ASK_CHANNEL command is sent to the consumer, the channel
 * information are sent back. This call receives that data and populates key
 * and stream_count.
 *
 * On success return 0 and both key and stream_count are set. On error, a
 * negative value is sent back and both parameters are untouched.
 */
int consumer_recv_status_channel(struct consumer_socket *sock,
		uint64_t *key, unsigned int *stream_count)
{
	int ret;
	struct lttcomm_consumer_status_channel reply;

	assert(sock);
	assert(stream_count);
	assert(key);

	ret = consumer_socket_recv(sock, &reply, sizeof(reply));
	if (ret < 0) {
		goto end;
	}

	/* An error is possible so don't touch the key and stream_count. */
	if (reply.ret_code != LTTCOMM_CONSUMERD_SUCCESS) {
		ret = -1;
		goto end;
	}

	*key = reply.key;
	*stream_count = reply.stream_count;
	ret = 0;

end:
	return ret;
}

/*
 * Send destroy relayd command to consumer.
 *
 * On success return positive value. On error, negative value.
 */
int consumer_send_destroy_relayd(struct consumer_socket *sock,
		struct consumer_output *consumer)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(consumer);
	assert(sock);

	DBG2("Sending destroy relayd command to consumer sock %d", *sock->fd_ptr);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_DESTROY_RELAYD;
	msg.u.destroy_relayd.net_seq_idx = consumer->net_seq_index;

	pthread_mutex_lock(sock->lock);
	ret = consumer_socket_send(sock, &msg, sizeof(msg));
	if (ret < 0) {
		goto error;
	}

	/* Don't check the return value. The caller will do it. */
	ret = consumer_recv_status_reply(sock);

	DBG2("Consumer send destroy relayd command done");

error:
	pthread_mutex_unlock(sock->lock);
	return ret;
}

/*
 * For each consumer socket in the consumer output object, send a destroy
 * relayd command.
 */
void consumer_output_send_destroy_relayd(struct consumer_output *consumer)
{
	struct lttng_ht_iter iter;
	struct consumer_socket *socket;

	assert(consumer);

	/* Destroy any relayd connection */
	if (consumer->type == CONSUMER_DST_NET) {
		rcu_read_lock();
		cds_lfht_for_each_entry(consumer->socks->ht, &iter.iter, socket,
				node.node) {
			int ret;

			/* Send destroy relayd command */
			ret = consumer_send_destroy_relayd(socket, consumer);
			if (ret < 0) {
				DBG("Unable to send destroy relayd command to consumer");
				/* Continue since we MUST delete everything at this point. */
			}
		}
		rcu_read_unlock();
	}
}

/*
 * From a consumer_data structure, allocate and add a consumer socket to the
 * consumer output.
 *
 * Return 0 on success, else negative value on error
 */
int consumer_create_socket(struct consumer_data *data,
		struct consumer_output *output)
{
	int ret = 0;
	struct consumer_socket *socket;

	assert(data);

	if (output == NULL || data->cmd_sock < 0) {
		/*
		 * Not an error. Possible there is simply not spawned consumer or it's
		 * disabled for the tracing session asking the socket.
		 */
		goto error;
	}

	rcu_read_lock();
	socket = consumer_find_socket(data->cmd_sock, output);
	rcu_read_unlock();
	if (socket == NULL) {
		socket = consumer_allocate_socket(&data->cmd_sock);
		if (socket == NULL) {
			ret = -1;
			goto error;
		}

		socket->registered = 0;
		socket->lock = &data->lock;
		rcu_read_lock();
		consumer_add_socket(socket, output);
		rcu_read_unlock();
	}

	socket->type = data->type;

	DBG3("Consumer socket created (fd: %d) and added to output",
			data->cmd_sock);

error:
	return ret;
}

/*
 * Return the consumer socket from the given consumer output with the right
 * bitness. On error, returns NULL.
 *
 * The caller MUST acquire a rcu read side lock and keep it until the socket
 * object reference is not needed anymore.
 */
struct consumer_socket *consumer_find_socket_by_bitness(int bits,
		struct consumer_output *consumer)
{
	int consumer_fd;
	struct consumer_socket *socket = NULL;

	switch (bits) {
	case 64:
		consumer_fd = uatomic_read(&ust_consumerd64_fd);
		break;
	case 32:
		consumer_fd = uatomic_read(&ust_consumerd32_fd);
		break;
	default:
		assert(0);
		goto end;
	}

	socket = consumer_find_socket(consumer_fd, consumer);
	if (!socket) {
		ERR("Consumer socket fd %d not found in consumer obj %p",
				consumer_fd, consumer);
	}

end:
	return socket;
}

/*
 * Find a consumer_socket in a consumer_output hashtable. Read side lock must
 * be acquired before calling this function and across use of the
 * returned consumer_socket.
 */
struct consumer_socket *consumer_find_socket(int key,
		struct consumer_output *consumer)
{
	struct lttng_ht_iter iter;
	struct lttng_ht_node_ulong *node;
	struct consumer_socket *socket = NULL;

	/* Negative keys are lookup failures */
	if (key < 0 || consumer == NULL) {
		return NULL;
	}

	lttng_ht_lookup(consumer->socks, (void *)((unsigned long) key),
			&iter);
	node = lttng_ht_iter_get_node_ulong(&iter);
	if (node != NULL) {
		socket = caa_container_of(node, struct consumer_socket, node);
	}

	return socket;
}

/*
 * Allocate a new consumer_socket and return the pointer.
 */
struct consumer_socket *consumer_allocate_socket(int *fd)
{
	struct consumer_socket *socket = NULL;

	assert(fd);

	socket = zmalloc(sizeof(struct consumer_socket));
	if (socket == NULL) {
		PERROR("zmalloc consumer socket");
		goto error;
	}

	socket->fd_ptr = fd;
	lttng_ht_node_init_ulong(&socket->node, *fd);

error:
	return socket;
}

/*
 * Add consumer socket to consumer output object. Read side lock must be
 * acquired before calling this function.
 */
void consumer_add_socket(struct consumer_socket *sock,
		struct consumer_output *consumer)
{
	assert(sock);
	assert(consumer);

	lttng_ht_add_unique_ulong(consumer->socks, &sock->node);
}

/*
 * Delte consumer socket to consumer output object. Read side lock must be
 * acquired before calling this function.
 */
void consumer_del_socket(struct consumer_socket *sock,
		struct consumer_output *consumer)
{
	int ret;
	struct lttng_ht_iter iter;

	assert(sock);
	assert(consumer);

	iter.iter.node = &sock->node.node;
	ret = lttng_ht_del(consumer->socks, &iter);
	assert(!ret);
}

/*
 * RCU destroy call function.
 */
static void destroy_socket_rcu(struct rcu_head *head)
{
	struct lttng_ht_node_ulong *node =
		caa_container_of(head, struct lttng_ht_node_ulong, head);
	struct consumer_socket *socket =
		caa_container_of(node, struct consumer_socket, node);

	free(socket);
}

/*
 * Destroy and free socket pointer in a call RCU. Read side lock must be
 * acquired before calling this function.
 */
void consumer_destroy_socket(struct consumer_socket *sock)
{
	assert(sock);

	/*
	 * We DO NOT close the file descriptor here since it is global to the
	 * session daemon and is closed only if the consumer dies or a custom
	 * consumer was registered,
	 */
	if (sock->registered) {
		DBG3("Consumer socket was registered. Closing fd %d", *sock->fd_ptr);
		lttcomm_close_unix_sock(*sock->fd_ptr);
	}

	call_rcu(&sock->node.head, destroy_socket_rcu);
}

/*
 * Allocate and assign data to a consumer_output object.
 *
 * Return pointer to structure.
 */
struct consumer_output *consumer_create_output(enum consumer_dst_type type)
{
	struct consumer_output *output = NULL;

	output = zmalloc(sizeof(struct consumer_output));
	if (output == NULL) {
		PERROR("zmalloc consumer_output");
		goto error;
	}

	/* By default, consumer output is enabled */
	output->enabled = 1;
	output->type = type;
	output->net_seq_index = (uint64_t) -1ULL;
	urcu_ref_init(&output->ref);

	output->socks = lttng_ht_new(0, LTTNG_HT_TYPE_ULONG);

error:
	return output;
}

/*
 * Iterate over the consumer output socket hash table and destroy them. The
 * socket file descriptor are only closed if the consumer output was
 * registered meaning it's an external consumer.
 */
void consumer_destroy_output_sockets(struct consumer_output *obj)
{
	struct lttng_ht_iter iter;
	struct consumer_socket *socket;

	if (!obj->socks) {
		return;
	}

	rcu_read_lock();
	cds_lfht_for_each_entry(obj->socks->ht, &iter.iter, socket, node.node) {
		consumer_del_socket(socket, obj);
		consumer_destroy_socket(socket);
	}
	rcu_read_unlock();
}

/*
 * Delete the consumer_output object from the list and free the ptr.
 *
 * Should *NOT* be called with RCU read-side lock held.
 */
static void consumer_release_output(struct urcu_ref *ref)
{
	struct consumer_output *obj =
		caa_container_of(ref, struct consumer_output, ref);

	consumer_destroy_output_sockets(obj);

	if (obj->socks) {
		/* Finally destroy HT */
		ht_cleanup_push(obj->socks);
	}

	free(obj);
}

/*
 * Get the consumer_output object.
 */
void consumer_output_get(struct consumer_output *obj)
{
	urcu_ref_get(&obj->ref);
}

/*
 * Put the consumer_output object.
 *
 * Should *NOT* be called with RCU read-side lock held.
 */
void consumer_output_put(struct consumer_output *obj)
{
	if (!obj) {
		return;
	}
	urcu_ref_put(&obj->ref, consumer_release_output);
}

/*
 * Copy consumer output and returned the newly allocated copy.
 *
 * Should *NOT* be called with RCU read-side lock held.
 */
struct consumer_output *consumer_copy_output(struct consumer_output *src)
{
	int ret;
	struct consumer_output *output;

	assert(src);

	output = consumer_create_output(src->type);
	if (output == NULL) {
		goto end;
	}
	output->enabled = src->enabled;
	output->net_seq_index = src->net_seq_index;
	memcpy(output->domain_subdir, src->domain_subdir,
			sizeof(output->domain_subdir));
	output->snapshot = src->snapshot;
	output->relay_major_version = src->relay_major_version;
	output->relay_minor_version = src->relay_minor_version;
	memcpy(&output->dst, &src->dst, sizeof(output->dst));
	ret = consumer_copy_sockets(output, src);
	if (ret < 0) {
		goto error_put;
	}
end:
	return output;

error_put:
	consumer_output_put(output);
	return NULL;
}

/*
 * Copy consumer sockets from src to dst.
 *
 * Return 0 on success or else a negative value.
 */
int consumer_copy_sockets(struct consumer_output *dst,
		struct consumer_output *src)
{
	int ret = 0;
	struct lttng_ht_iter iter;
	struct consumer_socket *socket, *copy_sock;

	assert(dst);
	assert(src);

	rcu_read_lock();
	cds_lfht_for_each_entry(src->socks->ht, &iter.iter, socket, node.node) {
		/* Ignore socket that are already there. */
		copy_sock = consumer_find_socket(*socket->fd_ptr, dst);
		if (copy_sock) {
			continue;
		}

		/* Create new socket object. */
		copy_sock = consumer_allocate_socket(socket->fd_ptr);
		if (copy_sock == NULL) {
			rcu_read_unlock();
			ret = -ENOMEM;
			goto error;
		}

		copy_sock->registered = socket->registered;
		/*
		 * This is valid because this lock is shared accross all consumer
		 * object being the global lock of the consumer data structure of the
		 * session daemon.
		 */
		copy_sock->lock = socket->lock;
		consumer_add_socket(copy_sock, dst);
	}
	rcu_read_unlock();

error:
	return ret;
}

/*
 * Set network URI to the consumer output.
 *
 * Return 0 on success. Return 1 if the URI were equal. Else, negative value on
 * error.
 */
int consumer_set_network_uri(const struct ltt_session *session,
		struct consumer_output *output,
		struct lttng_uri *uri)
{
	int ret;
	struct lttng_uri *dst_uri = NULL;

	/* Code flow error safety net. */
	assert(output);
	assert(uri);

	switch (uri->stype) {
	case LTTNG_STREAM_CONTROL:
		dst_uri = &output->dst.net.control;
		output->dst.net.control_isset = 1;
		if (uri->port == 0) {
			/* Assign default port. */
			uri->port = DEFAULT_NETWORK_CONTROL_PORT;
		} else {
			if (output->dst.net.data_isset && uri->port ==
					output->dst.net.data.port) {
				ret = -LTTNG_ERR_INVALID;
				goto error;
			}
		}
		DBG3("Consumer control URI set with port %d", uri->port);
		break;
	case LTTNG_STREAM_DATA:
		dst_uri = &output->dst.net.data;
		output->dst.net.data_isset = 1;
		if (uri->port == 0) {
			/* Assign default port. */
			uri->port = DEFAULT_NETWORK_DATA_PORT;
		} else {
			if (output->dst.net.control_isset && uri->port ==
					output->dst.net.control.port) {
				ret = -LTTNG_ERR_INVALID;
				goto error;
			}
		}
		DBG3("Consumer data URI set with port %d", uri->port);
		break;
	default:
		ERR("Set network uri type unknown %d", uri->stype);
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	ret = uri_compare(dst_uri, uri);
	if (!ret) {
		/* Same URI, don't touch it and return success. */
		DBG3("URI network compare are the same");
		goto equal;
	}

	/* URIs were not equal, replacing it. */
	memcpy(dst_uri, uri, sizeof(struct lttng_uri));
	output->type = CONSUMER_DST_NET;
	if (dst_uri->stype != LTTNG_STREAM_CONTROL) {
		/* Only the control uri needs to contain the path. */
		goto end;
	}

	/*
	 * If the user has specified a subdir as part of the control
	 * URL, the session's base output directory is:
	 *   /RELAYD_OUTPUT_PATH/HOSTNAME/USER_SPECIFIED_DIR
	 *
	 * Hence, the "base_dir" from which all stream files and
	 * session rotation chunks are created takes the form
	 *   /HOSTNAME/USER_SPECIFIED_DIR
	 *
	 * If the user has not specified an output directory as part of
	 * the control URL, the base output directory has the form:
	 *   /RELAYD_OUTPUT_PATH/HOSTNAME/SESSION_NAME-CREATION_TIME
	 *
	 * Hence, the "base_dir" from which all stream files and
	 * session rotation chunks are created takes the form
	 *   /HOSTNAME/SESSION_NAME-CREATION_TIME
	 *
	 * Note that automatically generated session names already
	 * contain the session's creation time. In that case, the
	 * creation time is omitted to prevent it from being duplicated
	 * in the final directory hierarchy.
	 */
	if (*uri->subdir) {
		if (strstr(uri->subdir, "../")) {
			ERR("Network URI subdirs are not allowed to walk up the path hierarchy");
			ret = -LTTNG_ERR_INVALID;
			goto error;
		}
		ret = snprintf(output->dst.net.base_dir,
				sizeof(output->dst.net.base_dir),
				"/%s/%s/", session->hostname, uri->subdir);
	} else {
		if (session->has_auto_generated_name) {
			ret = snprintf(output->dst.net.base_dir,
					sizeof(output->dst.net.base_dir),
					"/%s/%s/", session->hostname,
					session->name);
		} else {
			char session_creation_datetime[16];
			size_t strftime_ret;
			struct tm *timeinfo;

			timeinfo = localtime(&session->creation_time);
			if (!timeinfo) {
				ret = -LTTNG_ERR_FATAL;
				goto error;
			}
			strftime_ret = strftime(session_creation_datetime,
					sizeof(session_creation_datetime),
					"%Y%m%d-%H%M%S", timeinfo);
			if (strftime_ret == 0) {
				ERR("Failed to format session creation timestamp while setting network URI");
				ret = -LTTNG_ERR_FATAL;
				goto error;
			}
			ret = snprintf(output->dst.net.base_dir,
					sizeof(output->dst.net.base_dir),
					"/%s/%s-%s/", session->hostname,
					session->name,
					session_creation_datetime);
		}
	}
	if (ret >= sizeof(output->dst.net.base_dir)) {
		ret = -LTTNG_ERR_INVALID;
		ERR("Truncation occurred while setting network output base directory");
		goto error;
	} else if (ret == -1) {
		ret = -LTTNG_ERR_INVALID;
		PERROR("Error occurred while setting network output base directory");
		goto error;
	}

	DBG3("Consumer set network uri base_dir path %s",
			output->dst.net.base_dir);

end:
	return 0;
equal:
	return 1;
error:
	return ret;
}

/*
 * Send file descriptor to consumer via sock.
 *
 * The consumer socket lock must be held by the caller.
 */
int consumer_send_fds(struct consumer_socket *sock, const int *fds,
		size_t nb_fd)
{
	int ret;

	assert(fds);
	assert(sock);
	assert(nb_fd > 0);
	assert(pthread_mutex_trylock(sock->lock) == EBUSY);

	ret = lttcomm_send_fds_unix_sock(*sock->fd_ptr, fds, nb_fd);
	if (ret < 0) {
		/* The above call will print a PERROR on error. */
		DBG("Error when sending consumer fds on sock %d", *sock->fd_ptr);
		goto error;
	}

	ret = consumer_recv_status_reply(sock);
error:
	return ret;
}

/*
 * Consumer send communication message structure to consumer.
 *
 * The consumer socket lock must be held by the caller.
 */
int consumer_send_msg(struct consumer_socket *sock,
		struct lttcomm_consumer_msg *msg)
{
	int ret;

	assert(msg);
	assert(sock);
	assert(pthread_mutex_trylock(sock->lock) == EBUSY);

	ret = consumer_socket_send(sock, msg, sizeof(struct lttcomm_consumer_msg));
	if (ret < 0) {
		goto error;
	}

	ret = consumer_recv_status_reply(sock);

error:
	return ret;
}

/*
 * Consumer send channel communication message structure to consumer.
 *
 * The consumer socket lock must be held by the caller.
 */
int consumer_send_channel(struct consumer_socket *sock,
		struct lttcomm_consumer_msg *msg)
{
	int ret;

	assert(msg);
	assert(sock);

	ret = consumer_send_msg(sock, msg);
	if (ret < 0) {
		goto error;
	}

error:
	return ret;
}

/*
 * Populate the given consumer msg structure with the ask_channel command
 * information.
 */
void consumer_init_ask_channel_comm_msg(struct lttcomm_consumer_msg *msg,
		uint64_t subbuf_size,
		uint64_t num_subbuf,
		int overwrite,
		unsigned int switch_timer_interval,
		unsigned int read_timer_interval,
		unsigned int live_timer_interval,
		unsigned int monitor_timer_interval,
		int output,
		int type,
		uint64_t session_id,
		const char *pathname,
		const char *name,
		uid_t uid,
		gid_t gid,
		uint64_t relayd_id,
		uint64_t key,
		unsigned char *uuid,
		uint32_t chan_id,
		uint64_t tracefile_size,
		uint64_t tracefile_count,
		uint64_t session_id_per_pid,
		unsigned int monitor,
		uint32_t ust_app_uid,
		int64_t blocking_timeout,
		const char *root_shm_path,
		const char *shm_path,
		uint64_t trace_archive_id)
{
	assert(msg);

	/* Zeroed structure */
	memset(msg, 0, sizeof(struct lttcomm_consumer_msg));

	msg->cmd_type = LTTNG_CONSUMER_ASK_CHANNEL_CREATION;
	msg->u.ask_channel.subbuf_size = subbuf_size;
	msg->u.ask_channel.num_subbuf = num_subbuf ;
	msg->u.ask_channel.overwrite = overwrite;
	msg->u.ask_channel.switch_timer_interval = switch_timer_interval;
	msg->u.ask_channel.read_timer_interval = read_timer_interval;
	msg->u.ask_channel.live_timer_interval = live_timer_interval;
	msg->u.ask_channel.monitor_timer_interval = monitor_timer_interval;
	msg->u.ask_channel.output = output;
	msg->u.ask_channel.type = type;
	msg->u.ask_channel.session_id = session_id;
	msg->u.ask_channel.session_id_per_pid = session_id_per_pid;
	msg->u.ask_channel.uid = uid;
	msg->u.ask_channel.gid = gid;
	msg->u.ask_channel.relayd_id = relayd_id;
	msg->u.ask_channel.key = key;
	msg->u.ask_channel.chan_id = chan_id;
	msg->u.ask_channel.tracefile_size = tracefile_size;
	msg->u.ask_channel.tracefile_count = tracefile_count;
	msg->u.ask_channel.monitor = monitor;
	msg->u.ask_channel.ust_app_uid = ust_app_uid;
	msg->u.ask_channel.blocking_timeout = blocking_timeout;
	msg->u.ask_channel.trace_archive_id = trace_archive_id;

	memcpy(msg->u.ask_channel.uuid, uuid, sizeof(msg->u.ask_channel.uuid));

	if (pathname) {
		strncpy(msg->u.ask_channel.pathname, pathname,
				sizeof(msg->u.ask_channel.pathname));
		msg->u.ask_channel.pathname[sizeof(msg->u.ask_channel.pathname)-1] = '\0';
	}

	strncpy(msg->u.ask_channel.name, name, sizeof(msg->u.ask_channel.name));
	msg->u.ask_channel.name[sizeof(msg->u.ask_channel.name) - 1] = '\0';

	if (root_shm_path) {
		strncpy(msg->u.ask_channel.root_shm_path, root_shm_path,
			sizeof(msg->u.ask_channel.root_shm_path));
		msg->u.ask_channel.root_shm_path[sizeof(msg->u.ask_channel.root_shm_path) - 1] = '\0';
	}
	if (shm_path) {
		strncpy(msg->u.ask_channel.shm_path, shm_path,
			sizeof(msg->u.ask_channel.shm_path));
		msg->u.ask_channel.shm_path[sizeof(msg->u.ask_channel.shm_path) - 1] = '\0';
	}
}

/*
 * Init channel communication message structure.
 */
void consumer_init_add_channel_comm_msg(struct lttcomm_consumer_msg *msg,
		uint64_t channel_key,
		uint64_t session_id,
		const char *pathname,
		uid_t uid,
		gid_t gid,
		uint64_t relayd_id,
		const char *name,
		unsigned int nb_init_streams,
		enum lttng_event_output output,
		int type,
		uint64_t tracefile_size,
		uint64_t tracefile_count,
		unsigned int monitor,
		unsigned int live_timer_interval,
		unsigned int monitor_timer_interval)
{
	assert(msg);

	/* Zeroed structure */
	memset(msg, 0, sizeof(struct lttcomm_consumer_msg));

	/* Send channel */
	msg->cmd_type = LTTNG_CONSUMER_ADD_CHANNEL;
	msg->u.channel.channel_key = channel_key;
	msg->u.channel.session_id = session_id;
	msg->u.channel.uid = uid;
	msg->u.channel.gid = gid;
	msg->u.channel.relayd_id = relayd_id;
	msg->u.channel.nb_init_streams = nb_init_streams;
	msg->u.channel.output = output;
	msg->u.channel.type = type;
	msg->u.channel.tracefile_size = tracefile_size;
	msg->u.channel.tracefile_count = tracefile_count;
	msg->u.channel.monitor = monitor;
	msg->u.channel.live_timer_interval = live_timer_interval;
	msg->u.channel.monitor_timer_interval = monitor_timer_interval;

	strncpy(msg->u.channel.pathname, pathname,
			sizeof(msg->u.channel.pathname));
	msg->u.channel.pathname[sizeof(msg->u.channel.pathname) - 1] = '\0';

	strncpy(msg->u.channel.name, name, sizeof(msg->u.channel.name));
	msg->u.channel.name[sizeof(msg->u.channel.name) - 1] = '\0';
}

/*
 * Init stream communication message structure.
 */
void consumer_init_add_stream_comm_msg(struct lttcomm_consumer_msg *msg,
		uint64_t channel_key,
		uint64_t stream_key,
		int32_t cpu,
		uint64_t trace_archive_id)
{
	assert(msg);

	memset(msg, 0, sizeof(struct lttcomm_consumer_msg));

	msg->cmd_type = LTTNG_CONSUMER_ADD_STREAM;
	msg->u.stream.channel_key = channel_key;
	msg->u.stream.stream_key = stream_key;
	msg->u.stream.cpu = cpu;
	msg->u.stream.trace_archive_id = trace_archive_id;
}

void consumer_init_streams_sent_comm_msg(struct lttcomm_consumer_msg *msg,
		enum lttng_consumer_command cmd,
		uint64_t channel_key, uint64_t net_seq_idx)
{
	assert(msg);

	memset(msg, 0, sizeof(struct lttcomm_consumer_msg));

	msg->cmd_type = cmd;
	msg->u.sent_streams.channel_key = channel_key;
	msg->u.sent_streams.net_seq_idx = net_seq_idx;
}

/*
 * Send stream communication structure to the consumer.
 */
int consumer_send_stream(struct consumer_socket *sock,
		struct consumer_output *dst, struct lttcomm_consumer_msg *msg,
		const int *fds, size_t nb_fd)
{
	int ret;

	assert(msg);
	assert(dst);
	assert(sock);
	assert(fds);

	ret = consumer_send_msg(sock, msg);
	if (ret < 0) {
		goto error;
	}

	ret = consumer_send_fds(sock, fds, nb_fd);
	if (ret < 0) {
		goto error;
	}

error:
	return ret;
}

/*
 * Send relayd socket to consumer associated with a session name.
 *
 * The consumer socket lock must be held by the caller.
 *
 * On success return positive value. On error, negative value.
 */
int consumer_send_relayd_socket(struct consumer_socket *consumer_sock,
		struct lttcomm_relayd_sock *rsock, struct consumer_output *consumer,
		enum lttng_stream_type type, uint64_t session_id,
		char *session_name, char *hostname, int session_live_timer)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	/* Code flow error. Safety net. */
	assert(rsock);
	assert(consumer);
	assert(consumer_sock);

	memset(&msg, 0, sizeof(msg));
	/* Bail out if consumer is disabled */
	if (!consumer->enabled) {
		ret = LTTNG_OK;
		goto error;
	}

	if (type == LTTNG_STREAM_CONTROL) {
		ret = relayd_create_session(rsock,
				&msg.u.relayd_sock.relayd_session_id,
				session_name, hostname, session_live_timer,
				consumer->snapshot);
		if (ret < 0) {
			/* Close the control socket. */
			(void) relayd_close(rsock);
			goto error;
		}
	}

	msg.cmd_type = LTTNG_CONSUMER_ADD_RELAYD_SOCKET;
	/*
	 * Assign network consumer output index using the temporary consumer since
	 * this call should only be made from within a set_consumer_uri() function
	 * call in the session daemon.
	 */
	msg.u.relayd_sock.net_index = consumer->net_seq_index;
	msg.u.relayd_sock.type = type;
	msg.u.relayd_sock.session_id = session_id;
	memcpy(&msg.u.relayd_sock.sock, rsock, sizeof(msg.u.relayd_sock.sock));

	DBG3("Sending relayd sock info to consumer on %d", *consumer_sock->fd_ptr);
	ret = consumer_send_msg(consumer_sock, &msg);
	if (ret < 0) {
		goto error;
	}

	DBG3("Sending relayd socket file descriptor to consumer");
	ret = consumer_send_fds(consumer_sock, &rsock->sock.fd, 1);
	if (ret < 0) {
		goto error;
	}

	DBG2("Consumer relayd socket sent");

error:
	return ret;
}

static
int consumer_send_pipe(struct consumer_socket *consumer_sock,
		enum lttng_consumer_command cmd, int pipe)
{
	int ret;
	struct lttcomm_consumer_msg msg;
	const char *pipe_name;
	const char *command_name;

	switch (cmd) {
	case LTTNG_CONSUMER_SET_CHANNEL_MONITOR_PIPE:
		pipe_name = "channel monitor";
		command_name = "SET_CHANNEL_MONITOR_PIPE";
		break;
	default:
		ERR("Unexpected command received in %s (cmd = %d)", __func__,
				(int) cmd);
		abort();
	}

	/* Code flow error. Safety net. */

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = cmd;

	pthread_mutex_lock(consumer_sock->lock);
	DBG3("Sending %s command to consumer", command_name);
	ret = consumer_send_msg(consumer_sock, &msg);
	if (ret < 0) {
		goto error;
	}

	DBG3("Sending %s pipe %d to consumer on socket %d",
			pipe_name,
			pipe, *consumer_sock->fd_ptr);
	ret = consumer_send_fds(consumer_sock, &pipe, 1);
	if (ret < 0) {
		goto error;
	}

	DBG2("%s pipe successfully sent", pipe_name);
error:
	pthread_mutex_unlock(consumer_sock->lock);
	return ret;
}

int consumer_send_channel_monitor_pipe(struct consumer_socket *consumer_sock,
		int pipe)
{
	return consumer_send_pipe(consumer_sock,
			LTTNG_CONSUMER_SET_CHANNEL_MONITOR_PIPE, pipe);
}

/*
 * Ask the consumer if the data is pending for the specific session id.
 * Returns 1 if data is pending, 0 otherwise, or < 0 on error.
 */
int consumer_is_data_pending(uint64_t session_id,
		struct consumer_output *consumer)
{
	int ret;
	int32_t ret_code = 0;  /* Default is that the data is NOT pending */
	struct consumer_socket *socket;
	struct lttng_ht_iter iter;
	struct lttcomm_consumer_msg msg;

	assert(consumer);

	DBG3("Consumer data pending for id %" PRIu64, session_id);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_DATA_PENDING;
	msg.u.data_pending.session_id = session_id;

	/* Send command for each consumer */
	rcu_read_lock();
	cds_lfht_for_each_entry(consumer->socks->ht, &iter.iter, socket,
			node.node) {
		pthread_mutex_lock(socket->lock);
		ret = consumer_socket_send(socket, &msg, sizeof(msg));
		if (ret < 0) {
			pthread_mutex_unlock(socket->lock);
			goto error_unlock;
		}

		/*
		 * No need for a recv reply status because the answer to the command is
		 * the reply status message.
		 */

		ret = consumer_socket_recv(socket, &ret_code, sizeof(ret_code));
		if (ret < 0) {
			pthread_mutex_unlock(socket->lock);
			goto error_unlock;
		}
		pthread_mutex_unlock(socket->lock);

		if (ret_code == 1) {
			break;
		}
	}
	rcu_read_unlock();

	DBG("Consumer data is %s pending for session id %" PRIu64,
			ret_code == 1 ? "" : "NOT", session_id);
	return ret_code;

error_unlock:
	rcu_read_unlock();
	return -1;
}

/*
 * Send a flush command to consumer using the given channel key.
 *
 * Return 0 on success else a negative value.
 */
int consumer_flush_channel(struct consumer_socket *socket, uint64_t key)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG2("Consumer flush channel key %" PRIu64, key);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_FLUSH_CHANNEL;
	msg.u.flush_channel.key = key;

	pthread_mutex_lock(socket->lock);
	health_code_update();

	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		goto end;
	}

end:
	health_code_update();
	pthread_mutex_unlock(socket->lock);
	return ret;
}

/*
 * Send a clear quiescent command to consumer using the given channel key.
 *
 * Return 0 on success else a negative value.
 */
int consumer_clear_quiescent_channel(struct consumer_socket *socket, uint64_t key)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG2("Consumer clear quiescent channel key %" PRIu64, key);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_CLEAR_QUIESCENT_CHANNEL;
	msg.u.clear_quiescent_channel.key = key;

	pthread_mutex_lock(socket->lock);
	health_code_update();

	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		goto end;
	}

end:
	health_code_update();
	pthread_mutex_unlock(socket->lock);
	return ret;
}

/*
 * Send a close metadata command to consumer using the given channel key.
 * Called with registry lock held.
 *
 * Return 0 on success else a negative value.
 */
int consumer_close_metadata(struct consumer_socket *socket,
		uint64_t metadata_key)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG2("Consumer close metadata channel key %" PRIu64, metadata_key);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_CLOSE_METADATA;
	msg.u.close_metadata.key = metadata_key;

	pthread_mutex_lock(socket->lock);
	health_code_update();

	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		goto end;
	}

end:
	health_code_update();
	pthread_mutex_unlock(socket->lock);
	return ret;
}

/*
 * Send a setup metdata command to consumer using the given channel key.
 *
 * Return 0 on success else a negative value.
 */
int consumer_setup_metadata(struct consumer_socket *socket,
		uint64_t metadata_key)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG2("Consumer setup metadata channel key %" PRIu64, metadata_key);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_SETUP_METADATA;
	msg.u.setup_metadata.key = metadata_key;

	pthread_mutex_lock(socket->lock);
	health_code_update();

	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		goto end;
	}

end:
	health_code_update();
	pthread_mutex_unlock(socket->lock);
	return ret;
}

/*
 * Send metadata string to consumer.
 * RCU read-side lock must be held to guarantee existence of socket.
 *
 * Return 0 on success else a negative value.
 */
int consumer_push_metadata(struct consumer_socket *socket,
		uint64_t metadata_key, char *metadata_str, size_t len,
		size_t target_offset, uint64_t version)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG2("Consumer push metadata to consumer socket %d", *socket->fd_ptr);

	pthread_mutex_lock(socket->lock);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_PUSH_METADATA;
	msg.u.push_metadata.key = metadata_key;
	msg.u.push_metadata.target_offset = target_offset;
	msg.u.push_metadata.len = len;
	msg.u.push_metadata.version = version;

	health_code_update();
	ret = consumer_send_msg(socket, &msg);
	if (ret < 0 || len == 0) {
		goto end;
	}

	DBG3("Consumer pushing metadata on sock %d of len %zu", *socket->fd_ptr,
			len);

	ret = consumer_socket_send(socket, metadata_str, len);
	if (ret < 0) {
		goto end;
	}

	health_code_update();
	ret = consumer_recv_status_reply(socket);
	if (ret < 0) {
		goto end;
	}

end:
	pthread_mutex_unlock(socket->lock);
	health_code_update();
	return ret;
}

/*
 * Ask the consumer to snapshot a specific channel using the key.
 *
 * Returns LTTNG_OK on success or else an LTTng error code.
 */
enum lttng_error_code consumer_snapshot_channel(struct consumer_socket *socket,
		uint64_t key, struct snapshot_output *output, int metadata,
		uid_t uid, gid_t gid, const char *session_path, int wait,
		uint64_t nb_packets_per_stream, uint64_t trace_archive_id)
{
	int ret;
	enum lttng_error_code status = LTTNG_OK;
	struct lttcomm_consumer_msg msg;

	assert(socket);
	assert(output);
	assert(output->consumer);

	DBG("Consumer snapshot channel key %" PRIu64, key);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_SNAPSHOT_CHANNEL;
	msg.u.snapshot_channel.key = key;
	msg.u.snapshot_channel.nb_packets_per_stream = nb_packets_per_stream;
	msg.u.snapshot_channel.metadata = metadata;
	msg.u.snapshot_channel.trace_archive_id = trace_archive_id;

	if (output->consumer->type == CONSUMER_DST_NET) {
		msg.u.snapshot_channel.relayd_id = output->consumer->net_seq_index;
		msg.u.snapshot_channel.use_relayd = 1;
		ret = snprintf(msg.u.snapshot_channel.pathname,
				sizeof(msg.u.snapshot_channel.pathname),
				"%s/%s/%s-%s-%" PRIu64 "%s",
				output->consumer->dst.net.base_dir,
				output->consumer->domain_subdir,
				output->name, output->datetime,
				output->nb_snapshot,
				session_path);
		if (ret < 0) {
			status = LTTNG_ERR_INVALID;
			goto error;
		} else if (ret >= sizeof(msg.u.snapshot_channel.pathname)) {
			ERR("Snapshot path exceeds the maximal allowed length of %zu bytes (%i bytes required) with path \"%s/%s/%s-%s-%" PRIu64 "%s\"",
					sizeof(msg.u.snapshot_channel.pathname),
					ret, output->consumer->dst.net.base_dir,
					output->consumer->domain_subdir,
					output->name, output->datetime,
					output->nb_snapshot,
					session_path);
			status = LTTNG_ERR_SNAPSHOT_FAIL;
			goto error;
		}
	} else {
		ret = snprintf(msg.u.snapshot_channel.pathname,
				sizeof(msg.u.snapshot_channel.pathname),
				"%s/%s-%s-%" PRIu64 "%s",
				output->consumer->dst.session_root_path,
				output->name, output->datetime,
				output->nb_snapshot,
				session_path);
		if (ret < 0) {
			status = LTTNG_ERR_NOMEM;
			goto error;
		} else if (ret >= sizeof(msg.u.snapshot_channel.pathname)) {
			ERR("Snapshot path exceeds the maximal allowed length of %zu bytes (%i bytes required) with path \"%s/%s-%s-%" PRIu64 "%s\"",
					sizeof(msg.u.snapshot_channel.pathname),
					ret, output->consumer->dst.session_root_path,
					output->name, output->datetime, output->nb_snapshot,
					session_path);
			status = LTTNG_ERR_SNAPSHOT_FAIL;
			goto error;
		}

		msg.u.snapshot_channel.relayd_id = (uint64_t) -1ULL;

		/* Create directory. Ignore if exist. */
		ret = run_as_mkdir_recursive(msg.u.snapshot_channel.pathname,
				S_IRWXU | S_IRWXG, uid, gid);
		if (ret < 0) {
			if (errno != EEXIST) {
				status = LTTNG_ERR_CREATE_DIR_FAIL;
				PERROR("Trace directory creation error");
				goto error;
			}
		}
	}

	health_code_update();
	pthread_mutex_lock(socket->lock);
	ret = consumer_send_msg(socket, &msg);
	pthread_mutex_unlock(socket->lock);
	if (ret < 0) {
		switch (-ret) {
		case LTTCOMM_CONSUMERD_CHAN_NOT_FOUND:
			status = LTTNG_ERR_CHAN_NOT_FOUND;
			break;
		default:
			status = LTTNG_ERR_SNAPSHOT_FAIL;
			break;
		}
		goto error;
	}

error:
	health_code_update();
	return status;
}

/*
 * Ask the consumer the number of discarded events for a channel.
 */
int consumer_get_discarded_events(uint64_t session_id, uint64_t channel_key,
		struct consumer_output *consumer, uint64_t *discarded)
{
	int ret;
	struct consumer_socket *socket;
	struct lttng_ht_iter iter;
	struct lttcomm_consumer_msg msg;

	assert(consumer);

	DBG3("Consumer discarded events id %" PRIu64, session_id);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_DISCARDED_EVENTS;
	msg.u.discarded_events.session_id = session_id;
	msg.u.discarded_events.channel_key = channel_key;

	*discarded = 0;

	/* Send command for each consumer */
	rcu_read_lock();
	cds_lfht_for_each_entry(consumer->socks->ht, &iter.iter, socket,
			node.node) {
		uint64_t consumer_discarded = 0;
		pthread_mutex_lock(socket->lock);
		ret = consumer_socket_send(socket, &msg, sizeof(msg));
		if (ret < 0) {
			pthread_mutex_unlock(socket->lock);
			goto end;
		}

		/*
		 * No need for a recv reply status because the answer to the
		 * command is the reply status message.
		 */
		ret = consumer_socket_recv(socket, &consumer_discarded,
				sizeof(consumer_discarded));
		if (ret < 0) {
			ERR("get discarded events");
			pthread_mutex_unlock(socket->lock);
			goto end;
		}
		pthread_mutex_unlock(socket->lock);
		*discarded += consumer_discarded;
	}
	ret = 0;
	DBG("Consumer discarded %" PRIu64 " events in session id %" PRIu64,
			*discarded, session_id);

end:
	rcu_read_unlock();
	return ret;
}

/*
 * Ask the consumer the number of lost packets for a channel.
 */
int consumer_get_lost_packets(uint64_t session_id, uint64_t channel_key,
		struct consumer_output *consumer, uint64_t *lost)
{
	int ret;
	struct consumer_socket *socket;
	struct lttng_ht_iter iter;
	struct lttcomm_consumer_msg msg;

	assert(consumer);

	DBG3("Consumer lost packets id %" PRIu64, session_id);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_LOST_PACKETS;
	msg.u.lost_packets.session_id = session_id;
	msg.u.lost_packets.channel_key = channel_key;

	*lost = 0;

	/* Send command for each consumer */
	rcu_read_lock();
	cds_lfht_for_each_entry(consumer->socks->ht, &iter.iter, socket,
			node.node) {
		uint64_t consumer_lost = 0;
		pthread_mutex_lock(socket->lock);
		ret = consumer_socket_send(socket, &msg, sizeof(msg));
		if (ret < 0) {
			pthread_mutex_unlock(socket->lock);
			goto end;
		}

		/*
		 * No need for a recv reply status because the answer to the
		 * command is the reply status message.
		 */
		ret = consumer_socket_recv(socket, &consumer_lost,
				sizeof(consumer_lost));
		if (ret < 0) {
			ERR("get lost packets");
			pthread_mutex_unlock(socket->lock);
			goto end;
		}
		pthread_mutex_unlock(socket->lock);
		*lost += consumer_lost;
	}
	ret = 0;
	DBG("Consumer lost %" PRIu64 " packets in session id %" PRIu64,
			*lost, session_id);

end:
	rcu_read_unlock();
	return ret;
}

/*
 * Ask the consumer to rotate a channel.
 * domain_path contains "/kernel" for kernel or the complete path for UST
 * (ex: /ust/uid/1000/64-bit);
 *
 * The new_chunk_id is the session->rotate_count that has been incremented
 * when the rotation started. On the relay, this allows to keep track in which
 * chunk each stream is currently writing to (for the rotate_pending operation).
 */
int consumer_rotate_channel(struct consumer_socket *socket, uint64_t key,
		uid_t uid, gid_t gid, struct consumer_output *output,
		const char *domain_path, bool is_metadata_channel,
		uint64_t new_chunk_id)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG("Consumer rotate channel key %" PRIu64, key);

	pthread_mutex_lock(socket->lock);
	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_ROTATE_CHANNEL;
	msg.u.rotate_channel.key = key;
	msg.u.rotate_channel.metadata = !!is_metadata_channel;
	msg.u.rotate_channel.new_chunk_id = new_chunk_id;

	if (output->type == CONSUMER_DST_NET) {
		msg.u.rotate_channel.relayd_id = output->net_seq_index;
		ret = snprintf(msg.u.rotate_channel.pathname,
				sizeof(msg.u.rotate_channel.pathname), "%s%s%s",
				output->dst.net.base_dir,
				output->chunk_path, domain_path);
		if (ret < 0 || ret >= sizeof(msg.u.rotate_channel.pathname)) {
			ERR("Failed to format channel path name when asking consumer to rotate channel");
			ret = -LTTNG_ERR_INVALID;
			goto error;
		}
	} else {
		msg.u.rotate_channel.relayd_id = (uint64_t) -1ULL;
		ret = snprintf(msg.u.rotate_channel.pathname,
				sizeof(msg.u.rotate_channel.pathname), "%s/%s%s",
				output->dst.session_root_path,
				output->chunk_path, domain_path);
		if (ret < 0 || ret >= sizeof(msg.u.rotate_channel.pathname)) {
			ERR("Failed to format channel path name when asking consumer to rotate channel");
			ret = -LTTNG_ERR_INVALID;
			goto error;
		}
	}

	health_code_update();
	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		switch (-ret) {
		case LTTCOMM_CONSUMERD_CHAN_NOT_FOUND:
			ret = -LTTNG_ERR_CHAN_NOT_FOUND;
			break;
		default:
			ret = -LTTNG_ERR_ROTATION_FAIL_CONSUMER;
			break;
		}
		goto error;
	}
error:
	pthread_mutex_unlock(socket->lock);
	health_code_update();
	return ret;
}

int consumer_rotate_rename(struct consumer_socket *socket, uint64_t session_id,
		const struct consumer_output *output, const char *old_path,
		const char *new_path, uid_t uid, gid_t gid)
{
	int ret;
	struct lttcomm_consumer_msg msg;
	size_t old_path_length, new_path_length;

	assert(socket);
	assert(old_path);
	assert(new_path);

	DBG("Consumer rotate rename session %" PRIu64 ", old path = \"%s\", new_path = \"%s\"",
			session_id, old_path, new_path);

	old_path_length = strlen(old_path);
	if (old_path_length >= sizeof(msg.u.rotate_rename.old_path)) {
		ERR("consumer_rotate_rename: old path length (%zu bytes) exceeds the maximal length allowed by the consumer protocol (%zu bytes)",
				old_path_length + 1, sizeof(msg.u.rotate_rename.old_path));
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	new_path_length = strlen(new_path);
	if (new_path_length >= sizeof(msg.u.rotate_rename.new_path)) {
		ERR("consumer_rotate_rename: new path length (%zu bytes) exceeds the maximal length allowed by the consumer protocol (%zu bytes)",
				new_path_length + 1, sizeof(msg.u.rotate_rename.new_path));
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_ROTATE_RENAME;
	msg.u.rotate_rename.session_id = session_id;
	msg.u.rotate_rename.uid = uid;
	msg.u.rotate_rename.gid = gid;
	strcpy(msg.u.rotate_rename.old_path, old_path);
	strcpy(msg.u.rotate_rename.new_path, new_path);

	if (output->type == CONSUMER_DST_NET) {
		msg.u.rotate_rename.relayd_id = output->net_seq_index;
	} else {
		msg.u.rotate_rename.relayd_id = -1ULL;
	}

	health_code_update();
	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		ret = -LTTNG_ERR_ROTATE_RENAME_FAIL_CONSUMER;
		goto error;
	}

error:
	health_code_update();
	return ret;
}

/*
 * Ask the consumer if a rotation is locally pending. Must be called with the
 * socket lock held.
 *
 * Return 1 if the rotation is still pending, 0 if finished, a negative value
 * on error.
 */
int consumer_check_rotation_pending_local(struct consumer_socket *socket,
		uint64_t session_id, uint64_t chunk_id)
{
	int ret;
	struct lttcomm_consumer_msg msg;
	uint32_t pending = 0;

	assert(socket);

	DBG("Asking consumer to locally check for pending rotation for session %" PRIu64 ", chunk id %" PRIu64,
			session_id, chunk_id);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_CHECK_ROTATION_PENDING_LOCAL;
	msg.u.check_rotation_pending_local.session_id = session_id;
	msg.u.check_rotation_pending_local.chunk_id = chunk_id;

	health_code_update();
	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		ret = -LTTNG_ERR_ROTATION_PENDING_LOCAL_FAIL_CONSUMER;
		goto error;
	}

	ret = consumer_socket_recv(socket, &pending, sizeof(pending));
	if (ret < 0) {
		goto error;
	}

	ret = pending;

error:
	health_code_update();
	return ret;
}

/*
 * Ask the consumer if a rotation is pending on the relayd. Must be called with
 * the socket lock held.
 *
 * Return 1 if the rotation is still pending, 0 if finished, a negative value
 * on error.
 */
int consumer_check_rotation_pending_relay(struct consumer_socket *socket,
		const struct consumer_output *output, uint64_t session_id,
		uint64_t chunk_id)
{
	int ret;
	struct lttcomm_consumer_msg msg;
	uint32_t pending = 0;

	assert(socket);

	DBG("Asking consumer to check for pending rotation on relay for session %" PRIu64 ", chunk id %" PRIu64,
			session_id, chunk_id);
	assert(output->type == CONSUMER_DST_NET);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_CHECK_ROTATION_PENDING_RELAY;
	msg.u.check_rotation_pending_relay.session_id = session_id;
	msg.u.check_rotation_pending_relay.relayd_id = output->net_seq_index;
	msg.u.check_rotation_pending_relay.chunk_id = chunk_id;

	health_code_update();
	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		ret = -LTTNG_ERR_ROTATION_PENDING_RELAY_FAIL_CONSUMER;
		goto error;
	}

	ret = consumer_socket_recv(socket, &pending, sizeof(pending));
	if (ret < 0) {
		goto error;
	}

	ret = pending;

error:
	health_code_update();
	return ret;
}

/*
 * Ask the consumer to create a directory.
 *
 * Called with the consumer socket lock held.
 */
int consumer_mkdir(struct consumer_socket *socket, uint64_t session_id,
		const struct consumer_output *output, const char *path,
		uid_t uid, gid_t gid)
{
	int ret;
	struct lttcomm_consumer_msg msg;

	assert(socket);

	DBG("Consumer mkdir %s in session %" PRIu64, path, session_id);

	memset(&msg, 0, sizeof(msg));
	msg.cmd_type = LTTNG_CONSUMER_MKDIR;
	msg.u.mkdir.session_id = session_id;
	msg.u.mkdir.uid = uid;
	msg.u.mkdir.gid = gid;
	ret = snprintf(msg.u.mkdir.path, sizeof(msg.u.mkdir.path), "%s", path);
	if (ret < 0 || ret >= sizeof(msg.u.mkdir.path)) {
		ERR("Format path");
		ret = -LTTNG_ERR_INVALID;
		goto error;
	}

	if (output->type == CONSUMER_DST_NET) {
		msg.u.mkdir.relayd_id = output->net_seq_index;
	} else {
		msg.u.mkdir.relayd_id = -1ULL;
	}

	health_code_update();
	ret = consumer_send_msg(socket, &msg);
	if (ret < 0) {
		ret = -LTTNG_ERR_MKDIR_FAIL_CONSUMER;
		goto error;
	}

error:
	health_code_update();
	return ret;
}
