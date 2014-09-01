#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>

#include <bsd/string.h>

#include <pcap/pcap.h>

#include <rte_log.h>

#include "common.h"
#include "sdn_sensor.h"

/* COMMON */

char* ss_json_string_get(json_object* items, const char* key) {
    json_object* item;
    const char* value;
    char* rv;
    
    item = json_object_object_get(items, key);
    if (item == NULL) {
        fprintf(stderr, "key %s not present\n", key);
        return NULL;
    }
    if (!json_object_is_type(item, json_type_string)) {
        fprintf(stderr, "value for %s is not object\n", key);
        return NULL;
    }
    value = json_object_get_string(item);
    if (value == NULL) {
        fprintf(stderr, "value for %s is null\n", key);
        return NULL;
    }
    rv = strdup(value);
    if (rv == NULL) {
        fprintf(stderr, "could not allocate return value for %s\n", key);
        return NULL;
    }
    return rv;
}

int ss_nn_queue_create(json_object* items, nn_queue_t* nn_queue) {
    char* value;
    
    value = ss_json_string_get(items, "nm_queue_url");
    if (value == NULL) {
        fprintf(stderr, "nm_queue_url is null\n");
        goto error_out;
    }
    strlcpy(nn_queue->url, value, sizeof(nn_queue->url));
    
    value = ss_json_string_get(items, "nm_queue_type");
    if (value == NULL) {
        fprintf(stderr, "nm_queue_type is null\n");
        goto error_out;
    }
    if      (!strcasecmp(value, "BUS"))        nn_queue->type = NN_BUS;
    else if (!strcasecmp(value, "PAIR"))       nn_queue->type = NN_PAIR;
    else if (!strcasecmp(value, "PUSH"))       nn_queue->type = NN_PUSH;
    else if (!strcasecmp(value, "PULL"))       nn_queue->type = NN_PULL;
    else if (!strcasecmp(value, "PUB"))        nn_queue->type = NN_PUB;
    else if (!strcasecmp(value, "SUB"))        nn_queue->type = NN_SUB;
    else if (!strcasecmp(value, "REQ"))        nn_queue->type = NN_REQ;
    else if (!strcasecmp(value, "REP"))        nn_queue->type = NN_REP;
    else if (!strcasecmp(value, "SURVEYOR"))   nn_queue->type = NN_SURVEYOR;
    else if (!strcasecmp(value, "RESPONDENT")) nn_queue->type = NN_RESPONDENT;
    else {
        fprintf(stderr, "unknown nm_queue_type %s\n", value);
    }
    
    nn_queue->conn = nn_socket(AF_SP, nn_queue->type);
    if (nn_queue->conn < 0) {
        fprintf(stderr, "could not allocate nm queue socket: %s\n", nn_strerror(nn_errno()));
        goto error_out;
    }
    
    fprintf(stderr, "created nm_queue type %s url %s\n", value, nn_queue->url);
    
    return 0;
    
    error_out:
    ss_nn_queue_destroy(nn_queue);
    return -1;
}

int ss_nn_queue_destroy(nn_queue_t* nn_queue) {
    if (nn_queue->conn >= 0) { nn_close(nn_queue->conn); nn_queue->conn = -1; }
    nn_queue->type = -1;
    memset(nn_queue->url, 0, sizeof(nn_queue->url));
    return 0;
}

/* RE CHAIN */

int ss_re_chain_destroy(ss_re_chain_t* re_chain) {
    return 0;
}

ss_re_entry_t* ss_re_entry_create(json_object* re_json) {
    return NULL;
}

int ss_re_entry_destroy(ss_re_entry_t* re_entry) {
    return 0;
}
 
int ss_re_chain_add(ss_re_chain_t* re_chain, ss_re_entry_t* re_entry) {
    return 0;
}
 
int ss_re_chain_remove_index(ss_re_chain_t* re_chain, int index) {
    return 0;
}

int ss_re_chain_remove_re(ss_re_chain_t* re_chain, char* re) {
    return 0;
}

int ss_re_chain_match(ss_re_chain_t* re_chain, char* input) {
    return 0;
}

/* PCAP CHAIN */

int ss_pcap_chain_destroy(ss_pcap_chain_t* pcap_chain) {
    return 0;
}

ss_pcap_entry_t* ss_pcap_entry_create(json_object* pcap_json) {
    // mhall
    ss_pcap_entry_t* pcap_entry = NULL;
    int rv                      = -1;
    
    pcap_entry = malloc(sizeof(ss_pcap_entry_t));
    if (pcap_entry == NULL) {
        fprintf(stderr, "could not create pcap entry\n");
        goto error_out;
    }
    memset(pcap_entry, 0, sizeof(ss_pcap_entry_t));
    
    if (!pcap_json) {
        fprintf(stderr, "empty pcap configuration entry\n");
        goto error_out;
    }
    if (!json_object_is_type(pcap_json, json_type_object)) {
        fprintf(stderr, "pcap_json is not object\n");
        goto error_out;
    }
    
    // name, filter, nm_queue_format, nm_queue_type, nm_queue_url
    // char* ss_json_string_get(json_object* items, char* key) {
    pcap_entry->name = ss_json_string_get(pcap_json, "name");
    if (pcap_entry->name == NULL) {
        fprintf(stderr, "pcap_entry name is null\n");
        goto error_out;
    }
    pcap_entry->filter = ss_json_string_get(pcap_json, "filter");
    if (pcap_entry->filter == NULL) {
        fprintf(stderr, "pcap_entry filter is null\n");
        goto error_out;
    }
    
    rv = ss_nn_queue_create(pcap_json, &pcap_entry->nn_queue);
    if (rv) {
        fprintf(stderr, "could not create pcap nm_queue\n");
        goto error_out;
    }
    
    rv = pcap_compile(ss_pcap, &pcap_entry->bpf_filter, pcap_entry->filter, 1 /* optimize */, PCAP_NETMASK_UNKNOWN);
    if (rv) {
        fprintf(stderr, "could not compile pcap filter [%s]: %s\n",
            pcap_entry->filter, pcap_geterr(ss_pcap));
        goto error_out;
    }
    
    fprintf(stderr, "created pcap entry [%s]\n", pcap_entry->name);
    return pcap_entry;
    
    error_out:
    ss_pcap_entry_destroy(pcap_entry);
    return NULL;
}

int ss_pcap_entry_destroy(ss_pcap_entry_t* pcap_entry) {
    pcap_entry->match_count = 0;
    if (pcap_entry)             pcap_freecode(&pcap_entry->bpf_filter);
    if (pcap_entry->name)       free(pcap_entry->name);       pcap_entry->name = NULL;
    if (pcap_entry->filter)     free(pcap_entry->filter);     pcap_entry->filter = NULL;
    if (pcap_entry)             free(pcap_entry);             pcap_entry = NULL;
    return 0;
}

int ss_pcap_chain_add(ss_pcap_chain_t* pcap_match, ss_pcap_entry_t* pcap_entry) {
    TAILQ_INSERT_TAIL(&pcap_match->pcap_list, pcap_entry, entry);
    return 0;
}

int ss_pcap_chain_remove_index(ss_pcap_chain_t* pcap_match, int index) {
    return 0;
}

int ss_pcap_chain_remove_filter(ss_pcap_chain_t* pcap_match, char* filter) {
    return 0;
}

int ss_pcap_match_prepare(ss_pcap_match_t* pcap_match, uint8_t* packet, uint16_t length) {
    // XXX: set to useless values for speed
    pcap_match->header.ts.tv_sec  = 0;
    pcap_match->header.ts.tv_usec = 0;
    pcap_match->header.caplen     = length;
    pcap_match->header.len        = length;
    pcap_match->packet            = packet;
    return 0;
}

int ss_pcap_match(ss_pcap_chain_t* pcap_chain, ss_pcap_match_t* pcap_match) {
    return 0;
}

/* CIDR TABLE */

ss_cidr_table_t* ss_cidr_table_create(json_object* cidr_table_json) {
    return NULL;
}

int ss_cidr_table_destroy(ss_cidr_table_t* cidr_table) {
    return 0;
}

ss_cidr_entry_t* ss_cidr_entry_create(json_object* cidr_json) {
    return NULL;
}

int ss_cidr_entry_destroy(ss_cidr_entry_t* cidr_entry) {
    return 0;
}

int ss_cidr_table_add(ss_cidr_table_t* cidr_table, ss_cidr_entry_t* cidr_entry) {
    return 0;
}

int ss_cidr_table_remove(ss_cidr_table_t* cidr_table, char* cidr) {
    return 0;
}
