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

Task<int> xconnect()
{
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    utils::set_non_blocking(fd);

    sockaddr_in sa;
    sa.sin_family = AF_INET;
    sa.sin_port = htons(port);
    inet_pton(
        AF_INET,
        host,
        &sa.sin_addr
    );

    cout << "try connect.." << endl;
    connect(fd, (sockaddr*)&sa, sizeof(sockaddr_in));
    co_await epoll::writable(fd);
    cout << "connected!" << endl; // or failed
    
    cout << "send: hello" << endl;
    send(fd, "hello\n", 6, 0);

    char buf[64];
    co_await epoll::readable(fd);
    int n = recv(fd, buf, 64, 0);
    cout << "recv: " << buf;

    close(fd);
    co_return n;
}


int main()
{
    auto n = minio::block_on(xconnect());
    cout << "recv " << n << " bytes" << endl;   
}
