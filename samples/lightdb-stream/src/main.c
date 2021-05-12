/*
 * Copyright (c) 2021 Golioth, Inc.
 *
 * SPDX-License-Identifier: Apache-2.0
 */

#include <logging/log.h>
LOG_MODULE_REGISTER(golioth_lightdb_stream, LOG_LEVEL_DBG);

#include <errno.h>
#include <logging/golioth.h>
#include <net/socket.h>
#include <net/coap.h>
#include <net/golioth.h>
#include <device.h>
#include <devicetree.h>
#include <drivers/sensor.h>
#include <net/tls_credentials.h>
#include <posix/sys/eventfd.h>

#include <drivers/gpio.h>
#include <tinycbor/cbor.h>
#include <tinycbor/cbor_buf_reader.h>

#include "wifi.h"

#define RX_TIMEOUT		K_SECONDS(30)

#define MAX_COAP_MSG_LEN	256

#define TLS_PSK_ID		CONFIG_GOLIOTH_SERVER_DTLS_PSK_ID
#define TLS_PSK			CONFIG_GOLIOTH_SERVER_DTLS_PSK

#define PSK_TAG			1

/* Golioth instance */
static struct golioth_client g_client;
static struct golioth_client *client = &g_client;

static uint8_t rx_buffer[MAX_COAP_MSG_LEN];

static struct sockaddr addr;

#define POLLFD_EVENT_RECONNECT	0
#define POLLFD_SOCKET		1

static struct zsock_pollfd fds[2];
static struct coap_reply coap_replies[2];

static K_SEM_DEFINE(golioth_client_ready, 0, 1);

static void client_request_reconnect(void)
{
	eventfd_write(fds[POLLFD_EVENT_RECONNECT].fd, 1);
}

static void client_rx_timeout_work(struct k_work *work)
{
	LOG_ERR("RX client timeout!");

	client_request_reconnect();
}

static K_WORK_DEFINE(rx_timeout_work, client_rx_timeout_work);

static void client_rx_timeout(struct k_timer *timer)
{
	k_work_submit(&rx_timeout_work);
}

static K_TIMER_DEFINE(rx_timeout, client_rx_timeout, NULL);

static void golioth_on_message(struct golioth_client *client,
			       struct coap_packet *rx)
{
	uint16_t payload_len;
	const uint8_t *payload;
	uint8_t type;

	type = coap_header_get_type(rx);
	payload = coap_packet_get_payload(rx, &payload_len);

	if (!IS_ENABLED(CONFIG_LOG_BACKEND_GOLIOTH) && payload) {
		LOG_HEXDUMP_DBG(payload, payload_len, "Payload");
	}

	(void)coap_response_received(rx, NULL, coap_replies,
				     ARRAY_SIZE(coap_replies));
}

static int init_tls(void)
{
	int err;

	err = tls_credential_add(PSK_TAG,
				TLS_CREDENTIAL_PSK,
				TLS_PSK,
				sizeof(TLS_PSK) - 1);
	if (err < 0) {
		LOG_ERR("Failed to register PSK: %d", err);
		return err;
	}

	err = tls_credential_add(PSK_TAG,
				TLS_CREDENTIAL_PSK_ID,
				TLS_PSK_ID,
				sizeof(TLS_PSK_ID) - 1);
	if (err < 0) {
		LOG_ERR("Failed to register PSK ID: %d", err);
		return err;
	}

	return 0;
}

static int initialize_client(void)
{
	sec_tag_t sec_tag_list[] = {
#if defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS)
		PSK_TAG,
#endif /* defined(CONFIG_NET_SOCKETS_SOCKOPT_TLS) */
	};
	int err;

	if (IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS)) {
		init_tls();
	}

	golioth_init(client);

	client->rx_buffer = rx_buffer;
	client->rx_buffer_len = sizeof(rx_buffer);

	client->on_message = golioth_on_message;

	if (IS_ENABLED(CONFIG_NET_SOCKETS_SOCKOPT_TLS)) {
		err = golioth_set_proto_coap_dtls(client, sec_tag_list,
						  ARRAY_SIZE(sec_tag_list));
	} else {
		err = golioth_set_proto_coap_udp(client, TLS_PSK_ID,
						 sizeof(TLS_PSK_ID) - 1);
	}
	if (err) {
		LOG_ERR("Failed to set protocol: %d", err);
		return err;
	}

	if (IS_ENABLED(CONFIG_NET_IPV4)) {
		struct sockaddr_in *addr4 = (struct sockaddr_in *) &addr;

		addr4->sin_family = AF_INET;
		addr4->sin_port = htons(CONFIG_GOLIOTH_SERVER_PORT);

		zsock_inet_pton(addr4->sin_family, CONFIG_GOLIOTH_SERVER_IP_ADDR,
				&addr4->sin_addr);

		client->server = (struct sockaddr *)addr4;
	} else if (IS_ENABLED(CONFIG_NET_IPV6)) {
		struct sockaddr_in6 *addr6 = (struct sockaddr_in6 *) &addr;

		addr6->sin6_family = AF_INET6;
		addr6->sin6_port = htons(CONFIG_GOLIOTH_SERVER_PORT);

		zsock_inet_pton(addr6->sin6_family, CONFIG_GOLIOTH_SERVER_IP_ADDR,
				&addr6->sin6_addr);

		client->server = (struct sockaddr *)addr6;
	}

	fds[POLLFD_EVENT_RECONNECT].fd = eventfd(0, EFD_NONBLOCK);
	fds[POLLFD_EVENT_RECONNECT].events = ZSOCK_POLLIN;

	if (IS_ENABLED(CONFIG_LOG_BACKEND_GOLIOTH)) {
		log_backend_golioth_init(client);
	}

	return 0;
}

static int connect_client(void)
{
	int err;
	int i;

	err = golioth_connect(client);
	if (err) {
		LOG_ERR("Failed to connect: %d", err);
		return err;
	}

	fds[POLLFD_SOCKET].fd = client->sock;
	fds[POLLFD_SOCKET].events = ZSOCK_POLLIN;

  for (i = 0; i < ARRAY_SIZE(coap_replies); i++) {
		coap_reply_clear(&coap_replies[i]);
	}

	return 0;
}

static void golioth_main(void *arg1, void *arg2, void *arg3)
{
	eventfd_t eventfd_value;
	int err;

	LOG_INF("Initializing golioth client");

	err = initialize_client();
	if (err) {
		LOG_ERR("Failed to initialize client: %d", err);
		return;
	}

	LOG_INF("Golioth client initialized");

  if (IS_ENABLED(CONFIG_NET_L2_WIFI_MGMT)) {
		LOG_INF("Connecting to WiFi");
		wifi_connect();
	}

	k_sem_give(&golioth_client_ready);

	while (true) {
		if (client->sock < 0) {
			LOG_INF("Starting connect");
			err = connect_client();
			if (err) {
				LOG_WRN("Failed to connect: %d", err);
				k_sleep(RX_TIMEOUT);
				continue;
			}

			/* Flush reconnect requests */
			(void)eventfd_read(fds[POLLFD_EVENT_RECONNECT].fd,
					   &eventfd_value);

			/* Add RX timeout */
			k_timer_start(&rx_timeout, RX_TIMEOUT, K_NO_WAIT);

			LOG_INF("Client connected!");
		}

		if (zsock_poll(fds, ARRAY_SIZE(fds), -1) < 0) {
			LOG_ERR("Error in poll:%d", errno);
			/* TODO: reconnect */
			break;
		}

		if (fds[POLLFD_EVENT_RECONNECT].revents) {
			(void)eventfd_read(fds[POLLFD_EVENT_RECONNECT].fd,
					   &eventfd_value);
			LOG_INF("Reconnect request");
			golioth_disconnect(client);
			continue;
		}

		if (fds[POLLFD_SOCKET].revents) {
			/* Restart timer */
			k_timer_start(&rx_timeout, RX_TIMEOUT, K_NO_WAIT);

			err = golioth_process_rx(client);
			if (err) {
				LOG_ERR("Failed to receive: %d", err);
				golioth_disconnect(client);
			}
		}
	}
}

K_THREAD_DEFINE(golioth_main_thread, 2048, golioth_main, NULL, NULL, NULL,
		K_LOWEST_APPLICATION_THREAD_PRIO, 0, 0);

/*
 * Get a device structure from a devicetree node with compatible
 * "bosch,bme280". (If there are multiple, just pick one.)
 */
static const struct device *get_bme280_device(void)
{
	const struct device *dev = DEVICE_DT_GET_ANY(bosch_bme280);

	if (dev == NULL) {
		/* No such node, or the node does not have status "okay". */
		printk("\nError: no device found.\n");
		return NULL;
	}

	if (!device_is_ready(dev)) {
		printk("\nError: Device \"%s\" is not ready; "
		       "check the driver initialization logs for errors.\n",
		       dev->name);
		return NULL;
	}

	printk("Found device \"%s\", getting sensor data\n", dev->name);
	return dev;
}

void main(void)
{
	char buffer[sizeof("{ \"temp\" : 00.00, \"press\" : 00.00, \"hum\" : 00.00 }")];	
	int err;

	LOG_DBG("Start Light DB Stream sample");

  const struct device *sensor = get_bme280_device();

	if (sensor == NULL) {
		return;
	}

	k_sem_take(&golioth_client_ready, K_FOREVER);

	while (true) {    
    struct sensor_value temp, press, humidity;

		sensor_sample_fetch(sensor);
		sensor_channel_get(sensor, SENSOR_CHAN_AMBIENT_TEMP, &temp);
		sensor_channel_get(sensor, SENSOR_CHAN_PRESS, &press);
		sensor_channel_get(sensor, SENSOR_CHAN_HUMIDITY, &humidity);

		snprintk(buffer, sizeof(buffer) - 1, "{\"temp\":%d.%02d,\"press\": %d.%02d,\"hum\":%d.%02d} ", temp.val1, temp.val2, press.val1, press.val2, humidity.val1, humidity.val2);

    LOG_INF("sending env data: %s", log_strdup(buffer));

		err = golioth_lightdb_stream_send(client,
					  GOLIOTH_LIGHTDB_STREAM_PATH("env"),
					  COAP_CONTENT_FORMAT_APP_JSON,
					  buffer, strlen(buffer));
		if (err) {
			LOG_WRN("Failed to send env data: %d", err);
		}

		k_sleep(K_SECONDS(60));
	}

	LOG_DBG("Quit");
}
