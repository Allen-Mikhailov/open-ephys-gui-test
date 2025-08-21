#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/signalfd.h>
#include <sys/socket.h>
#include <unistd.h>

#include <array>
#include <iostream>
#include <string_view>

static int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return -1;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

int main(int argc, char** argv) {
    const uint16_t port = (argc > 1) ? static_cast<uint16_t>(std::stoi(argv[1])) : 8080;

    // Create UDP socket (IPv4)
    int sock = ::socket(AF_INET, SOCK_DGRAM, 0);
    if (sock == -1) {
        perror("socket");
        return 1;
    }

    // Allow quick rebinding (useful during dev)
    int yes = 1;
    if (setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1) {
        perror("setsockopt(SO_REUSEADDR)");
        return 1;
    }

    if (set_nonblocking(sock) == -1) {
        perror("fcntl(O_NONBLOCK)");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(port);
    if (bind(sock, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1) {
        perror("bind");
        return 1;
    }

    // Create signalfd so we can shut down cleanly via epoll (Ctrl+C)
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGINT);
    sigaddset(&mask, SIGTERM);
    // Block signals from default handling
    if (pthread_sigmask(SIG_BLOCK, &mask, nullptr) != 0) {
        perror("pthread_sigmask");
        return 1;
    }
    int sfd = signalfd(-1, &mask, SFD_NONBLOCK | SFD_CLOEXEC);
    if (sfd == -1) {
        perror("signalfd");
        return 1;
    }

    // epoll setup
    int ep = epoll_create1(EPOLL_CLOEXEC);
    if (ep == -1) {
        perror("epoll_create1");
        return 1;
    }

    epoll_event ev{};
    ev.events = EPOLLIN | EPOLLET;   // edge-triggered, read events
    ev.data.fd = sock;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sock, &ev) == -1) {
        perror("epoll_ctl(ADD sock)");
        return 1;
    }

    epoll_event sig_ev{};
    sig_ev.events = EPOLLIN;
    sig_ev.data.fd = sfd;
    if (epoll_ctl(ep, EPOLL_CTL_ADD, sfd, &sig_ev) == -1) {
        perror("epoll_ctl(ADD signalfd)");
        return 1;
    }

    std::cout << "UDP server listening on port " << port << " (Ctrl+C to quit)\n";

    std::array<char, 65536> buf{}; // max UDP payload size (practical)

    constexpr int MAX_EVENTS = 64;
    std::array<epoll_event, MAX_EVENTS> events;

    bool running = true;
    while (running) {
        int n = epoll_wait(ep, events.data(), MAX_EVENTS, -1);
        if (n == -1) {
            if (errno == EINTR) continue;
            perror("epoll_wait");
            break;
        }

        for (int i = 0; i < n; ++i) {
            int fd = events[i].data.fd;

            if (fd == sfd) {
                // Handle shutdown signal
                signalfd_siginfo si;
                ssize_t r = read(sfd, &si, sizeof(si));
                (void)r;
                running = false;
                break;
            }

            if (fd == sock) {
                // Drain all readable datagrams (edge-triggered!)
                while (true) {
                    sockaddr_in src{};
                    socklen_t srclen = sizeof(src);
                    ssize_t r = recvfrom(sock, buf.data(), buf.size(), 0,
                                         reinterpret_cast<sockaddr*>(&src), &srclen);
                    if (r > 0) {
                        // Example "processing": print and echo back
                        char ip[INET_ADDRSTRLEN];
                        inet_ntop(AF_INET, &src.sin_addr, ip, sizeof(ip));
                        uint16_t sport = ntohs(src.sin_port);

                        std::string_view msg(buf.data(), static_cast<size_t>(r));
                        std::cout << "Got " << r << " bytes from " << ip << ":" << sport
                                  << " -> \"" << msg << "\"\n";

                        // Optional: echo response
                        // sendto(sock, msg.data(), msg.size(), 0,
                        //        reinterpret_cast<sockaddr*>(&src), srclen);
                    } else if (r == -1 && (errno == EAGAIN || errno == EWOULDBLOCK)) {
                        // No more packets
                        break;
                    } else if (r == 0) {
                        // UDP doesn't really give 0 here, but handle defensively
                        break;
                    } else {
                        perror("recvfrom");
                        break;
                    }
                }
            }
        }
    }

    close(ep);
    close(sfd);
    close(sock);
    std::cout << "Bye.\n";
    return 0;
}

