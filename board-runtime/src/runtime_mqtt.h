#ifndef BOARD_RUNTIME_MQTT_H
#define BOARD_RUNTIME_MQTT_H

#include <stdbool.h>
#include <stddef.h>

typedef void (*br_mqtt_publish_callback)(const char *topic, const char *payload, void *userdata);

typedef struct {
  int socket_fd;
  bool connected;
  char url[256];
  char host[128];
  int port;
  char client_id[128];
  char username[128];
  char password[128];
  char will_topic[256];
  char will_payload[1024];
  int keepalive_seconds;
  unsigned short next_packet_id;
  long long last_read_ms;
  long long last_write_ms;
  long long last_connect_attempt_ms;
  br_mqtt_publish_callback on_publish;
  void *userdata;
} br_mqtt_client;

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
);
void br_mqtt_client_close(br_mqtt_client *client);
int br_mqtt_client_ensure_connected(br_mqtt_client *client, int reconnect_delay_ms);
int br_mqtt_client_poll(br_mqtt_client *client, int timeout_ms);
int br_mqtt_client_publish(
  br_mqtt_client *client,
  const char *topic,
  const char *payload,
  bool retain
);
int br_mqtt_client_subscribe(br_mqtt_client *client, const char *topic);
int br_mqtt_client_unsubscribe(br_mqtt_client *client, const char *topic);

#endif
