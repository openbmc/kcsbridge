/* Copyright 2017 - 2018 Intel
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *    http://www.apache.org/licenses/LICENSE-2.0
 *
 *	Unless required by applicable law or agreed to in writing, software
 *	distributed under the License is distributed on an "AS IS" BASIS,
 *	WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *	See the License for the specific language governing permissions and
 *	limitations under the License.
 */

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <getopt.h>
#include <limits.h>
#include <linux/ipmi_bmc.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <sys/mman.h>
#include <sys/ioctl.h>
#include <sys/stat.h>
#include <sys/timerfd.h>
#include <systemd/sd-bus.h>
#include <time.h>
#include <unistd.h>
#include <stdbool.h>

#define DBUS_ERR "org.openbmc.error"

#define LOG_PREFIX "KCSBRIDGED"
#define DBUS_NAME "org.openbmc.HostIpmi."
#define OBJ_NAME "/org/openbmc/HostIpmi/"

#define DEFAULT_DBUS "org.openbmc.HostIpmi"
#define DEFAULT_OBJ "/org/openbmc/HostIpmi/1"
#define DBUS_INTF "org.openbmc.HostIpmi"

#define KCS_TIMEOUT_IN_SEC 5
#define KCS_MESSAGE_SIZE 256

#define SD_BUS_FD 0
#define KCS_FD 1
#define TIMER_FD 2
#define TOTAL_FDS 3

#define NAMEBUFFERLEN 50
#define OPTMAXLEN (NAMEBUFFERLEN - sizeof(OBJ_NAME) - 1)

char kcsDevice[NAMEBUFFERLEN];
char busName[NAMEBUFFERLEN];
char objPath[NAMEBUFFERLEN];

struct kcs_msg_req {
	uint8_t netfn;
	uint8_t lun;
	uint8_t cmd;
	uint8_t *data;
	size_t data_len;
};

struct kcsbridged_context {
	struct pollfd fds[TOTAL_FDS];
	struct sd_bus *bus;

	/*
	 * Request and Response Messages are paired together as a Write Transfer
	 * to the BMC to send the request followed by a Read Transfer from the
	 * BMC to get the response.
	 */
	int expired;
	uint8_t seqnum;
	struct kcs_msg_req req;
};

enum { KCS_LOG_NONE = 0, KCS_LOG_VERBOSE, KCS_LOG_DEBUG };

static void (*kcs_vlog)(int p, const char *fmt, va_list args);
static int verbosity = KCS_LOG_NONE;

#define MSG_OUT(f_, ...)                                                       \
	do {                                                                   \
		if (verbosity != KCS_LOG_NONE)                                 \
			kcs_log(LOG_INFO, f_, ##__VA_ARGS__);                  \
	} while (0)

#define MSG_ERR(f_, ...)                                                       \
	do {                                                                   \
		if (verbosity != KCS_LOG_NONE)                                 \
			kcs_log(LOG_ERR, f_, ##__VA_ARGS__);                   \
	} while (0)

static void kcs_log_console(int p, const char *fmt, va_list args)
{
	vfprintf(stderr, fmt, args);
}

__attribute__((format(printf, 2, 3))) static void kcs_log(int p,
							  const char *fmt, ...)
{
	va_list args;

	va_start(args, fmt);
	kcs_vlog(p, fmt, args);
	va_end(args);
}

static void kcs_dump_data(uint8_t *data, size_t data_len)
{
	size_t i;
	int str_len;
	char str[64];

	str_len = 0;
	for (i = 0; i < data_len; i++) {
		if (i % 8 == 0) {
			if (i != 0) {
				kcs_log(LOG_INFO, "%s\n", str);
				str_len = 0;
			}
			str_len += sprintf(&str[str_len], "\t");
		}

		str_len += sprintf(&str[str_len], "0x%02x ", data[i]);
	}

	if (str_len != 0)
		kcs_log(LOG_INFO, "%s\n", str);
}

static void kcs_set_timer(struct kcsbridged_context *context, int seconds)
{
	struct itimerspec ts;
	int r;

	ts.it_interval.tv_sec = 0;
	ts.it_interval.tv_nsec = 0;
	ts.it_value.tv_nsec = 0;
	ts.it_value.tv_sec = seconds;

	r = timerfd_settime(context->fds[TIMER_FD].fd, 0, &ts, NULL);
	if (r == -1)
		MSG_ERR("Couldn't set timerfd: %s\n", strerror(errno));
}

static int handle_kcs_request(struct kcsbridged_context *context, uint8_t *msg,
			      size_t msglen)
{
	struct kcs_msg_req *req;

	if (msglen < 2) {
		MSG_ERR("KCS message with a short length (%zd)\n", msglen);
		return -1;
	}

	context->expired = 0;
	context->seqnum++;

	req = &context->req;
	req->netfn = msg[0] >> 2;
	req->lun = msg[0] & 0x3;
	req->cmd = msg[1];
	req->data = msg + 2;
	req->data_len = msglen - 2;

	return 0;
}

static int method_send_message(sd_bus_message *msg, void *userdata,
			       sd_bus_error *err)
{
	struct kcsbridged_context *context =
		static_cast<kcsbridged_context *>(userdata);
	uint8_t netfn, lun, seqnum, cmd, cc;
	struct kcs_msg_req *req;
	uint8_t *data;
	size_t data_sz;
	int r;
	uint8_t rsp[KCS_MESSAGE_SIZE];

	if (!context || context->expired) {
		sd_bus_error_set_const(err, DBUS_ERR, "Internal error");
		r = 0;
		goto out;
	}

	r = sd_bus_message_read(msg, "yyyyy", &seqnum, &netfn, &lun, &cmd, &cc);
	if (r < 0) {
		sd_bus_error_set_const(err, DBUS_ERR, "Bad message");
		r = -EINVAL;
		goto out;
	}

	req = &context->req;
	if (context->seqnum != seqnum || (req->netfn | 1) != netfn
	    || req->lun != lun || req->cmd != cmd) {
		sd_bus_error_set_const(err, DBUS_ERR, "No matching request");
		r = -EINVAL;
		goto out;
	}

	kcs_set_timer(context, 0); /* Stop the timer. */

	r = sd_bus_message_read_array(msg, 'y', (const void **)&data, &data_sz);
	if (r < 0 || data_sz > sizeof(rsp) - 3) {
		sd_bus_error_set_const(err, DBUS_ERR, "Bad message data");
		r = -EINVAL;
		goto out;
	}

	rsp[0] = (netfn << 2) | (lun & 0x3);
	rsp[1] = cmd;
	rsp[2] = cc;
	if (data_sz)
		memcpy(rsp + 3, data, data_sz);

	r = write(context->fds[KCS_FD].fd, rsp, 3 + data_sz);
	if (r > 0)
		r = 0;

	MSG_OUT("Send rsp msg <- seq=0x%02x netfn=0x%02x lun=0x%02x cmd=0x%02x cc=0x%02x\n",
		seqnum, netfn, lun, cmd, cc);

	if (verbosity == KCS_LOG_DEBUG && data_sz != 0)
		kcs_dump_data(data, data_sz);

out:
	return sd_bus_reply_method_return(msg, "x", r);
}

static int method_set_sms_atn(sd_bus_message *msg, void *userdata,
			      sd_bus_error *err)
{
	struct kcsbridged_context *context =
		static_cast<kcsbridged_context *>(userdata);
	int r;

	MSG_OUT("Sending SET_SMS_ATN\n");

	r = ioctl(context->fds[KCS_FD].fd, IPMI_BMC_IOCTL_SET_SMS_ATN);
	if (r == -1) {
		r = errno;
		MSG_ERR("Couldn't SET_SMS_ATN: %s\n", strerror(r));
		return sd_bus_reply_method_errno(msg, errno, err);
	}

	r = 0;
	return sd_bus_reply_method_return(msg, "x", r);
}

static int method_clear_sms_atn(sd_bus_message *msg, void *userdata,
				sd_bus_error *err)
{
	struct kcsbridged_context *context =
		static_cast<kcsbridged_context *>(userdata);
	int r;

	MSG_OUT("Sending CLEAR_SMS_ATN\n");

	r = ioctl(context->fds[KCS_FD].fd, IPMI_BMC_IOCTL_CLEAR_SMS_ATN);
	if (r == -1) {
		r = errno;
		MSG_ERR("Couldn't CLEAR_SMS_ATN: %s\n", strerror(r));
		return sd_bus_reply_method_errno(msg, errno, err);
	}

	r = 0;
	return sd_bus_reply_method_return(msg, "x", r);
}

static int method_force_abort(sd_bus_message *msg, void *userdata,
			      sd_bus_error *err)
{
	struct kcsbridged_context *context =
		static_cast<kcsbridged_context *>(userdata);
	int r;

	MSG_OUT("Sending FORCE_ABORT\n");

	r = ioctl(context->fds[KCS_FD].fd, IPMI_BMC_IOCTL_FORCE_ABORT);
	if (r == -1) {
		r = errno;
		MSG_ERR("Couldn't FORCE_ABORT: %s\n", strerror(r));
		return sd_bus_reply_method_errno(msg, errno, err);
	}

	r = 0;
	return sd_bus_reply_method_return(msg, "x", r);
}

static int dispatch_sd_bus(struct kcsbridged_context *context)
{
	int r = 0;

	if (context->fds[SD_BUS_FD].revents) {
		// docs say to call this in a loop until no events are left
		// to be processed
		do {
			r = sd_bus_process(context->bus, NULL);
		} while (r > 0);

		if (r > 0)
			MSG_OUT("Processed dbus events\n");
	}

	return r;
}

static int dispatch_timer(struct kcsbridged_context *context)
{
	if (context->fds[TIMER_FD].revents & POLLIN) {
		struct kcs_msg_req *req;
		uint8_t rsp[3];

		MSG_OUT("Timeout on msg with seq: 0x%02x\n", context->seqnum);

		context->expired = 1;

		req = &context->req;
		rsp[0] = ((req->netfn | 1) << 2) | (req->lun & 0x3);
		rsp[1] = req->cmd;
		rsp[2] = 0xce; /* Command response could not be provided */
		if (write(context->fds[KCS_FD].fd, rsp, 3) < 0)
			MSG_ERR("Failed to send the timeout response!\n");
	}

	return 0;
}

static int dispatch_kcs(struct kcsbridged_context *context)
{
	struct kcs_msg_req *req = &context->req;
	sd_bus_message *msg;
	int r = 0, len;
	uint8_t data[KCS_MESSAGE_SIZE];

	if (!(context->fds[KCS_FD].revents & POLLIN))
		goto out;

	len = read(context->fds[KCS_FD].fd, data, sizeof(data));
	if (len < 0 || handle_kcs_request(context, data, len))
		goto out;

	r = sd_bus_message_new_signal(context->bus, &msg, objPath, DBUS_INTF,
				      "ReceivedMessage");
	if (r < 0) {
		MSG_ERR("Failed to create signal: %s\n", strerror(-r));
		goto out;
	}

	r = sd_bus_message_append(msg, "yyyy", context->seqnum, req->netfn,
				  req->lun, req->cmd);
	if (r < 0) {
		MSG_ERR("Couldn't append header to signal: %s\n", strerror(-r));
		goto bail;
	}

	r = sd_bus_message_append_array(msg, 'y', req->data, req->data_len);
	if (r < 0) {
		MSG_ERR("Couldn't append array to signal: %s\n", strerror(-r));
		goto bail;
	}

	r = sd_bus_send(context->bus, msg, NULL);
	if (r < 0) {
		MSG_ERR("Couldn't emit dbus signal: %s\n", strerror(-r));
		goto bail;
	}

	kcs_set_timer(context, KCS_TIMEOUT_IN_SEC);

	MSG_OUT("Recv req msg -> seq=0x%02x netfn=0x%02x lun=0x%02x cmd=0x%02x\n",
		context->seqnum, req->netfn, req->lun, req->cmd);

	if (verbosity == KCS_LOG_DEBUG && req->data_len != 0)
		kcs_dump_data(req->data, req->data_len);

bail:
	sd_bus_message_unref(msg);
out:
	return r;
}

static void usage(const char *name)
{
	fprintf(stderr,
		"Usage %s [--v[v] | --syslog] --i <ID> --d <DEVICE>\n"
		"--v                      Be verbose\n"
		"--vv                     Be verbose and dump entire messages\n"
		"--s, --syslog            Log output to syslog (pointless without --verbose)\n"
		"--i, --instanceid <ID>   instance id (string type) optional\n"
		"--d, --device <DEVICE>   Use <DEVICE> file.\n\n",
		name);
}

static const sd_bus_vtable ipmid_vtable[] = {
	SD_BUS_VTABLE_START(0),
	SD_BUS_METHOD("sendMessage", "yyyyyay", "x", &method_send_message,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("setAttention", "", "x", &method_set_sms_atn,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("clearAttention", "", "x", &method_clear_sms_atn,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_METHOD("forceAbort", "", "x", &method_force_abort,
		      SD_BUS_VTABLE_UNPRIVILEGED),
	SD_BUS_SIGNAL("ReceivedMessage", "yyyyay", 0),
	SD_BUS_VTABLE_END};

int main(int argc, char *argv[])
{
	struct kcsbridged_context *context;
	const char *name = argv[0];
	bool deviceOptFlag = false;
	int opt, polled, r;
	static const struct option long_options[] = {
		{"device", required_argument, 0, 'd'},
		{"instanceid", required_argument, 0, 'i'},
		{"v", no_argument, &verbosity, KCS_LOG_VERBOSE},
		{"vv", no_argument, &verbosity, KCS_LOG_DEBUG},
		{"syslog", no_argument, 0, 's'},
		{0, 0, 0, 0}};

	context =
		static_cast<kcsbridged_context *>(calloc(1, sizeof(*context)));
	if (!context) {
		fprintf(stderr, "OOM!\n");
		return -1;
	}

	snprintf(busName, NAMEBUFFERLEN, "%s", DEFAULT_DBUS);
	snprintf(objPath, NAMEBUFFERLEN, "%s", DEFAULT_OBJ);

	kcs_vlog = &kcs_log_console;
	while ((opt = getopt_long(argc, argv, "", long_options, NULL)) != -1) {
		switch (opt) {
		case 0:
			break;

		case 'd':
			snprintf(kcsDevice, NAMEBUFFERLEN, "%s", optarg);
			deviceOptFlag = true;
			break;

		case 'i':
			if (sizeof(*optarg) > OPTMAXLEN) {
				fprintf(stderr, "ID is too long!\n");
				exit(EXIT_FAILURE);
			}
			if ((NULL != strstr(optarg, "."))
			    || (NULL != strstr(optarg, "/"))) {
				fprintf(stderr, "invalid ID!\n");
				exit(EXIT_FAILURE);
			}
			snprintf(busName, NAMEBUFFERLEN, "%s%s", DBUS_NAME,
				 optarg);
			snprintf(objPath, NAMEBUFFERLEN, "%s%s", OBJ_NAME,
				 optarg);
			break;

		case 's':
			if (kcs_vlog != &vsyslog) {
				openlog(LOG_PREFIX, LOG_ODELAY, LOG_DAEMON);
				kcs_vlog = &vsyslog;
			}
			break;

		default:
			usage(name);
			exit(EXIT_FAILURE);
		}
	}

	if (false == deviceOptFlag) {
		usage(name);
		MSG_OUT("Flag: device %d \n", deviceOptFlag);
		exit(EXIT_FAILURE);
	}

	if (verbosity == KCS_LOG_VERBOSE)
		MSG_OUT("Verbose logging\n");
	else if (verbosity == KCS_LOG_DEBUG)
		MSG_OUT("Debug logging\n");

	MSG_OUT("Starting\n");
	r = sd_bus_default_system(&context->bus);
	if (r < 0) {
		MSG_ERR("Failed to connect to system bus: %s\n", strerror(-r));
		goto finish;
	}

	MSG_OUT("Registering dbus methods/signals\n");
	r = sd_bus_add_object_vtable(context->bus, NULL, objPath, DBUS_INTF,
				     ipmid_vtable, context);
	if (r < 0) {
		MSG_ERR("Failed to issue method call: %s\n", strerror(-r));
		goto finish;
	}

	MSG_OUT("Requesting dbus : %s objpath:%s \n", busName, objPath);
	r = sd_bus_request_name(context->bus, busName,
				SD_BUS_NAME_ALLOW_REPLACEMENT
					| SD_BUS_NAME_REPLACE_EXISTING);
	if (r < 0) {
		MSG_ERR("Failed to acquire service name: %s\n", strerror(-r));
		goto finish;
	}

	MSG_OUT("Getting dbus file descriptors\n");
	context->fds[SD_BUS_FD].fd = sd_bus_get_fd(context->bus);
	if (context->fds[SD_BUS_FD].fd < 0) {
		r = -errno;
		MSG_OUT("Couldn't get the bus file descriptor: %s\n",
			strerror(errno));
		goto finish;
	}

	MSG_OUT("Opening %s\n", kcsDevice);
	context->fds[KCS_FD].fd = open(kcsDevice, O_RDWR | O_NONBLOCK);
	if (context->fds[KCS_FD].fd < 0) {
		r = -errno;
		MSG_ERR("Couldn't open %s with flags O_RDWR: %s\n", kcsDevice,
			strerror(errno));
		goto finish;
	}

	MSG_OUT("Creating timer fd\n");
	context->fds[TIMER_FD].fd = timerfd_create(CLOCK_MONOTONIC, 0);
	if (context->fds[TIMER_FD].fd < 0) {
		r = -errno;
		MSG_ERR("Couldn't create timer fd: %s\n", strerror(errno));
		goto finish;
	}
	context->fds[SD_BUS_FD].events = POLLIN;
	context->fds[KCS_FD].events = POLLIN;
	context->fds[TIMER_FD].events = POLLIN;

	MSG_OUT("Entering polling loop\n");

	while (1) {
		polled = poll(context->fds, TOTAL_FDS, 5000);
		if (polled == 0)
			continue;
		if (polled < 0) {
			r = -errno;
			MSG_ERR("Error from poll(): %s\n", strerror(errno));
			goto finish;
		}

		r = dispatch_sd_bus(context);
		if (r < 0) {
			MSG_ERR("Error handling dbus event: %s\n",
				strerror(-r));
			goto finish;
		}
		r = dispatch_kcs(context);
		if (r < 0) {
			MSG_ERR("Error handling KCS event: %s\n", strerror(-r));
			goto finish;
		}
		r = dispatch_timer(context);
		if (r < 0) {
			MSG_ERR("Error handling timer event: %s\n",
				strerror(-r));
			goto finish;
		}
	}

finish:
	close(context->fds[KCS_FD].fd);
	close(context->fds[TIMER_FD].fd);
	sd_bus_unref(context->bus);
	free(context);

	return r;
}
