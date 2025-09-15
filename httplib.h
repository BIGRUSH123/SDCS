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
#include <mutex>
#include <unordered_map>
#include <memory>

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
        // 默认使用 keep-alive（客户端可以发送 Connection: close）
        if (res.headers.find("Connection") == res.headers.end()) {
            oss << "Connection: keep-alive\r\n";
        }
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

        if (::listen(server_fd, 128) < 0) {
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

            // Set a recv timeout so an idle keep-alive socket won't hang forever
            struct timeval tv;
            tv.tv_sec = 5; // 5s inactivity timeout
            tv.tv_usec = 0;
            setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

            std::thread([this, client_fd]() {
                char buffer[8192];
                std::string pending; // 保存未处理的残余数据（若有）
                bool keep_alive = true;

                while (keep_alive) {
                    ssize_t bytes_read = recv(client_fd, buffer, sizeof(buffer), 0);
                    if (bytes_read <= 0) break;
                    pending.append(buffer, bytes_read);

                    // 解析请求（支持单请求或请求流）
                    while (true) {
                        size_t first_line_end = pending.find("\r\n");
                        if (first_line_end == std::string::npos) break;
                        std::string first_line = pending.substr(0, first_line_end);
                        Request req;
                        parse_request_line(first_line, req);

                        size_t headers_end = pending.find("\r\n\r\n");
                        if (headers_end == std::string::npos) break;
                        std::string headers_str = pending.substr(first_line_end + 2, headers_end - first_line_end - 2);
                        parse_headers(headers_str, req);

                        // 计算 body 长度（如果有 Content-Length）
                        size_t content_length = 0;
                        auto it = req.headers.find("Content-Length");
                        if (it != req.headers.end()) {
                            try { content_length = std::stoul(it->second); } catch(...) { content_length = 0; }
                        }

                        if (pending.size() < headers_end + 4 + content_length) {
                            // body 尚未全部到达
                            break;
                        }

                        req.body = pending.substr(headers_end + 4, content_length);
                        // 移除已处理部分
                        pending = pending.substr(headers_end + 4 + content_length);

                        // 预处理
                        Response res;
                        if (pre_routing_handler) {
                            pre_routing_handler(req, res);
                        }

                        // 路由处理
                        bool handled = false;
                        for (const auto& handler_info : handlers) {
                            if (handler_info.first == req.method) {
                                const std::string& pattern = handler_info.second.first;
                                if (pattern.find_first_of("()[]{}*+?^$|\\") == std::string::npos) {
                                    if (req.path == pattern) {
                                        handler_info.second.second(req, res);
                                        handled = true;
                                        break;
                                    }
                                } else {
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

                        // 若请求头里包含 Connection: close, 那么我们也在响应头写 Connection: close
                        auto conn_it = req.headers.find("Connection");
                        if (conn_it != req.headers.end()) {
                            if (conn_it->second == "close" || conn_it->second == "Close") {
                                res.set_header("Connection", "close");
                                keep_alive = false;
                            }
                        }

                        std::string response_str = create_response(res);
                        send(client_fd, response_str.c_str(), response_str.length(), 0);

                        // 如果没有更多数据并且不设置 keep-alive，则退出
                        if (!keep_alive && pending.empty()) break;
                        // 否则继续循环，看看 pending 是否包含另一个请求
                    } // inner while
                } // outer while

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

    struct Conn {
        int sock = -1;
        std::mutex mtx;
        bool connected = false;
        std::chrono::steady_clock::time_point last_used;
    };

    // 简单连接池：key 为 "<host>:<port>"
    std::unordered_map<std::string, std::shared_ptr<Conn>> conn_map;
    std::mutex conn_map_mutex;

public:
    Client(const std::string& url) {
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
        } else {
            host = url;
            port = 80;
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
    std::shared_ptr<Conn> get_or_create_conn() {
        std::string key = host + ":" + std::to_string(port);
        std::lock_guard<std::mutex> lock(conn_map_mutex);
        auto it = conn_map.find(key);
        if (it != conn_map.end()) return it->second;
        auto conn = std::make_shared<Conn>();
        conn_map[key] = conn;
        return conn;
    }

    bool connect_conn(std::shared_ptr<Conn> conn) {
        if (conn->connected && conn->sock >= 0) return true;
        int sock = socket(AF_INET, SOCK_STREAM, 0);
        if (sock < 0) return false;

        struct sockaddr_in server_addr;
        server_addr.sin_family = AF_INET;
        server_addr.sin_port = htons(port);

        if (inet_pton(AF_INET, host.c_str(), &server_addr.sin_addr) <= 0) {
            struct hostent* he = gethostbyname(host.c_str());
            if (he == nullptr) {
                close(sock);
                return false;
            }
            memcpy(&server_addr.sin_addr, he->h_addr_list[0], he->h_length);
        }

        // 设置超时（connect blocking 为简化处理）
        if (connect(sock, (struct sockaddr*)&server_addr, sizeof(server_addr)) < 0) {
            close(sock);
            return false;
        }

        // 设置 socket 为非阻塞接收超时（用于 read）
        struct timeval tv;
        tv.tv_sec = timeout_sec;
        tv.tv_usec = 0;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, (const char*)&tv, sizeof(tv));

        conn->sock = sock;
        conn->connected = true;
        conn->last_used = std::chrono::steady_clock::now();
        return true;
    }

    // 从 socket 中读取完整 HTTP 响应（基于 Content-Length）
    bool read_http_response(int sock, std::string &out_response) {
        const size_t buf_size = 8192;
        char buffer[buf_size];
        out_response.clear();

        // 先读取头部，直到 \r\n\r\n
        while (true) {
            ssize_t n = recv(sock, buffer, buf_size, 0);
            if (n < 0) {
                return false;
            }
            if (n == 0) return false;
            out_response.append(buffer, n);
            size_t pos = out_response.find("\r\n\r\n");
            if (pos != std::string::npos) break;
            // 继续读取
        }

        size_t header_end = out_response.find("\r\n\r\n");
        if (header_end == std::string::npos) return false;
        std::string headers = out_response.substr(0, header_end);
        size_t content_length = 0;
        // 查找 Content-Length
        std::istringstream iss(headers);
        std::string line;
        while (std::getline(iss, line)) {
            if (line.find("Content-Length:") != std::string::npos) {
                size_t p = line.find(':');
                if (p != std::string::npos) {
                    std::string val = line.substr(p+1);
                    // trim
                    while (!val.empty() && isspace(val.front())) val.erase(0,1);
                    try { content_length = std::stoul(val); } catch(...) { content_length = 0; }
                }
                break;
            }
        }

        size_t already = out_response.size() - (header_end + 4);
        while (already < content_length) {
            ssize_t n = recv(sock, buffer, buf_size, 0);
            if (n <= 0) return false;
            out_response.append(buffer, n);
            already += n;
        }
        return true;
    }

    std::shared_ptr<Result> make_request(const std::string& method, const std::string& path, 
                                        const std::string& body, const std::string& content_type) {
        auto result = std::make_shared<Result>();
        auto conn = get_or_create_conn();

        std::lock_guard<std::mutex> l(conn->mtx);
        if (!connect_conn(conn)) {
            result->status = 0;
            return result;
        }

        std::ostringstream request;
        request << method << " " << path << " HTTP/1.1\r\n";
        request << "Host: " << host << ":" << port << "\r\n";
        request << "Connection: keep-alive\r\n";
        if (!content_type.empty()) {
            request << "Content-Type: " << content_type << "\r\n";
        }
        if (!body.empty()) {
            request << "Content-Length: " << body.length() << "\r\n";
        } else {
            request << "Content-Length: 0\r\n";
        }
        request << "\r\n";
        if (!body.empty()) request << body;

        std::string request_str = request.str();
        ssize_t sentsz = send(conn->sock, request_str.c_str(), request_str.length(), 0);
        if (sentsz < 0) {
            // 发送失败 -> 关闭连接并返回错误
            close(conn->sock);
            conn->sock = -1;
            conn->connected = false;
            result->status = 0;
            return result;
        }

        std::string raw_response;
        if (!read_http_response(conn->sock, raw_response)) {
            // 读失败 -> 关闭连接并返回错误
            close(conn->sock);
            conn->sock = -1;
            conn->connected = false;
            result->status = 0;
            return result;
        }

        // 解析状态码
        size_t status_pos = raw_response.find(' ');
        if (status_pos != std::string::npos) {
            size_t status_end = raw_response.find(' ', status_pos + 1);
            if (status_end != std::string::npos) {
                try {
                    result->status = std::stoi(raw_response.substr(status_pos + 1, status_end - status_pos - 1));
                } catch (...) {
                    result->status = 0;
                }
            }
        }

        size_t body_pos = raw_response.find("\r\n\r\n");
        if (body_pos != std::string::npos) {
            result->body = raw_response.substr(body_pos + 4);
        }

        conn->last_used = std::chrono::steady_clock::now();
        return result;
    }
};

} // namespace httplib

#endif // HTTPLIB_H
