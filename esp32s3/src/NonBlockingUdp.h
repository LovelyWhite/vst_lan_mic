#pragma once

#include <Arduino.h>
#include <IPAddress.h>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <unistd.h>

class NonBlockingUdp {
public:
    struct Packet {
        IPAddress ip;
        uint16_t port = 0;
        int length = 0;
    };

    struct Ready {
        bool read = false;
        bool write = false;
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

    static bool poll(NonBlockingUdp& first, Ready& firstReady,
                     NonBlockingUdp& second, Ready& secondReady) {
        firstReady = {};
        secondReady = {};
        fd_set readFds, writeFds;
        FD_ZERO(&readFds);
        FD_ZERO(&writeFds);

        int maxFd = -1;
        addFd(first.fd_, readFds, writeFds, maxFd);
        addFd(second.fd_, readFds, writeFds, maxFd);
        if (maxFd < 0) return false;

        timeval timeout{0, 0};
        const int result = ::select(maxFd + 1, &readFds, &writeFds, nullptr, &timeout);
        if (result < 0) return false;
        firstReady = readyFor(first.fd_, readFds, writeFds);
        secondReady = readyFor(second.fd_, readFds, writeFds);
        return true;
    }

    int read(const Ready& ready, uint8_t* data, size_t capacity, Packet& packet) {
        packet = {};
        if (fd_ < 0) return -1;
        if (!ready.read) return 0;

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

    SendResult sendTo(Ready& ready, IPAddress ip, uint16_t port,
                      const uint8_t* data, size_t bytes) {
        if (fd_ < 0) return SendResult::Error;
        if (!ready.write) return SendResult::WouldBlock;

        sockaddr_in to{};
        to.sin_family = AF_INET;
        to.sin_port = htons(port);
        to.sin_addr.s_addr = uint32_t(ip);
        const int sent = ::sendto(fd_, data, bytes, 0,
                                  reinterpret_cast<const sockaddr*>(&to),
                                  sizeof(to));
        if (sent == int(bytes)) return SendResult::Sent;
        ready.write = false;
        return (sent < 0 && isWouldBlock(errno)) ? SendResult::WouldBlock
                                                 : SendResult::Error;
    }

private:
    static bool isWouldBlock(int error) {
        return error == EAGAIN || error == EWOULDBLOCK;
    }

    static void addFd(int fd, fd_set& readFds, fd_set& writeFds, int& maxFd) {
        if (fd < 0) return;
        FD_SET(fd, &readFds);
        FD_SET(fd, &writeFds);
        if (fd > maxFd) maxFd = fd;
    }

    static Ready readyFor(int fd, const fd_set& readFds, const fd_set& writeFds) {
        if (fd < 0) return {};
        return {FD_ISSET(fd, &readFds) != 0, FD_ISSET(fd, &writeFds) != 0};
    }

    int fd_ = -1;
};
