#include <arpa/inet.h>
#include <netdb.h>
#include <sys/socket.h>
#include <unistd.h>

#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>

using namespace std;
namespace fs = std::filesystem;

uint64_t htonll(uint64_t x) {
    if (htonl(1) != 1)
        return ((uint64_t)htonl(x & 0xFFFFFFFF) << 32) | htonl(x >> 32);
    return x;
}

int main(int argc, char* argv[]) {
    if (argc != 4) {
        cerr << "Usage: client <file_path> <host> <port>\n";
        return 1;
    }

    fs::path file_path = argv[1];
    string host = argv[2];
    int port = stoi(argv[3]);

    if (!fs::exists(file_path)) {
        cerr << "File does not exist\n";
        return 1;
    }

    uint64_t file_size = fs::file_size(file_path);
    string filename = file_path.filename().string();

    int sock = socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        perror("socket");
        return 1;
    }

    hostent* server = gethostbyname(host.c_str());
    if (!server) {
        cerr << "Host not found\n";
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
    memcpy(&addr.sin_addr.s_addr, server->h_addr, server->h_length);

    if (connect(sock, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("connect");
        return 1;
    }

    uint32_t name_len = htonl(filename.size());
    uint64_t size_net = htonll(file_size);

    send(sock, &name_len, sizeof(name_len), 0);
    send(sock, filename.data(), filename.size(), 0);
    send(sock, &size_net, sizeof(size_net), 0);

    ifstream in(file_path, ios::binary);
    vector<char> buf(64 * 1024);

    while (in) {
        in.read(buf.data(), buf.size());
        streamsize r = in.gcount();
        if (r > 0)
            send(sock, buf.data(), r, 0);
    }

    uint8_t status;
    recv(sock, &status, sizeof(status), MSG_WAITALL);

    if (status)
        cout << "File transfer successful\n";
    else
        cout << "File transfer failed\n";

    close(sock);
}
