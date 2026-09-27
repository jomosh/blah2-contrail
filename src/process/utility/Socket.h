/// @file Socket.h
/// @class Socket
/// @brief A class to implement network socket functionality.
/// @details Used to pass radar data from app to the API.
/// @author 30hours

#ifndef SOCKET_H
#define SOCKET_H

#include <string>
#include <cstdint>
#include <asio.hpp>

class Socket 
{
private:
    /// @brief Common io_context for all socket objects.
    static asio::io_context io_context;

    /// @brief Common MTU size for all socket objects.
    static const uint32_t MTU;

    /// @brief Send deadline (ms) applied to each blocking socket send.
    /// @details Prevents a stalled API/consumer from wedging the real-time
    /// CPI thread indefinitely.
    static const uint32_t SEND_TIMEOUT_MS;

    /// @brief The ASIO endpoint.
    asio::ip::tcp::endpoint endpoint;

    /// @brief The ASIO socket.
    asio::ip::tcp::socket socket;

    /// @brief True once a send has failed/timed out; later sends are skipped.
    /// @details Bounds the cost of a hung peer to a single timeout rather than
    /// repeated blocking on every CPI.
    bool dead_ = false;

public:
    /// @brief Constructor for Socket.
    /// @param ip IP address of data destination.
    /// @param port Port of data destination.
    /// @return The object.
    Socket(const std::string& ip, uint16_t port);

    /// @brief Destructor.
    /// @return Void.
    ~Socket();

    /// @brief Helper function to send data in chunks.
    /// @param data String of complete data to send.
    /// @return Void.
    void sendData(const std::string& data);

};

#endif
