#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <cctype>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <map>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif
using Socket = SOCKET;
static const Socket INVALID_SOCKET_VALUE = INVALID_SOCKET;
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
using Socket = int;
static const Socket INVALID_SOCKET_VALUE = -1;
#endif

namespace fs = std::filesystem;

struct Config {
    int port = 8888;
    int worker_threads = 4;
    int max_connections = 32;
    fs::path root = "webroot";
    std::string python = "python";
};

struct Request {
    std::string method;
    std::string target;
    std::string path;
    std::string query;
    std::string version;
    std::map<std::string, std::string> headers;
    std::string body;
    std::string client_ip;
};

struct Response {
    int status = 200;
    std::string reason = "OK";
    std::string content_type = "text/html; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
};

std::atomic<bool> g_running{true};

std::string lower_copy(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    return s;
}

std::string trim(const std::string &s) {
    size_t first = 0;
    while (first < s.size() && std::isspace(static_cast<unsigned char>(s[first]))) first++;
    size_t last = s.size();
    while (last > first && std::isspace(static_cast<unsigned char>(s[last - 1]))) last--;
    return s.substr(first, last - first);
}

std::string status_reason(int status) {
    switch (status) {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 403: return "Forbidden";
        case 404: return "Not Found";
        case 500: return "Internal Server Error";
        case 501: return "Not Implemented";
        default: return "Error";
    }
}

void close_socket(Socket s) {
    if (s == INVALID_SOCKET_VALUE) return;
#ifdef _WIN32
    closesocket(s);
#else
    close(s);
#endif
}

bool send_all(Socket s, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
#ifdef _WIN32
        int n = send(s, data.data() + sent, static_cast<int>(data.size() - sent), 0);
#else
        ssize_t n = send(s, data.data() + sent, data.size() - sent, 0);
#endif
        if (n <= 0) return false;
        sent += static_cast<size_t>(n);
    }
    return true;
}

std::string http_date() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%a, %d %b %Y %H:%M:%S GMT");
    return oss.str();
}

std::string log_date() {
    auto now = std::chrono::system_clock::now();
    std::time_t t = std::chrono::system_clock::to_time_t(now);
    std::tm tm{};
#ifdef _WIN32
    localtime_s(&tm, &t);
#else
    localtime_r(&t, &tm);
#endif
    std::ostringstream oss;
    oss << std::put_time(&tm, "%d/%b/%Y:%H:%M:%S %z");
    return oss.str();
}

std::string url_decode(const std::string &input) {
    std::string out;
    for (size_t i = 0; i < input.size(); ++i) {
        if (input[i] == '%' && i + 2 < input.size()) {
            std::string hex = input.substr(i + 1, 2);
            char *end = nullptr;
            long value = std::strtol(hex.c_str(), &end, 16);
            if (end && *end == '\0') {
                out.push_back(static_cast<char>(value));
                i += 2;
            } else {
                out.push_back(input[i]);
            }
        } else if (input[i] == '+') {
            out.push_back(' ');
        } else {
            out.push_back(input[i]);
        }
    }
    return out;
}

bool sanitize_path(const std::string &raw_path, fs::path &relative) {
    std::string path = raw_path.empty() ? "/" : raw_path;
    std::replace(path.begin(), path.end(), '\\', '/');
    if (path.empty() || path[0] != '/') return false;
    std::stringstream ss(path);
    std::string part;
    fs::path result;
    while (std::getline(ss, part, '/')) {
        if (part.empty() || part == ".") continue;
        if (part == "..") return false;
        result /= part;
    }
    if (result.empty()) result = "index.html";
    relative = result;
    return true;
}

std::string mime_type(const fs::path &path) {
    std::string ext = lower_copy(path.extension().string());
    if (ext == ".html" || ext == ".htm") return "text/html; charset=utf-8";
    if (ext == ".css") return "text/css; charset=utf-8";
    if (ext == ".js") return "application/javascript; charset=utf-8";
    if (ext == ".json") return "application/json; charset=utf-8";
    if (ext == ".png") return "image/png";
    if (ext == ".jpg" || ext == ".jpeg") return "image/jpeg";
    if (ext == ".gif") return "image/gif";
    if (ext == ".svg") return "image/svg+xml";
    if (ext == ".txt") return "text/plain; charset=utf-8";
    return "application/octet-stream";
}

bool read_http_request(Socket s, std::string &buffer, Request &req) {
    const size_t max_header_size = 64 * 1024;
    char temp[4096];
    size_t header_end = std::string::npos;

    while ((header_end = buffer.find("\r\n\r\n")) == std::string::npos) {
        if (buffer.size() > max_header_size) return false;
#ifdef _WIN32
        int n = recv(s, temp, sizeof(temp), 0);
#else
        ssize_t n = recv(s, temp, sizeof(temp), 0);
#endif
        if (n <= 0) return false;
        buffer.append(temp, temp + n);
    }

    std::string header_text = buffer.substr(0, header_end);
    std::string rest = buffer.substr(header_end + 4);
    std::istringstream stream(header_text);
    std::string line;
    if (!std::getline(stream, line)) return false;
    if (!line.empty() && line.back() == '\r') line.pop_back();

    std::istringstream first(line);
    if (!(first >> req.method >> req.target >> req.version)) return false;
    req.headers.clear();
    req.body.clear();

    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = lower_copy(trim(line.substr(0, colon)));
        std::string value = trim(line.substr(colon + 1));
        req.headers[key] = value;
    }

    size_t qmark = req.target.find('?');
    req.path = url_decode(qmark == std::string::npos ? req.target : req.target.substr(0, qmark));
    req.query = qmark == std::string::npos ? "" : req.target.substr(qmark + 1);

    size_t content_length = 0;
    auto it = req.headers.find("content-length");
    if (it != req.headers.end()) {
        try {
            content_length = static_cast<size_t>(std::stoul(it->second));
        } catch (...) {
            return false;
        }
    }

    while (rest.size() < content_length) {
#ifdef _WIN32
        int n = recv(s, temp, sizeof(temp), 0);
#else
        ssize_t n = recv(s, temp, sizeof(temp), 0);
#endif
        if (n <= 0) return false;
        rest.append(temp, temp + n);
    }
    req.body = rest.substr(0, content_length);
    buffer = rest.substr(content_length);
    return true;
}

std::string read_file_binary(const fs::path &path) {
    std::ifstream file(path, std::ios::binary);
    std::ostringstream ss;
    ss << file.rdbuf();
    return ss.str();
}

Response error_response(int status, const Config &config) {
    Response res;
    res.status = status;
    res.reason = status_reason(status);
    res.content_type = "text/html; charset=utf-8";

    if (status == 404) {
        fs::path p = config.root / "404.html";
        if (fs::exists(p) && fs::is_regular_file(p)) {
            res.body = read_file_binary(p);
            return res;
        }
    }

    res.body = "<html><body><h1>" + std::to_string(status) + " " + res.reason + "</h1></body></html>";
    return res;
}

struct CgiOutput {
    int exit_code = 0;
    std::string output;
    std::string error;
};

#ifdef _WIN32
bool env_key_equal(const std::string &entry, const std::string &key) {
    auto pos = entry.find('=');
    if (pos == std::string::npos) return false;
    return lower_copy(entry.substr(0, pos)) == lower_copy(key);
}

std::vector<char> build_windows_environment(const std::map<std::string, std::string> &extra) {
    std::vector<std::string> entries;
    LPCH env = GetEnvironmentStringsA();
    if (env) {
        for (LPCH p = env; *p; p += std::strlen(p) + 1) {
            std::string entry = p;
            bool replaced = false;
            for (const auto &kv : extra) {
                if (env_key_equal(entry, kv.first)) {
                    replaced = true;
                    break;
                }
            }
            if (!replaced) entries.push_back(entry);
        }
        FreeEnvironmentStringsA(env);
    }
    for (const auto &kv : extra) entries.push_back(kv.first + "=" + kv.second);
    std::sort(entries.begin(), entries.end(), [](const std::string &a, const std::string &b) {
        return lower_copy(a) < lower_copy(b);
    });
    std::vector<char> block;
    for (const auto &entry : entries) {
        block.insert(block.end(), entry.begin(), entry.end());
        block.push_back('\0');
    }
    block.push_back('\0');
    return block;
}

std::string quote_arg(const std::string &arg) {
    std::string out = "\"";
    for (char c : arg) {
        if (c == '"') out += "\\\"";
        else out += c;
    }
    out += "\"";
    return out;
}

CgiOutput run_process(const std::vector<std::string> &args, const std::string &stdin_data,
                      const std::map<std::string, std::string> &env) {
    CgiOutput result;
    SECURITY_ATTRIBUTES sa{};
    sa.nLength = sizeof(sa);
    sa.bInheritHandle = TRUE;

    HANDLE child_stdout_read = nullptr, child_stdout_write = nullptr;
    HANDLE child_stdin_read = nullptr, child_stdin_write = nullptr;
    if (!CreatePipe(&child_stdout_read, &child_stdout_write, &sa, 0) ||
        !SetHandleInformation(child_stdout_read, HANDLE_FLAG_INHERIT, 0) ||
        !CreatePipe(&child_stdin_read, &child_stdin_write, &sa, 0) ||
        !SetHandleInformation(child_stdin_write, HANDLE_FLAG_INHERIT, 0)) {
        result.exit_code = -1;
        result.error = "CreatePipe failed";
        return result;
    }

    std::string cmd;
    for (size_t i = 0; i < args.size(); ++i) {
        if (i) cmd += " ";
        cmd += quote_arg(args[i]);
    }
    std::vector<char> cmdline(cmd.begin(), cmd.end());
    cmdline.push_back('\0');
    auto env_block = build_windows_environment(env);

    STARTUPINFOA si{};
    si.cb = sizeof(si);
    si.hStdError = child_stdout_write;
    si.hStdOutput = child_stdout_write;
    si.hStdInput = child_stdin_read;
    si.dwFlags |= STARTF_USESTDHANDLES;

    PROCESS_INFORMATION pi{};
    BOOL ok = CreateProcessA(nullptr, cmdline.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             env_block.data(), nullptr, &si, &pi);
    CloseHandle(child_stdout_write);
    CloseHandle(child_stdin_read);
    if (!ok) {
        CloseHandle(child_stdout_read);
        CloseHandle(child_stdin_write);
        result.exit_code = -1;
        result.error = "CreateProcess failed";
        return result;
    }

    DWORD written = 0;
    if (!stdin_data.empty()) {
        WriteFile(child_stdin_write, stdin_data.data(), static_cast<DWORD>(stdin_data.size()), &written, nullptr);
    }
    CloseHandle(child_stdin_write);

    char buffer[4096];
    DWORD read = 0;
    while (ReadFile(child_stdout_read, buffer, sizeof(buffer), &read, nullptr) && read > 0) {
        result.output.append(buffer, buffer + read);
    }
    CloseHandle(child_stdout_read);
    WaitForSingleObject(pi.hProcess, 10000);
    DWORD code = 0;
    GetExitCodeProcess(pi.hProcess, &code);
    result.exit_code = static_cast<int>(code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return result;
}
#else
CgiOutput run_process(const std::vector<std::string> &args, const std::string &stdin_data,
                      const std::map<std::string, std::string> &env) {
    CgiOutput result;
    int in_pipe[2];
    int out_pipe[2];
    if (pipe(in_pipe) != 0 || pipe(out_pipe) != 0) {
        result.exit_code = -1;
        result.error = "pipe failed";
        return result;
    }

    pid_t pid = fork();
    if (pid == 0) {
        dup2(in_pipe[0], STDIN_FILENO);
        dup2(out_pipe[1], STDOUT_FILENO);
        dup2(out_pipe[1], STDERR_FILENO);
        close(in_pipe[1]);
        close(out_pipe[0]);
        for (const auto &kv : env) setenv(kv.first.c_str(), kv.second.c_str(), 1);
        std::vector<char *> argv;
        for (const auto &arg : args) argv.push_back(const_cast<char *>(arg.c_str()));
        argv.push_back(nullptr);
        execvp(argv[0], argv.data());
        _exit(127);
    }
    close(in_pipe[0]);
    close(out_pipe[1]);
    if (!stdin_data.empty()) {
        ssize_t ignored = write(in_pipe[1], stdin_data.data(), stdin_data.size());
        (void)ignored;
    }
    close(in_pipe[1]);
    char buffer[4096];
    ssize_t n = 0;
    while ((n = read(out_pipe[0], buffer, sizeof(buffer))) > 0) {
        result.output.append(buffer, buffer + n);
    }
    close(out_pipe[0]);
    int status = 0;
    waitpid(pid, &status, 0);
    result.exit_code = WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    return result;
}
#endif

Response parse_cgi_response(const std::string &cgi_output) {
    Response res;
    res.status = 200;
    res.reason = "OK";
    std::string normalized = cgi_output;
    size_t split = normalized.find("\r\n\r\n");
    size_t sep_len = 4;
    if (split == std::string::npos) {
        split = normalized.find("\n\n");
        sep_len = 2;
    }
    if (split == std::string::npos) {
        res.body = normalized;
        return res;
    }

    std::string header_text = normalized.substr(0, split);
    res.body = normalized.substr(split + sep_len);
    std::istringstream hs(header_text);
    std::string line;
    while (std::getline(hs, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        auto colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string key = trim(line.substr(0, colon));
        std::string value = trim(line.substr(colon + 1));
        if (lower_copy(key) == "status") {
            std::istringstream ss(value);
            ss >> res.status;
            res.reason = trim(value.substr(value.find(' ') == std::string::npos ? value.size() : value.find(' ') + 1));
            if (res.reason.empty()) res.reason = status_reason(res.status);
        } else if (lower_copy(key) == "content-type") {
            res.content_type = value;
        } else {
            res.headers[key] = value;
        }
    }
    return res;
}

class Logger {
public:
    explicit Logger(fs::path path) : path_(std::move(path)) {
        fs::create_directories(path_.parent_path());
    }

    void log(const Request &req, int status, size_t bytes) {
        std::lock_guard<std::mutex> lock(mu_);
        std::ofstream out(path_, std::ios::app);
        std::string ref = header(req, "referer", "-");
        std::string ua = header(req, "user-agent", "-");
        out << req.client_ip << " - - [" << log_date() << "] "
            << '"' << req.method << ' ' << req.target << ' ' << req.version << '"' << ' '
            << status << ' ' << bytes << ' '
            << '"' << ref << '"' << ' ' << '"' << ua << '"' << "\n";
    }

private:
    static std::string header(const Request &req, const std::string &key, const std::string &fallback) {
        auto it = req.headers.find(key);
        return it == req.headers.end() ? fallback : it->second;
    }

    fs::path path_;
    std::mutex mu_;
};

class ConnectionRegistry {
public:
    explicit ConnectionRegistry(size_t max_connections) : max_connections_(max_connections) {}

    void add(Socket s) {
        std::lock_guard<std::mutex> lock(mu_);
        while (connections_.size() >= max_connections_) {
            Socket old = connections_.front();
            connections_.pop_front();
            close_socket(old);
        }
        connections_.push_back(s);
    }

    void remove(Socket s) {
        std::lock_guard<std::mutex> lock(mu_);
        auto it = std::find(connections_.begin(), connections_.end(), s);
        if (it != connections_.end()) connections_.erase(it);
    }

private:
    size_t max_connections_;
    std::deque<Socket> connections_;
    std::mutex mu_;
};

class ThreadPool {
public:
    ThreadPool(int count, const Config &config, Logger &logger, ConnectionRegistry &registry)
        : config_(config), logger_(logger), registry_(registry) {
        for (int i = 0; i < count; ++i) {
            workers_.emplace_back([this] { worker_loop(); });
        }
    }

    ~ThreadPool() {
        {
            std::lock_guard<std::mutex> lock(mu_);
            stopped_ = true;
        }
        cv_.notify_all();
        for (auto &thread : workers_) {
            if (thread.joinable()) thread.join();
        }
    }

    void enqueue(Socket s, std::string ip) {
        {
            std::lock_guard<std::mutex> lock(mu_);
            queue_.push({s, std::move(ip)});
        }
        cv_.notify_one();
    }

private:
    struct Job {
        Socket socket;
        std::string ip;
    };

    void worker_loop() {
        while (g_running) {
            Job job{INVALID_SOCKET_VALUE, ""};
            {
                std::unique_lock<std::mutex> lock(mu_);
                cv_.wait(lock, [this] { return stopped_ || !queue_.empty(); });
                if (stopped_ && queue_.empty()) return;
                job = queue_.front();
                queue_.pop();
            }
            handle_client(job.socket, job.ip);
            registry_.remove(job.socket);
            close_socket(job.socket);
        }
    }

    bool is_cgi_request(const fs::path &relative) {
        auto it = relative.begin();
        return it != relative.end() && it->string() == "cgi-bin";
    }

    Response run_cgi(const Request &req, const fs::path &script) {
        fs::path script_for_child = script;
        fs::path root_for_child = config_.root;
        std::error_code ec;
        fs::path cwd = fs::current_path(ec);
        if (!ec) {
            fs::path prox_script = fs::proximate(script, cwd, ec);
            if (!ec && !prox_script.empty()) script_for_child = prox_script;
            ec.clear();
            fs::path prox_root = fs::proximate(config_.root, cwd, ec);
            if (!ec && !prox_root.empty()) root_for_child = prox_root;
        }

        std::map<std::string, std::string> env;
        env["REQUEST_METHOD"] = req.method;
        env["QUERY_STRING"] = req.query;
        env["CONTENT_LENGTH"] = std::to_string(req.body.size());
        env["CONTENT_TYPE"] = header(req, "content-type", "application/x-www-form-urlencoded");
        env["SCRIPT_FILENAME"] = script_for_child.string();
        env["SCRIPT_NAME"] = req.path;
        env["SERVER_PROTOCOL"] = req.version;
        env["SERVER_SOFTWARE"] = "Project2-CGI-Server/1.0";
        env["REMOTE_ADDR"] = req.client_ip;
        env["DOCUMENT_ROOT"] = root_for_child.string();

        std::vector<std::string> args;
        std::string ext = lower_copy(script.extension().string());
        if (ext == ".py") {
            args = {config_.python, script_for_child.string()};
        } else {
            args = {script_for_child.string()};
        }

        CgiOutput out = run_process(args, req.body, env);
        if (out.exit_code != 0 && out.output.empty()) {
            Response res = error_response(500, config_);
            res.body = "<html><body><h1>500 Internal Server Error</h1><pre>" + out.error + "</pre></body></html>";
            return res;
        }
        return parse_cgi_response(out.output);
    }

    Response serve_static(const fs::path &file) {
        Response res;
        res.status = 200;
        res.reason = "OK";
        res.content_type = mime_type(file);
        res.body = read_file_binary(file);
        return res;
    }

    Response handle_request(const Request &req) {
        if (req.method != "GET" && req.method != "POST" && req.method != "HEAD") {
            return error_response(400, config_);
        }
        if (req.version.rfind("HTTP/", 0) != 0) {
            return error_response(400, config_);
        }

        fs::path relative;
        if (!sanitize_path(req.path, relative)) return error_response(403, config_);
        fs::path full = config_.root / relative;

        if (fs::is_directory(full)) full /= "index.html";
        bool cgi = is_cgi_request(relative);
        if (!fs::exists(full) || !fs::is_regular_file(full)) return error_response(404, config_);
        if (cgi) return run_cgi(req, full);
        if (req.method == "POST") return error_response(403, config_);
        return serve_static(full);
    }

    void write_response(Socket s, const Request &req, Response &res) {
        res.reason = res.reason.empty() ? status_reason(res.status) : res.reason;
        std::ostringstream header;
        header << "HTTP/1.0 " << res.status << ' ' << res.reason << "\r\n";
        header << "Date: " << http_date() << "\r\n";
        header << "Server: Project2-CGI-Server/1.0\r\n";
        header << "Content-Type: " << res.content_type << "\r\n";
        header << "Content-Length: " << res.body.size() << "\r\n";
        for (const auto &kv : res.headers) header << kv.first << ": " << kv.second << "\r\n";
        bool keep_alive = lower_copy(header_value(req, "connection")) == "keep-alive";
        header << "Connection: " << (keep_alive ? "keep-alive" : "close") << "\r\n\r\n";
        send_all(s, header.str());
        if (req.method != "HEAD") send_all(s, res.body);
        logger_.log(req, res.status, res.body.size());
    }

    void handle_client(Socket s, const std::string &ip) {
        std::string buffer;
        while (g_running) {
            Request req;
            req.client_ip = ip;
            if (!read_http_request(s, buffer, req)) break;
            Response res = handle_request(req);
            write_response(s, req, res);
            std::string conn = lower_copy(header_value(req, "connection"));
            bool keep_alive = conn == "keep-alive" && req.version == "HTTP/1.0";
            if (!keep_alive) break;
        }
    }

    static std::string header_value(const Request &req, const std::string &key) {
        auto it = req.headers.find(lower_copy(key));
        return it == req.headers.end() ? "" : it->second;
    }

    static std::string header(const Request &req, const std::string &key, const std::string &fallback) {
        auto it = req.headers.find(lower_copy(key));
        return it == req.headers.end() ? fallback : it->second;
    }

    const Config &config_;
    Logger &logger_;
    ConnectionRegistry &registry_;
    std::vector<std::thread> workers_;
    std::queue<Job> queue_;
    std::mutex mu_;
    std::condition_variable cv_;
    bool stopped_ = false;
};

Config parse_args(int argc, char **argv) {
    Config cfg;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        auto require_value = [&](const std::string &name) -> std::string {
            if (i + 1 >= argc) {
                std::cerr << "Missing value for " << name << "\n";
                std::exit(1);
            }
            return argv[++i];
        };
        if (arg == "--port") cfg.port = std::stoi(require_value(arg));
        else if (arg == "--threads") cfg.worker_threads = std::stoi(require_value(arg));
        else if (arg == "--max-connections") cfg.max_connections = std::stoi(require_value(arg));
        else if (arg == "--root") cfg.root = require_value(arg);
        else if (arg == "--python") cfg.python = require_value(arg);
        else if (arg == "--help" || arg == "-h") {
            std::cout << "Usage: webserver [--port 8888] [--threads 4] [--max-connections 32] [--root webroot] [--python python]\n";
            std::exit(0);
        }
    }
    if (cfg.worker_threads <= 0) cfg.worker_threads = 1;
    if (cfg.max_connections <= 0) cfg.max_connections = cfg.worker_threads;
    cfg.root = fs::absolute(cfg.root);
    return cfg;
}

void handle_signal(int) {
    g_running = false;
}

int main(int argc, char **argv) {
    std::signal(SIGINT, handle_signal);
    std::signal(SIGTERM, handle_signal);
    Config config = parse_args(argc, argv);
    fs::create_directories(config.root / "log");

#ifdef _WIN32
    WSADATA wsa{};
    if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0) {
        std::cerr << "WSAStartup failed\n";
        return 1;
    }
#endif

    Socket server = socket(AF_INET, SOCK_STREAM, 0);
    if (server == INVALID_SOCKET_VALUE) {
        std::cerr << "socket failed\n";
        return 1;
    }

    int yes = 1;
#ifdef _WIN32
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
#endif

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = htonl(INADDR_ANY);
    addr.sin_port = htons(static_cast<uint16_t>(config.port));
    if (bind(server, reinterpret_cast<sockaddr *>(&addr), sizeof(addr)) != 0) {
        std::cerr << "bind failed on port " << config.port << "\n";
        close_socket(server);
        return 1;
    }
    if (listen(server, SOMAXCONN) != 0) {
        std::cerr << "listen failed\n";
        close_socket(server);
        return 1;
    }

    Logger logger(config.root / "log" / "server.log");
    ConnectionRegistry registry(static_cast<size_t>(config.max_connections));
    ThreadPool pool(config.worker_threads, config, logger, registry);

    std::error_code display_ec;
    fs::path display_root = fs::proximate(config.root, fs::current_path(), display_ec);
    if (display_ec || display_root.empty()) display_root = config.root.filename();

    std::cout << "Project2 CGI Web Server\n"
              << "Root: " << display_root.string() << "\n"
              << "URL:  http://127.0.0.1:" << config.port << "/\n"
              << "Threads: " << config.worker_threads << ", max connections: " << config.max_connections << "\n";

    while (g_running) {
        sockaddr_in client_addr{};
#ifdef _WIN32
        int len = sizeof(client_addr);
#else
        socklen_t len = sizeof(client_addr);
#endif
        Socket client = accept(server, reinterpret_cast<sockaddr *>(&client_addr), &len);
        if (client == INVALID_SOCKET_VALUE) {
            if (!g_running) break;
            continue;
        }
        char ip[INET_ADDRSTRLEN] = {0};
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        registry.add(client);
        pool.enqueue(client, ip[0] ? ip : "unknown");
    }

    close_socket(server);
#ifdef _WIN32
    WSACleanup();
#endif
    return 0;
}
