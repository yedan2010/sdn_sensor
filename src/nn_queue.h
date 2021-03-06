#pragma once

#include <stdint.h>

#include <json-c/json.h>
#include <json-c/json_object_private.h>

/* should be enough for the nanomsg queue URL */
#define NN_URL_MAX 256

enum nn_content_type_e {
    NN_OBJECT_PCAP    = 1,
    NN_OBJECT_SYSLOG  = 2,
    NN_OBJECT_SFLOW   = 3,
    NN_OBJECT_NETFLOW = 4,
    NN_OBJECT_MAX,
};

typedef enum nn_content_type_e nn_content_type_t;

enum nn_queue_format_e {
    NN_FORMAT_METADATA = 1,
    NN_FORMAT_PACKET   = 2,
    NN_FORMAT_MAX,
};

typedef enum nn_queue_format_e nn_queue_format_t;

struct nn_queue_s {
    int               conn;
    int               remote_id;
    nn_queue_format_t format;
    nn_content_type_t content;
    int               type;
    uint64_t          tx_messages;
    uint64_t          tx_bytes;
    uint64_t          tx_discards;
    char              url[NN_URL_MAX];
};

typedef struct nn_queue_s nn_queue_t;

/* BEGIN PROTOTYPES */

int ss_nn_queue_create(json_object* items, nn_queue_t* nn_queue);
int ss_nn_queue_destroy(nn_queue_t* nn_queue);
const char* ss_nn_queue_type_dump(int nn_queue_type);
const char* ss_nn_queue_format_dump(nn_queue_format_t nn_format);
const char* ss_nn_queue_content_dump(nn_content_type_t nn_type);
int ss_nn_queue_dump(nn_queue_t* nn_queue);
int ss_nn_queue_send(nn_queue_t* nn_queue, uint8_t* message, uint16_t length);

/* END PROTOTYPES */
