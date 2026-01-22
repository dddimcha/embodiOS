/* EMBODIOS Minimal TCP/IP Stack
 *
 * Lightweight TCP/IP implementation for bare-metal networking.
 * Provides basic IP, ICMP, UDP, and TCP functionality.
 *
 * Features:
 * - Ethernet frame handling
 * - ARP (Address Resolution Protocol)
 * - IPv4 with basic routing
 * - ICMP echo (ping)
 * - UDP datagrams
 * - TCP connections (basic)
 */

#ifndef EMBODIOS_TCPIP_H
#define EMBODIOS_TCPIP_H

#include <embodios/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ============================================================================
 * Protocol Numbers and Constants
 * ============================================================================ */

/* Ethernet */
#define ETH_ALEN            6           /* Ethernet address length */
#define ETH_MTU             1500        /* Maximum transmission unit */
#define ETH_FRAME_MIN       60          /* Minimum frame size */
#define ETH_FRAME_MAX       1514        /* Maximum frame size */

/* EtherType values */
#define ETH_TYPE_IP         0x0800      /* IPv4 */
#define ETH_TYPE_ARP        0x0806      /* ARP */
#define ETH_TYPE_IP6        0x86DD      /* IPv6 */

/* IP Protocol numbers */
#define IP_PROTO_ICMP       1           /* ICMP */
#define IP_PROTO_TCP        6           /* TCP */
#define IP_PROTO_UDP        17          /* UDP */

/* ICMP types */
#define ICMP_ECHO_REPLY     0           /* Echo reply */
#define ICMP_ECHO_REQUEST   8           /* Echo request */

/* ARP */
#define ARP_REQUEST         1           /* ARP request */
#define ARP_REPLY           2           /* ARP reply */
#define ARP_CACHE_SIZE      16          /* ARP cache entries */
#define ARP_TIMEOUT_SEC     300         /* ARP cache timeout */

/* TCP flags */
#define TCP_FIN             0x01
#define TCP_SYN             0x02
#define TCP_RST             0x04
#define TCP_PSH             0x08
#define TCP_ACK             0x10
#define TCP_URG             0x20

/* TCP states */
#define TCP_CLOSED          0
#define TCP_LISTEN          1
#define TCP_SYN_SENT        2
#define TCP_SYN_RECEIVED    3
#define TCP_ESTABLISHED     4
#define TCP_FIN_WAIT_1      5
#define TCP_FIN_WAIT_2      6
#define TCP_CLOSE_WAIT      7
#define TCP_CLOSING         8
#define TCP_LAST_ACK        9
#define TCP_TIME_WAIT       10

/* ============================================================================
 * Protocol Headers
 * ============================================================================ */

/* Ethernet header */
typedef struct eth_header {
    uint8_t  dst[ETH_ALEN];     /* Destination MAC */
    uint8_t  src[ETH_ALEN];     /* Source MAC */
    uint16_t type;              /* EtherType (big-endian) */
} __packed eth_header_t;

/* ARP header */
typedef struct arp_header {
    uint16_t hw_type;           /* Hardware type (1 = Ethernet) */
    uint16_t proto_type;        /* Protocol type (0x0800 = IP) */
    uint8_t  hw_len;            /* Hardware address length (6) */
    uint8_t  proto_len;         /* Protocol address length (4) */
    uint16_t opcode;            /* Operation (1=request, 2=reply) */
    uint8_t  sender_mac[ETH_ALEN];  /* Sender MAC */
    uint32_t sender_ip;         /* Sender IP */
    uint8_t  target_mac[ETH_ALEN];  /* Target MAC */
    uint32_t target_ip;         /* Target IP */
} __packed arp_header_t;

/* IPv4 header */
typedef struct ip_header {
    uint8_t  version_ihl;       /* Version (4) and IHL (5) */
    uint8_t  tos;               /* Type of service */
    uint16_t total_len;         /* Total length */
    uint16_t id;                /* Identification */
    uint16_t flags_frag;        /* Flags and fragment offset */
    uint8_t  ttl;               /* Time to live */
    uint8_t  protocol;          /* Protocol (TCP=6, UDP=17, ICMP=1) */
    uint16_t checksum;          /* Header checksum */
    uint32_t src_ip;            /* Source IP */
    uint32_t dst_ip;            /* Destination IP */
} __packed ip_header_t;

/* ICMP header */
typedef struct icmp_header {
    uint8_t  type;              /* Message type */
    uint8_t  code;              /* Type-specific code */
    uint16_t checksum;          /* Checksum */
    uint16_t id;                /* Identifier */
    uint16_t seq;               /* Sequence number */
} __packed icmp_header_t;

/* UDP header */
typedef struct udp_header {
    uint16_t src_port;          /* Source port */
    uint16_t dst_port;          /* Destination port */
    uint16_t length;            /* Length (header + data) */
    uint16_t checksum;          /* Checksum (optional for IPv4) */
} __packed udp_header_t;

/* TCP header */
typedef struct tcp_header {
    uint16_t src_port;          /* Source port */
    uint16_t dst_port;          /* Destination port */
    uint32_t seq_num;           /* Sequence number */
    uint32_t ack_num;           /* Acknowledgment number */
    uint8_t  data_offset;       /* Data offset (header length) */
    uint8_t  flags;             /* Control flags */
    uint16_t window;            /* Window size */
    uint16_t checksum;          /* Checksum */
    uint16_t urgent;            /* Urgent pointer */
} __packed tcp_header_t;

/* ============================================================================
 * Network Configuration
 * ============================================================================ */

typedef struct net_config {
    uint32_t ip_addr;           /* Our IP address */
    uint32_t netmask;           /* Network mask */
    uint32_t gateway;           /* Default gateway */
    uint32_t dns_server;        /* DNS server */
    uint8_t  mac_addr[ETH_ALEN]; /* Our MAC address */
    bool     dhcp_enabled;      /* Use DHCP */
} net_config_t;

/* ============================================================================
 * Socket-like Interface
 * ============================================================================ */

#define MAX_SOCKETS         16
#define SOCKET_BUFFER_SIZE  4096

typedef struct socket {
    int      fd;                /* Socket descriptor */
    int      type;              /* SOCK_STREAM or SOCK_DGRAM */
    int      protocol;          /* Protocol (TCP or UDP) */
    int      state;             /* Connection state */
    uint32_t local_ip;          /* Local IP */
    uint16_t local_port;        /* Local port */
    uint32_t remote_ip;         /* Remote IP */
    uint16_t remote_port;       /* Remote port */
    uint32_t seq_num;           /* TCP sequence number */
    uint32_t ack_num;           /* TCP ack number */
    uint8_t  rx_buffer[SOCKET_BUFFER_SIZE];  /* Receive buffer */
    size_t   rx_len;            /* Data in receive buffer */
    bool     active;            /* Socket in use */
} socket_t;

/* Socket types */
#define SOCK_STREAM         1   /* TCP */
#define SOCK_DGRAM          2   /* UDP */

/* ============================================================================
 * Statistics
 * ============================================================================ */

typedef struct net_stats {
    uint64_t rx_packets;        /* Packets received */
    uint64_t tx_packets;        /* Packets transmitted */
    uint64_t rx_bytes;          /* Bytes received */
    uint64_t tx_bytes;          /* Bytes transmitted */
    uint64_t rx_errors;         /* Receive errors */
    uint64_t tx_errors;         /* Transmit errors */
    uint64_t arp_requests;      /* ARP requests sent */
    uint64_t arp_replies;       /* ARP replies received */
    uint64_t icmp_echo_req;     /* ICMP echo requests received */
    uint64_t icmp_echo_reply;   /* ICMP echo replies sent */
    uint64_t tcp_connections;   /* TCP connections established */
    uint64_t udp_datagrams;     /* UDP datagrams processed */
} net_stats_t;

/* ============================================================================
 * Error Codes
 * ============================================================================ */

#define NET_OK              0
#define NET_ERR_INIT        -1
#define NET_ERR_NOMEM       -2
#define NET_ERR_TIMEOUT     -3
#define NET_ERR_REFUSED     -4
#define NET_ERR_UNREACHABLE -5
#define NET_ERR_NOSOCKET    -6
#define NET_ERR_INVALID     -7

/* ============================================================================
 * Public API
 * ============================================================================ */

/**
 * Initialize TCP/IP stack
 * @return NET_OK on success
 */
int tcpip_init(void);

/**
 * Configure network interface
 * @param config Network configuration
 * @return NET_OK on success
 */
int tcpip_configure(const net_config_t *config);

/**
 * Set IP address (convenience function)
 * @param ip IP address as string "192.168.1.100"
 * @param netmask Netmask as string "255.255.255.0"
 * @param gateway Gateway as string "192.168.1.1"
 */
int tcpip_set_ip(const char *ip, const char *netmask, const char *gateway);

/**
 * Process incoming packets
 * Call periodically to handle network traffic
 * @return Number of packets processed
 */
int tcpip_poll(void);

/**
 * Send a raw IP packet
 * @param dst_ip Destination IP
 * @param protocol Protocol number
 * @param data Packet data
 * @param len Data length
 * @return NET_OK on success
 */
int tcpip_send_ip(uint32_t dst_ip, uint8_t protocol, const void *data, size_t len);

/**
 * Send a UDP datagram
 * @param dst_ip Destination IP
 * @param dst_port Destination port
 * @param src_port Source port
 * @param data Datagram data
 * @param len Data length
 * @return NET_OK on success
 */
int tcpip_send_udp(uint32_t dst_ip, uint16_t dst_port, uint16_t src_port,
                   const void *data, size_t len);

/**
 * Send ICMP echo request (ping)
 * @param dst_ip Destination IP
 * @param id Echo ID
 * @param seq Sequence number
 * @return NET_OK on success
 */
int tcpip_ping(uint32_t dst_ip, uint16_t id, uint16_t seq);

/* Socket API */
int socket_create(int type, int protocol);
int socket_bind(int fd, uint32_t ip, uint16_t port);
int socket_listen(int fd, int backlog);
int socket_connect(int fd, uint32_t ip, uint16_t port);
int socket_accept(int fd, uint32_t *remote_ip, uint16_t *remote_port);
int socket_send(int fd, const void *data, size_t len);
int socket_recv(int fd, void *buffer, size_t len);
int socket_close(int fd);

/* Utility functions */
uint32_t ip_from_string(const char *str);
void ip_to_string(uint32_t ip, char *str, size_t len);
uint16_t htons(uint16_t val);
uint16_t ntohs(uint16_t val);
uint32_t htonl(uint32_t val);
uint32_t ntohl(uint32_t val);

/* Statistics and debugging */
void tcpip_get_stats(net_stats_t *stats);
void tcpip_print_info(void);
int tcpip_run_tests(void);
int tcpip_start_server(uint16_t port);

/* Macros for IP address handling */
#define IP4(a,b,c,d) (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))
#define IP4_ADDR(ip,a,b,c,d) do { ip = IP4(a,b,c,d); } while(0)

#ifdef __cplusplus
}
#endif

#endif /* EMBODIOS_TCPIP_H */
