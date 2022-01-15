#include "../minio.hpp"
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <vector>

using std::cout;
using std::endl;
using std::vector;

using minio::Task;
namespace utils = minio::utils;
namespace epoll = minio::epoll;
using epoll::Mode::ET;

constexpr const char* host = "127.0.0.1";
constexpr int src_port = 10000; 
constexpr int dst_port = 20000;

int xread(int fd, void *buf, int max)
{
    int n = 0;
    int total = 0;
    while(n < max) {
        n = recv(fd, buf+total, max - total, 0);
        // EWOULDBLOCK or EOF or Error (skip check here..)
        if (n <=0) break;

        total += n;
    }
    return total;
}

int xwrite(int fd, void *buf, int max)
{
    int n = 0;
    int total = 0;
    while(n < max) {
        n = send(fd, buf+total, max - total, 0);
        if (n < 0) break;

        total += n;
    }
    return total;
}

Task<void> copy(int src, int dst)
{
    
    int n = 0;
    vector<char> buffer{};
    buffer.reserve(0x4000);
    char *buf = buffer.data();

    while(true) {
        co_await epoll::readable<ET>(src);
        n = xread(src, buf, 0x4000);
        if (n <= 0) break;

        // actually should flush all data here..
        co_await epoll::writable<ET>(dst);
        xwrite(dst, buf, n);
        if (n < 0) break;
    }

}

Task<void> bidi_copy(int src)
{
    int dst = socket(AF_INET, SOCK_STREAM, 0);
    utils::set_non_blocking(dst);

    sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(dst_port);
    inet_pton(
        AF_INET,
        host,
        &sa.sin_addr
    );

    cout << "try connect.." << endl;
    connect(dst, (sockaddr*)&sa, sizeof(sockaddr_in));
    co_await epoll::writable(dst);
    cout << "connected!" << endl; // or failed

    minio::spawn(copy(src, dst));
    minio::spawn(copy(dst, src));
}

Task<void> proxy()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    utils::set_non_blocking(fd);
    utils::set_reuse_addr(fd);
    sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(src_port);
    inet_pton(
        AF_INET,
        host,
        &sa.sin_addr
    );

    cout << "listen.." << endl;
    bind(fd, (sockaddr*)&sa, sizeof(sockaddr_in));
    listen(fd, 4);

    while(true) {
        co_await epoll::readable(fd);
        int conn = accept(fd, nullptr, nullptr);
        if (conn < 0) break;

        utils::set_non_blocking(conn);
        cout << "accept!" << endl;

        minio::spawn(bidi_copy(conn));
    }

    close(fd);
}

int main()
{
    minio::block_on(proxy());
    cout << "exit" << endl;
}
