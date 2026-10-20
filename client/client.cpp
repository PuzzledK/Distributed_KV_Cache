#include <cerrno>
#include <cstring>
#include <iostream>
#include <netdb.h>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <unistd.h>

namespace {

int connectTo(const std::string& host, const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const int lookupResult = getaddrinfo(host.c_str(), port.c_str(), &hints, &results);
    if (lookupResult != 0) {
        throw std::runtime_error("Address lookup failed: " +
                                 std::string(gai_strerror(lookupResult)));
    }

    int fd = -1;
    int lastError = 0;
    for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
        fd = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (fd == -1) {
            lastError = errno;
            continue;
        }
        if (connect(fd, address->ai_addr, address->ai_addrlen) == 0) {
            break;
        }
        lastError = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(results);

    if (fd == -1) {
        throw std::runtime_error("Could not connect: " + std::string(std::strerror(lastError)));
    }
    return fd;
}

void sendAll(int fd, const std::string& message) {
    std::size_t sent = 0;
    while (sent < message.size()) {
        const ssize_t count =
            send(fd, message.data() + sent, message.size() - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Socket write failed: " +
                                     std::string(std::strerror(errno)));
        }
        if (count == 0) {
            throw std::runtime_error("Socket closed while writing");
        }
        sent += static_cast<std::size_t>(count);
    }
}

std::string readResponse(int fd) {
    std::string response;
    char character = '\0';
    while (response.size() < 4096) {
        const ssize_t count = recv(fd, &character, 1, 0);
        if (count == 0) {
            throw std::runtime_error("Server closed the connection");
        }
        if (count < 0) {
            if (errno == EINTR) {
                continue;
            }
            throw std::runtime_error("Socket read failed: " +
                                     std::string(std::strerror(errno)));
        }
        if (character == '\n') {
            if (!response.empty() && response.back() == '\r') {
                response.pop_back();
            }
            return response;
        }
        response.push_back(character);
    }
    throw std::runtime_error("Server response exceeds maximum size");
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: kv_client <host> <port>\n"
                     "Commands: SET key value | GET key | DEL key | exit\n";
        return 0;
    }
    if (argc != 3) {
        std::cerr << "Usage: kv_client <host> <port>\n";
        return 2;
    }

    int fd = -1;
    try {
        fd = connectTo(argv[1], argv[2]);
        std::string request;
        while (std::getline(std::cin, request)) {
            if (request == "exit" || request == "quit") {
                break;
            }
            if (request.empty()) {
                continue;
            }
            sendAll(fd, request + "\n");
            std::cout << readResponse(fd) << '\n';
        }
        close(fd);
    } catch (const std::exception& error) {
        if (fd >= 0) {
            close(fd);
        }
        std::cerr << "Client error: " << error.what() << '\n';
        return 1;
    }
    return 0;
}
