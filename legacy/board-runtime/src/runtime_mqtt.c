#include "runtime_mqtt.h"

#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

#include "runtime_common.h"

static int br_mqtt_write_all(int fd, const unsigned char *data, size_t length) {
  while (length > 0) {
    ssize_t written = send(fd, data, length, 0);
    if (written < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    data += (size_t) written;
    length -= (size_t) written;
  }
  return 0;
}

static int br_mqtt_read_exact(int fd, unsigned char *data, size_t length) {
  while (length > 0) {
    ssize_t read_size = recv(fd, data, length, 0);
    if (read_size == 0) {
      return -1;
    }
    if (read_size < 0) {
      if (errno == EINTR) {
        continue;
      }
      return -1;
    }
    data += (size_t) read_size;
    length -= (size_t) read_size;
  }
  return 0;
}

static size_t br_mqtt_encode_length(unsigned char *output, size_t value) {
  size_t used = 0;
  do {
    unsigned char byte = (unsigned char) (value % 128U);
    value /= 128U;
    if (value > 0) {
      byte |= 0x80U;
    }
    output[used++] = byte;
  } while (value > 0 && used < 4);
  return used;
}

static int br_mqtt_append_string(unsigned char *buffer, size_t size, size_t *used, const char *text) {
  size_t length = strlen(text);
  if (*used + 2 + length > size) {
    return -1;
  }
  buffer[*used] = (unsigned char) ((length >> 8) & 0xffU);
  buffer[*used + 1] = (unsigned char) (length & 0xffU);
  memcpy(buffer + *used + 2, text, length);
  *used += 2 + length;
  return 0;
}

static int br_mqtt_send_packet(br_mqtt_client *client, unsigned char header, const unsigned char *payload, size_t payload_size) {
  unsigned char fixed[5];
  if (!client || client->socket_fd < 0) {
    return -1;
  }
  fixed[0] = header;
  size_t fixed_size = 1 + br_mqtt_encode_length(fixed + 1, payload_size);
  if (br_mqtt_write_all(client->socket_fd, fixed, fixed_size) != 0) {
    return -1;
  }
  if (payload_size > 0 && br_mqtt_write_all(client->socket_fd, payload, payload_size) != 0) {
    return -1;
  }
  client->last_write_ms = br_now_ms();
  return 0;
}

void br_mqtt_client_init(
  br_mqtt_client *client,
  const char *url,
  const char *client_id,
  const char *username,
  const char *password,
  const char *will_topic,
  const char *will_payload,
  int keepalive_seconds,
  br_mqtt_publish_callback on_publish,
  void *userdata
) {
  memset(client, 0, sizeof(*client));
  client->socket_fd = -1;
  br_normalize_text(url, "mqtt://broker.openclaw.example:1883", client->url, sizeof(client->url));
  br_normalize_text(client_id, "board-runtime", client->client_id, sizeof(client->client_id));
  br_normalize_text(username, "", client->username, sizeof(client->username));
  br_normalize_text(password, "", client->password, sizeof(client->password));
  br_normalize_text(will_topic, "", client->will_topic, sizeof(client->will_topic));
  br_normalize_text(will_payload, "", client->will_payload, sizeof(client->will_payload));
  client->keepalive_seconds = keepalive_seconds > 0 ? keepalive_seconds : 30;
  client->next_packet_id = 1;
  client->on_publish = on_publish;
  client->userdata = userdata;

  br_mqtt_endpoint endpoint;
  if (br_parse_mqtt_url(client->url, &endpoint)) {
    br_normalize_text(endpoint.host, "127.0.0.1", client->host, sizeof(client->host));
    client->port = endpoint.port;
  } else {
    br_normalize_text("127.0.0.1", "127.0.0.1", client->host, sizeof(client->host));
    client->port = 1883;
  }
}

void br_mqtt_client_close(br_mqtt_client *client) {
  if (!client) {
    return;
  }
  if (client->socket_fd >= 0) {
    close(client->socket_fd);
  }
  client->socket_fd = -1;
  client->connected = false;
}

static int br_mqtt_open_socket(const char *host, int port) {
  struct addrinfo hints;
  struct addrinfo *result = NULL;
  struct addrinfo *entry = NULL;
  char port_text[16];
  int fd = -1;

  memset(&hints, 0, sizeof(hints));
  hints.ai_family = AF_UNSPEC;
  hints.ai_socktype = SOCK_STREAM;
  snprintf(port_text, sizeof(port_text), "%d", port);
  if (getaddrinfo(host, port_text, &hints, &result) != 0) {
    return -1;
  }

  for (entry = result; entry; entry = entry->ai_next) {
    fd = socket(entry->ai_family, entry->ai_socktype, entry->ai_protocol);
    if (fd < 0) {
      continue;
    }
    /* Non-blocking connect with 2s timeout to avoid stalling the main loop. */
    {
      int flags = fcntl(fd, F_GETFL, 0);
      fcntl(fd, F_SETFL, flags | O_NONBLOCK);
      int rc = connect(fd, entry->ai_addr, entry->ai_addrlen);
      if (rc == 0) {
        fcntl(fd, F_SETFL, flags);
        break;
      }
      if (errno == EINPROGRESS) {
        fd_set wset;
        struct timeval tv;
        FD_ZERO(&wset);
        FD_SET(fd, &wset);
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        if (select(fd + 1, NULL, &wset, NULL, &tv) > 0) {
          int err = 0;
          socklen_t len = sizeof(err);
          getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &len);
          if (err == 0) {
            fcntl(fd, F_SETFL, flags);
            break;
          }
        }
      }
    }
    close(fd);
    fd = -1;
  }

  freeaddrinfo(result);
  return fd;
}

static int br_mqtt_send_connect(br_mqtt_client *client) {
  unsigned char payload[2048];
  unsigned char response[8];
  size_t used = 0;
  unsigned char flags = 0x02;  /* clean session */

  if (br_mqtt_append_string(payload, sizeof(payload), &used, "MQTT") != 0) {
    return -1;
  }
  payload[used++] = 4;  /* protocol level */
  if (client->will_topic[0] != '\0') {
    flags |= 0x24;  /* will flag + will QoS 1 */
    flags |= 0x20;  /* will retain */
  }
  if (client->username[0] != '\0') {
    flags |= 0x80;  /* username flag */
  }
  if (client->password[0] != '\0') {
    flags |= 0x40;  /* password flag */
  }
  payload[used++] = flags;
  payload[used++] = (unsigned char) ((client->keepalive_seconds >> 8) & 0xffU);
  payload[used++] = (unsigned char) (client->keepalive_seconds & 0xffU);
  if (br_mqtt_append_string(payload, sizeof(payload), &used, client->client_id) != 0) {
    return -1;
  }
  if (client->will_topic[0] != '\0') {
    if (br_mqtt_append_string(payload, sizeof(payload), &used, client->will_topic) != 0) {
      return -1;
    }
    if (br_mqtt_append_string(payload, sizeof(payload), &used, client->will_payload) != 0) {
      return -1;
    }
  }
  if (client->username[0] != '\0') {
    if (br_mqtt_append_string(payload, sizeof(payload), &used, client->username) != 0) {
      return -1;
    }
  }
  if (client->password[0] != '\0') {
    if (br_mqtt_append_string(payload, sizeof(payload), &used, client->password) != 0) {
      return -1;
    }
  }

  if (br_mqtt_send_packet(client, 0x10U, payload, used) != 0) {
    return -1;
  }

  if (br_mqtt_read_exact(client->socket_fd, response, 4) != 0) {
    return -1;
  }
  if (response[0] != 0x20U || response[1] != 0x02U || response[3] != 0x00U) {
    return -1;
  }
  client->last_read_ms = br_now_ms();
  client->connected = true;
  return 0;
}

int br_mqtt_client_ensure_connected(br_mqtt_client *client, int reconnect_delay_ms) {
  long long now;

  if (!client) {
    return -1;
  }
  if (client->connected && client->socket_fd >= 0) {
    return 0;
  }

  now = br_now_ms();
  if (client->last_connect_attempt_ms > 0 &&
      now - client->last_connect_attempt_ms < reconnect_delay_ms) {
    return -1;
  }
  client->last_connect_attempt_ms = now;

  br_mqtt_client_close(client);
  client->socket_fd = br_mqtt_open_socket(client->host, client->port);
  if (client->socket_fd < 0) {
    return -1;
  }
  if (br_mqtt_send_connect(client) != 0) {
    br_mqtt_client_close(client);
    return -1;
  }
  return 0;
}

static unsigned short br_mqtt_next_packet_id(br_mqtt_client *client) {
  unsigned short packet_id = client->next_packet_id++;
  if (client->next_packet_id == 0) {
    client->next_packet_id = 1;
  }
  return packet_id;
}

static int br_mqtt_read_packet(br_mqtt_client *client, unsigned char *header, unsigned char *payload, size_t payload_size, size_t *payload_length) {
  unsigned char fixed[4];
  size_t used = 0;
  size_t multiplier = 1;
  size_t value = 0;
  unsigned char byte = 0;

  if (br_mqtt_read_exact(client->socket_fd, header, 1) != 0) {
    return -1;
  }
  do {
    if (used >= sizeof(fixed)) {
      return -1;
    }
    if (br_mqtt_read_exact(client->socket_fd, &byte, 1) != 0) {
      return -1;
    }
    fixed[used++] = byte;
    value += (size_t) (byte & 0x7fU) * multiplier;
    multiplier *= 128U;
  } while ((byte & 0x80U) != 0);

  if (value > payload_size) {
    return -1;
  }
  if (value > 0 && br_mqtt_read_exact(client->socket_fd, payload, value) != 0) {
    return -1;
  }
  *payload_length = value;
  client->last_read_ms = br_now_ms();
  return 0;
}

static void br_mqtt_handle_publish(
  br_mqtt_client *client,
  unsigned char header,
  const unsigned char *payload,
  size_t payload_length
) {
  size_t position = 0;
  size_t topic_length;
  unsigned short packet_id = 0;

  if (payload_length < 2) {
    return;
  }
  topic_length = ((size_t) payload[0] << 8U) | (size_t) payload[1];
  position = 2;
  if (position + topic_length > payload_length) {
    return;
  }

  char topic[BR_MAX_TOPIC];
  char text[BR_MAX_JSON];
  size_t copy_length = topic_length < sizeof(topic) - 1 ? topic_length : sizeof(topic) - 1;
  memcpy(topic, payload + position, copy_length);
  topic[copy_length] = '\0';
  position += topic_length;

  if (((header >> 1U) & 0x03U) == 1U) {
    if (position + 2 > payload_length) {
      return;
    }
    packet_id = (unsigned short) (((unsigned short) payload[position] << 8U) | payload[position + 1]);
    position += 2;
  }

  size_t body_length = payload_length - position;
  if (body_length >= sizeof(text)) {
    body_length = sizeof(text) - 1;
  }
  memcpy(text, payload + position, body_length);
  text[body_length] = '\0';
  if (client->on_publish) {
    client->on_publish(topic, text, client->userdata);
  }

  if (packet_id != 0U) {
    unsigned char ack[2];
    ack[0] = (unsigned char) ((packet_id >> 8U) & 0xffU);
    ack[1] = (unsigned char) (packet_id & 0xffU);
    br_mqtt_send_packet(client, 0x40U, ack, sizeof(ack));
  }
}

int br_mqtt_client_poll(br_mqtt_client *client, int timeout_ms) {
  fd_set read_set;
  struct timeval timeout;
  unsigned char header = 0;
  unsigned char payload[BR_MAX_JSON];
  size_t payload_length = 0;
  long long now;

  if (!client || !client->connected || client->socket_fd < 0) {
    return -1;
  }

  FD_ZERO(&read_set);
  FD_SET(client->socket_fd, &read_set);
  timeout.tv_sec = timeout_ms / 1000;
  timeout.tv_usec = (timeout_ms % 1000) * 1000;

  int ready = select(client->socket_fd + 1, &read_set, NULL, NULL, &timeout);
  if (ready < 0) {
    br_mqtt_client_close(client);
    return -1;
  }
  if (ready > 0 && FD_ISSET(client->socket_fd, &read_set)) {
    if (br_mqtt_read_packet(client, &header, payload, sizeof(payload), &payload_length) != 0) {
      br_mqtt_client_close(client);
      return -1;
    }
    switch (header & 0xf0U) {
      case 0x30U:
        br_mqtt_handle_publish(client, header, payload, payload_length);
        break;
      case 0xd0U:
        break;
      default:
        break;
    }
  }

  now = br_now_ms();
  if (client->last_write_ms == 0 || now - client->last_write_ms >= 20000) {
    br_mqtt_send_packet(client, 0xc0U, NULL, 0);
  }
  if (client->last_read_ms > 0 && now - client->last_read_ms >= (long long) client->keepalive_seconds * 2000LL) {
    br_mqtt_client_close(client);
    return -1;
  }
  return 0;
}

int br_mqtt_client_publish(
  br_mqtt_client *client,
  const char *topic,
  const char *payload,
  bool retain
) {
  unsigned char packet[BR_MAX_JSON];
  size_t used = 0;
  if (!client || !client->connected || !topic || !payload) {
    return -1;
  }
  if (br_mqtt_append_string(packet, sizeof(packet), &used, topic) != 0) {
    return -1;
  }
  size_t payload_length = strlen(payload);
  if (used + payload_length > sizeof(packet)) {
    return -1;
  }
  memcpy(packet + used, payload, payload_length);
  used += payload_length;
  return br_mqtt_send_packet(client, (unsigned char) (0x30U | (retain ? 0x01U : 0x00U)), packet, used);
}

int br_mqtt_client_subscribe(br_mqtt_client *client, const char *topic) {
  unsigned char packet[512];
  size_t used = 0;
  unsigned short packet_id;
  if (!client || !client->connected || !topic) {
    return -1;
  }
  packet_id = br_mqtt_next_packet_id(client);
  packet[used++] = (unsigned char) ((packet_id >> 8U) & 0xffU);
  packet[used++] = (unsigned char) (packet_id & 0xffU);
  if (br_mqtt_append_string(packet, sizeof(packet), &used, topic) != 0) {
    return -1;
  }
  packet[used++] = 0x00U;
  return br_mqtt_send_packet(client, 0x82U, packet, used);
}

int br_mqtt_client_unsubscribe(br_mqtt_client *client, const char *topic) {
  unsigned char packet[512];
  size_t used = 0;
  unsigned short packet_id;
  if (!client || !client->connected || !topic) {
    return -1;
  }
  packet_id = br_mqtt_next_packet_id(client);
  packet[used++] = (unsigned char) ((packet_id >> 8U) & 0xffU);
  packet[used++] = (unsigned char) (packet_id & 0xffU);
  if (br_mqtt_append_string(packet, sizeof(packet), &used, topic) != 0) {
    return -1;
  }
  return br_mqtt_send_packet(client, 0xa2U, packet, used);
}
