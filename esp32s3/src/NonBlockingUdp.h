#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

class NonBlockingUdp {
public:
    struct Packet {
        IPAddress ip;
        uint16_t port = 0;
        int length = 0;
    };

    enum class SendResult {
        Sent,
        WouldBlock,
        Error
    };

    ~NonBlockingUdp() {
        stop();
    }

    bool begin(uint16_t port) {
        stop();
        fd_ = ::socket(AF_INET, SOCK_DGRAM, IPPROTO_IP);
        if (fd_ < 0) return false;

        int yes = 1;
        ::setsockopt(fd_, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));

        const int flags = ::fcntl(fd_, F_GETFL, 0);
        if (flags < 0 || ::fcntl(fd_, F_SETFL, flags | O_NONBLOCK) < 0) {
            stop();
            return false;
        }

        sockaddr_in addr{};
        addr.sin_family = AF_INET;
        addr.sin_port = htons(port);
        addr.sin_addr.s_addr = htonl(INADDR_ANY);
        if (::bind(fd_, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) < 0) {
            stop();
            return false;
        }
        return true;
    }

    void stop() {
        if (fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    // Sockets are O_NONBLOCK, so recvfrom/sendto report readiness themselves
    // via EAGAIN; a per-pass select() would only add latency.
    int read(uint8_t* data, size_t capacity, Packet& packet) {
        packet = {};
        if (fd_ < 0) return -1;

        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        const int got = ::recvfrom(fd_, data, capacity, 0,
                                   reinterpret_cast<sockaddr*>(&from), &fromLen);
        if (got < 0) {
            return (errno == EAGAIN || errno == EWOULDBLOCK) ? 0 : -1;
        }
        packet.ip = IPAddress(from.sin_addr.s_addr);
        packet.port = ntohs(from.sin_port);
        packet.length = got;
        return got;
    }

    SendResult sendTo(IPAddress ip, uint16_t port,
                      const uint8_t* data, size_t bytes) {
        lastError_ = 0;
        if (fd_ < 0) {
            lastError_ = EBADF;
            return SendResult::Error;
        }

        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = uint32_t(ip);
        const int sent = ::sendto(fd_, data, bytes, 0,
                                  reinterpret_cast<const sockaddr*>(&to),
                                  sizeof(to));
        if (sent == int(bytes)) return SendResult::Sent;
        lastError_ = sent < 0 ? errno : EIO;
        return isWouldBlock(lastError_) ? SendResult::WouldBlock
                                        : SendResult::Error;
    }

    int lastError() const {
        return lastError_;
    }

private:
    static bool isWouldBlock(int error) {
        return error == EAGAIN || error == EWOULDBLOCK || error == ENOMEM;
    }

    int fd_ = -1;
    int lastError_ = 0;
};
