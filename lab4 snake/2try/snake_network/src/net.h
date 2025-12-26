#pragma once

#include <string>
#include "snakes.pb.h"
#include <cstdint>
#include <netinet/in.h>

struct NetAddress {
    std::string ip;
    uint16_t port;
};

class Net {
public:
    Net();
    ~Net();

    // Инициализирует multicast receive socket и unicast socket (bind any port)
    void init();

    // Отправка (unicast)
    bool sendTo(const snakes::GameMessage& msg, const NetAddress& addr);

    // Отправка announcement на multicast (через unicast socket)
    bool sendAnnouncement(const snakes::GameMessage& msg);

    // Блокирующий recv: ждёт сообщений либо на multicast_sock_, либо на unicast_sock_
    // Возвращает true при успешном разборе protobuf-сообщения и заполняет sender
    bool recv(snakes::GameMessage& out, NetAddress& sender);

    // Возвращает локальный порт unicast сокета
    uint16_t localPort() const;

private:
    int multicast_sock_;
    int unicast_sock_;
    struct sockaddr_in multicast_addr_;
};
