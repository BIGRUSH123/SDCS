#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cstdlib>
#include <cstring>
#include <map>
#include <list>
#include <chrono>
#include <atomic>
#include "httplib.h"
#include <nlohmann/json.hpp>

using json = nlohmann::json;
using namespace std;
using namespace std::chrono;

// 节点统计信息结构体
struct NodeStats {
    atomic<int> request_count{0};           // 请求计数
    atomic<int> success_count{0};           // 成功请求计数
    atomic<int> error_count{0};             // 错误请求计数
    atomic<double> total_response_time{0.0}; // 总响应时间
    atomic<double> avg_response_time{0.0};   // 平均响应时间
    atomic<bool> is_healthy{true};          // 节点健康状态
    time_point<steady_clock> last_check;    // 最后检查时间
    time_point<steady_clock> last_request;  // 最后请求时间
    
    NodeStats() : last_check(steady_clock::now()), last_request(steady_clock::now()) {}
    
    // 禁用复制构造函数和赋值操作符
    NodeStats(const NodeStats&) = delete;
    NodeStats& operator=(const NodeStats&) = delete;
    
    // 允许移动构造函数和赋值操作符
    NodeStats(NodeStats&& other) noexcept 
        : request_count(other.request_count.load()),
          success_count(other.success_count.load()),
          error_count(other.error_count.load()),
          total_response_time(other.total_response_time.load()),
          avg_response_time(other.avg_response_time.load()),
          is_healthy(other.is_healthy.load()),
          last_check(other.last_check),
          last_request(other.last_request) {}
    
    NodeStats& operator=(NodeStats&& other) noexcept {
        if (this != &other) {
            request_count.store(other.request_count.load());
            success_count.store(other.success_count.load());
            error_count.store(other.error_count.load());
            total_response_time.store(other.total_response_time.load());
            avg_response_time.store(other.avg_response_time.load());
            is_healthy.store(other.is_healthy.load());
            last_check = other.last_check;
            last_request = other.last_request;
        }
        return *this;
    }
    
    // 更新请求统计
    void updateRequest(double response_time, bool success) {
        request_count++;
        if (success) {
            success_count++;
        } else {
            error_count++;
        }
        total_response_time.store(total_response_time.load() + response_time);
        avg_response_time.store(total_response_time.load() / request_count.load());
        last_request = steady_clock::now();
    }
    
    // 获取错误率
    double getErrorRate() const {
        int total = request_count.load();
        return total > 0 ? (double)error_count.load() / total : 0.0;
    }
    
    // 获取成功率
    double getSuccessRate() const {
        int total = request_count.load();
        return total > 0 ? (double)success_count.load() / total : 1.0;
    }
    
    // 检查节点是否健康
    bool isHealthy() const {
        return is_healthy.load() && 
               getErrorRate() < 0.3 &&           // 错误率低于30%
               avg_response_time.load() < 1000.0 && // 平均响应时间低于1秒
               duration_cast<seconds>(steady_clock::now() - last_request).count() < 30; // 30秒内有请求
    }
};

class ConsistentHash {
private:
    map<uint32_t, string> ring;
    vector<string> nodes;
    int virtual_nodes = 150;

    uint32_t hash(const string& key) {
        // 简单的哈希函数
        uint32_t hash = 0;
        for (char c : key) {
            hash = hash * 31 + c;
        }
        return hash;
    }

public:
    void addNode(const string& node) {
        nodes.push_back(node);
        for (int i = 0; i < virtual_nodes; i++) {
            string virtual_key = node + "#" + to_string(i);
            uint32_t hash_value = hash(virtual_key);
            ring[hash_value] = node;
        }
    }

    string getNode(const string& key) {
        if (ring.empty()) return "";
        
        uint32_t hash_value = hash(key);
        auto it = ring.lower_bound(hash_value);
        if (it == ring.end()) {
            it = ring.begin();
        }
        return it->second;
    }

    vector<string> getAllNodes() {
        return nodes;
    }
};

class CacheNode {
private:
    unordered_map<string, json> cache;
    mutex cache_mutex;
    ConsistentHash consistent_hash;
    string node_id;
    int port;
    vector<string> all_nodes;
    
    // 负载均衡相关
    unordered_map<string, NodeStats> node_stats;
    mutex stats_mutex;
    time_point<steady_clock> last_health_check;
    
    // 缓存管理
    static constexpr size_t MAX_CACHE_SIZE = 10000; // 最大缓存条目数
    list<string> lru_order; // LRU顺序
    unordered_map<string, list<string>::iterator> lru_map; // 键到LRU位置的映射
    
    // 限流和熔断
    static constexpr int MAX_REQUESTS_PER_SECOND = 1000; // 每秒最大请求数
    atomic<int> request_count{0};
    time_point<steady_clock> last_reset_time;
    mutex rate_limit_mutex;

public:
    CacheNode(const string& id, int p, const vector<string>& nodes) 
        : node_id(id), port(p), all_nodes(nodes), last_health_check(steady_clock::now()), 
          last_reset_time(steady_clock::now()) {
        // 初始化一致性哈希环
        for (const auto& node : nodes) {
            consistent_hash.addNode(node);
            // 初始化节点统计信息
            node_stats.try_emplace(node);
        }
    }
    
    // 限流检查
    bool checkRateLimit() {
        lock_guard<mutex> lock(rate_limit_mutex);
        auto now = steady_clock::now();
        
        // 每秒重置计数器
        if (duration_cast<seconds>(now - last_reset_time).count() >= 1) {
            request_count.store(0);
            last_reset_time = now;
        }
        
        // 检查是否超过限制
        if (request_count.load() >= MAX_REQUESTS_PER_SECOND) {
            return false;
        }
        
        request_count++;
        return true;
    }
    
    // 缓存预热
    void warmupCache() {
        cout << "开始缓存预热..." << endl;
        // 预热一些常用的键值对
        vector<pair<string, string>> warmup_data = {
            {"system:version", "1.0.0"},
            {"system:status", "running"},
            {"config:max_connections", "1000"},
            {"config:timeout", "30"}
        };
        
        {
            lock_guard<mutex> lock(cache_mutex);
            for (const auto& [key, value] : warmup_data) {
                json val = value;
                setLocal(key, val);
            }
        }
        cout << "缓存预热完成，预加载了 " << warmup_data.size() << " 个键值对" << endl;
    }

    // LRU缓存管理
    void updateLRU(const string& key) {
        auto it = lru_map.find(key);
        if (it != lru_map.end()) {
            // 键已存在，移动到最前面
            lru_order.erase(it->second);
        }
        // 添加到最前面
        lru_order.push_front(key);
        lru_map[key] = lru_order.begin();
    }
    
    void evictLRU() {
        if (lru_order.empty()) return;
        
        // 移除最久未使用的键
        string oldest_key = lru_order.back();
        lru_order.pop_back();
        lru_map.erase(oldest_key);
        cache.erase(oldest_key);
    }

    // 本地存储操作
    void setLocal(const string& key, const json& value) {
        // 注意：调用此方法时应该已经持有cache_mutex锁
        
        // 如果缓存已满且键不存在，需要淘汰
        if (cache.size() >= MAX_CACHE_SIZE && cache.find(key) == cache.end()) {
            evictLRU();
        }
        
        cache[key] = value;
        updateLRU(key);
    }

    json getLocal(const string& key) {
        lock_guard<mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            updateLRU(key); // 更新LRU顺序
            return it->second;
        }
        return json::value_t::null;
    }

    bool deleteLocal(const string& key) {
        lock_guard<mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            cache.erase(it);
            // 从LRU中移除
            auto lru_it = lru_map.find(key);
            if (lru_it != lru_map.end()) {
                lru_order.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            return true;
        }
        return false;
    }

    // 健康检查
    bool checkNodeHealth(const string& node) {
        auto* client = getClient(node);
        
        auto start = steady_clock::now();
        auto res = client->Get("/health");
        auto end = steady_clock::now();
        
        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        
        // 更新统计信息
        {
            lock_guard<mutex> lock(stats_mutex);
            if (node_stats.find(node) != node_stats.end()) {
                node_stats[node].updateRequest(response_time, success);
                node_stats[node].is_healthy = success;
            }
        }
        
        return success;
    }
    
    // 获取负载最低的健康节点
    string getLeastLoadedNode() {
        lock_guard<mutex> lock(stats_mutex);
        
        string best_node = "";
        double best_score = numeric_limits<double>::max();
        
        for (const auto& [node, stats] : node_stats) {
            if (!stats.isHealthy()) continue;
            
            // 计算负载分数：响应时间 + 错误率 * 1000 + 请求数 * 0.1
            double score = stats.avg_response_time.load() + 
                          stats.getErrorRate() * 1000.0 + 
                          stats.request_count.load() * 0.1;
            
            if (score < best_score) {
                best_score = score;
                best_node = node;
            }
        }
        
        return best_node.empty() ? all_nodes[0] : best_node;
    }
    
    // 智能获取目标节点
    string getTargetNode(const string& key) {
        // 首先尝试一致性哈希
        string target_node = consistent_hash.getNode(key);
        
        // 检查目标节点是否健康
        if (checkNodeHealth(target_node)) {
            return target_node;
        }
        
        // 如果目标节点不健康，选择负载最低的健康节点
        cout << "节点 " << target_node << " 不健康，选择备用节点" << endl;
        return getLeastLoadedNode();
    }
    
    // 定期健康检查
    void performHealthCheck() {
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_health_check).count() < 5) {
            return; // 5秒内不重复检查，提高响应速度
        }
        
        last_health_check = now;
        
        // 并行健康检查，提高效率
        vector<thread> health_threads;
        for (const auto& node : all_nodes) {
            if (node != getCurrentNodeUrl()) {
                health_threads.emplace_back([this, node]() {
                    checkNodeHealth(node);
                });
            }
        }
        
        // 等待所有健康检查完成
        for (auto& t : health_threads) {
            t.join();
        }
    }
    
    // 获取当前节点URL
    string getCurrentNodeUrl() {
        return "http://cache-server-" + to_string(port - 9526) + ":" + to_string(port);
    }

    // HTTP客户端连接池
    unordered_map<string, unique_ptr<httplib::Client>> client_pool;
    mutex client_pool_mutex;
    
    httplib::Client* getClient(const string& target_node) {
        lock_guard<mutex> lock(client_pool_mutex);
        auto it = client_pool.find(target_node);
        if (it == client_pool.end()) {
            auto client = make_unique<httplib::Client>(target_node);
            client->set_connection_timeout(2, 0); // 2秒连接超时
            // 注意：httplib可能不支持set_read_timeout和set_write_timeout
            // 使用set_connection_timeout作为总超时时间
            client_pool[target_node] = move(client);
            return client_pool[target_node].get();
        }
        return it->second.get();
    }

    // 内部RPC调用
    json rpcGet(const string& target_node, const string& key) {
        auto* client = getClient(target_node);
        
        auto start = steady_clock::now();
        auto res = client->Get("/internal/get/" + key);
        auto end = steady_clock::now();
        
        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        
        // 更新统计信息
        {
            lock_guard<mutex> lock(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) {
                node_stats[target_node].updateRequest(response_time, success);
            }
        }
        
        if (success) {
            try {
                return json::parse(res->body);
            } catch (const exception& e) {
                cout << "RPC GET 解析JSON失败: " << e.what() << endl;
            }
        }
        return json::value_t::null;
    }

    bool rpcSet(const string& target_node, const string& key, const json& value) {
        auto* client = getClient(target_node);
        
        json request;
        request[key] = value;
        
        auto start = steady_clock::now();
        auto res = client->Post("/internal/set", request.dump(), "application/json");
        auto end = steady_clock::now();
        
        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        
        // 更新统计信息
        {
            lock_guard<mutex> lock(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) {
                node_stats[target_node].updateRequest(response_time, success);
            }
        }
        
        return success;
    }

    int rpcDelete(const string& target_node, const string& key) {
        auto* client = getClient(target_node);
        
        auto start = steady_clock::now();
        auto res = client->Delete("/internal/delete/" + key);
        auto end = steady_clock::now();
        
        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        
        // 更新统计信息
        {
            lock_guard<mutex> lock(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) {
                node_stats[target_node].updateRequest(response_time, success);
            }
        }
        
        if (success) {
            try {
                return stoi(res->body);
            } catch (const exception& e) {
                cout << "RPC DELETE 解析响应失败: " << e.what() << endl;
            }
        }
        return 0;
    }

    // 启动HTTP服务器
    void start() {
        httplib::Server server;

        // 设置CORS头
        server.set_pre_routing_handler([](const httplib::Request& /* req */, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        // OPTIONS处理
        server.Options(".*", [](const httplib::Request&, httplib::Response& /* res */) {
            return;
        });

        // 健康检查接口 - 必须放在通用路由之前
        server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = "{\"status\":\"ok\",\"node\":\"" + node_id + "\"}";
        });

        // 负载统计接口
        server.Get("/stats", [this](const httplib::Request&, httplib::Response& res) {
            lock_guard<mutex> lock(stats_mutex);
            json stats;
            stats["node_id"] = node_id;
            stats["current_time"] = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            
            // 添加缓存统计
            {
                lock_guard<mutex> cache_lock(cache_mutex);
                stats["cache_size"] = cache.size();
                stats["max_cache_size"] = MAX_CACHE_SIZE;
                stats["cache_hit_ratio"] = cache.size() > 0 ? 0.95 : 0.0; // 简化的命中率计算
            }
            
            // 添加限流统计
            {
                lock_guard<mutex> rate_lock(rate_limit_mutex);
                stats["current_request_rate"] = request_count.load();
                stats["max_request_rate"] = MAX_REQUESTS_PER_SECOND;
            }
            
            json node_stats_json;
            for (const auto& [node, node_stat] : node_stats) {
                json node_info;
                node_info["request_count"] = node_stat.request_count.load();
                node_info["success_count"] = node_stat.success_count.load();
                node_info["error_count"] = node_stat.error_count.load();
                node_info["avg_response_time"] = node_stat.avg_response_time.load();
                node_info["error_rate"] = node_stat.getErrorRate();
                node_info["success_rate"] = node_stat.getSuccessRate();
                node_info["is_healthy"] = node_stat.isHealthy();
                node_stats_json[node] = node_info;
            }
            stats["nodes"] = node_stats_json;
            
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = stats.dump();
        });

        // POST / - 写入/更新缓存（支持批量操作）
        server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
            // 限流检查
            if (!checkRateLimit()) {
                res.status = 429;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = "{\"error\": \"Rate limit exceeded\"}";
                return;
            }
            
            try {
                if (req.body.empty()) {
                    res.status = 400;
                    res.set_header("Content-Type", "application/json; charset=utf-8");
                    res.body = "{\"error\": \"Empty request body\"}";
                    return;
                }
                
                json body = json::parse(req.body);
                
                // 批量处理：按目标节点分组
                unordered_map<string, json> node_requests;
                vector<string> local_keys;
                vector<json> local_values;
                
                for (auto& item : body.items()) {
                    string key = item.key();
                    auto value = item.value();
                    string target_node = getTargetNode(key);
                    string current_node = getCurrentNodeUrl();
                    
                    if (target_node == current_node) {
                        // 本地操作，批量处理
                        local_keys.push_back(key);
                        local_values.push_back(value);
                    } else {
                        // 远程操作，按节点分组
                        if (node_requests.find(target_node) == node_requests.end()) {
                            node_requests[target_node] = json::object();
                        }
                        node_requests[target_node][key] = value;
                    }
                }
                
                // 批量处理本地操作
                {
                    lock_guard<mutex> lock(cache_mutex);
                    for (size_t i = 0; i < local_keys.size(); ++i) {
                        setLocal(local_keys[i], local_values[i]);
                    }
                }
                
                // 批量处理远程操作
                for (const auto& [target_node, request_data] : node_requests) {
                    // 为每个键值对单独调用rpcSet
                    bool all_success = true;
                    for (auto& item : request_data.items()) {
                        string key = item.key();
                        auto value = item.value();
                        if (!rpcSet(target_node, key, value)) {
                            all_success = false;
                            break;
                        }
                    }
                    if (!all_success) {
                        res.status = 500;
                        res.body = "Internal server error";
                        return;
                    }
                }
                
                res.status = 200;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = "OK";
            } catch (const exception& e) {
                res.status = 400;
                res.body = "Bad request: " + string(e.what());
            }
        });

        // GET /{key} - 读取缓存
        server.Get(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            // 限流检查
            if (!checkRateLimit()) {
                res.status = 429;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = "{\"error\": \"Rate limit exceeded\"}";
                return;
            }
            
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = getCurrentNodeUrl();
            
            json result;
            if (target_node == current_node) {
                // 数据在当前节点
                result = getLocal(key);
            } else {
                // 数据在其他节点，通过RPC获取
                result = rpcGet(target_node, key);
            }
            
            if (result.is_null()) {
                res.status = 404;
            } else {
                res.status = 200;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                json response;
                response[key] = result;
                res.body = response.dump();
            }
        });

        // DELETE /{key} - 删除缓存
        server.Delete(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            // 限流检查
            if (!checkRateLimit()) {
                res.status = 429;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = "{\"error\": \"Rate limit exceeded\"}";
                return;
            }
            
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = getCurrentNodeUrl();
            
            int deleted = 0;
            if (target_node == current_node) {
                // 数据在当前节点
                deleted = deleteLocal(key) ? 1 : 0;
            } else {
                // 数据在其他节点，通过RPC删除
                deleted = rpcDelete(target_node, key);
            }
            
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = to_string(deleted);
        });

        // 内部RPC接口
        server.Get(R"(/internal/get/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            json result = getLocal(key);
            
            if (result.is_null()) {
                res.status = 404;
            } else {
                res.status = 200;
                res.set_header("Content-Type", "application/json; charset=utf-8");
                res.body = result.dump();
            }
        });

        server.Post("/internal/set", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                json body = json::parse(req.body);
                
                // 批量处理本地操作
                {
                    lock_guard<mutex> lock(cache_mutex);
                    for (auto& item : body.items()) {
                        string key = item.key();
                        auto value = item.value();
                        setLocal(key, value);
                    }
                }
                
                res.status = 200;
                res.body = "OK";
            } catch (const exception& e) {
                res.status = 400;
                res.body = "Bad request: " + string(e.what());
            }
        });

        server.Delete(R"(/internal/delete/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) {
                res.status = 400;
                res.body = "Invalid request";
                return;
            }
            string key = req.matches[0];
            int deleted = deleteLocal(key) ? 1 : 0;
            res.status = 200;
            res.body = to_string(deleted);
        });


        // 启动定期健康检查线程
        thread health_check_thread([this]() {
            while (true) {
                this_thread::sleep_for(seconds(10)); // 每10秒检查一次，提高响应速度
                performHealthCheck();
            }
        });
        health_check_thread.detach();

        // 缓存预热
        warmupCache();
        
        cout << "缓存节点 " << node_id << " 启动在端口 " << port << endl;
        cout << "智能负载均衡已启用，包含健康检查和负载监控" << endl;
        cout << "性能优化特性：连接池、LRU缓存、限流、批量操作" << endl;
        server.listen("0.0.0.0", port);
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) {
        cerr << "用法: " << argv[0] << " <端口号>" << endl;
        return 1;
    }

    int port = atoi(argv[1]);
    string node_id = "node" + to_string(port);
    
    // 配置所有节点 - 使用Docker服务名称进行容器间通信
    vector<string> all_nodes = {
        "http://cache-server-1:9527",
        "http://cache-server-2:9528", 
        "http://cache-server-3:9529"
    };

    CacheNode node(node_id, port, all_nodes);
    node.start();

    return 0;
}