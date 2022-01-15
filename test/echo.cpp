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
constexpr int port = 10000; 

Task<void>echo()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    utils::set_non_blocking(fd);
    utils::set_reuse_addr(fd);
    sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(
        AF_INET,
        host,
        &sa.sin_addr
    );

    cout << "listen.." << endl;
    bind(fd, (sockaddr*)&sa, sizeof(sockaddr_in));
    listen(fd, 4);

    co_await epoll::readable(fd);
    int conn = accept(fd, nullptr, nullptr);
    utils::set_non_blocking(conn);
    cout << "accept!" << endl;

    char buf[64];
    while(true) {
        co_await epoll::readable(conn);
        int n = recv(conn, buf, 64, 0);
        if(n <= 0) break;

        co_await epoll::writable(conn);
        send(conn, buf, n, 0);
    }

    close(conn);
    close(fd);
}

int main()
{
    epoll::block_on(echo());
    cout << "exit" << endl;
}
