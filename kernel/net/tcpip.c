/* EMBODIOS Minimal TCP/IP Stack Implementation
 *
 * Provides basic networking functionality for bare-metal operation.
 */

#include <embodios/tcpip.h>
#include <embodios/virtio_net.h>
#include <embodios/e1000e.h>
#include <embodios/console.h>
#include <embodios/mm.h>
#include <embodios/kernel.h>

/* Stack state */
static bool tcpip_initialized = false;
static net_config_t net_cfg;
static net_stats_t net_stats;

/* ARP cache */
typedef struct arp_entry {
    uint32_t ip;
    uint8_t  mac[ETH_ALEN];
    uint32_t timestamp;
    bool     valid;
} arp_entry_t;

static arp_entry_t arp_cache[ARP_CACHE_SIZE];

/* Sockets */
static socket_t sockets[MAX_SOCKETS];
static uint16_t next_ephemeral_port = 49152;  /* Ephemeral port range: 49152-65535 */

/* Packet buffer */
static uint8_t rx_buffer[ETH_FRAME_MAX];
static uint8_t tx_buffer[ETH_FRAME_MAX];

/* Network driver interface */
typedef int (*net_send_fn)(const void *data, size_t len);
typedef int (*net_recv_fn)(void *buffer, size_t max_len);
typedef void (*net_mac_fn)(uint8_t *mac);

static net_send_fn net_send = NULL;
static net_recv_fn net_recv = NULL;
static net_mac_fn net_get_mac = NULL;

/* ============================================================================
 * Byte Order Conversion
 * ============================================================================ */

uint16_t htons(uint16_t val)
{
    return ((val & 0xFF) << 8) | ((val >> 8) & 0xFF);
}

uint16_t ntohs(uint16_t val)
{
    return htons(val);
}

uint32_t htonl(uint32_t val)
{
    return ((val & 0xFF) << 24) |
           ((val & 0xFF00) << 8) |
           ((val >> 8) & 0xFF00) |
           ((val >> 24) & 0xFF);
}

uint32_t ntohl(uint32_t val)
{
    return htonl(val);
}

/* ============================================================================
 * IP Address Utilities
 * ============================================================================ */

uint32_t ip_from_string(const char *str)
{
    uint32_t ip = 0;
    uint32_t octet = 0;
    int shift = 24;

    while (*str && shift >= 0) {
        if (*str >= '0' && *str <= '9') {
            octet = octet * 10 + (*str - '0');
        } else if (*str == '.') {
            ip |= (octet << shift);
            shift -= 8;
            octet = 0;
        }
        str++;
    }
    ip |= octet;  /* Last octet */

    return ip;
}

void ip_to_string(uint32_t ip, char *str, size_t len)
{
    if (len < 16) return;

    int pos = 0;
    for (int i = 3; i >= 0; i--) {
        uint8_t octet = (ip >> (i * 8)) & 0xFF;
        if (octet >= 100) str[pos++] = '0' + octet / 100;
        if (octet >= 10) str[pos++] = '0' + (octet / 10) % 10;
        str[pos++] = '0' + octet % 10;
        if (i > 0) str[pos++] = '.';
    }
    str[pos] = '\0';
}

/* ============================================================================
 * Checksum Calculation
 * ============================================================================ */

static uint16_t checksum(const void *data, size_t len)
{
    const uint16_t *ptr = (const uint16_t *)data;
    uint32_t sum = 0;

    while (len > 1) {
        sum += *ptr++;
        len -= 2;
    }

    if (len == 1) {
        sum += *(const uint8_t *)ptr;
    }

    while (sum >> 16) {
        sum = (sum & 0xFFFF) + (sum >> 16);
    }

    return (uint16_t)~sum;
}

/* ============================================================================
 * ARP Cache Management
 * ============================================================================ */

static arp_entry_t *arp_lookup(uint32_t ip)
{
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (arp_cache[i].valid && arp_cache[i].ip == ip) {
            return &arp_cache[i];
        }
    }
    return NULL;
}

static void arp_add(uint32_t ip, const uint8_t *mac)
{
    /* Find existing or free entry */
    int idx = -1;
    for (int i = 0; i < ARP_CACHE_SIZE; i++) {
        if (!arp_cache[i].valid || arp_cache[i].ip == ip) {
            idx = i;
            break;
        }
    }

    if (idx < 0) {
        idx = 0;  /* Overwrite first entry if cache full */
    }

    arp_cache[idx].ip = ip;
    memcpy(arp_cache[idx].mac, mac, ETH_ALEN);
    arp_cache[idx].valid = true;
    arp_cache[idx].timestamp = 0;  /* TODO: Add timer */
}

static int arp_request(uint32_t target_ip)
{
    eth_header_t *eth = (eth_header_t *)tx_buffer;
    arp_header_t *arp = (arp_header_t *)(tx_buffer + sizeof(eth_header_t));

    /* Ethernet header - broadcast */
    memset(eth->dst, 0xFF, ETH_ALEN);
    memcpy(eth->src, net_cfg.mac_addr, ETH_ALEN);
    eth->type = htons(ETH_TYPE_ARP);

    /* ARP header */
    arp->hw_type = htons(1);        /* Ethernet */
    arp->proto_type = htons(ETH_TYPE_IP);
    arp->hw_len = ETH_ALEN;
    arp->proto_len = 4;
    arp->opcode = htons(ARP_REQUEST);
    memcpy(arp->sender_mac, net_cfg.mac_addr, ETH_ALEN);
    arp->sender_ip = htonl(net_cfg.ip_addr);
    memset(arp->target_mac, 0, ETH_ALEN);
    arp->target_ip = htonl(target_ip);

    net_stats.arp_requests++;

    return net_send(tx_buffer, sizeof(eth_header_t) + sizeof(arp_header_t));
}

/* ============================================================================
 * Packet Handlers
 * ============================================================================ */

static void handle_arp(const uint8_t *pkt, size_t len)
{
    if (len < sizeof(eth_header_t) + sizeof(arp_header_t)) return;

    const eth_header_t *eth = (const eth_header_t *)pkt;
    const arp_header_t *arp = (const arp_header_t *)(pkt + sizeof(eth_header_t));

    uint16_t opcode = ntohs(arp->opcode);
    uint32_t sender_ip = ntohl(arp->sender_ip);
    uint32_t target_ip = ntohl(arp->target_ip);

    /* Add sender to ARP cache */
    arp_add(sender_ip, arp->sender_mac);

    if (opcode == ARP_REQUEST && target_ip == net_cfg.ip_addr) {
        /* Send ARP reply */
        eth_header_t *reply_eth = (eth_header_t *)tx_buffer;
        arp_header_t *reply_arp = (arp_header_t *)(tx_buffer + sizeof(eth_header_t));

        memcpy(reply_eth->dst, eth->src, ETH_ALEN);
        memcpy(reply_eth->src, net_cfg.mac_addr, ETH_ALEN);
        reply_eth->type = htons(ETH_TYPE_ARP);

        reply_arp->hw_type = htons(1);
        reply_arp->proto_type = htons(ETH_TYPE_IP);
        reply_arp->hw_len = ETH_ALEN;
        reply_arp->proto_len = 4;
        reply_arp->opcode = htons(ARP_REPLY);
        memcpy(reply_arp->sender_mac, net_cfg.mac_addr, ETH_ALEN);
        reply_arp->sender_ip = htonl(net_cfg.ip_addr);
        memcpy(reply_arp->target_mac, arp->sender_mac, ETH_ALEN);
        reply_arp->target_ip = arp->sender_ip;

        net_send(tx_buffer, sizeof(eth_header_t) + sizeof(arp_header_t));
        net_stats.arp_replies++;
    }
}

static void handle_icmp(const ip_header_t *ip, const uint8_t *data, size_t len)
{
    if (len < sizeof(icmp_header_t)) return;

    const icmp_header_t *icmp = (const icmp_header_t *)data;

    if (icmp->type == ICMP_ECHO_REQUEST) {
        net_stats.icmp_echo_req++;

        /* Send echo reply */
        uint32_t src_ip = ntohl(ip->src_ip);

        /* Build reply */
        eth_header_t *eth = (eth_header_t *)tx_buffer;
        ip_header_t *reply_ip = (ip_header_t *)(tx_buffer + sizeof(eth_header_t));
        icmp_header_t *reply_icmp = (icmp_header_t *)(tx_buffer + sizeof(eth_header_t) + sizeof(ip_header_t));

        /* Need to resolve MAC */
        arp_entry_t *entry = arp_lookup(src_ip);
        if (!entry) {
            arp_request(src_ip);
            return;
        }

        /* Ethernet header */
        memcpy(eth->dst, entry->mac, ETH_ALEN);
        memcpy(eth->src, net_cfg.mac_addr, ETH_ALEN);
        eth->type = htons(ETH_TYPE_IP);

        /* IP header */
        reply_ip->version_ihl = 0x45;
        reply_ip->tos = 0;
        reply_ip->total_len = htons(sizeof(ip_header_t) + len);
        reply_ip->id = ip->id;
        reply_ip->flags_frag = 0;
        reply_ip->ttl = 64;
        reply_ip->protocol = IP_PROTO_ICMP;
        reply_ip->checksum = 0;
        reply_ip->src_ip = htonl(net_cfg.ip_addr);
        reply_ip->dst_ip = ip->src_ip;
        reply_ip->checksum = checksum(reply_ip, sizeof(ip_header_t));

        /* ICMP reply */
        memcpy(reply_icmp, icmp, len);
        reply_icmp->type = ICMP_ECHO_REPLY;
        reply_icmp->checksum = 0;
        reply_icmp->checksum = checksum(reply_icmp, len);

        net_send(tx_buffer, sizeof(eth_header_t) + sizeof(ip_header_t) + len);
        net_stats.icmp_echo_reply++;
    }
}

static void handle_udp(const ip_header_t *ip, const uint8_t *data, size_t len)
{
    if (len < sizeof(udp_header_t)) return;

    const udp_header_t *udp = (const udp_header_t *)data;
    uint16_t dst_port = ntohs(udp->dst_port);
    uint16_t data_len = ntohs(udp->length) - sizeof(udp_header_t);

    net_stats.udp_datagrams++;

    /* Find listening socket */
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].active && sockets[i].type == SOCK_DGRAM &&
            sockets[i].local_port == dst_port) {

            /* Copy data to socket buffer */
            if (sockets[i].rx_len + data_len <= SOCKET_BUFFER_SIZE) {
                memcpy(sockets[i].rx_buffer + sockets[i].rx_len,
                       data + sizeof(udp_header_t), data_len);
                sockets[i].rx_len += data_len;
                sockets[i].remote_ip = ntohl(ip->src_ip);
                sockets[i].remote_port = ntohs(udp->src_port);
            }
            break;
        }
    }
}

static void handle_tcp(const ip_header_t *ip, const uint8_t *data, size_t len)
{
    if (len < sizeof(tcp_header_t)) return;

    const tcp_header_t *tcp = (const tcp_header_t *)data;
    uint16_t dst_port = ntohs(tcp->dst_port);

    net_stats.tcp_connections++;

    /* Find socket */
    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (sockets[i].active && sockets[i].type == SOCK_STREAM &&
            sockets[i].local_port == dst_port) {

            uint8_t flags = tcp->flags;
            uint32_t seq = ntohl(tcp->seq_num);
            uint32_t ack = ntohl(tcp->ack_num);
            (void)ack;  /* Used for ACK validation in full implementation */

            /* Handle TCP state machine */
            switch (sockets[i].state) {
            case TCP_LISTEN:
                if (flags & TCP_SYN) {
                    sockets[i].remote_ip = ntohl(ip->src_ip);
                    sockets[i].remote_port = ntohs(tcp->src_port);
                    sockets[i].ack_num = seq + 1;
                    sockets[i].state = TCP_SYN_RECEIVED;
                    /* TODO: Send SYN+ACK */
                }
                break;

            case TCP_SYN_SENT:
                if ((flags & (TCP_SYN | TCP_ACK)) == (TCP_SYN | TCP_ACK)) {
                    sockets[i].ack_num = seq + 1;
                    sockets[i].state = TCP_ESTABLISHED;
                    /* TODO: Send ACK */
                }
                break;

            case TCP_ESTABLISHED:
                if (flags & TCP_FIN) {
                    sockets[i].state = TCP_CLOSE_WAIT;
                    /* TODO: Send ACK */
                } else if (flags & TCP_ACK) {
                    /* Handle data */
                    size_t header_len = (tcp->data_offset >> 4) * 4;
                    size_t data_len = len - header_len;
                    if (data_len > 0 && sockets[i].rx_len + data_len <= SOCKET_BUFFER_SIZE) {
                        memcpy(sockets[i].rx_buffer + sockets[i].rx_len,
                               data + header_len, data_len);
                        sockets[i].rx_len += data_len;
                        sockets[i].ack_num = seq + data_len;
                    }
                }
                break;

            default:
                break;
            }
            break;
        }
    }
}

static void handle_ip(const uint8_t *pkt, size_t len)
{
    if (len < sizeof(eth_header_t) + sizeof(ip_header_t)) return;

    const ip_header_t *ip = (const ip_header_t *)(pkt + sizeof(eth_header_t));

    /* Verify IP version */
    if ((ip->version_ihl >> 4) != 4) return;

    /* Check destination */
    uint32_t dst_ip = ntohl(ip->dst_ip);
    if (dst_ip != net_cfg.ip_addr && dst_ip != 0xFFFFFFFF) return;

    /* Get IP header length and payload */
    size_t ip_hdr_len = (ip->version_ihl & 0x0F) * 4;
    const uint8_t *payload = pkt + sizeof(eth_header_t) + ip_hdr_len;
    size_t payload_len = ntohs(ip->total_len) - ip_hdr_len;

    switch (ip->protocol) {
    case IP_PROTO_ICMP:
        handle_icmp(ip, payload, payload_len);
        break;
    case IP_PROTO_UDP:
        handle_udp(ip, payload, payload_len);
        break;
    case IP_PROTO_TCP:
        handle_tcp(ip, payload, payload_len);
        break;
    }
}

/* ============================================================================
 * Public API
 * ============================================================================ */

int tcpip_init(void)
{
    if (tcpip_initialized) return NET_OK;

    memset(&net_cfg, 0, sizeof(net_cfg));
    memset(&net_stats, 0, sizeof(net_stats));
    memset(arp_cache, 0, sizeof(arp_cache));
    memset(sockets, 0, sizeof(sockets));

    /* Try to find a network driver */
    if (virtio_net_is_ready()) {
        net_send = virtio_net_send;
        net_recv = virtio_net_receive;
        net_get_mac = virtio_net_get_mac;
        console_printf("tcpip: Using VirtIO network driver\n");
    } else if (e1000e_is_ready()) {
        net_send = e1000e_send;
        net_recv = e1000e_receive;
        net_get_mac = e1000e_get_mac;
        console_printf("tcpip: Using e1000e network driver\n");
    } else {
        console_printf("tcpip: No network driver available\n");
        return NET_ERR_INIT;
    }

    /* Get MAC address */
    net_get_mac(net_cfg.mac_addr);

    /* Default IP configuration (can be changed with tcpip_configure) */
    net_cfg.ip_addr = IP4(10, 0, 2, 15);      /* QEMU user networking */
    net_cfg.netmask = IP4(255, 255, 255, 0);
    net_cfg.gateway = IP4(10, 0, 2, 2);

    tcpip_initialized = true;

    console_printf("tcpip: Stack initialized\n");
    console_printf("tcpip: MAC %02x:%02x:%02x:%02x:%02x:%02x\n",
                   net_cfg.mac_addr[0], net_cfg.mac_addr[1],
                   net_cfg.mac_addr[2], net_cfg.mac_addr[3],
                   net_cfg.mac_addr[4], net_cfg.mac_addr[5]);

    char ip_str[16];
    ip_to_string(net_cfg.ip_addr, ip_str, sizeof(ip_str));
    console_printf("tcpip: IP %s\n", ip_str);

    return NET_OK;
}

int tcpip_configure(const net_config_t *config)
{
    if (!config) return NET_ERR_INVALID;

    net_cfg.ip_addr = config->ip_addr;
    net_cfg.netmask = config->netmask;
    net_cfg.gateway = config->gateway;
    net_cfg.dns_server = config->dns_server;

    return NET_OK;
}

int tcpip_set_ip(const char *ip, const char *netmask, const char *gateway)
{
    if (ip) net_cfg.ip_addr = ip_from_string(ip);
    if (netmask) net_cfg.netmask = ip_from_string(netmask);
    if (gateway) net_cfg.gateway = ip_from_string(gateway);

    return NET_OK;
}

int tcpip_poll(void)
{
    if (!tcpip_initialized || !net_recv) return 0;

    int packets = 0;

    while (1) {
        int len = net_recv(rx_buffer, sizeof(rx_buffer));
        if (len <= 0) break;

        net_stats.rx_packets++;
        net_stats.rx_bytes += len;

        if (len < (int)sizeof(eth_header_t)) continue;

        const eth_header_t *eth = (const eth_header_t *)rx_buffer;
        uint16_t eth_type = ntohs(eth->type);

        switch (eth_type) {
        case ETH_TYPE_ARP:
            handle_arp(rx_buffer, len);
            break;
        case ETH_TYPE_IP:
            handle_ip(rx_buffer, len);
            break;
        }

        packets++;
    }

    return packets;
}

int tcpip_send_udp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                   const void *data, size_t len)
{
    if (!tcpip_initialized) return NET_ERR_INIT;

    /* Resolve MAC address */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & net_cfg.netmask) != (net_cfg.ip_addr & net_cfg.netmask)) {
        next_hop = net_cfg.gateway;
    }

    arp_entry_t *entry = arp_lookup(next_hop);
    if (!entry) {
        arp_request(next_hop);
        return NET_ERR_UNREACHABLE;
    }

    /* Build packet */
    eth_header_t *eth = (eth_header_t *)tx_buffer;
    ip_header_t *ip = (ip_header_t *)(tx_buffer + sizeof(eth_header_t));
    udp_header_t *udp = (udp_header_t *)(tx_buffer + sizeof(eth_header_t) + sizeof(ip_header_t));
    uint8_t *payload = tx_buffer + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t);

    /* Ethernet */
    memcpy(eth->dst, entry->mac, ETH_ALEN);
    memcpy(eth->src, net_cfg.mac_addr, ETH_ALEN);
    eth->type = htons(ETH_TYPE_IP);

    /* IP */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(sizeof(ip_header_t) + sizeof(udp_header_t) + len);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_UDP;
    ip->checksum = 0;
    ip->src_ip = htonl(net_cfg.ip_addr);
    ip->dst_ip = htonl(dst_ip);
    ip->checksum = checksum(ip, sizeof(ip_header_t));

    /* UDP */
    udp->src_port = htons(src_port);
    udp->dst_port = htons(dst_port);
    udp->length = htons(sizeof(udp_header_t) + len);
    udp->checksum = 0;  /* Optional for IPv4 */

    /* Payload */
    memcpy(payload, data, len);

    size_t total_len = sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(udp_header_t) + len;

    net_stats.tx_packets++;
    net_stats.tx_bytes += total_len;

    return net_send(tx_buffer, total_len);
}

int tcpip_ping(uint32_t dst_ip, uint16_t id, uint16_t seq)
{
    if (!tcpip_initialized) return NET_ERR_INIT;

    /* Resolve MAC */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & net_cfg.netmask) != (net_cfg.ip_addr & net_cfg.netmask)) {
        next_hop = net_cfg.gateway;
    }

    arp_entry_t *entry = arp_lookup(next_hop);
    if (!entry) {
        arp_request(next_hop);
        return NET_ERR_UNREACHABLE;
    }

    /* Build ICMP echo request */
    eth_header_t *eth = (eth_header_t *)tx_buffer;
    ip_header_t *ip = (ip_header_t *)(tx_buffer + sizeof(eth_header_t));
    icmp_header_t *icmp = (icmp_header_t *)(tx_buffer + sizeof(eth_header_t) + sizeof(ip_header_t));

    memcpy(eth->dst, entry->mac, ETH_ALEN);
    memcpy(eth->src, net_cfg.mac_addr, ETH_ALEN);
    eth->type = htons(ETH_TYPE_IP);

    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(sizeof(ip_header_t) + sizeof(icmp_header_t));
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_ICMP;
    ip->checksum = 0;
    ip->src_ip = htonl(net_cfg.ip_addr);
    ip->dst_ip = htonl(dst_ip);
    ip->checksum = checksum(ip, sizeof(ip_header_t));

    icmp->type = ICMP_ECHO_REQUEST;
    icmp->code = 0;
    icmp->checksum = 0;
    icmp->id = htons(id);
    icmp->seq = htons(seq);
    icmp->checksum = checksum(icmp, sizeof(icmp_header_t));

    return net_send(tx_buffer, sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(icmp_header_t));
}

static int tcp_send_packet(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                           uint32_t seq, uint32_t ack, uint8_t flags,
                           const void *data, size_t len)
{
    if (!tcpip_initialized) return NET_ERR_INIT;

    /* Resolve MAC address */
    uint32_t next_hop = dst_ip;
    if ((dst_ip & net_cfg.netmask) != (net_cfg.ip_addr & net_cfg.netmask)) {
        next_hop = net_cfg.gateway;
    }

    arp_entry_t *entry = arp_lookup(next_hop);
    if (!entry) {
        arp_request(next_hop);
        return NET_ERR_UNREACHABLE;
    }

    /* Build packet */
    eth_header_t *eth = (eth_header_t *)tx_buffer;
    ip_header_t *ip = (ip_header_t *)(tx_buffer + sizeof(eth_header_t));
    tcp_header_t *tcp = (tcp_header_t *)(tx_buffer + sizeof(eth_header_t) + sizeof(ip_header_t));
    uint8_t *payload = tx_buffer + sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(tcp_header_t);

    /* Ethernet */
    memcpy(eth->dst, entry->mac, ETH_ALEN);
    memcpy(eth->src, net_cfg.mac_addr, ETH_ALEN);
    eth->type = htons(ETH_TYPE_IP);

    /* IP */
    ip->version_ihl = 0x45;
    ip->tos = 0;
    ip->total_len = htons(sizeof(ip_header_t) + sizeof(tcp_header_t) + len);
    ip->id = 0;
    ip->flags_frag = 0;
    ip->ttl = 64;
    ip->protocol = IP_PROTO_TCP;
    ip->checksum = 0;
    ip->src_ip = htonl(net_cfg.ip_addr);
    ip->dst_ip = htonl(dst_ip);
    ip->checksum = checksum(ip, sizeof(ip_header_t));

    /* TCP */
    tcp->src_port = htons(src_port);
    tcp->dst_port = htons(dst_port);
    tcp->seq_num = htonl(seq);
    tcp->ack_num = htonl(ack);
    tcp->data_offset = (sizeof(tcp_header_t) / 4) << 4;  /* Header length in 32-bit words */
    tcp->flags = flags;
    tcp->window = htons(8192);  /* Default window size */
    tcp->checksum = 0;
    tcp->urgent = 0;

    /* Payload */
    if (data && len > 0) {
        memcpy(payload, data, len);
    }

    /* TCP checksum - simplified (set to 0 for now) */
    /* TODO: Implement proper TCP checksum with pseudo-header */

    size_t total_len = sizeof(eth_header_t) + sizeof(ip_header_t) + sizeof(tcp_header_t) + len;

    net_stats.tx_packets++;
    net_stats.tx_bytes += total_len;

    return net_send(tx_buffer, total_len);
}

/* Socket API */
int socket_create(int type, int protocol)
{
    (void)protocol;

    for (int i = 0; i < MAX_SOCKETS; i++) {
        if (!sockets[i].active) {
            memset(&sockets[i], 0, sizeof(socket_t));
            sockets[i].fd = i;
            sockets[i].type = type;
            sockets[i].protocol = (type == SOCK_STREAM) ? IP_PROTO_TCP : IP_PROTO_UDP;
            sockets[i].state = TCP_CLOSED;
            sockets[i].active = true;
            return i;
        }
    }
    return NET_ERR_NOSOCKET;
}

int socket_bind(int fd, uint32_t ip, uint16_t port)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].active)
        return NET_ERR_INVALID;

    sockets[fd].local_ip = ip;
    sockets[fd].local_port = port;
    return NET_OK;
}

int socket_listen(int fd, int backlog)
{
    (void)backlog;

    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].active)
        return NET_ERR_INVALID;

    sockets[fd].state = TCP_LISTEN;
    return NET_OK;
}

int socket_connect(int fd, uint32_t ip, uint16_t port)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].active)
        return NET_ERR_INVALID;

    /* Assign ephemeral port if not bound */
    if (sockets[fd].local_port == 0) {
        sockets[fd].local_port = next_ephemeral_port++;
        if (next_ephemeral_port == 0 || next_ephemeral_port < 49152) {
            next_ephemeral_port = 49152;  /* Wrap around ephemeral range */
        }
    }

    sockets[fd].remote_ip = ip;
    sockets[fd].remote_port = port;

    if (sockets[fd].type == SOCK_DGRAM) {
        /* UDP doesn't need connection setup */
        return NET_OK;
    }

    /* TCP: Send SYN */
    sockets[fd].seq_num = 12345;  /* TODO: Random ISN */
    sockets[fd].state = TCP_SYN_SENT;
    /* TODO: Actually send SYN packet */

    return NET_OK;
}

int socket_accept(int fd, uint32_t *remote_ip, uint16_t *remote_port)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].active)
        return NET_ERR_INVALID;

    if (sockets[fd].state != TCP_SYN_RECEIVED)
        return NET_ERR_INVALID;

    /* Return connection info */
    if (remote_ip) *remote_ip = sockets[fd].remote_ip;
    if (remote_port) *remote_port = sockets[fd].remote_port;

    sockets[fd].state = TCP_ESTABLISHED;

    return fd;  /* Return same fd for simplicity */
}

int socket_send(int fd, const void *data, size_t len)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].active)
        return NET_ERR_INVALID;

    if (sockets[fd].type == SOCK_DGRAM) {
        return tcpip_send_udp(sockets[fd].remote_ip, sockets[fd].remote_port,
                              sockets[fd].local_port, data, len);
    }

    /* TCP send - simplified */
    return NET_ERR_INVALID;  /* TODO: Implement TCP send */
}

int socket_recv(int fd, void *buffer, size_t len)
{
    if (fd < 0 || fd >= MAX_SOCKETS || !sockets[fd].active)
        return NET_ERR_INVALID;

    if (sockets[fd].rx_len == 0) return 0;

    size_t to_copy = (len < sockets[fd].rx_len) ? len : sockets[fd].rx_len;
    memcpy(buffer, sockets[fd].rx_buffer, to_copy);

    /* Shift remaining data */
    sockets[fd].rx_len -= to_copy;
    if (sockets[fd].rx_len > 0) {
        memmove(sockets[fd].rx_buffer, sockets[fd].rx_buffer + to_copy, sockets[fd].rx_len);
    }

    return to_copy;
}

int socket_close(int fd)
{
    if (fd < 0 || fd >= MAX_SOCKETS)
        return NET_ERR_INVALID;

    memset(&sockets[fd], 0, sizeof(socket_t));
    return NET_OK;
}

void tcpip_get_stats(net_stats_t *stats)
{
    if (stats) {
        *stats = net_stats;
    }
}

void tcpip_print_info(void)
{
    console_printf("\n=== TCP/IP Stack Information ===\n");
    console_printf("Initialized: %s\n", tcpip_initialized ? "Yes" : "No");

    if (tcpip_initialized) {
        char ip_str[16];

        console_printf("\nConfiguration:\n");
        console_printf("  MAC: %02x:%02x:%02x:%02x:%02x:%02x\n",
                       net_cfg.mac_addr[0], net_cfg.mac_addr[1],
                       net_cfg.mac_addr[2], net_cfg.mac_addr[3],
                       net_cfg.mac_addr[4], net_cfg.mac_addr[5]);

        ip_to_string(net_cfg.ip_addr, ip_str, sizeof(ip_str));
        console_printf("  IP: %s\n", ip_str);

        ip_to_string(net_cfg.netmask, ip_str, sizeof(ip_str));
        console_printf("  Netmask: %s\n", ip_str);

        ip_to_string(net_cfg.gateway, ip_str, sizeof(ip_str));
        console_printf("  Gateway: %s\n", ip_str);

        console_printf("\nStatistics:\n");
        console_printf("  RX Packets: %lu\n", net_stats.rx_packets);
        console_printf("  TX Packets: %lu\n", net_stats.tx_packets);
        console_printf("  RX Bytes: %lu\n", net_stats.rx_bytes);
        console_printf("  TX Bytes: %lu\n", net_stats.tx_bytes);
        console_printf("  ARP Requests: %lu\n", net_stats.arp_requests);
        console_printf("  ICMP Echo Requests: %lu\n", net_stats.icmp_echo_req);
        console_printf("  ICMP Echo Replies: %lu\n", net_stats.icmp_echo_reply);
        console_printf("  UDP Datagrams: %lu\n", net_stats.udp_datagrams);
    }
}

int tcpip_run_tests(void)
{
    console_printf("\n=== TCP/IP Stack Tests ===\n");

    console_printf("TEST: Initialization... ");
    if (!tcpip_initialized) {
        int ret = tcpip_init();
        if (ret != NET_OK) {
            console_printf("FAILED (no network driver)\n");
            return -1;
        }
    }
    console_printf("PASSED\n");

    console_printf("TEST: IP address conversion... ");
    uint32_t ip = ip_from_string("192.168.1.100");
    if (ip != IP4(192, 168, 1, 100)) {
        console_printf("FAILED\n");
        return -1;
    }
    char ip_str[16];
    ip_to_string(ip, ip_str, sizeof(ip_str));
    if (strcmp(ip_str, "192.168.1.100") != 0) {
        console_printf("FAILED (to_string)\n");
        return -1;
    }
    console_printf("PASSED\n");

    console_printf("TEST: Byte order... ");
    if (htons(0x1234) != 0x3412 || ntohs(0x3412) != 0x1234) {
        console_printf("FAILED\n");
        return -1;
    }
    console_printf("PASSED\n");

    console_printf("TEST: Socket creation... ");
    int sock = socket_create(SOCK_DGRAM, 0);
    if (sock < 0) {
        console_printf("FAILED\n");
        return -1;
    }
    socket_close(sock);
    console_printf("PASSED\n");

    console_printf("=== All TCP/IP tests passed ===\n");
    return 0;
}
