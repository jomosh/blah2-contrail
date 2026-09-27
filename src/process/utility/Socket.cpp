#include "Socket.h"
#include <iostream>
#include <algorithm>
#include <sys/socket.h>

asio::io_context Socket::io_context;
const uint32_t Socket::MTU = 1024;
const uint32_t Socket::SEND_TIMEOUT_MS = 200;

Socket::Socket(const std::string& ip, uint16_t port)
    : endpoint(asio::ip::address::from_string(ip), port), socket(io_context) {
    try {
        socket.connect(endpoint);

        // Bound every blocking send so a stalled API/consumer cannot wedge the
        // real-time CPI thread. SO_SNDTIMEO is honoured by the blocking
        // asio::write() path under Linux.
        const struct timeval timeout = {
            static_cast<long>(SEND_TIMEOUT_MS / 1000),
            static_cast<long>((SEND_TIMEOUT_MS % 1000) * 1000)
        };
        ::setsockopt(socket.native_handle(), SOL_SOCKET, SO_SNDTIMEO,
            &timeout, sizeof(timeout));
    } catch (const std::exception& e) {
        std::cerr << "Error connecting to endpoint: " << e.what() << std::endl;
        throw;
    }
}

Socket::~Socket()
{
}

void Socket::sendData(const std::string& data) {
    // Once a peer has stalled, drop this channel's frames rather than paying a
    // timeout (and risking a FIFO overflow upstream) on every CPI.
    if (dead_) {
        return;
    }

    asio::error_code err;

    for (std::size_t i = 0; i < (data.size() + MTU - 1) / MTU; ++i) {
        const std::size_t offset = i * MTU;
        const std::size_t len = std::min<std::size_t>(MTU, data.size() - offset);
        asio::write(socket, asio::buffer(data.data() + offset, len), err);

        if (err) {
            std::cerr << "Error sending data: " << err.message() << std::endl;
            dead_ = true;
            return;
        }
    }

    asio::write(socket, asio::buffer("\n", 1), err);
    if (err) {
        std::cerr << "Error sending frame delimiter: " << err.message() << std::endl;
        dead_ = true;
    }
}