#include <errno.h>
#include <getopt.h>
#include <limits.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <wordexp.h>
#include <sys/types.h>

#include <rte_branch_prediction.h>
#include <rte_common.h>
#include <rte_config.h>
#include <rte_cycles.h>
#include <rte_eal.h>
#include <rte_ether.h>
#include <rte_ethdev.h>
#include <rte_interrupts.h>
#include <rte_launch.h>
#include <rte_lcore.h>
#include <rte_log.h>
#include <rte_mbuf.h>
#include <rte_mempool.h>
#include <rte_power.h>
#include <rte_prefetch.h>
#include <rte_spinlock.h>
#include <rte_timer.h>

#include <pcap/pcap.h>

#include "common.h"
#include "dpdk.h"
#include "ethernet.h"
#include "je_utils.h"
#include "re_utils.h"
#include "sdn_sensor.h"
#include "sensor_conf.h"
#include "sflow_cb.h"
#include "tcp.h"

/* GLOBAL VARIABLES */

pcap_t*        ss_pcap = NULL;
ss_conf_t*     ss_conf = NULL;
rte_mempool_t* ss_pool[SOCKET_COUNT] = { NULL };

/* ethernet addresses of ports */
struct ether_addr port_eth_addrs[RTE_MAX_ETHPORTS];

static mbuf_table_entry_t mbuf_table[RTE_MAX_ETHPORTS][RTE_MAX_LCORE];

static ss_port_statistics_t port_statistics[RTE_MAX_ETHPORTS] __rte_cache_aligned;
static ss_core_statistics_t core_statistics[RTE_MAX_LCORE] __rte_cache_aligned;
static rte_timer_t          power_timers[RTE_MAX_LCORE] __rte_cache_aligned;

static uint8_t port_count = 0;

// http://www.ndsl.kaist.edu/~kyoungsoo/papers/TR-symRSS.pdf
static uint8_t rss_key[] = {
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
    0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a, 0x6d, 0x5a,
};

static struct rte_eth_conf port_conf = {
    .rxmode = {
        .mq_mode        = ETH_MQ_RX_RSS,
        .max_rx_pkt_len = MBUF_SIZE,
        .split_hdr_size = 0,
        .header_split   = 0, // Header Split disabled
        .hw_ip_checksum = 0, // IP checksum offload disabled
        .hw_vlan_filter = 0, // VLAN filtering disabled
        .jumbo_frame    = 0, // Jumbo Frame Support disabled
        .hw_strip_crc   = 0, // CRC stripped by hardware
    },
    .rx_adv_conf = {
        .rss_conf = {
            .rss_key    = rss_key,
            .rss_hf     = ETH_RSS_IP,
        },
    },
    .txmode = {
        .mq_mode        = ETH_MQ_TX_NONE,
    },
    .intr_conf = {
        .lsc = 1,
        .rxq = 1,
    },
};

static const struct rte_eth_rxconf rx_conf = {
    .rx_thresh = {
        .pthresh = RX_PTHRESH,
        .hthresh = RX_HTHRESH,
        .wthresh = RX_WTHRESH,
    },
};

static const struct rte_eth_txconf tx_conf = {
    .tx_thresh = {
        .pthresh = TX_PTHRESH,
        .hthresh = TX_HTHRESH,
        .wthresh = TX_WTHRESH,
    },
    .tx_free_thresh = 0, // Use PMD default values
    .tx_rs_thresh = 1,   // Use PMD default values
    .txq_flags = ETH_TXQ_FLAGS_NOMULTSEGS | ETH_TXQ_FLAGS_NOOFFLOADS,
};

/* TX burst of packets on a port */
int ss_send_burst(uint8_t port_id, uint16_t lcore_id) {
    unsigned int count;
    rte_mbuf_t** mbufs;
    unsigned int rv;

    count = mbuf_table[port_id][lcore_id].length;
    mbufs = (rte_mbuf_t**) mbuf_table[port_id][lcore_id].mbufs;

    rv = rte_eth_tx_burst(port_id, (uint16_t) lcore_id, mbufs, (uint16_t) count);
    port_statistics[port_id].tx += rv;
    if (unlikely(rv < count)) {
        port_statistics[port_id].dropped += (count - rv);
        do {
            rte_pktmbuf_free(mbufs[rv]);
        } while (++rv < count);
    }

    return 0;
}

/* Queue and prepare packets for TX in a burst */
int ss_send_packet(rte_mbuf_t* mbuf, uint8_t port_id, uint16_t lcore_id) {
    mbuf_table_entry_t* mbuf_entry;
    unsigned int length;

    mbuf_entry = &mbuf_table[port_id][lcore_id];
    length     = mbuf_entry->length;
    mbuf_entry->mbufs[length] = mbuf;
    length++;

    /* enough pkts to be sent */
    if (unlikely(length == BURST_PACKETS_MAX)) {
        ss_send_burst(port_id, lcore_id);
        length = 0;
    }

    mbuf_entry->length = length;
    return 0;
}

static void ss_timer_callback(uint16_t lcore_id, uint64_t* timer_tsc) {
    uint8_t port_id;

    for (port_id = 0; port_id < RTE_MAX_ETHPORTS; port_id++) {
        //RTE_LOG(INFO, SS, "attempt send for port %d\n", port_id);
        if (mbuf_table[port_id][lcore_id].length == 0) {
            //RTE_LOG(INFO, SS, "send no frames for port %d\n", port_id);
            continue;
        }
        //RTE_LOG(INFO, SS, "send %u frames for port %d\n", queue_conf->tx_table[port_id].length, port_id);
        ss_send_burst((uint8_t) port_id, lcore_id);
        mbuf_table[port_id][lcore_id].length = 0;
    }

    // return if statistics timer is not ready yet
    if (likely(*timer_tsc < ss_conf->timer_cycles)) return;

    // return if not on master lcore
    if (likely(lcore_id != rte_get_master_lcore())) return;

    double elapsed = *timer_tsc / (double) rte_get_tsc_hz();
    RTE_LOG(NOTICE, SS, "call ss_port_stats_print after %011.6f secs.\n", elapsed);
    ss_port_stats_print(port_statistics, port_count);

    ss_tcp_timer_callback();
    sflow_timer_callback();

    *timer_tsc = 0;
}

void ss_power_timer_callback(struct rte_timer* timer, void* arg) {
    uint64_t hz;
    double sleep_ratio;
    unsigned lcore_id = rte_lcore_id();

    // collect total execution time in msecs so far
    sleep_ratio = (double) (core_statistics[lcore_id].sleep_msecs) / (double) SCALING_PERIOD;

    // scale down if the core sleeps a lot
    if (sleep_ratio >= SCALING_TIME_RATIO) {
        if (rte_power_freq_down) {
            rte_power_freq_down(lcore_id);
        }
    }
    else if ( (unsigned)(core_statistics[lcore_id].rx_processed /
        core_statistics[lcore_id].loop_iterations) < BURST_PACKETS_MAX) {
        // scale down if the core does not have large bursts
        if (rte_power_freq_down) {
            rte_power_freq_down(lcore_id);
        }
    }

    // re-arm timer with current freq so it fires relatively consistently
    hz = rte_get_timer_hz();
    rte_timer_reset(&power_timers[lcore_id], hz / TIMER_TICKS_PER_SEC, SINGLE, lcore_id, ss_power_timer_callback, NULL);

    core_statistics[lcore_id].rx_processed = 0;
    core_statistics[lcore_id].loop_iterations = 0;

    core_statistics[lcore_id].sleep_msecs = 0;
}

uint32_t ss_power_check_idle(uint64_t zero_rx) {
    // If zero count is less than 100, sleep 1 us
    // If zero count is less than 1000, sleep 100 us
    // This is is the minimum latency between C3/C6 and C0
    if (zero_rx < LCORE_SUSPEND_USECS)
        return LCORE_SLEEP_USECS;
    else {
        return LCORE_SUSPEND_USECS;
    }
}

ss_freq_hint_t ss_power_check_scale_up(uint16_t lcore_id, uint8_t port_id) {
    if (likely(rte_eth_rx_descriptor_done(port_id, lcore_id, BURST_PACKETS_BATCH_3) > 0)) {
        core_statistics[lcore_id].freq_trend = 0;
        return FREQ_HIGHEST;
    }
    else if (likely(rte_eth_rx_descriptor_done(port_id, lcore_id, BURST_PACKETS_BATCH_2) > 0)) {
        core_statistics[lcore_id].freq_trend += BURST_TREND_BATCH_2;
    }
    else if (likely(rte_eth_rx_descriptor_done(port_id, lcore_id, BURST_PACKETS_BATCH_1) > 0)) {
        core_statistics[lcore_id].freq_trend += BURST_TREND_BATCH_1;
    }

    if (likely(core_statistics[lcore_id].freq_trend > BURST_TREND_FREQ_UP)) {
        core_statistics[lcore_id].freq_trend = 0;
        return FREQ_HIGHER;
    }

    return FREQ_CURRENT;
}

int ss_power_irq_register(uint16_t lcore_id) {
    uint32_t data;
    int rv;

    for (uint8_t port_id = 0; port_id < port_count; ++port_id) {
        data = (uint32_t) (port_id << CHAR_BIT | lcore_id);

        rv = rte_eth_dev_rx_intr_ctl_q(
            port_id, lcore_id,
            RTE_EPOLL_PER_THREAD, RTE_INTR_EVENT_ADD,
            (void*) ((uintptr_t) data)
        );

        if (rv) {
            RTE_LOG(ERR, SS, "could not register irq for port_id %u lcore_id %u\n", port_id, lcore_id);
            return rv;
        }
    }

    return 0;
}

int ss_power_irq_enable(uint16_t lcore_id) {
    int rv;
    for (uint8_t port_id = 0; port_id < port_count; ++port_id) {
        rte_spinlock_lock(&port_statistics[port_id].port_lock);
        rv = rte_eth_dev_rx_intr_enable(port_id, lcore_id);
        rte_spinlock_unlock(&port_statistics[port_id].port_lock);
        if (rv) return rv;
    }
    return 0;
}

// force polling thread sleep until one-shot rx interrupt triggers
int ss_power_irq_handle() {
    struct rte_epoll_event event[port_count];
    int rv;
    uint8_t port_id, queue_id;
    void *data;

    RTE_LOG(INFO, SS, "lcore_id %u sleeping for irq...\n", rte_lcore_id());

    rv = rte_epoll_wait(RTE_EPOLL_PER_THREAD, event, port_count, -1);
    for (int i = 0; i < rv; i++) {
        data = event[i].epdata.data;
        port_id  = ((uintptr_t) data) >> CHAR_BIT & 0x0ff;
        queue_id = ((uintptr_t) data) & RTE_LEN2MASK(CHAR_BIT, uint8_t);
        rte_eth_dev_rx_intr_disable(port_id, queue_id);
        RTE_LOG(INFO, SS,
            "lcore_id %u port_id %d queue_id %d waking from irq...\n",
            rte_lcore_id(), port_id, queue_id);
    }

    return 0;
}

/* main processing loop */
int ss_main_loop(__attribute__((unused)) void* arg) __attribute__ ((noreturn)) {
    rte_mbuf_t* mbufs[BURST_PACKETS_MAX];
    rte_mbuf_t* mbuf;
    ss_queue_statistics_t queue_statistics[RTE_MAX_ETHPORTS];

    uint16_t lcore_id;
    uint16_t socket_id;
    uint64_t prev_tsc, diff_tsc, curr_tsc, timer_tsc;
    uint64_t prev_tsc_power, curr_tsc_power, diff_tsc_power;
    uint64_t drain_tsc = (rte_get_tsc_hz() + US_PER_S - 1) / US_PER_S * BURST_TX_DRAIN_USECS;
    unsigned int rx_count;
    uint8_t i;
    uint8_t port_id;
    int rv;

    ss_freq_hint_t freq_hint;
    uint64_t idle_count = 0;
    uint32_t idle_hint = 0;
    int irq_enabled = 0;

    prev_tsc = 0;
    timer_tsc = 0;
    prev_tsc_power = 0;

    lcore_id   = (uint16_t) rte_lcore_id();
    socket_id  = (uint16_t) rte_socket_id();

    rv = ss_power_irq_register(lcore_id);
    if (rv) {
        RTE_LOG(WARNING, SS, "rx_irq could not be enabled\n");
    }
    else {
        RTE_LOG(INFO, SS, "rx_irq enabled successfully\n");
        irq_enabled = 1;
    }

    RTE_LOG(INFO, SS, "entering main loop on lcore_id %u\n", lcore_id);

    prev_tsc = 0;

    while (1) {
        core_statistics[lcore_id].loop_iterations++;

        curr_tsc = rte_rdtsc();
        curr_tsc_power = curr_tsc;

start_tx:
        /* TX queue drain */
        diff_tsc = curr_tsc - prev_tsc;
        if (unlikely(diff_tsc > drain_tsc)) {
            timer_tsc += diff_tsc;
            ss_timer_callback(lcore_id, &timer_tsc);
            prev_tsc = curr_tsc;
        }

        diff_tsc_power = curr_tsc_power - prev_tsc_power;
        if (diff_tsc_power > TIMER_RESOLUTION_CYCLES) {
            rte_timer_manage();
            prev_tsc_power = curr_tsc_power;
        }

start_rx:
        /* RX queue processing */
        freq_hint = FREQ_CURRENT;
        idle_count = 0;
        for (port_id = 0; port_id < port_count; port_id++) {
            rx_count = rte_eth_rx_burst((uint8_t) port_id, lcore_id, mbufs, BURST_PACKETS_MAX);
            core_statistics[lcore_id].rx_processed += rx_count;
            if (unlikely(rx_count == 0)) {
                // no rx packets
                // allow lcore to enter C states
                queue_statistics[port_id].zero_rx++;
                if (queue_statistics[port_id].zero_rx <= ZERO_RX_MIN_COUNT)
                    continue;
                queue_statistics[port_id].idle_hint =
                    ss_power_check_idle(queue_statistics[port_id].zero_rx);
                idle_count++;
            }
            else {
                queue_statistics[port_id].zero_rx = 0;

                // scaling up has syscall overhead
                // use heuristics to pick the right time
                queue_statistics[port_id].freq_hint = ss_power_check_scale_up(lcore_id, port_id);
            }


            if (rx_count == 0) {
                continue;
            }

            port_statistics[port_id].rx += rx_count;

            for (i = 0; i < rx_count; i++) {
                mbuf = mbufs[i];
                rte_prefetch0(rte_pktmbuf_mtod(mbuf, void *));
                ss_frame_handle(mbuf, lcore_id, port_id);
            }
        }

        if (likely(idle_count != port_count)) {
            for (port_id = 1, freq_hint = queue_statistics[0].freq_hint; port_id < port_count; ++port_id) {
                if (queue_statistics[port_id].freq_hint > freq_hint) {
                    freq_hint = queue_statistics[port_id].freq_hint;
                }
            }

            if (freq_hint == FREQ_HIGHEST && rte_power_freq_max) {
                rte_power_freq_max(lcore_id);
            }
            else if (freq_hint == FREQ_HIGHER && rte_power_freq_up) {
                rte_power_freq_up(lcore_id);
            }
        }
        else {
            // all rx queues are empty recently;
            // sleep as conservatively as possible
            for (i = 1, idle_hint = queue_statistics[0].idle_hint; port_id < port_count; ++port_id) {
                if (queue_statistics[port_id].idle_hint < idle_hint) {
                    idle_hint = queue_statistics[port_id].idle_hint;
                }
            }

            if (idle_hint < LCORE_SUSPEND_USECS)
                // avoid context switching (hundreds of usecs)
                rte_delay_us(idle_hint);
            else {
                // sleep until rx irq triggers
                if (irq_enabled) {
                    ss_power_irq_enable(lcore_id);
                    ss_power_irq_handle();
                }
                // start receiving packets immediately
                goto start_rx;
            }
            core_statistics[lcore_id].sleep_msecs += idle_hint;
        }
    }

    //return 0;
}

void ss_fatal_signal_handler(int signal) {
    int rv;

    fprintf(stderr, "received fatal signal %d...\n", signal);
    for (uint8_t port = 0; port < port_count; ++port) {
        fprintf(stderr, "closing dpdk port_id %d...\n", port);
        rte_eth_dev_close(port);
        fprintf(stderr, "closed dpdk port_id %d.\n", port);
    }
    for (uint8_t lcore_id = 0; lcore_id < RTE_MAX_LCORE; ++lcore_id) {
        if (rte_lcore_is_enabled(lcore_id) == 0) {
            continue;
        }

        fprintf(stderr, "shutdown librte_power on lcore_id: %d\n", lcore_id);
        rv = rte_power_exit(lcore_id);
        if (rv) {
            fprintf(stderr, "could not disable librte_power on lcore_id %d", lcore_id);
        }
    }
    ss_conf_destroy();
    kill(getpid(), signal);
}

void ss_signal_handler_init(const char* signal_name, int signal) {
    int rv;
    struct sigaction sa;

    memset(&sa, 0, sizeof(sa));

    sa.sa_handler = &ss_fatal_signal_handler;
    sigfillset(&sa.sa_mask);
    sa.sa_flags   = SA_RESTART | ~SA_SIGINFO;

    rv = sigaction(SIGINT, &sa, NULL);
    if (rv) {
        fprintf(stderr, "warning: could not install %s handler: rv %d: %s\n",
            signal_name, rv, strerror(errno));
    }
}

int main(int argc, char* argv[]) {
    struct rte_eth_dev_info dev_info;
    int rv;
    int c;
    uint8_t port_id, last_port;
    uint16_t lcore_count, lcore_id;
    char* conf_path = NULL;
    char pool_name[32];
    uint64_t hz;

    fprintf(stderr, "launching sdn_sensor version %s\n", SS_VERSION);

    opterr = 0;
    while ((c = getopt(argc, argv, "c:")) != -1) {
        switch (c) {
            case 'c': {
                rv = access(optarg, R_OK);
                if (rv != 0) {
                    fprintf(stderr, "could not read conf file: %s: %s\n", optarg, strerror(errno));
                    exit(1);
                }
                conf_path = je_strdup(optarg);
                break;
            }
            case '?': {
                break;
            }
            default: {
                break;
            }
        }
    }

    // NOTE: optind must be reset, since it contains hidden state,
    // otherwise rte_eal_init will fail extremely mysteriously
    optind = 1;

    rv = ss_re_init();
    if (rv) {
        fprintf(stderr, "could not initialize regular expression libraries\n");
        exit(1);
    }

    ss_pcap = pcap_open_dead(DLT_EN10MB, 65536);
    if (ss_pcap == NULL) {
        fprintf(stderr, "could not prepare BPF filtering code\n");
        exit(1);
    }

    ss_conf = ss_conf_file_parse(conf_path);
    if (ss_conf == NULL) {
        fprintf(stderr, "could not parse sdn_sensor configuration\n");
        exit(1);
    }

    /* copy over any ss_conf settings used in DPDK */
// XXX: temp override RSS code
/*
    if (ss_conf->rss_enabled) {
        port_conf.rxmode.mq_mode = ETH_MQ_RX_RSS;
        port_conf.rx_adv_conf.rss_conf.rss_hf = ETH_RSS_IP;
    }
    else {
        port_conf.rxmode.mq_mode = ETH_MQ_RX_NONE;
    }
*/
    /* init EAL */
    rv = rte_eal_init((int) ss_conf->eal_vector.we_wordc, ss_conf->eal_vector.we_wordv);
    if (rv < 0) {
        rte_exit(EXIT_FAILURE, "invalid dpdk eal launch arguments\n");
    }
    rte_set_log_level(ss_conf->log_level);

    rte_timer_subsystem_init();

    /* create the mbuf pool */
    for (int i = 0; i < SOCKET_COUNT; ++i) {
        snprintf(pool_name, sizeof(pool_name), "mbuf_pool_socket_%02d", i);
        RTE_LOG(WARNING, SS, "create mbuf_pool %s\n", pool_name);
        ss_pool[i] =
            rte_mempool_create(pool_name, MBUF_COUNT,
                       MBUF_SIZE, 32,
                       sizeof(struct rte_pktmbuf_pool_private),
                       rte_pktmbuf_pool_init, NULL,
                       rte_pktmbuf_init, NULL,
                       (int) rte_socket_id(), 0);
        if (ss_pool[i] == NULL) {
            rte_exit(EXIT_FAILURE, "could not create mbuf_pool %s\n", pool_name);
        }
    }

    rv = ss_conf_ioc_file_parse();
    if (rv) {
        rte_exit(EXIT_FAILURE, "could not initialize ioc files\n");
    }

    rv = ss_tcp_init();
    if (rv) {
        rte_exit(EXIT_FAILURE, "could not initialize tcp protocol\n");
    }

    rv = sflow_init();
    if (rv) {
        rte_exit(EXIT_FAILURE, "could not initialize sflow protocol\n");
    }

    lcore_count = (uint16_t) rte_lcore_count();
    port_count = rte_eth_dev_count();
    RTE_LOG(NOTICE, SS, "lcore_count %d port_count %d\n", lcore_count, port_count);
    if (port_count == 0) {
        rte_exit(EXIT_FAILURE, "could not detect any active ethernet nics or ports\n");
    }

    if (port_count > RTE_MAX_ETHPORTS) {
        port_count = RTE_MAX_ETHPORTS;
    }

    ss_signal_handler_init("SIGHUP",  SIGHUP);
    ss_signal_handler_init("SIGINT",  SIGINT);
    ss_signal_handler_init("SIGQUIT", SIGQUIT);
    ss_signal_handler_init("SIGILL",  SIGILL);
    ss_signal_handler_init("SIGABRT", SIGABRT);
    ss_signal_handler_init("SIGSEGV", SIGSEGV);
    ss_signal_handler_init("SIGPIPE", SIGPIPE);
    ss_signal_handler_init("SIGTERM", SIGTERM);
    ss_signal_handler_init("SIGBUS",  SIGBUS);

    last_port = 0;

    /* XXX: simple hard-coded lcore mapping */
    /* each lcore has 1 RX and 1 TX queue on each port */
    for (port_id = 0; port_id < port_count; ++port_id) {
        rte_eth_dev_info_get(port_id, &dev_info);

        /* Configure port */
        RTE_LOG(INFO, SS, "initializing port %u...\n", (unsigned) port_id);
        fflush(stderr);
        rv = rte_eth_dev_configure(port_id, lcore_count, lcore_count, &port_conf);
        if (rv < 0) {
            rte_exit(EXIT_FAILURE, "cannot configure ethernet port: %u, error: %d\n", (unsigned) port_id, rv);
        }

        for (lcore_id = 0; lcore_id < lcore_count; ++lcore_id) {
            int eth_socket_id = rte_eth_dev_socket_id(port_id);
            // XXX: work around non-NUMA socket ID bug
            if (eth_socket_id == -1) eth_socket_id = 0;
            u_int u_eth_socket_id = (u_int) eth_socket_id;

            /* init one RX queue */
            fflush(stderr);
            rv = rte_eth_rx_queue_setup(
                port_id, lcore_id /*queue_id*/, ss_conf->rxd_count,
                u_eth_socket_id, &rx_conf,
                ss_pool[u_eth_socket_id]);
            if (rv < 0) {
                rte_exit(EXIT_FAILURE, "rte_eth_rx_queue_setup: error: port: %u lcore_id: %d error: %d\n", port_id, lcore_id, rv);
            }

            /* init one TX queue */
            fflush(stderr);
            rv = rte_eth_tx_queue_setup(
                port_id, lcore_id /*queue_id*/, ss_conf->txd_count,
                u_eth_socket_id, &tx_conf);
            if (rv < 0) {
                rte_exit(EXIT_FAILURE, "rte_eth_tx_queue_setup: error: port: %u lcore_id: %d error: %d\n", port_id, lcore_id, rv);
            }
        }

        /* cache MAC address */
        rte_eth_macaddr_get(port_id, &port_eth_addrs[port_id]);

        RTE_LOG(INFO, SS, "port %u: now active with mac address: %02X:%02X:%02X:%02X:%02X:%02X\n",
            (unsigned) port_id,
            port_eth_addrs[port_id].addr_bytes[0],
            port_eth_addrs[port_id].addr_bytes[1],
            port_eth_addrs[port_id].addr_bytes[2],
            port_eth_addrs[port_id].addr_bytes[3],
            port_eth_addrs[port_id].addr_bytes[4],
            port_eth_addrs[port_id].addr_bytes[5]);

        /* initialize port stats */
        memset(&port_statistics, 0, sizeof(port_statistics));
    }

    for (lcore_id = 0; lcore_id < lcore_count; ++lcore_id) {
        /* init power management library */
        rv = rte_power_init(lcore_id);
        if (rv) {
            RTE_LOG(WARNING, SS, "librte_power is not available on lcore_id: %d\n", lcore_id);
        }

        /* init timer structures for each enabled lcore */
        rte_timer_init(&power_timers[lcore_id]);
        hz = rte_get_timer_hz();
        rte_timer_reset(&power_timers[lcore_id],
            hz / TIMER_TICKS_PER_SEC, SINGLE, lcore_id,
            ss_power_timer_callback, NULL);
    }

    for (port_id = 0; port_id < port_count; ++port_id) {
        /* Start port */
        rv = rte_eth_dev_start(port_id);
        if (rv < 0) {
            rte_exit(EXIT_FAILURE, "rte_eth_dev_start: error: port: %u error: %d\n", (unsigned) port_id, rv);
        }

        rte_eth_promiscuous_enable(port_id);

        /* initialize spinlock for each port */
        rte_spinlock_init(&port_statistics[port_id].port_lock);
    }

    //ss_port_link_status_check_all(ss_conf->port_count);

    /* launch per-lcore init on every lcore */
    rte_eal_mp_remote_launch(ss_main_loop, NULL, CALL_MASTER);
    RTE_LCORE_FOREACH_SLAVE(lcore_id) {
        if (rte_eal_wait_lcore(lcore_id) < 0)
            return -1;
    }

    return 0;
}
