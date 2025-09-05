#ifndef HTTPLIB_H
#define HTTPLIB_H

#include <string>
#include <map>
#include <vector>
#include <functional>
#include <regex>
#include <thread>
#include <chrono>
#include <sstream>
#include <iostream>
#include <cstring>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <fcntl.h>
#include <netdb.h>

namespace httplib {

struct Request {
    std::string method;
    std::string path;
    std::map<std::string, std::string> headers;
    std::string body;
    std::vector<std::string> matches;
};

struct Response {
    int status = 200;
    std::map<std::string, std::string> headers;
    std::string body;
    
    void set_header(const std::string& key, const std::string& value) {
        headers[key] = value;
    }
};

class Server {
public:
    enum class HandlerResponse {
        Handled,
        Unhandled
    };

    using Handler = std::function<void(const Request&, Response&)>;
    using PreRoutingHandler = std::function<HandlerResponse(const Request&, Response&)>;

private:
    std::vector<std::pair<std::string, std::pair<std::string, Handler>>> handlers;
    PreRoutingHandler pre_routing_handler;

    std::string parse_request_line(const std::string& line, Request& req) {
        std::istringstream iss(line);
        std::string version;
        iss >> req.method >> req.path >> version;
        return req.path;
    }

    void parse_headers(const std::string& headers_str, Request& req) {
        std::istringstream iss(headers_str);
        std::string line;
        while (std::getline(iss, line) && !line.empty() && line != "\r") {
            size_t pos = line.find(':');
            if (pos != std::string::npos) {
                std::string key = line.substr(0, pos);
                std::string value = line.substr(pos + 1);
                // 去除前后空格
                while (!key.empty() && isspace(key.back())) key.pop_back();
                while (!value.empty() && isspace(value.front())) value.erase(0, 1);
                while (!value.empty() && isspace(value.back())) value.pop_back();
                req.headers[key] = value;
            }
        }
    }

    std::string create_response(const Response& res) {
        std::ostringstream oss;
        oss << "HTTP/1.1 " << res.status << " ";
        
        switch (res.status) {
            case 200: oss << "OK"; break;
            case 404: oss << "Not Found"; break;
            case 400: oss << "Bad Request"; break;
            case 500: oss << "Internal Server Error"; break;
            default: oss << "Unknown"; break;
        }
        oss << "\r\n";

        for (const auto& header : res.headers) {
            oss << header.first << ": " << header.second << "\r\n";
        }
        
        oss << "Content-Length: " << res.body.length() << "\r\n";
        oss << "\r\n";
        oss << res.body;
        
        return oss.str();
    }

public:
    void set_pre_routing_handler(PreRoutingHandler handler) {
        pre_routing_handler = handler;
    }

    void Get(const std::string& pattern, Handler handler) {
        handlers.push_back({"GET", {pattern, handler}});
    }

    void Post(const std::string& pattern, Handler handler) {
        handlers.push_back({"POST", {pattern, handler}});
    }

    void Delete(const std::string& pattern, Handler handler) {
        handlers.push_back({"DELETE", {pattern, handler}});
    }

    void Options(const std::string& pattern, Handler handler) {
        handlers.push_back({"OPTIONS", {pattern, handler}});
    }

    bool listen(const std::string& host, int port) {
        int server_fd = socket(AF_INET, SOCK_STREAM, 0);
        if (server_fd < 0) {
            std::cerr << "Socket creation failed" << std::endl;
            return false;
        }

        int opt = 1;
        setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

        struct sockaddr_in address;
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = INADDR_ANY;
        address.sin_port = htons(port);

        if (bind(server_fd, (struct sockaddr*)&address, sizeof(address)) < 0) {
            std::cerr << "Bind failed on port " << port << std::endl;
            close(server_fd);
            return false;
        }

        if (::listen(server_fd, 10) < 0) {
            std::cerr << "Listen failed" << std::endl;
            close(server_fd);
            return false;
        }

        std::cout << "Server listening on " << host << ":" << port << std::endl;

        while (true) {
            struct sockaddr_in client_addr;
            socklen_t client_len = sizeof(client_addr);
            int client_fd = accept(server_fd, (struct sockaddr*)&client_addr, &client_len);
            
            if (client_fd < 0) {
                continue;
            }

            std::thread([this, client_fd]() {
                char buffer[4096] = {0};
                ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer) - 1, 0);
                
                if (bytes_read > 0) {
                    std::string request_str(buffer, bytes_read);
                    
                    Request req;
                    Response res;
                    
                    // 解析请求
                    size_t first_line_end = request_str.find("\r\n");
                    if (first_line_end != std::string::npos) {
                        parse_request_line(request_str.substr(0, first_line_end), req);
                        
                        size_t headers_end = request_str.find("\r\n\r\n");
                        if (headers_end != std::string::npos) {
                            parse_headers(request_str.substr(first_line_end + 2, headers_end - first_line_end - 2), req);
                            req.body = request_str.substr(headers_end + 4);
                        }
                    }

                    // 预处理
                    if (pre_routing_handler) {
                        pre_routing_handler(req, res);
                    }

                    // 路由处理
                    bool handled = false;
                    for (const auto& handler_info : handlers) {
                        if (handler_info.first == req.method) {
                            const std::string& pattern = handler_info.second.first;
                            
                            // 检查是否是精确匹配（不包含正则表达式特殊字符）
                            if (pattern.find_first_of("()[]{}*+?^$|\\") == std::string::npos) {
                                // 精确字符串匹配
                                if (req.path == pattern) {
                                    handler_info.second.second(req, res);
                                    handled = true;
                                    break;
                                }
                            } else {
                                // 正则表达式匹配
                                std::regex pattern_regex(pattern);
                                std::smatch matches;
                                if (std::regex_match(req.path, matches, pattern_regex)) {
                                    for (size_t i = 1; i < matches.size(); ++i) {
                                        req.matches.push_back(matches[i].str());
                                    }
                                    handler_info.second.second(req, res);
                                    handled = true;
                                    break;
                                }
                            }
                        }
                    }

                    if (!handled) {
                        res.status = 404;
                        res.body = "Not Found";
                    }

                    std::string response_str = create_response(res);
                    send(client_fd, response_str.c_str(), response_str.length(), 0);
                }
                
                close(client_fd);
            }).detach();
        }

        close(server_fd);
        return true;
    }
};

class Client {
private:
    std::string host;
    int port;
    int timeout_sec;

public:
    Client(const std::string& url) {
        // 简单的URL解析
        if (url.substr(0, 7) == "http://") {
            std::string addr = url.substr(7);
            size_t colon_pos = addr.find(':');
            if (colon_pos != std::string::npos) {
                host = addr.substr(0, colon_pos);
                port = std::stoi(addr.substr(colon_pos + 1));
            } else {
                host = addr;
                port = 80;
            }
        }
        timeout_sec = 30;
    }

    void set_connection_timeout(int sec, int /* usec */) {
        timeout_sec = sec;
    }

    struct Result {
        int status;
        std::string body;
        operator bool() const { return status > 0; }
    };

    std::shared_ptr<Result> Get(const std::string& path) {
        return make_request("GET", path, "", "");
    }

    std::shared_ptr<Result> Post(const std::string& path, const std::string& body, const std::string& content_type) {
        return make_request("POST", path, body, content_type);
    }

    std::shared_ptr<Result> Delete(const std::string& path) {
        return make_request("DELETE", path, "", "");
    }

private:
    std::shared_ptr<Result> make_request(const std::string& method, const std::string& path, 
                                        const std::string& body, const std::string& content_type) {
        auto result = std::make_shared<Result>();
        
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) {
            result->status = 0;
            return result;
        }

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);
        
        // 尝试直接解析IP地址
        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
            // 如果不是IP地址，尝试解析主机名
            struct hostent* he = gethostbyname(host.c_str());
            if (he == nullptr) {
                close(sock);
                result->status = 0;
                return result;
            }
            memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
        }

        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sock);
            result->status = 0;
            return result;
        }

        std::ostringstream request;
        request << method << " " << path << " HTTP/1.1\r\n";
        request << "Host: " << host << ":" << port << "\r\n";
        if (!content_type.empty()) {
            request << "Content-Type: " << content_type << "\r\n";
        }
        if (!body.empty()) {
            request << "Content-Length: " << body.length() << "\r\n";
        }
        request << "Connection: close\r\n";
        request << "\r\n";
        if (!body.empty()) {
            request << body;
        }

        std::string request_str = request.str();
        if (send(sock, request_str.c_str(), request_str.length(), 0) < 0) {
            close(sock);
            result->status = 0;
            return result;
        }

        char buffer[4096];
        std::string response;
        ssize_t bytes_received;
        while ((bytes_received = recv(sock, buffer, sizeof(buffer), 0)) > 0) {
            response.append(buffer, bytes_received);
        }

        close(sock);

        if (!response.empty()) {
            size_t status_pos = response.find(' ');
            if (status_pos != std::string::npos) {
                size_t status_end = response.find(' ', status_pos + 1);
                if (status_end != std::string::npos) {
                    result->status = std::stoi(response.substr(status_pos + 1, status_end - status_pos - 1));
                }
            }

            size_t body_pos = response.find("\r\n\r\n");
            if (body_pos != std::string::npos) {
                result->body = response.substr(body_pos + 4);
            }
        }

        return result;
    }
};

} // namespace httplib

#endif // HTTPLIB_H
