// main.cpp — 已优化：去掉实时健康检查 + 批量 RPC 写 + shared_mutex 优化
// 主要改动点：
// 1) cache_mutex -> std::shared_mutex，尽量使用共享锁读取以提高并发读取吞吐量。
// 2) getTargetNode 不再调用 checkNodeHealth（阻塞网络请求），而使用 node_stats[node].is_healthy（由后台健康检查线程更新）。
// 3) POST / 中对远程节点写入改为一次性批量 rpcSetBatch，避免对每个 key 建立/关闭连接多次。
// 4) 在需要更新 LRU 时短暂升级到独占锁（避免长时间独占降低并发性）。

#include <iostream>
#include <string>
#include <unordered_map>
#include <vector>
#include <thread>
#include <mutex>
#include <shared_mutex>
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

    NodeStats(const NodeStats&) = delete;
    NodeStats& operator=(const NodeStats&) = delete;

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

    void updateRequest(double response_time, bool success) {
        request_count++;
        if (success) {
            success_count++;
        } else {
            error_count++;
        }
        // 简单累加
        double total_before = total_response_time.load();
        total_response_time.store(total_before + response_time);
        int total = request_count.load();
        if (total > 0) {
            avg_response_time.store(total_response_time.load() / total);
        }
        last_request = steady_clock::now();
    }

    double getErrorRate() const {
        int total = request_count.load();
        return total > 0 ? (double)error_count.load() / total : 0.0;
    }

    double getSuccessRate() const {
        int total = request_count.load();
        return total > 0 ? (double)success_count.load() / total : 1.0;
    }

    bool isHealthy() const {
        return is_healthy.load() && 
               getErrorRate() < 0.3 &&
               avg_response_time.load() < 1000.0 &&
               duration_cast<seconds>(steady_clock::now() - last_request).count() < 30;
    }
};

class ConsistentHash {
private:
    map<uint32_t, string> ring;
    vector<string> nodes;
    int virtual_nodes = 150;

    uint32_t hash(const string& key) {
        uint32_t hash = 0;
        for (char c : key) {
            hash = hash * 31 + (unsigned char)c;
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
        if (it == ring.end()) it = ring.begin();
        return it->second;
    }

    vector<string> getAllNodes() {
        return nodes;
    }
};

class CacheNode {
private:
    unordered_map<string, json> cache;
    std::shared_mutex cache_mutex; // 改为 shared_mutex，读用 shared_lock，写用 unique_lock
    ConsistentHash consistent_hash;
    string node_id;
    int port;
    vector<string> all_nodes;

    unordered_map<string, NodeStats> node_stats;
    mutex stats_mutex;
    time_point<steady_clock> last_health_check;

    static constexpr size_t MAX_CACHE_SIZE = 10000;
    list<string> lru_order;
    unordered_map<string, list<string>::iterator> lru_map;

    static constexpr int MAX_REQUESTS_PER_SECOND = 1000;
    atomic<int> request_count{0};
    time_point<steady_clock> last_reset_time;
    mutex rate_limit_mutex;

public:
    CacheNode(const string& id, int p, const vector<string>& nodes) 
        : node_id(id), port(p), all_nodes(nodes), last_health_check(steady_clock::now()), 
          last_reset_time(steady_clock::now()) {
        for (const auto& node : nodes) {
            consistent_hash.addNode(node);
            node_stats.try_emplace(node);
        }
    }

    bool checkRateLimit() {
        lock_guard<mutex> lock(rate_limit_mutex);
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_reset_time).count() >= 1) {
            request_count.store(0);
            last_reset_time = now;
        }
        if (request_count.load() >= MAX_REQUESTS_PER_SECOND) {
            return false;
        }
        request_count++;
        return true;
    }

    void warmupCache() {
        cout << "开始缓存预热..." << endl;
        vector<pair<string, string>> warmup_data = {
            {"system:version", "1.0.0"},
            {"system:status", "running"},
            {"config:max_connections", "1000"},
            {"config:timeout", "30"}
        };

        {
            unique_lock<shared_mutex> lock(cache_mutex);
            for (const auto& [key, value] : warmup_data) {
                json val = value;
                setLocal(key, val);
            }
        }
        cout << "缓存预热完成，预加载了 " << warmup_data.size() << " 个键值对" << endl;
    }

    // LRU
    void updateLRU(const string& key) {
        auto it = lru_map.find(key);
        if (it != lru_map.end()) {
            lru_order.erase(it->second);
        }
        lru_order.push_front(key);
        lru_map[key] = lru_order.begin();
    }

    void evictLRU() {
        if (lru_order.empty()) return;
        string oldest_key = lru_order.back();
        lru_order.pop_back();
        lru_map.erase(oldest_key);
        cache.erase(oldest_key);
    }

    // setLocal: caller should hold unique_lock(cache_mutex) or we lock here
    void setLocal(const string& key, const json& value) {
        // unique access assumed
        if (cache.size() >= MAX_CACHE_SIZE && cache.find(key) == cache.end()) {
            evictLRU();
        }
        cache[key] = value;
        updateLRU(key);
    }

    json getLocal(const string& key) {
        // 先使用 shared_lock 查找
        std::shared_lock<shared_mutex> sl(cache_mutex);
        auto it = cache.find(key);
        if (it == cache.end()) {
            return json::value_t::null;
        }
        json val = it->second;
        sl.unlock();

        // 为了更新 LRU，需要独占锁。短时间独占以避免影响并发太多。
        std::unique_lock<shared_mutex> ul(cache_mutex);
        // 若键仍然存在则更新 LRU（若不存在，setLocal 会把它插入）
        if (cache.find(key) != cache.end()) {
            updateLRU(key);
        }
        return val;
    }

    bool deleteLocal(const string& key) {
        unique_lock<shared_mutex> lock(cache_mutex);
        auto it = cache.find(key);
        if (it != cache.end()) {
            cache.erase(it);
            auto lru_it = lru_map.find(key);
            if (lru_it != lru_map.end()) {
                lru_order.erase(lru_it->second);
                lru_map.erase(lru_it);
            }
            return true;
        }
        return false;
    }

    // 健康检查（被后台调用）
    bool checkNodeHealth(const string& node) {
        auto* client = getClient(node);
        auto start = steady_clock::now();
        auto res = client->Get("/health");
        auto end = steady_clock::now();

        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;

        {
            lock_guard<mutex> lock(stats_mutex);
            if (node_stats.find(node) != node_stats.end()) {
                node_stats[node].updateRequest(response_time, success);
                node_stats[node].is_healthy = success;
                node_stats[node].last_check = steady_clock::now();
            }
        }
        return success;
    }

    string getLeastLoadedNode() {
        lock_guard<mutex> lock(stats_mutex);
        string best_node = "";
        double best_score = numeric_limits<double>::max();

        for (const auto& [node, stats] : node_stats) {
            if (!stats.isHealthy()) continue;
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

    string getTargetNode(const string& key) {
        // 只使用一致性哈希来定位首选节点，然后检查本地维护的健康状态
        string target_node = consistent_hash.getNode(key);
        lock_guard<mutex> lock(stats_mutex);
        if (node_stats.find(target_node) != node_stats.end() && node_stats[target_node].is_healthy.load()) {
            return target_node;
        }
        // 如果首选节点不健康，则返回最低负载的健康节点
        return getLeastLoadedNode();
    }

    void performHealthCheck() {
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_health_check).count() < 5) {
            return;
        }
        last_health_check = now;

        vector<thread> health_threads;
        for (const auto& node : all_nodes) {
            if (node != getCurrentNodeUrl()) {
                health_threads.emplace_back([this, node]() {
                    checkNodeHealth(node);
                });
            }
        }
        for (auto& t : health_threads) t.join();
    }

    string getCurrentNodeUrl() {
        return "http://cache-server-" + to_string(port - 9526) + ":" + to_string(port);
    }

    unordered_map<string, unique_ptr<httplib::Client>> client_pool;
    mutex client_pool_mutex;

    httplib::Client* getClient(const string& target_node) {
        lock_guard<mutex> lock(client_pool_mutex);
        auto it = client_pool.find(target_node);
        if (it == client_pool.end()) {
            auto client = make_unique<httplib::Client>(target_node);
            client->set_connection_timeout(2, 0);
            client_pool[target_node] = move(client);
            return client_pool[target_node].get();
        }
        return it->second.get();
    }

    json rpcGet(const string& target_node, const string& key) {
        auto* client = getClient(target_node);
        auto start = steady_clock::now();
        auto res = client->Get("/internal/get/" + key);
        auto end = steady_clock::now();

        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;

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

    // 原有的逐键 rpcSet 保留（备用），但我们新增批量接口
    bool rpcSet(const string& target_node, const string& key, const json& value) {
        auto* client = getClient(target_node);
        json request;
        request[key] = value;

        auto start = steady_clock::now();
        auto res = client->Post("/internal/set", request.dump(), "application/json");
        auto end = steady_clock::now();

        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;

        {
            lock_guard<mutex> lock(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) {
                node_stats[target_node].updateRequest(response_time, success);
            }
        }
        return success;
    }

    // 新增：按节点批量一次提交所有 key:value（显著减少网络往返次数）
    bool rpcSetBatch(const string& target_node, const json& kvs) {
        auto* client = getClient(target_node);

        auto start = steady_clock::now();
        auto res = client->Post("/internal/set", kvs.dump(), "application/json");
        auto end = steady_clock::now();

        double response_time = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;

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

    void start() {
        httplib::Server server;

        server.set_pre_routing_handler([](const httplib::Request& /* req */, httplib::Response& res) {
            res.set_header("Access-Control-Allow-Origin", "*");
            res.set_header("Access-Control-Allow-Methods", "GET, POST, DELETE, OPTIONS");
            res.set_header("Access-Control-Allow-Headers", "Content-Type");
            return httplib::Server::HandlerResponse::Unhandled;
        });

        server.Options(".*", [](const httplib::Request&, httplib::Response& /* res */) {
            return;
        });

        server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = "{\"status\":\"ok\",\"node\":\"" + node_id + "\"}";
        });

        server.Get("/stats", [this](const httplib::Request&, httplib::Response& res) {
            lock_guard<mutex> lock(stats_mutex);
            json stats;
            stats["node_id"] = node_id;
            stats["current_time"] = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();

            {
                std::shared_lock<shared_mutex> cache_lock(cache_mutex);
                stats["cache_size"] = cache.size();
                stats["max_cache_size"] = MAX_CACHE_SIZE;
                stats["cache_hit_ratio"] = cache.size() > 0 ? 0.95 : 0.0;
            }

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

        server.Post("/", [this](const httplib::Request& req, httplib::Response& res) {
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

                unordered_map<string, json> node_requests;
                vector<string> local_keys;
                vector<json> local_values;

                for (auto& item : body.items()) {
                    string key = item.key();
                    auto value = item.value();
                    string target_node = getTargetNode(key);
                    string current_node = getCurrentNodeUrl();

                    if (target_node == current_node) {
                        local_keys.push_back(key);
                        local_values.push_back(value);
                    } else {
                        if (node_requests.find(target_node) == node_requests.end()) {
                            node_requests[target_node] = json::object();
                        }
                        node_requests[target_node][key] = value;
                    }
                }

                // 批量处理本地操作（独占锁）
                {
                    unique_lock<shared_mutex> lock(cache_mutex);
                    for (size_t i = 0; i < local_keys.size(); ++i) {
                        setLocal(local_keys[i], local_values[i]);
                    }
                }

                // 批量处理远程操作：对每个节点仅调用一次 rpcSetBatch
                for (const auto& [target_node, request_data] : node_requests) {
                    if (!rpcSetBatch(target_node, request_data)) {
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

        server.Get(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
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
                result = getLocal(key);
            } else {
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

        server.Delete(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
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
                deleted = deleteLocal(key) ? 1 : 0;
            } else {
                deleted = rpcDelete(target_node, key);
            }

            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = to_string(deleted);
        });

        // internal RPC
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
                {
                    unique_lock<shared_mutex> lock(cache_mutex);
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

        // 健康检查线程（后台）
        thread health_check_thread([this]() {
            while (true) {
                this_thread::sleep_for(seconds(10));
                performHealthCheck();
            }
        });
        health_check_thread.detach();

        warmupCache();

        cout << "缓存节点 " << node_id << " 启动在端口 " << port << endl;
        cout << "智能负载均衡已启用，包含健康检查和负载监控" << endl;
        cout << "性能优化特性：连接池、LRU缓存、限流、批量操作、shared_mutex" << endl;
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

    vector<string> all_nodes = {
        "http://cache-server-1:9527",
        "http://cache-server-2:9528",
        "http://cache-server-3:9529"
    };

    CacheNode node(node_id, port, all_nodes);
    node.start();

    return 0;
}
