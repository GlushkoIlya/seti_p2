#include <iostream>
#include <unordered_map>
#include <chrono>
#include <thread>
#include <cstring>
#include <sstream>

#include <sys/types.h>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

constexpr int PORT = 50000;
constexpr int SEND_INTERVAL_MS = 100;
constexpr int TIMEOUT_MS = 300;

using Clock = std::chrono::steady_clock;

struct Peer {
    std::string ip;
    Clock::time_point last_seen;
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <multicast_address>\n";
        return 1;
    }

    const char* group = argv[1];
    bool ipv6 = strchr(group, ':') != nullptr;
    pid_t my_pid = getpid();

    int sock;
    sockaddr_storage mcast_addr{};
    socklen_t mcast_len{};

    if (ipv6) {
        sock = socket(AF_INET6, SOCK_DGRAM, 0);
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in6 local{};
        local.sin6_family = AF_INET6;
        local.sin6_port = htons(PORT);
        local.sin6_addr = in6addr_any;
        bind(sock, (sockaddr*)&local, sizeof(local));

        ipv6_mreq mreq{};
        inet_pton(AF_INET6, group, &mreq.ipv6mr_multiaddr);
        mreq.ipv6mr_interface = 0;
        setsockopt(sock, IPPROTO_IPV6, IPV6_JOIN_GROUP, &mreq, sizeof(mreq));

        sockaddr_in6 addr{};
        addr.sin6_family = AF_INET6;
        addr.sin6_port = htons(PORT);
        inet_pton(AF_INET6, group, &addr.sin6_addr);
        memcpy(&mcast_addr, &addr, sizeof(addr));
        mcast_len = sizeof(addr);
    } else {
        sock = socket(AF_INET, SOCK_DGRAM, 0);
        int reuse = 1;
        setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

        sockaddr_in local{};
        local.sin_family = AF_INET;
        local.sin_port = htons(PORT);
        local.sin_addr.s_addr = INADDR_ANY;
        bind(sock, (sockaddr*)&local, sizeof(local));

        ip_mreq mreq{};
        inet_pton(AF_INET, group, &mreq.imr_multiaddr);
        mreq.imr_interface.s_addr = INADDR_ANY;
        setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq));

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(PORT);
        inet_pton(AF_INET, group, &addr.sin_addr);
        memcpy(&mcast_addr, &addr, sizeof(addr));
        mcast_len = sizeof(addr);
    }

    std::unordered_map<pid_t, Peer> peers;

    auto print_peers = [&]() {
        std::cout << "Alive copies:\n";
        for (auto& [pid, peer] : peers)
            std::cout << "  " << peer.ip << " (pid=" << pid << ")\n";
        std::cout << "-----------------\n";
    };

    char buf[128];

    while (true) {
        // send HELLO <pid>
        std::ostringstream os;
        os << "HELLO " << my_pid;
        std::string msg = os.str();

        sendto(sock, msg.c_str(), msg.size(), 0,
               (sockaddr*)&mcast_addr, mcast_len);

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);

        timeval tv{};
        tv.tv_usec = SEND_INTERVAL_MS * 1000;

        if (select(sock + 1, &fds, nullptr, nullptr, &tv) > 0) {
            sockaddr_storage src{};
            socklen_t slen = sizeof(src);
            int n = recvfrom(sock, buf, sizeof(buf) - 1, 0,
                             (sockaddr*)&src, &slen);
            if (n > 0) {
                buf[n] = 0;
                pid_t pid;
                if (sscanf(buf, "HELLO %d", &pid) != 1) continue;
                if (pid == my_pid) continue;

                char ip[INET6_ADDRSTRLEN];
                if (src.ss_family == AF_INET) {
                    auto* s = (sockaddr_in*)&src;
                    inet_ntop(AF_INET, &s->sin_addr, ip, sizeof(ip));
                } else {
                    auto* s = (sockaddr_in6*)&src;
                    inet_ntop(AF_INET6, &s->sin6_addr, ip, sizeof(ip));
                }

                bool is_new = peers.count(pid) == 0;
                peers[pid] = {ip, Clock::now()};
                if (is_new) print_peers();
            }
        }

        auto now = Clock::now();
        bool changed = false;

        for (auto it = peers.begin(); it != peers.end();) {
            auto dt = std::chrono::duration_cast<std::chrono::milliseconds>(
                now - it->second.last_seen).count();
            if (dt > TIMEOUT_MS) {
                it = peers.erase(it);
                changed = true;
            } else ++it;
        }

        if (changed) print_peers();
        std::this_thread::sleep_for(
            std::chrono::milliseconds(SEND_INTERVAL_MS));
    }
}
