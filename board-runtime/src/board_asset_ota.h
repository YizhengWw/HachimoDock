/*
 * board_asset_ota.h — public entry points for the appearance/widget OTA module.
 *
 * The nine br_handle_* handlers are dispatched by board_server.c's USB/MQTT
 * message router. The remaining helpers are exposed so the transfer mechanics
 * can be unit-tested directly (asset_transaction_tests.c). Everything else in
 * board_asset_ota.c stays file-static.
 */
#ifndef BOARD_ASSET_OTA_H
#define BOARD_ASSET_OTA_H

#include <stddef.h>

#include "board_server_state.h"

/* Message-router entry points (called from board_server.c). */
void br_handle_asset_begin(br_server_state *server, const char *payload);
void br_handle_asset_stat(br_server_state *server, const char *payload);
void br_handle_asset_chunk(br_server_state *server, const char *payload);
void br_handle_asset_commit(br_server_state *server, const char *payload);
void br_handle_asset_file_commit(br_server_state *server, const char *payload);
void br_handle_asset_patch_commit(br_server_state *server, const char *payload);
void br_handle_widget_install_begin(br_server_state *server, const char *payload);
void br_handle_widget_install_chunk(br_server_state *server, const char *payload);
void br_handle_widget_install_commit(br_server_state *server, const char *payload);

/* Shared transfer mechanics — return NULL on success or a short error string. */
const char *br_ota_prepare_staging(br_server_state *server, const char *staging_name);
const char *br_ota_stream_chunk(br_server_state *server, const char *staging_name,
                                const char *payload, char *tid, size_t tid_size);
const char *br_ota_rotate_activate(const char *staging, const char *target, const char *previous);

/* Staging/tree helpers exposed for unit tests. */
int br_asset_write_all(int fd, const char *data, size_t len);
int br_asset_mkdir_p(const char *path);
int br_asset_remove_tree(const char *path);
bool br_asset_is_audio_patch_path(const char *rel);
int br_asset_decode_b64_file(const char *b64_path, const char *outpath);
int br_asset_file_stats_checksum(const char *path, unsigned long long *size_out, char checksum_hex[17]);
int br_asset_tree_stats(const char *dir_path, unsigned long long *file_count,
                        unsigned long long *byte_count, bool *has_b64);

#endif /* BOARD_ASSET_OTA_H */
