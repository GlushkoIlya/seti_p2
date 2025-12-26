// socks5proxy.cpp
#include <iostream>
#include <vector>
#include <unordered_map>
#include <string>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <sys/select.h>
#include <fcntl.h>
#include <unistd.h>
#include <netdb.h>
#include <cstdlib>
#include <algorithm>
#include <cassert>
#include <csignal>

#define MAX_BUF 8192
#define DNS_PORT 53
#define DNS_SERVER_IP "8.8.8.8"

enum ClientState {
    ST_HANDSHAKE,
    ST_REQUEST,
    ST_DNS_WAIT,
    ST_CONNECTING,
    ST_RELAY,
    ST_CLOSED
};

struct Client {
    int client_fd = -1;
    int remote_fd = -1;
    ClientState state = ST_HANDSHAKE;

    std::string domain_name;
    uint16_t dns_txid = 0;
    uint16_t remote_port = 0;

    std::vector<uint8_t> c2r_buf;
    std::vector<uint8_t> r2c_buf;

    bool client_eof = false;
    bool remote_eof = false;

    std::vector<uint8_t> sock_buf;

    Client(int fd) : client_fd(fd) {
        c2r_buf.reserve(MAX_BUF);
        r2c_buf.reserve(MAX_BUF);
    }
    ~Client() {
        if (client_fd != -1) close(client_fd);
        if (remote_fd != -1) close(remote_fd);
    }
};

std::unordered_map<int, Client*> clients;

int listen_fd = -1;
int dns_fd = -1;
sockaddr_in dns_server_addr{};

uint16_t dns_txid_counter = 1;
std::unordered_map<uint16_t, int> dns_pending;

int set_nonblocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) return flags;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK);
}

void perror_exit(const char* msg) {
    perror(msg);
    exit(1);
}

int create_and_bind_tcp(int port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) perror_exit("socket");

    int opt = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (sockaddr*)&addr, sizeof(addr)) < 0)
        perror_exit("bind");

    if (listen(sock, SOMAXCONN) < 0)
        perror_exit("listen");

    if (set_nonblocking(sock) < 0)
        perror_exit("set_nonblocking");

    return sock;
}

ssize_t send_all(int fd, const uint8_t* data, size_t len) {
    size_t total_sent = 0;
    while (total_sent < len) {
        ssize_t n = send(fd, data + total_sent, len - total_sent, 0);
        if (n <= 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) break;
            return -1;
        }
        total_sent += n;
    }
    return total_sent;
}

void send_socks5_reply(int client_fd) {
    uint8_t reply[10] = {0x05, 0x00, 0x00, 0x01, 0,0,0,0, 0,0};
    send_all(client_fd, reply, 10);
}

size_t build_dns_query(uint8_t* buf, size_t bufsize, uint16_t txid, const std::string& domain) {
    if (bufsize < 12) return 0;
    memset(buf, 0, 512);
    buf[0] = txid >> 8;
    buf[1] = txid & 0xFF;
    buf[2] = 0x01; buf[3] = 0x00;
    buf[4] = 0x00; buf[5] = 0x01; // QDCOUNT=1
    buf[6] = 0x00; buf[7] = 0x00; // ANCOUNT=0
    buf[8] = 0x00; buf[9] = 0x00; // NSCOUNT=0
    buf[10] = 0x00; buf[11] = 0x00; // ARCOUNT=0

    size_t pos = 12;
    size_t start = 0;
    while (true) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t label_len = dot - start;
        if (pos + label_len + 1 >= bufsize) return 0;
        buf[pos++] = (uint8_t)label_len;
        memcpy(buf + pos, domain.data() + start, label_len);
        pos += label_len;
        if (dot == domain.size()) break;
        start = dot + 1;
    }
    buf[pos++] = 0x00;

    buf[pos++] = 0x00; buf[pos++] = 0x01; // QTYPE A
    buf[pos++] = 0x00; buf[pos++] = 0x01; // QCLASS IN

    return pos;
}

uint32_t parse_dns_response(const uint8_t* buf, size_t len) {
    if (len < 12) return 0;
    uint16_t qdcount = (buf[4] << 8) | buf[5];
    uint16_t ancount = (buf[6] << 8) | buf[7];
    if (ancount == 0) return 0;

    size_t pos = 12;
    for (int i = 0; i < qdcount; i++) {
        while (pos < len && buf[pos] != 0) {
            uint8_t lablen = buf[pos];
            if ((lablen & 0xC0) == 0xC0) {
                pos += 2;
                break;
            } else {
                pos += lablen + 1;
            }
        }
        pos++;
        if (pos + 4 > len) return 0;
        pos += 4;
    }

    for (int i = 0; i < ancount; i++) {
        if (pos + 12 > len) return 0;
        uint16_t type = (buf[pos + 2] << 8) | buf[pos + 3];
        uint16_t data_len = (buf[pos + 10] << 8) | buf[pos + 11];
        pos += 12;

        if (type == 1 && data_len == 4 && pos + 4 <= len) {
            uint32_t ip;
            memcpy(&ip, buf + pos, 4);
            return ip;
        }
        pos += data_len;
    }
    return 0;
}

int async_connect_ipv4(uint32_t ip, uint16_t port) {
    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) return -1;
    set_nonblocking(sock);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    addr.sin_addr.s_addr = ip;

    int res = connect(sock, (sockaddr*)&addr, sizeof(addr));
    if (res == 0) return sock;
    if (errno == EINPROGRESS) return sock;
    close(sock);
    return -1;
}

void close_client(int fd) {
    auto it = clients.find(fd);
    if (it != clients.end()) {
        Client* c = it->second;
        clients.erase(it);
        if (c->remote_fd != -1) close(c->remote_fd);
        close(c->client_fd);
        delete c;
    }
}

bool handle_socks5_handshake(Client* c) {
    uint8_t buf[512];
    ssize_t n = recv(c->client_fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (n == 0) close_client(c->client_fd);
        else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recv handshake");
            close_client(c->client_fd);
        }
        return false;
    }
    c->sock_buf.insert(c->sock_buf.end(), buf, buf + n);
    if (c->sock_buf.size() < 2) return true;
    if (c->sock_buf[0] != 0x05) {
        std::cerr << "Unsupported SOCKS version\n";
        close_client(c->client_fd);
        return false;
    }
    size_t nmethods = c->sock_buf[1];
    if (c->sock_buf.size() < 2 + nmethods) return true;

    uint8_t resp[2] = {0x05, 0x00};
    send_all(c->client_fd, resp, 2);
    c->sock_buf.clear();
    c->state = ST_REQUEST;
    return true;
}

bool handle_socks5_request(Client* c) {
    uint8_t buf[512];
    ssize_t n = recv(c->client_fd, buf, sizeof(buf), 0);
    if (n <= 0) {
        if (n == 0) close_client(c->client_fd);
        else if (errno != EAGAIN && errno != EWOULDBLOCK) {
            perror("recv request");
            close_client(c->client_fd);
        }
        return false;
    }
    c->sock_buf.insert(c->sock_buf.end(), buf, buf + n);
    if (c->sock_buf.size() < 7) return true;

    if (c->sock_buf[0] != 0x05 || c->sock_buf[1] != 0x01 || c->sock_buf[2] != 0x00) {
        std::cerr << "Unsupported request\n";
        close_client(c->client_fd);
        return false;
    }

    uint8_t atyp = c->sock_buf[3];
    size_t req_len = 0;

    if (atyp == 0x01) {
        if (c->sock_buf.size() < 10) return true;
        uint32_t ip = *(uint32_t*)&c->sock_buf[4];
        c->remote_port = (c->sock_buf[8] << 8) | c->sock_buf[9];
        req_len = 10;
        int rfd = async_connect_ipv4(ip, c->remote_port);
        if (rfd < 0) {
            std::cerr << "Failed to connect remote IPv4\n";
            close_client(c->client_fd);
            return false;
        }
        c->remote_fd = rfd;
        c->state = ST_CONNECTING;
    } else if (atyp == 0x03) {
        size_t addr_len = c->sock_buf[4];
        if (c->sock_buf.size() < 5 + addr_len + 2) return true;
        c->domain_name = std::string((char*)&c->sock_buf[5], addr_len);
        c->remote_port = (c->sock_buf[5 + addr_len] << 8) | c->sock_buf[6 + addr_len];
        req_len = 5 + addr_len + 2;

        uint8_t dns_buf[512];
        uint16_t txid = dns_txid_counter++;
        if (txid == 0) txid = 1;
        c->dns_txid = txid;
        dns_pending[txid] = c->client_fd;

        size_t dns_len = build_dns_query(dns_buf, sizeof(dns_buf), txid, c->domain_name);
        if (dns_len == 0) {
            std::cerr << "Failed to build DNS query\n";
            close_client(c->client_fd);
            return false;
        }
        ssize_t sent = sendto(dns_fd, dns_buf, dns_len, 0, (sockaddr*)&dns_server_addr, sizeof(dns_server_addr));
        if (sent != (ssize_t)dns_len) {
            perror("sendto dns");
            close_client(c->client_fd);
            return false;
        }
        c->state = ST_DNS_WAIT;
    } else {
        std::cerr << "Unsupported address type\n";
        close_client(c->client_fd);
        return false;
    }
    c->sock_buf.erase(c->sock_buf.begin(), c->sock_buf.begin() + req_len);
    return true;
}

void handle_dns_response() {
    uint8_t buf[512];
    sockaddr_in from{};
    socklen_t fromlen = sizeof(from);

    ssize_t n = recvfrom(dns_fd, buf, sizeof(buf), 0, (sockaddr*)&from, &fromlen);
    if (n <= 0) return;
    if ((size_t)n < 12) return;

    uint16_t txid = (buf[0] << 8) | buf[1];
    auto it = dns_pending.find(txid);
    if (it == dns_pending.end()) return;

    int client_fd = it->second;
    dns_pending.erase(it);

    auto cl_it = clients.find(client_fd);
    if (cl_it == clients.end()) return;
    Client* c = cl_it->second;

    uint32_t ip = parse_dns_response(buf, n);
    if (ip == 0) {
        std::cerr << "DNS resolution failed\n";
        close_client(client_fd);
        return;
    }

    int rfd = async_connect_ipv4(ip, c->remote_port);
    if (rfd < 0) {
        std::cerr << "Failed to connect remote IPv4 (DNS resolved)\n";
        close_client(client_fd);
        return;
    }
    c->remote_fd = rfd;
    c->state = ST_CONNECTING;
}

void process_relay(Client* c, fd_set &read_fds, fd_set &write_fds) {
    // client -> remote
    if (FD_ISSET(c->client_fd, &read_fds) && c->c2r_buf.size() < MAX_BUF) {
        uint8_t buf[4096];
        ssize_t n = recv(c->client_fd, buf, sizeof(buf), 0);
        if (n > 0) c->c2r_buf.insert(c->c2r_buf.end(), buf, buf + n);
        else if (n == 0) c->client_eof = true;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) { close_client(c->client_fd); return; }
    }
    if (FD_ISSET(c->remote_fd, &write_fds) && !c->c2r_buf.empty()) {
        ssize_t n = send(c->remote_fd, c->c2r_buf.data(), c->c2r_buf.size(), 0);
        if (n > 0) c->c2r_buf.erase(c->c2r_buf.begin(), c->c2r_buf.begin() + n);
        else if (errno != EAGAIN && errno != EWOULDBLOCK) { close_client(c->client_fd); return; }
    }

    // remote -> client
    if (FD_ISSET(c->remote_fd, &read_fds) && c->r2c_buf.size() < MAX_BUF) {
        uint8_t buf[4096];
        ssize_t n = recv(c->remote_fd, buf, sizeof(buf), 0);
        if (n > 0) c->r2c_buf.insert(c->r2c_buf.end(), buf, buf + n);
        else if (n == 0) c->remote_eof = true;
        else if (errno != EAGAIN && errno != EWOULDBLOCK) { close_client(c->client_fd); return; }
    }
    if (FD_ISSET(c->client_fd, &write_fds) && !c->r2c_buf.empty()) {
        ssize_t n = send(c->client_fd, c->r2c_buf.data(), c->r2c_buf.size(), 0);
        if (n > 0) c->r2c_buf.erase(c->r2c_buf.begin(), c->r2c_buf.begin() + n);
        else if (errno != EAGAIN && errno != EWOULDBLOCK) { close_client(c->client_fd); return; }
    }

    if (c->client_eof && c->remote_eof && c->c2r_buf.empty() && c->r2c_buf.empty()) {
        close_client(c->client_fd);
    }
}

void sigsegv_handler(int) {
    std::cerr << "Segmentation fault detected!\n";
    exit(1);
}

int main(int argc, char* argv[]) {
    signal(SIGSEGV, sigsegv_handler);
    if (argc != 2) {
        std::cerr << "Usage: " << argv[0] << " <listen_port>\n";
        return 1;
    }
    int port = atoi(argv[1]);
    if (port <= 0 || port > 65535) {
        std::cerr << "Invalid port\n";
        return 1;
    }

    listen_fd = create_and_bind_tcp(port);
    std::cout << "Listening on port " << port << "\n";

    dns_fd = socket(AF_INET, SOCK_DGRAM, 0);
    if (dns_fd < 0) perror_exit("socket dns");

    set_nonblocking(dns_fd);

    memset(&dns_server_addr, 0, sizeof(dns_server_addr));
    dns_server_addr.sin_family = AF_INET;
    dns_server_addr.sin_port = htons(DNS_PORT);
    inet_pton(AF_INET, DNS_SERVER_IP, &dns_server_addr.sin_addr);

    while (true) {
        fd_set read_fds, write_fds;
        FD_ZERO(&read_fds);
        FD_ZERO(&write_fds);

        FD_SET(listen_fd, &read_fds);
        FD_SET(dns_fd, &read_fds);

        int maxfd = std::max(listen_fd, dns_fd);

        for (auto& p : clients) {
            Client* c = p.second;
            if (c->state == ST_HANDSHAKE || c->state == ST_REQUEST || c->state == ST_DNS_WAIT || c->state == ST_RELAY) {
                FD_SET(c->client_fd, &read_fds);
                maxfd = std::max(maxfd, c->client_fd);
            }
            if (c->state == ST_CONNECTING || c->state == ST_RELAY) {
                if (c->remote_fd != -1) {
                    if (c->state == ST_CONNECTING) FD_SET(c->remote_fd, &write_fds);
                    else {
                        FD_SET(c->remote_fd, &read_fds);
                        maxfd = std::max(maxfd, c->remote_fd);
                    }
                    maxfd = std::max(maxfd, c->remote_fd);
                }
            }
            // В режиме relay ставим write_fds для клиентского сокета, если есть данные в r2c_buf
            if (c->state == ST_RELAY && !c->r2c_buf.empty()) {
                FD_SET(c->client_fd, &write_fds);
            }
            // Пишем в remote_fd, если есть данные в c2r_buf
            if (c->state == ST_RELAY && !c->c2r_buf.empty()) {
                FD_SET(c->remote_fd, &write_fds);
            }
        }

        int rv = select(maxfd + 1, &read_fds, &write_fds, nullptr, nullptr);
        if (rv < 0) {
            if (errno == EINTR) continue;
            perror_exit("select");
        }

        if (FD_ISSET(listen_fd, &read_fds)) {
            sockaddr_in client_addr{};
            socklen_t client_len = sizeof(client_addr);
            int cfd = accept(listen_fd, (sockaddr*)&client_addr, &client_len);
            if (cfd >= 0) {
                set_nonblocking(cfd);
                clients[cfd] = new Client(cfd);
            }
        }

        if (FD_ISSET(dns_fd, &read_fds)) {
            handle_dns_response();
        }

        std::vector<int> to_close;
        for (auto& p : clients) {
            Client* c = p.second;
            if (c->state == ST_HANDSHAKE) {
                handle_socks5_handshake(c);
            } else if (c->state == ST_REQUEST) {
                handle_socks5_request(c);
            } else if (c->state == ST_CONNECTING && c->remote_fd != -1 && FD_ISSET(c->remote_fd, &write_fds)) {
                int err = 0;
                socklen_t len = sizeof(err);
                if (getsockopt(c->remote_fd, SOL_SOCKET, SO_ERROR, &err, &len) < 0 || err != 0) {
                    close_client(c->client_fd);
                    continue;
                }
                send_socks5_reply(c->client_fd);
                c->state = ST_RELAY;
            } else if (c->state == ST_RELAY) {
                process_relay(c, read_fds, write_fds);
            }
        }
    }
    return 0;
}
