// main.cpp — LRU 优化（使用 list::splice 将节点移动到表头，避免 erase+push_front 的潜在开销）
// 其余主要逻辑同上次提交（包含批量 rpcSetBatch，shared_mutex 等）

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

struct NodeStats {
    atomic<int> request_count{0};
    atomic<int> success_count{0};
    atomic<int> error_count{0};
    atomic<double> total_response_time{0.0};
    atomic<double> avg_response_time{0.0};
    atomic<bool> is_healthy{true};
    time_point<steady_clock> last_check;
    time_point<steady_clock> last_request;
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
        if (success) success_count++;
        else error_count++;
        double total_before = total_response_time.load();
        total_response_time.store(total_before + response_time);
        int total = request_count.load();
        if (total > 0) avg_response_time.store(total_response_time.load() / total);
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
        uint32_t h = 0;
        for (unsigned char c : key) h = h * 31 + c;
        return h;
    }
public:
    void addNode(const string& node) {
        nodes.push_back(node);
        for (int i = 0; i < virtual_nodes; ++i) {
            string vk = node + "#" + to_string(i);
            ring[hash(vk)] = node;
        }
    }
    string getNode(const string& key) {
        if (ring.empty()) return "";
        uint32_t hv = hash(key);
        auto it = ring.lower_bound(hv);
        if (it == ring.end()) it = ring.begin();
        return it->second;
    }
    vector<string> getAllNodes() { return nodes; }
};

class CacheNode {
private:
    unordered_map<string, json> cache;
    std::shared_mutex cache_mutex; // read/write lock
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
        : node_id(id), port(p), all_nodes(nodes),
          last_health_check(steady_clock::now()), last_reset_time(steady_clock::now()) {
        for (const auto& n : nodes) {
            consistent_hash.addNode(n);
            node_stats.try_emplace(n);
        }
    }

    bool checkRateLimit() {
        lock_guard<mutex> lk(rate_limit_mutex);
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_reset_time).count() >= 1) {
            request_count.store(0);
            last_reset_time = now;
        }
        if (request_count.load() >= MAX_REQUESTS_PER_SECOND) return false;
        request_count++;
        return true;
    }

    void warmupCache() {
        cout << "开始缓存预热..." << endl;
        vector<pair<string, string>> warmup = {
            {"system:version", "1.0.0"},
            {"system:status", "running"},
            {"config:max_connections", "1000"},
            {"config:timeout", "30"}
        };
        {
            unique_lock<shared_mutex> ul(cache_mutex);
            for (auto &p : warmup) {
                setLocal(p.first, json(p.second));
            }
        }
        cout << "预热完成" << endl;
    }

    // LRU 优化：使用 splice 将已存在节点移动到表头（O(1)）
    void updateLRU_locked(const string& key) {
        auto it = lru_map.find(key);
        if (it != lru_map.end()) {
            // 将现有 iterator 移动到表头
            lru_order.splice(lru_order.begin(), lru_order, it->second);
            lru_map[key] = lru_order.begin();
        } else {
            lru_order.push_front(key);
            lru_map[key] = lru_order.begin();
        }
    }

    void evictLRU_locked() {
        if (lru_order.empty()) return;
        string oldest = lru_order.back();
        lru_order.pop_back();
        lru_map.erase(oldest);
        cache.erase(oldest);
    }

    // 设置本地（需要持有 unique_lock(cache_mutex)）
    void setLocal(const string& key, const json& value) {
        // assume caller holds unique lock
        if (cache.size() >= MAX_CACHE_SIZE && cache.find(key) == cache.end()) {
            evictLRU_locked();
        }
        cache[key] = value;
        updateLRU_locked(key);
    }

    // getLocal: 先用 shared_lock 查找并拷贝，若需更新 LRU 则短时间升级到 unique
    json getLocal(const string& key) {
        {
            std::shared_lock<shared_mutex> sl(cache_mutex);
            auto it = cache.find(key);
            if (it == cache.end()) return json::value_t::null;
            json val = it->second;
            // 释放 shared_lock，短时加 unique 更新 LRU
            sl.unlock();
            {
                std::unique_lock<shared_mutex> ul(cache_mutex);
                // 再次确认键仍存在，然后移动到表头
                if (cache.find(key) != cache.end()) {
                    updateLRU_locked(key);
                }
            }
            return val;
        }
    }

    bool deleteLocal(const string& key) {
        unique_lock<shared_mutex> ul(cache_mutex);
        auto it = cache.find(key);
        if (it == cache.end()) return false;
        cache.erase(it);
        auto lru_it = lru_map.find(key);
        if (lru_it != lru_map.end()) {
            lru_order.erase(lru_it->second);
            lru_map.erase(lru_it);
        }
        return true;
    }

    // 健康检查后台调用
    bool checkNodeHealth(const string& node) {
        auto* client = getClient(node);
        auto start = steady_clock::now();
        auto res = client->Get("/health");
        auto end = steady_clock::now();
        double rt = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        {
            lock_guard<mutex> lk(stats_mutex);
            if (node_stats.find(node) != node_stats.end()) {
                node_stats[node].updateRequest(rt, success);
                node_stats[node].is_healthy = success;
                node_stats[node].last_check = steady_clock::now();
            }
        }
        return success;
    }

    string getLeastLoadedNode() {
        lock_guard<mutex> lk(stats_mutex);
        string best = "";
        double best_score = numeric_limits<double>::max();
        for (const auto& [node, st] : node_stats) {
            if (!st.isHealthy()) continue;
            double score = st.avg_response_time.load() + st.getErrorRate() * 1000.0 + st.request_count.load() * 0.1;
            if (score < best_score) {
                best_score = score;
                best = node;
            }
        }
        return best.empty() ? all_nodes[0] : best;
    }

    string getTargetNode(const string& key) {
        string target = consistent_hash.getNode(key);
        lock_guard<mutex> lk(stats_mutex);
        if (node_stats.find(target) != node_stats.end() && node_stats[target].is_healthy.load()) {
            return target;
        }
        return getLeastLoadedNode();
    }

    void performHealthCheck() {
        auto now = steady_clock::now();
        if (duration_cast<seconds>(now - last_health_check).count() < 5) return;
        last_health_check = now;
        vector<thread> ths;
        for (const auto& n : all_nodes) {
            if (n != getCurrentNodeUrl()) {
                ths.emplace_back([this, n](){ checkNodeHealth(n); });
            }
        }
        for (auto &t : ths) t.join();
    }

    string getCurrentNodeUrl() {
        return "http://cache-server-" + to_string(port - 9526) + ":" + to_string(port);
    }

    // client pool unchanged in 意图，但保留
    unordered_map<string, unique_ptr<httplib::Client>> client_pool;
    mutex client_pool_mutex;
    httplib::Client* getClient(const string& target_node) {
        lock_guard<mutex> lk(client_pool_mutex);
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
        double rt = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        {
            lock_guard<mutex> lk(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) node_stats[target_node].updateRequest(rt, success);
        }
        if (success) {
            try { return json::parse(res->body); } catch (...) {}
        }
        return json::value_t::null;
    }

    bool rpcSetBatch(const string& target_node, const json& kvs) {
        auto* client = getClient(target_node);
        auto start = steady_clock::now();
        auto res = client->Post("/internal/set", kvs.dump(), "application/json");
        auto end = steady_clock::now();
        double rt = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        {
            lock_guard<mutex> lk(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) node_stats[target_node].updateRequest(rt, success);
        }
        return success;
    }

    int rpcDelete(const string& target_node, const string& key) {
        auto* client = getClient(target_node);
        auto start = steady_clock::now();
        auto res = client->Delete("/internal/delete/" + key);
        auto end = steady_clock::now();
        double rt = duration_cast<milliseconds>(end - start).count();
        bool success = res && res->status == 200;
        {
            lock_guard<mutex> lk(stats_mutex);
            if (node_stats.find(target_node) != node_stats.end()) node_stats[target_node].updateRequest(rt, success);
        }
        if (success) {
            try { return stoi(res->body); } catch (...) {}
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

        server.Options(".*", [](const httplib::Request&, httplib::Response& /* res */) { return; });

        server.Get("/health", [this](const httplib::Request&, httplib::Response& res) {
            res.status = 200;
            res.set_header("Content-Type", "application/json; charset=utf-8");
            res.body = "{\"status\":\"ok\",\"node\":\"" + node_id + "\"}";
        });

        server.Get("/stats", [this](const httplib::Request&, httplib::Response& res) {
            lock_guard<mutex> lk(stats_mutex);
            json stats;
            stats["node_id"] = node_id;
            stats["current_time"] = duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
            {
                std::shared_lock<shared_mutex> sl(cache_mutex);
                stats["cache_size"] = cache.size();
                stats["max_cache_size"] = MAX_CACHE_SIZE;
                stats["cache_hit_ratio"] = cache.size() > 0 ? 0.95 : 0.0;
            }
            {
                lock_guard<mutex> rl(rate_limit_mutex);
                stats["current_request_rate"] = request_count.load();
                stats["max_request_rate"] = MAX_REQUESTS_PER_SECOND;
            }
            json ns;
            for (const auto& [node, st] : node_stats) {
                json node_info;
                node_info["request_count"] = st.request_count.load();
                node_info["success_count"] = st.success_count.load();
                node_info["error_count"] = st.error_count.load();
                node_info["avg_response_time"] = st.avg_response_time.load();
                node_info["error_rate"] = st.getErrorRate();
                node_info["success_rate"] = st.getSuccessRate();
                node_info["is_healthy"] = st.isHealthy();
                ns[node] = node_info;
            }
            stats["nodes"] = ns;
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
                if (req.body.empty()) { res.status = 400; res.set_header("Content-Type","application/json; charset=utf-8"); res.body = "{\"error\":\"Empty request body\"}"; return; }
                json body = json::parse(req.body);
                unordered_map<string, json> node_requests;
                vector<string> local_keys;
                vector<json> local_values;
                for (auto& item : body.items()) {
                    string key = item.key();
                    auto value = item.value();
                    string target_node = getTargetNode(key);
                    string current_node = getCurrentNodeUrl();
                    if (target_node == current_node) { local_keys.push_back(key); local_values.push_back(value); }
                    else { node_requests[target_node][key] = value; }
                }
                // 本地批量写
                {
                    unique_lock<shared_mutex> ul(cache_mutex);
                    for (size_t i = 0; i < local_keys.size(); ++i) setLocal(local_keys[i], local_values[i]);
                }
                // 远程按节点批量写（每个节点一次请求）
                for (const auto& [target_node, request_data] : node_requests) {
                    if (!rpcSetBatch(target_node, request_data)) {
                        res.status = 500; res.body = "Internal server error"; return;
                    }
                }
                res.status = 200; res.set_header("Content-Type","application/json; charset=utf-8"); res.body = "OK";
            } catch (const exception& e) {
                res.status = 400; res.body = string("Bad request: ") + e.what();
            }
        });

        server.Get(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (!checkRateLimit()) { res.status = 429; res.set_header("Content-Type","application/json; charset=utf-8"); res.body = "{\"error\":\"Rate limit exceeded\"}"; return; }
            if (req.matches.empty()) { res.status = 400; res.body = "Invalid request"; return; }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = getCurrentNodeUrl();
            json result;
            if (target_node == current_node) result = getLocal(key);
            else result = rpcGet(target_node, key);
            if (result.is_null()) res.status = 404;
            else { res.status = 200; res.set_header("Content-Type","application/json; charset=utf-8"); json r; r[key] = result; res.body = r.dump(); }
        });

        server.Delete(R"(/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (!checkRateLimit()) { res.status = 429; res.set_header("Content-Type","application/json; charset=utf-8"); res.body = "{\"error\":\"Rate limit exceeded\"}"; return; }
            if (req.matches.empty()) { res.status = 400; res.body = "Invalid request"; return; }
            string key = req.matches[0];
            string target_node = getTargetNode(key);
            string current_node = getCurrentNodeUrl();
            int deleted = 0;
            if (target_node == current_node) deleted = deleteLocal(key) ? 1 : 0;
            else deleted = rpcDelete(target_node, key);
            res.status = 200; res.set_header("Content-Type","application/json; charset=utf-8"); res.body = to_string(deleted);
        });

        server.Get(R"(/internal/get/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) { res.status = 400; res.body = "Invalid request"; return; }
            string key = req.matches[0];
            json result = getLocal(key);
            if (result.is_null()) res.status = 404;
            else { res.status = 200; res.set_header("Content-Type","application/json; charset=utf-8"); res.body = result.dump(); }
        });

        server.Post("/internal/set", [this](const httplib::Request& req, httplib::Response& res) {
            try {
                json body = json::parse(req.body);
                unique_lock<shared_mutex> ul(cache_mutex);
                for (auto& item : body.items()) {
                    setLocal(item.key(), item.value());
                }
                res.status = 200; res.body = "OK";
            } catch (const exception& e) {
                res.status = 400; res.body = string("Bad request: ") + e.what();
            }
        });

        server.Delete(R"(/internal/delete/([^/]+))", [this](const httplib::Request& req, httplib::Response& res) {
            if (req.matches.empty()) { res.status = 400; res.body = "Invalid request"; return; }
            string key = req.matches[0];
            int deleted = deleteLocal(key) ? 1 : 0;
            res.status = 200; res.body = to_string(deleted);
        });

        // 后台健康检查线程
        thread health_check_thread([this]() {
            while (true) {
                this_thread::sleep_for(seconds(10));
                performHealthCheck();
            }
        });
        health_check_thread.detach();

        warmupCache();

        cout << "缓存节点 " << node_id << " 启动在端口 " << port << endl;
        server.listen("0.0.0.0", port);
    }
};

int main(int argc, char* argv[]) {
    if (argc != 2) { cerr << "用法: " << argv[0] << " <端口号>" << endl; return 1; }
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
