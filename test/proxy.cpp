#include "../minio.hpp"
#include <iostream>
#include <sys/socket.h>
#include <arpa/inet.h>

using std::cout;
using std::endl;

using minio::Task;
namespace utils = minio::utils;
namespace epoll = minio::epoll;

constexpr const char* host = "127.0.0.1";
constexpr int src_port = 10000; 
constexpr int dst_port = 20000;


Task<void> copy(int src, int dst)
{
    char buf[4096];
    int n = 0;

    while(true) {
        co_await epoll::readable(src);
        n = recv(src, buf, 4096, 0);
        if (n <= 0) break;

        co_await epoll::writable(dst);
        send(dst, buf, n, 0);
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
    epoll::block_on(proxy());
    cout << "exit" << endl;
}
