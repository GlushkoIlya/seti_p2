#include "net.h"
#include <arpa/inet.h>
#include <sys/socket.h>
#include <unistd.h>
#include <netinet/in.h>
#include <cstring>
#include <iostream>
#include <sys/select.h>

using namespace std;
using namespace snakes;

const char* MULTICAST_IP = "239.192.0.4";
const uint16_t MULTICAST_PORT = 9192;

Net::Net() : multicast_sock_(-1), unicast_sock_(-1) {}
Net::~Net() { if (multicast_sock_!=-1) close(multicast_sock_); if (unicast_sock_!=-1) close(unicast_sock_); }

void Net::init() {
    // multicast socket (for receive only)
    multicast_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (multicast_sock_ < 0) { perror("multicast socket"); exit(1); }
    int reuse = 1;
    setsockopt(multicast_sock_, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    struct sockaddr_in local{};
    local.sin_family = AF_INET;
    local.sin_addr.s_addr = INADDR_ANY;
    local.sin_port = htons(MULTICAST_PORT);
    if (bind(multicast_sock_, (struct sockaddr*)&local, sizeof(local)) < 0) { perror("bind multicast"); exit(1); }

    ip_mreq mreq{};
    mreq.imr_multiaddr.s_addr = inet_addr(MULTICAST_IP);
    mreq.imr_interface.s_addr = INADDR_ANY;
    if (setsockopt(multicast_sock_, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0) { perror("IP_ADD_MEMBERSHIP"); exit(1); }

    // unicast socket (send/recv)
    unicast_sock_ = socket(AF_INET, SOCK_DGRAM, 0);
    if (unicast_sock_ < 0) { perror("unicast socket"); exit(1); }
    struct sockaddr_in ulocal{};
    ulocal.sin_family = AF_INET;
    ulocal.sin_addr.s_addr = INADDR_ANY;
    ulocal.sin_port = 0; // any
    if (bind(unicast_sock_, (struct sockaddr*)&ulocal, sizeof(ulocal)) < 0) { perror("bind unicast"); exit(1); }

    memset(&multicast_addr_, 0, sizeof(multicast_addr_));
    multicast_addr_.sin_family = AF_INET;
    multicast_addr_.sin_port = htons(MULTICAST_PORT);
    inet_pton(AF_INET, MULTICAST_IP, &multicast_addr_.sin_addr);
}

bool Net::sendTo(const GameMessage& msg, const NetAddress& addr) {
    std::string buf = msg.SerializeAsString();
    struct sockaddr_in dst{};
    dst.sin_family = AF_INET;
    dst.sin_port = htons(addr.port);
    if (inet_pton(AF_INET, addr.ip.c_str(), &dst.sin_addr) <= 0) dst.sin_addr.s_addr = inet_addr(addr.ip.c_str());
    ssize_t sent = sendto(unicast_sock_, buf.data(), buf.size(), 0, (struct sockaddr*)&dst, sizeof(dst));
    return sent == (ssize_t)buf.size();
}

bool Net::sendAnnouncement(const GameMessage& msg) {
    std::string buf = msg.SerializeAsString();
    ssize_t sent = sendto(unicast_sock_, buf.data(), buf.size(), 0, (struct sockaddr*)&multicast_addr_, sizeof(multicast_addr_));
    return sent == (ssize_t)buf.size();
}

bool Net::recv(GameMessage& out, NetAddress& sender) {
    fd_set readfds;
    int maxfd = std::max(multicast_sock_, unicast_sock_);
    FD_ZERO(&readfds);
    FD_SET(multicast_sock_, &readfds);
    FD_SET(unicast_sock_, &readfds);

    int rv = select(maxfd+1, &readfds, nullptr, nullptr, nullptr);
    if (rv <= 0) return false;

    char buf[65536];
    struct sockaddr_in src{};
    socklen_t slen = sizeof(src);
    ssize_t r = 0;
    if (FD_ISSET(unicast_sock_, &readfds)) {
        r = recvfrom(unicast_sock_, buf, sizeof(buf), 0, (struct sockaddr*)&src, &slen);
    } else if (FD_ISSET(multicast_sock_, &readfds)) {
        r = recvfrom(multicast_sock_, buf, sizeof(buf), 0, (struct sockaddr*)&src, &slen);
    } else {
        return false;
    }
    if (r <= 0) return false;
    if (!out.ParseFromArray(buf, r)) return false;
    sender.ip = inet_ntoa(src.sin_addr);
    sender.port = ntohs(src.sin_port);
    return true;
}

uint16_t Net::localPort() const {
    struct sockaddr_in addr{}; socklen_t len = sizeof(addr);
    if (getsockname(unicast_sock_, (struct sockaddr*)&addr, &len) == 0) return ntohs(addr.sin_port);
    return 0;
}
