/*
 * Copyright (C) 2015 Gris Ge
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <http://www.gnu.org/licenses/>.
 *
 * Author: Gris Ge <cnfourt@gmail.com>
 */

/*
 * Some code was copy from tcp4.c by Copyright (C) 2011-2015  P.D. Buchan
 * (pdbuchan@yahoo.com)
 */

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <string.h>
#include <unistd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/ioctl.h>
#include <linux/if.h>
#include <linux/if_tun.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <poll.h>
#include <netinet/ip.h>
#include <errno.h>
#include <arpa/inet.h>
#include <unistd.h>

#include "encrypt.h"

#define _IP4_HDRLEN 20

#define _TUN_NAME "turtle"
#define _IP_PROTOCOL_ID 253    /* For test only, RFC3692 */

static char _SRC_IP[16];
static char _DST_IP[16];
#define _KEY "ABC"

#define error(format, ...) \
    fprintf(stderr, "ERROR(%s:%d): " format, \
        __FILE__, __LINE__, ##__VA_ARGS__)

#define info(format, ...) \
    fprintf(stdout, "INFO(%s:%d): " format, \
        __FILE__, __LINE__, ##__VA_ARGS__)

static int _tun_alloc(const char *dev);
static int _setup_ip_raw_socket(int protocol);
static int _setup_ip_header(struct ip *ip_header);
static uint16_t _checksum(uint16_t *addr, int len);
static void _parse_arg(int argc, char *argv[]);
static void _help(void);

static void _help(void)
{
    printf("Invalid argument\n");
    exit(1);
}

static void _parse_arg(int argc, char *argv[])
{
    char *src = NULL;
    char *dst = NULL;
    int index;
    int c;

    opterr = 0;

    while ((c = getopt (argc, argv, "s:d:")) != -1)
        switch (c)
        {
        case 's':
            src = optarg;
            break;
        case 'd':
            dst = optarg;
            break;
        default:
            _help();
        }
    if ((src == NULL) || (dst == NULL))
        _help();

    snprintf(_SRC_IP, sizeof(_SRC_IP)/sizeof(_SRC_IP[0]), "%s", src);
    snprintf(_DST_IP, sizeof(_DST_IP)/sizeof(_DST_IP[0]), "%s", dst);

    return;
}

/*
 * RFC 1071
 */
static uint16_t _checksum(uint16_t *addr, int len)
{
    int count = len;
    register uint32_t sum = 0;
    uint16_t answer = 0;

    // Sum up 2-byte values until none or only one byte left.
    while (count > 1) {
        sum += *(addr++);
        count -= 2;
    }

    // Add left-over byte, if any.
    if (count > 0) {
        sum += *(uint8_t *) addr;
    }

    // Fold 32-bit sum into 16 bits; we lose information by doing this,
    // increasing the chances of a collision.
    // sum = (lower 16 bits) + (upper 16 bits shifted right 16 bits)
    while (sum >> 16) {
        sum = (sum & 0xffff) + (sum >> 16);
    }

    // Checksum is one's compliment of sum.
    answer = ~sum;

    return (answer);
}

static int _tun_alloc(const char *dev)
{
    struct ifreq ifr;
    int fd, err;


    if (dev == NULL) {
        error("Got NULL tun name");
        return -1;
    }


    fd = open("/dev/net/tun", O_RDWR);

    if (fd < 0) {
        error("Failed to open /dev/net/tun");
        return -1;
    }

    memset(&ifr, 0, sizeof(ifr));

    ifr.ifr_flags = IFF_TUN | IFF_NO_PI;
    if (*dev)
        strncpy(ifr.ifr_name, dev, IFNAMSIZ);

    if ((err = ioctl(fd, TUNSETIFF, (void *)&ifr)) < 0) {
        error("Failed to create tun %s, error %d\n", dev, err);
        close(fd);
        return err;
    }
    return fd;
}

static int _setup_ip_raw_socket(int protocol)
{
    int one = 1;
    const int *var = &one;
    int raw_socket = socket(AF_INET, SOCK_RAW, protocol);
    if (raw_socket < 0) {
        error("Failed to set ip RAW socket with protocol id: %d\n", protocol);
        return -1;
    }
    if (setsockopt(raw_socket, IPPROTO_IP, IP_HDRINCL, var,
                   sizeof(one)) != 0) {
        error("Failed to setsockopt() IP_HDRINCL: %d %s\n", errno,
              strerror(errno));
        return -1;
    }
    return raw_socket;
}

static int _setup_ip_header(struct ip *ip_header)
{
    // IPv4 header length (4 bits): Number of 32-bit words in header = 5
    ip_header->ip_hl = sizeof(struct ip)/ sizeof(uint32_t);

    // Internet Protocol version (4 bits): IPv4
    ip_header->ip_v = 4;

    // Type of service (8 bits)
    ip_header->ip_tos = 0;

    // Total length of datagram (16 bits): IP header + next header
    ip_header->ip_len = htons(sizeof(struct ip));

    // ID sequence number (16 bits): unused, since single datagram
    ip_header->ip_id = htons(0);

    // Time-to-Live (8 bits): default to maximum value
    ip_header->ip_ttl = 255;

    ip_header->ip_off = htons(IP_DF);

    // Transport layer protocol (8 bits): 6 for TCP
    ip_header->ip_p = _IP_PROTOCOL_ID;
      // Source IPv4 address (32 bits)

    if (inet_pton(AF_INET, _SRC_IP, &(ip_header->ip_src)) != 1) {
        error("inet_pton() failed. Error: %d %s\n", errno, strerror(errno));
        return -1;
    }

    // Destination IPv4 address (32 bits)
    if (inet_pton(AF_INET, _DST_IP, &(ip_header->ip_dst)) != 1) {
        error("inet_pton() failed. Error: %d %s\n", errno, strerror(errno));
        return -1;
    }

    // IPv4 header checksum (16 bits): set to 0 when calculating checksum
    ip_header->ip_sum = 0;
    ip_header->ip_sum = _checksum((uint16_t *) &ip_header, sizeof(struct ip));
    return 0;
}

int main(int argc, char **argv)
{
    int rc = 0;
    int tun_fd = -1;
    int raw_ip_fd = -1;
    ssize_t readed_size = 0;
    ssize_t written_size = 0;
    struct pollfd fds[2];
    int ret = 0;
    int i = 0;
    unsigned char buff[IP_MAXPACKET - sizeof(struct ip)];
    struct ip ip_header;
    unsigned char wire_ip_pkg[IP_MAXPACKET];
    struct sockaddr_in dst;

    _parse_arg(argc, argv);

    tun_fd = _tun_alloc(_TUN_NAME);
    if (tun_fd < 0) {
        error("Failed to create tun: %s\n", _TUN_NAME);
        exit(EXIT_FAILURE);
    }
    info("Tun '%s' created\n", _TUN_NAME);

    raw_ip_fd = _setup_ip_raw_socket(_IP_PROTOCOL_ID);

    fds[0].fd = tun_fd;
    fds[0].events = POLLIN;
    fds[1].fd = raw_ip_fd;
    fds[1].events = POLLIN;

    _setup_ip_header(&ip_header);
    memset(&dst, 0, sizeof(struct sockaddr_in));
    dst.sin_addr.s_addr = ip_header.ip_dst.s_addr;
    dst.sin_family = AF_INET;

    while(1) {
        ret = poll(fds, 2, 500 /* timeout in ms */);
        if (ret > 0) {
            if (fds[0].revents & POLLIN) {
                /* TUN got data, enrypt to ip raw socket */
                readed_size = read(tun_fd, buff, sizeof(buff));
                if (readed_size < 0) {
                    printf("Failed to write ip raw socket, errno: %d, %s\n",
                           errno, strerror(errno));
                    continue;
                }
                if (encrypt_data(buff, readed_size, _KEY, sizeof(_KEY)) != 0) {
                    printf("encrypt failed\n");
                    continue;
                }
                memset(wire_ip_pkg, 0, sizeof(IP_MAXPACKET));
                memcpy(wire_ip_pkg, &ip_header, sizeof(struct ip));
                memcpy((wire_ip_pkg + sizeof(struct ip)), buff, readed_size);
                written_size = sendto(raw_ip_fd, wire_ip_pkg, readed_size +
                                      sizeof(struct ip), 0 /* flag */,
                                      (struct sockaddr *) &dst,
                                      sizeof(struct sockaddr));

                if (written_size < 0) {
                    printf("Failed to write ip raw socket, errno: %d, %s\n",
                           errno, strerror(errno));
                    continue;
                }
            }
            else if (fds[1].revents & POLLIN) {
                /* Raw IP socket got data, decrypt to tun socket*/
                readed_size = read(raw_ip_fd, buff, sizeof(buff));
                if (readed_size < 0) {
                    printf("Failed to write ip raw socket, errno: %d, %s\n",
                           errno, strerror(errno));
                    continue;
                }
                if (readed_size < sizeof(struct ip)) {
                    printf("discarding package as it is smaller than ip "
                           "header\n");
                    continue;
                }
                if (decrypt_data(buff + sizeof(struct ip),
                                 readed_size - sizeof(struct ip),
                                 _KEY, sizeof(_KEY)) != 0) {
                    printf("decrypt failed\n");
                    continue;
                }
                memset(wire_ip_pkg, 0, sizeof(IP_MAXPACKET));
                memcpy(wire_ip_pkg, buff + sizeof(struct ip),
                       readed_size - sizeof(struct ip));
                written_size = write(tun_fd, buff + sizeof(struct ip),
                                     readed_size - sizeof(struct ip));

                if (written_size < 0) {
                    printf("Failed to write ip raw socket, errno: %d, %s\n",
                           errno, strerror(errno));
                    continue;
                }
            }
        }
    }

out:
    if (tun_fd >= 0)
        close(tun_fd);
    if (raw_ip_fd >= 0)
        close(raw_ip_fd);

    exit(EXIT_SUCCESS);
}
