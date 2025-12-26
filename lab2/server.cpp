#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <thread>
#include <vector>

using namespace std;
namespace fs = std::filesystem;

constexpr size_t BUF_SIZE = 64 * 1024;

uint64_t ntohll(uint64_t x) {
    if (htonl(1) != 1)
        return ((uint64_t)ntohl(x & 0xFFFFFFFF) << 32) | ntohl(x >> 32);
    return x;
}

void handle_client(int client_fd, sockaddr_in client_addr) {
    try {
        uint32_t name_len_net;
        recv(client_fd, &name_len_net, sizeof(name_len_net), MSG_WAITALL);
        uint32_t name_len = ntohl(name_len_net);

        if (name_len == 0 || name_len > 4096)
            throw runtime_error("Invalid filename length");

        string filename(name_len, '\0');
        recv(client_fd, filename.data(), name_len, MSG_WAITALL);

        uint64_t file_size_net;
        recv(client_fd, &file_size_net, sizeof(file_size_net), MSG_WAITALL);
        uint64_t file_size = ntohll(file_size_net);

        fs::create_directories("uploads");

        fs::path safe_name = fs::path(filename).filename();
        fs::path out_path = fs::path("uploads") / safe_name;

        ofstream out(out_path, ios::binary);
        if (!out)
            throw runtime_error("Cannot open output file");

        atomic<uint64_t> received_bytes{0};
        auto start = chrono::steady_clock::now();
        auto last_report = start;

        vector<char> buf(BUF_SIZE);
        uint64_t total = 0;

        while (total < file_size) {
            ssize_t r = recv(client_fd, buf.data(),
                             min<uint64_t>(BUF_SIZE, file_size - total), 0);
            if (r <= 0)
                throw runtime_error("Connection lost");

            out.write(buf.data(), r);
            total += r;
            received_bytes += r;

            auto now = chrono::steady_clock::now();
            if (chrono::duration_cast<chrono::seconds>(now - last_report).count() >= 3) {
                double elapsed = chrono::duration<double>(now - start).count();
                double inst = received_bytes.exchange(0) / 3.0;
                double avg = total / elapsed;

                cout << "[Client "
                     << inet_ntoa(client_addr.sin_addr)
                     << "] speed: instant=" << inst
                     << " B/s, avg=" << avg << " B/s\n";

                last_report = now;
            }
        }

        auto end = chrono::steady_clock::now();
        double elapsed = chrono::duration<double>(end - start).count();
        if (elapsed > 0) {
            cout << "[Client "
                 << inet_ntoa(client_addr.sin_addr)
                 << "] final avg speed: "
                 << (total / elapsed) << " B/s\n";
        }

        uint8_t status = (total == file_size) ? 1 : 0;
        send(client_fd, &status, sizeof(status), 0);

        out.close();
    } catch (const exception& e) {
        uint8_t status = 0;
        send(client_fd, &status, sizeof(status), 0);
        cerr << "Client error: " << e.what() << endl;
    }

    close(client_fd);
}

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "Usage: server <port>\n";
        return 1;
    }

    int port = stoi(argv[1]);

    int server_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("socket");
        return 1;
    }

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    addr.sin_port = htons(port);

    if (bind(server_fd, (sockaddr*)&addr, sizeof(addr)) < 0) {
        perror("bind");
        return 1;
    }

    listen(server_fd, SOMAXCONN);
    cout << "Server listening on port " << port << endl;

    while (true) {
        sockaddr_in client_addr{};
        socklen_t len = sizeof(client_addr);
        int client_fd = accept(server_fd, (sockaddr*)&client_addr, &len);
        if (client_fd < 0)
            continue;

        thread(handle_client, client_fd, client_addr).detach();
    }
}
