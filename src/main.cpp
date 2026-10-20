#include "HashTable.hpp"

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <netdb.h>
#include <sstream>
#include <stdexcept>
#include <string>
#include <sys/socket.h>
#include <netinet/in.h>
#include <thread>
#include <unistd.h>
#include <vector>

namespace {

constexpr std::size_t MAX_REQUEST_SIZE = 4096;
constexpr std::size_t MAX_ROUTING_HEADER_SIZE = 32;
constexpr std::size_t VIRTUAL_NODES_PER_SERVER = 128;
constexpr char FORWARD_PREFIX[] = "__KV_FORWARD ";

struct Endpoint {
    std::string host;
    std::string port;
};

Endpoint parseEndpoint(const std::string& value) {
    std::string host;
    std::string port;

    if (!value.empty() && value.front() == '[') {
        const std::size_t close = value.find(']');
        if (close == std::string::npos || close + 1 >= value.size() ||
            value[close + 1] != ':') {
            throw std::invalid_argument("Invalid endpoint: " + value);
        }
        host = value.substr(1, close - 1);
        port = value.substr(close + 2);
    } else {
        const std::size_t separator = value.rfind(':');
        if (separator == std::string::npos) {
            throw std::invalid_argument("Invalid endpoint: " + value);
        }
        host = value.substr(0, separator);
        port = value.substr(separator + 1);
    }

    if (host.empty() || port.empty()) {
        throw std::invalid_argument("Invalid endpoint: " + value);
    }

    std::size_t parsedCharacters = 0;
    const unsigned long portNumber = std::stoul(port, &parsedCharacters);
    if (parsedCharacters != port.size() || portNumber == 0 || portNumber > 65535) {
        throw std::invalid_argument("Invalid port in endpoint: " + value);
    }

    return {host, port};
}

std::uint64_t hashString(const std::string& value) {
    std::uint64_t hash = 14695981039346656037ULL;
    for (const unsigned char character : value) {
        hash ^= character;
        hash *= 1099511628211ULL;
    }
    return hash;
}

class HashRing {
    std::map<std::uint64_t, std::string> ring;
    std::size_t numberOfServers = 0;

public:
    explicit HashRing(std::vector<std::string> servers) {
        std::sort(servers.begin(), servers.end());
        servers.erase(std::unique(servers.begin(), servers.end()), servers.end());
        numberOfServers = servers.size();

        for (const std::string& server : servers) {
            for (std::size_t replica = 0; replica < VIRTUAL_NODES_PER_SERVER; ++replica) {
                ring.emplace(hashString(server + "#" + std::to_string(replica)), server);
            }
        }
    }

    const std::string& serverFor(const std::string& key) const {
        auto server = ring.lower_bound(hashString(key));
        if (server == ring.end()) {
            server = ring.begin();
        }
        return server->second;
    }

    std::size_t serverCount() const {
        return numberOfServers;
    }
};

int connectTo(const Endpoint& endpoint) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* results = nullptr;
    const int lookupResult =
        getaddrinfo(endpoint.host.c_str(), endpoint.port.c_str(), &hints, &results);
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
        throw std::runtime_error("Could not connect to " + endpoint.host + ":" +
                                 endpoint.port + ": " + std::strerror(lastError));
    }
    return fd;
}

void sendAll(int fd, const std::string& data) {
    std::size_t sent = 0;
    while (sent < data.size()) {
        const ssize_t count =
            send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
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

class SocketReader {
    int fd;
    char buffer[4096]{};
    std::size_t position = 0;
    std::size_t available = 0;

public:
    explicit SocketReader(int socket) : fd(socket) {}

    bool readLine(std::string& line) {
        line.clear();
        while (true) {
            if (position == available) {
                const ssize_t count = recv(fd, buffer, sizeof(buffer), 0);
                if (count == 0) {
                    return false;
                }
                if (count < 0) {
                    if (errno == EINTR) {
                        continue;
                    }
                    throw std::runtime_error("Socket read failed: " +
                                             std::string(std::strerror(errno)));
                }
                position = 0;
                available = static_cast<std::size_t>(count);
            }

            const char character = buffer[position++];
            if (character == '\n') {
                if (!line.empty() && line.back() == '\r') {
                    line.pop_back();
                }
                return true;
            }
            if (line.size() == MAX_REQUEST_SIZE + MAX_ROUTING_HEADER_SIZE) {
                throw std::length_error("Request exceeds maximum size");
            }
            line.push_back(character);
        }
    }
};

std::string processRequest(const std::string& request,
                           const std::string& self,
                           const HashRing& ring,
                           HashTable& table) {
    unsigned int forwardCount = 0;
    std::string payload = request;
    if (request.compare(0, sizeof(FORWARD_PREFIX) - 1, FORWARD_PREFIX) == 0) {
        std::istringstream routingHeader(
            request.substr(sizeof(FORWARD_PREFIX) - 1));
        if (!(routingHeader >> forwardCount) || forwardCount == 0) {
            return "ERROR invalid internal routing header";
        }
        std::getline(routingHeader, payload);
        if (!payload.empty() && payload.front() == ' ') {
            payload.erase(0, 1);
        }
    }

    std::string command;
    std::string key;
    std::string value;
    std::string extra;
    std::istringstream input(payload);
    input >> command >> key;

    if (command == "SET") {
        input >> value;
        if (key.empty() || value.empty() || input >> extra) {
            return "ERROR usage: SET key value";
        }
    } else if (command == "GET" || command == "DEL") {
        if (key.empty() || input >> extra) {
            return "ERROR usage: " + command + " key";
        }
    } else {
        return "ERROR unknown command";
    }

    const std::string& owner = ring.serverFor(key);
    if (owner != self) {
        if (forwardCount >= ring.serverCount()) {
            std::cerr << "Routing loop detected for key '" << key
                      << "'; verify every node uses the same --nodes list and "
                         "its own canonical --self endpoint\n";
            return "ERROR routing loop; verify identical --nodes and --self endpoints";
        }
        try {
            const int upstream = connectTo(parseEndpoint(owner));
            std::string response;
            try {
                sendAll(upstream, std::string(FORWARD_PREFIX) +
                                      std::to_string(forwardCount + 1) + " " +
                                      payload + "\n");
                SocketReader reader(upstream);
                if (!reader.readLine(response)) {
                    throw std::runtime_error("Owner closed the connection without a response");
                }
            } catch (...) {
                close(upstream);
                throw;
            }
            close(upstream);
            return response;
        } catch (const std::exception& error) {
            std::cerr << "Routing to " << owner << " failed: " << error.what() << '\n';
            return "ERROR routing request to key owner";
        }
    }

    if (command == "SET") {
        table.set(key, value);
        return "OK";
    }
    if (command == "GET") {
        std::string result;
        if (!table.get(key, result)) {
            return "NOT_FOUND";
        }
        return "VALUE " + result;
    }

    table.del(key);
    return "OK";
}

void handleClient(int fd,
                  const std::string& self,
                  const HashRing& ring,
                  HashTable& table) {
    SocketReader reader(fd);
    std::string request;
    try {
        while (reader.readLine(request)) {
            const std::string response = processRequest(request, self, ring, table);
            sendAll(fd, response + "\n");
        }
    } catch (const std::length_error& error) {
        std::cerr << "Closing client connection: " << error.what() << '\n';
    } catch (const std::exception& error) {
        std::cerr << "Client connection failed: " << error.what() << '\n';
    }
    close(fd);
}

int createListener(const std::string& port) {
    addrinfo hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    addrinfo* results = nullptr;
    const int lookupResult = getaddrinfo(nullptr, port.c_str(), &hints, &results);
    if (lookupResult != 0) {
        throw std::runtime_error("Address lookup failed: " +
                                 std::string(gai_strerror(lookupResult)));
    }

    int listener = -1;
    for (addrinfo* address = results; address != nullptr; address = address->ai_next) {
        listener = socket(address->ai_family, address->ai_socktype, address->ai_protocol);
        if (listener == -1) {
            continue;
        }

        int reuseAddress = 1;
        setsockopt(listener, SOL_SOCKET, SO_REUSEADDR, &reuseAddress, sizeof(reuseAddress));
        if (bind(listener, address->ai_addr, address->ai_addrlen) == 0 &&
            listen(listener, SOMAXCONN) == 0) {
            break;
        }
        close(listener);
        listener = -1;
    }
    freeaddrinfo(results);

    if (listener == -1) {
        throw std::runtime_error("Could not bind server socket on port " + port +
                                 ": " + std::strerror(errno));
    }
    return listener;
}

std::vector<std::string> parseServers(const std::string& value) {
    std::vector<std::string> servers;
    std::size_t start = 0;
    while (start <= value.size()) {
        const std::size_t separator = value.find(',', start);
        const std::string server =
            value.substr(start, separator == std::string::npos ? separator : separator - start);
        parseEndpoint(server);
        servers.push_back(server);
        if (separator == std::string::npos) {
            break;
        }
        start = separator + 1;
    }
    if (servers.empty()) {
        throw std::invalid_argument("At least one server endpoint is required");
    }
    return servers;
}

}  // namespace

int main(int argc, char* argv[]) {
    if (argc == 2 && std::string(argv[1]) == "--help") {
        std::cout << "Usage: kv_cache --self host:port --nodes host:port[,host:port...]\n";
        return 0;
    }
    if (argc != 5 || std::string(argv[1]) != "--self" ||
        std::string(argv[3]) != "--nodes") {
        std::cerr << "Usage: kv_cache --self host:port --nodes host:port[,host:port...]\n";
        return 2;
    }

    try {
        const std::string self = argv[2];
        const Endpoint selfEndpoint = parseEndpoint(self);
        std::vector<std::string> servers = parseServers(argv[4]);
        if (std::find(servers.begin(), servers.end(), self) == servers.end()) {
            throw std::invalid_argument("--self endpoint must be included in --nodes");
        }

        const HashRing ring(servers);
        HashTable table("cache_" + selfEndpoint.port + ".log", 16);
        const int listener = createListener(selfEndpoint.port);

        std::cout << "Listening on port " << selfEndpoint.port << " as " << self
                  << " with " << servers.size() << " ring nodes\n";

        while (true) {
            const int client = accept(listener, nullptr, nullptr);
            if (client < 0) {
                if (errno == EINTR) {
                    continue;
                }
                if (errno == EMFILE || errno == ENFILE) {
                    std::cerr << "Accept temporarily unavailable: "
                              << std::strerror(errno) << '\n';
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                    continue;
                }
                throw std::runtime_error("Accept failed: " +
                                         std::string(std::strerror(errno)));
            }
            std::thread(handleClient, client, std::cref(self), std::cref(ring),
                        std::ref(table))
                .detach();
        }
    } catch (const std::exception& error) {
        std::cerr << "Server startup/runtime error: " << error.what() << '\n';
        return 1;
    }
}
