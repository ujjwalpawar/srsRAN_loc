#include <arpa/inet.h>
#include <endian.h>
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

struct start_message {
    uint32_t magic;
    uint64_t start_time_ns;
};

static void send_start_message(const char* ip, int port, const struct start_message* msg)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return;
    }

    struct sockaddr_in dest;
    memset(&dest, 0, sizeof(dest));
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);

    if (inet_pton(AF_INET, ip, &dest.sin_addr) <= 0) {
        perror("inet_pton");
        close(sockfd);
        return;
    }

    ssize_t sent = sendto(sockfd, msg, sizeof(*msg), 0, (struct sockaddr*)&dest, sizeof(dest));
    if (sent < 0) {
        perror("sendto");
    }
    close(sockfd);
}

static int try_receive_slot_info(int* info, int sockfd)
{
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);
    ssize_t bytes = recvfrom(sockfd, info, sizeof(int), 0,
                             (struct sockaddr*)&sender_addr, &sender_len);

    if (bytes > 0) {
        printf("Received slot info: %d\n", *info);
        return (int)bytes;
    }

    if (bytes < 0 && errno != EAGAIN && errno != EWOULDBLOCK) {
        perror("recvfrom");
    }
    return (int)bytes;
}


static int create_nonblocking_udp_receiver(int port)
{
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        perror("socket");
        return -1;
    }

    int flags = fcntl(sockfd, F_GETFL, 0);
    if (flags < 0) {
        perror("fcntl(F_GETFL)");
        close(sockfd);
        return -1;
    }

    if (fcntl(sockfd, F_SETFL, flags | O_NONBLOCK) < 0) {
        perror("fcntl(F_SETFL)");
        close(sockfd);
        return -1;
    }

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sockfd, (struct sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        close(sockfd);
        return -1;
    }

    return sockfd;
}



int main(int argc, char *argv[]){
    if (argc < 4) {
        fprintf(stderr, "Usage: %s <NUM_IPS> <IP1> [IP2 ... IPn] <START_DELAY_MS>\n", argv[0]);
        return 1;
    }
    int num_ips = atoi(argv[1]);
    if (num_ips <= 0) {
        fprintf(stderr, "NUM_IPS must be positive\n");
        return 1;
    }
    if (argc != 3 + num_ips) {
        fprintf(stderr, "Error: Expected %d IPs followed by START_DELAY_MS, but got %d arguments.\n", num_ips, argc - 2);
        return 1;
    }
    long start_delay_ms = atol(argv[2 + num_ips]);
    if (start_delay_ms < 0) {
        fprintf(stderr, "START_DELAY_MS must be non-negative\n");
        return 1;
    }

    int number_of_gnb = num_ips;
    int udp_recv_fd = create_nonblocking_udp_receiver(9001);
    if (udp_recv_fd < 0) {
        return 1;
    }

    int info = 0;
    int count = 0;
    while (1) {
        if (count == number_of_gnb) {
            struct timespec ts;
            clock_gettime(CLOCK_REALTIME, &ts);
            uint64_t now_ns = (uint64_t)ts.tv_sec * 1000000000ULL + (uint64_t)ts.tv_nsec;
            uint64_t start_time_ns = now_ns + (uint64_t)start_delay_ms * 1000000ULL;

            struct start_message msg;
            msg.magic = htonl(0xABCD1234);
            msg.start_time_ns = htobe64(start_time_ns);

            for (int i = 0; i < num_ips; ++i) {
                const char* ip = argv[2 + i];
                send_start_message(ip, 9000, &msg);
            }
            printf("Sent start time %lu ns to %d gNB(s)\n", (unsigned long)start_time_ns, num_ips);
            close(udp_recv_fd);
            return 0;
        }

        if (try_receive_slot_info(&info, udp_recv_fd) > 0) {
            printf("[GNB] Ready received (%d/%d)\n", count + 1, number_of_gnb);
            count++;
        }
        usleep(1000);
    }
    return 0;
}
