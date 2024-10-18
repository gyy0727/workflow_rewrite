#include <iostream>
#include <string>
#include <sys/socket.h>
#include <arpa/inet.h>
#include <unistd.h>

const int PORT = 8080; // 使用的端口

std::string createHttpResponse() {
    return "HTTP/1.1 200 OK\r\n"
           "Content-Type: text/html\r\n"
           "Content-Length: 37\r\n"
           "\r\n"
           "<html><body>Hello World!</body></html>";
}

int main() {
    int server_socket;
    struct sockaddr_in server_address;
    socklen_t addrlen = sizeof(server_address);

    // 创建套接字
    server_socket = socket(AF_INET, SOCK_STREAM, 0);
    if (server_socket == -1) {
        std::cerr << "Socket creation failed." << std::endl;
        return -1;
    }

    // 设置地址重用选项
    int optval = 1;
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    // 绑定套接字到本地地址和端口
    server_address.sin_family = AF_INET;
    server_address.sin_addr.s_addr = INADDR_ANY;
    server_address.sin_port = htons(PORT);

    if (bind(server_socket, (struct sockaddr *)&server_address, sizeof(server_address)) == -1) {
        std::cerr << "Bind failed." << std::endl;
        close(server_socket);
        return -1;
    }

    // 监听连接
    if (listen(server_socket, 5) == -1) {
        std::cerr << "Listen failed." << std::endl;
        close(server_socket);
        return -1;
    }

    std::cout << "Server listening on port " << PORT << std::endl;

    // 主循环，接受客户端连接
    while (true) {
        int client_socket = accept(server_socket, (struct sockaddr *)&server_address, &addrlen);
        if (client_socket == -1) {
            std::cerr << "Accept failed." << std::endl;
            continue;
        }

        // 发送响应给客户端
        std::string response = createHttpResponse();
        send(client_socket, response.c_str(), response.size(), 0);

        // 关闭客户端套接字
        close(client_socket);
    }

    // 关闭服务器套接字
    close(server_socket);

    return 0;
}