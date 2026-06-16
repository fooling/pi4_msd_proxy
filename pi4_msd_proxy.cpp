// pi4_msd_proxy — Raspberry Pi 4 IPTV 代理 (msd_lite/udpxy 风格) + 按需硬件转码
//
// 默认 = 纯透传(原生 HTTP 中继, 不起 ffmpeg, 原始质量/原编码/原分辨率, 4K HEVC 原样转发)。
// 只有客户端 URL 带 ?target-resolution=720p 时, 才对该会话挂上硬件转码管线
// (drm 硬解 + h264_v4l2m2m 硬编, 多线程缩放)。Pi4 硬编上限 1080p, 故转码档 <=1080p。
//
// 像 msd_lite 那样 URL 里直接带上游地址, 零频道登记:
//   现有:  http://192.168.1.2:8888/rtp/239.0.0.1:1234
//   改成:  http://<pi>:8887/192.168.1.2:8888/rtp/239.0.0.1:1234
//
// 并发: 每个(源+档位)一路会话, 多客户端 shared_ptr 零拷贝扇出; 海量透传 + 个别转码并行。
// 缓冲按 msd_lite 4K 档: ringBuf 8M / precache 4M / sndBuf 1M (均可配)。
//
// 纯 POSIX + C++ 标准库, 无第三方依赖。编译: g++ -O2 -std=c++17 -pthread -o pi4_msd_proxy pi4_msd_proxy.cpp
//
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <poll.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <deque>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

static const char* PROG = "pi4_msd_proxy";
static const char* DEFAULT_CONF = "/usr/local/etc/pi4_msd_proxy.conf";

static std::atomic<bool> g_stop{false};
using Buf = std::shared_ptr<const std::string>;   // 一次构造, 全体订阅者共享(零拷贝扇出)

static void logmsg(const std::string& s) {
    char ts[32]; time_t t = time(nullptr); struct tm tmv{}; localtime_r(&t, &tmv);
    strftime(ts, sizeof(ts), "%H:%M:%S", &tmv);
    fprintf(stderr, "[%s] %s\n", ts, s.c_str()); fflush(stderr);
}
static std::string trim(const std::string& s) {
    size_t a = s.find_first_not_of(" \t\r\n"); if (a == std::string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n"); return s.substr(a, b - a + 1);
}
static bool starts_with(const std::string& s, const char* p) { return s.rfind(p, 0) == 0; }
static int to_int(const std::string& s, int def) { try { return std::stoi(s); } catch (...) { return def; } }
// 0/auto/orig/source = 保持原始(透传); 否则取数字("720p"也行)
static int parse_height(const std::string& v) {
    if (v == "auto" || v == "orig" || v == "original" || v == "source" || v == "src" || v == "0") return 0;
    return to_int(v, 0);
}
// "8m"/"4096k"/"262144" -> 字节
static size_t parse_size(const std::string& v, size_t def) {
    if (v.empty()) return def;
    char suf = tolower(v.back()); double mult = 1; std::string num = v;
    if (suf == 'k') { mult = 1024; num = v.substr(0, v.size() - 1); }
    else if (suf == 'm') { mult = 1024.0 * 1024; num = v.substr(0, v.size() - 1); }
    else if (suf == 'g') { mult = 1024.0 * 1024 * 1024; num = v.substr(0, v.size() - 1); }
    try { return (size_t)(std::stod(num) * mult); } catch (...) { return def; }
}
// 转码降档时按分辨率自动配码率
static std::string ladder_bitrate(int h) {
    if (h >= 1080) return "6000k";
    if (h >= 720)  return "2500k";
    if (h >= 540)  return "1800k";
    if (h >= 480)  return "1200k";
    return "1000k";
}
static bool send_all(int fd, const char* p, size_t n);
static int  http_connect(const std::string& url, std::string& leftover, bool& chunked);

// 原生 HTTP chunked 解码(透传遇到分块上游时用, 全程不碰 ffmpeg)
struct Dechunker {
    enum St { SIZE, DATA, CR } st = SIZE;
    size_t remain = 0; std::string sizeline; bool done = false;
    void feed(const char* p, size_t n, std::string& out) {
        size_t i = 0;
        while (i < n && !done) {
            if (st == SIZE) {
                char c = p[i++];
                if (c == '\n') {
                    size_t sz = strtoul(sizeline.c_str(), nullptr, 16);  // 忽略 ';' 扩展
                    sizeline.clear();
                    if (sz == 0) { done = true; } else { remain = sz; st = DATA; }
                } else if (c != '\r') sizeline.push_back(c);
            } else if (st == DATA) {
                size_t take = std::min(remain, n - i);
                out.append(p + i, take); i += take; remain -= take;
                if (remain == 0) st = CR;
            } else { /* CR: 跳过 chunk 数据后的 \r\n */
                if (p[i++] == '\n') st = SIZE;
            }
        }
    }
};

// ---------------- 配置 ----------------
struct ChannelCfg {
    std::string source;
    int         height = 0;            // 0 = 透传(原始); >0 = 转码到该高度
    std::string video_bitrate = "2500k";
    std::string audio = "copy";
    int         fps = 25;
    std::string key() const { return source + "|" + std::to_string(height) + "|" +
                                     video_bitrate + "|" + audio + "|" + std::to_string(fps); }
};
struct Config {
    int         listen_port = 8887;
    std::string ffmpeg = "ffmpeg";
    int         idle_timeout = 15;
    ChannelCfg  def;                   // 默认档(height=0 即默认透传)
    int         hw = -1;               // 转码用硬件: -1=自动(测 /dev/video11), 1=硬(Pi drm+h264_v4l2m2m), 0=软(libx264)
    // 缓冲(msd_lite 4K 推荐档)
    size_t ring_buf_size = 8u * 1024 * 1024;   // 单客户端积压上限, 超了断开慢客户端
    size_t precache      = 4u * 1024 * 1024;   // 新客户端连上立即灌的最近数据(快速起播)
    size_t snd_buf       = 1u * 1024 * 1024;   // 客户端 socket 发送缓冲 SO_SNDBUF
    size_t read_block    = 16384;              // 从上游/ffmpeg 每次读取块(16 KiB, 低延迟)
};

static bool load_config(const std::string& path, Config& cfg, std::string& err) {
    std::ifstream f(path);
    if (!f) { err = "打不开配置文件: " + path; return false; }
    std::string line; int lineno = 0;
    while (std::getline(f, line)) {
        ++lineno; std::string s = trim(line);
        if (s.empty() || s[0] == '#') continue;
        size_t eq = s.find('='); if (eq == std::string::npos) { err = "第" + std::to_string(lineno) + "行缺 '='"; return false; }
        std::string k = trim(s.substr(0, eq)), v = trim(s.substr(eq + 1));
        size_t hsh = v.find('#'); if (hsh != std::string::npos) v = trim(v.substr(0, hsh));  // 去掉行内注释
        if (k == "listen")            cfg.listen_port      = std::stoi(v);
        else if (k == "ffmpeg")       cfg.ffmpeg           = v;
        else if (k == "idle_timeout") cfg.idle_timeout     = std::stoi(v);
        else if (k == "height")       cfg.def.height       = parse_height(v);
        else if (k == "video_bitrate")cfg.def.video_bitrate= v;
        else if (k == "audio")        cfg.def.audio        = v;
        else if (k == "fps")          cfg.def.fps          = std::stoi(v);
        else if (k == "ring_buf_size")cfg.ring_buf_size    = parse_size(v, cfg.ring_buf_size);
        else if (k == "precache")     cfg.precache         = parse_size(v, cfg.precache);
        else if (k == "snd_buf")      cfg.snd_buf          = parse_size(v, cfg.snd_buf);
        else if (k == "read_block")   cfg.read_block       = parse_size(v, cfg.read_block);
        else if (k == "hw")           cfg.hw = (v=="on"||v=="1"||v=="true"||v=="hw") ? 1 : (v=="auto"||v=="-1") ? -1 : 0;
        else { err = "未知配置项: " + k; return false; }
    }
    return true;
}

static bool parse_target(const std::string& rawpath, const Config& g, ChannelCfg& out) {
    std::string p = rawpath;
    if (!p.empty() && p[0] == '/') p.erase(0, 1);
    std::string query;
    size_t q = p.find('?'); if (q != std::string::npos) { query = p.substr(q + 1); p = p.substr(0, q); }
    if (p.empty()) return false;
    if (starts_with(p, "http://") || starts_with(p, "https://") ||
        starts_with(p, "rtp://")  || starts_with(p, "udp://"))  out.source = p;
    else                                                        out.source = "http://" + p;
    out.height = g.def.height; out.video_bitrate = g.def.video_bitrate;
    out.audio = g.def.audio;   out.fps = g.def.fps;
    bool res_set = false, vb_set = false;
    std::stringstream ss(query); std::string kv;
    while (std::getline(ss, kv, '&')) {
        size_t e = kv.find('='); if (e == std::string::npos) continue;
        std::string k = kv.substr(0, e), v = kv.substr(e + 1);
        if (k == "target-resolution" || k == "res" || k == "h" || k == "height") { out.height = parse_height(v); res_set = true; }
        else if (k == "vb" || k == "bitrate") { out.video_bitrate = v; vb_set = true; }
        else if (k == "a" || k == "audio") out.audio = v;
        else if (k == "fps") out.fps = to_int(v, out.fps);
    }
    if (res_set && out.height > 0 && !vb_set) out.video_bitrate = ladder_bitrate(out.height);
    return true;
}

// ---------------- 订阅者(一个客户端连接) ----------------
struct Subscriber {
    explicit Subscriber(size_t c) : cap(c) {}
    size_t cap;
    std::mutex m; std::condition_variable cv;
    std::deque<Buf> q; size_t bytes = 0; bool dead = false;
    void push(const Buf& b) {
        std::lock_guard<std::mutex> lk(m);
        if (dead) return;
        if (bytes + b->size() > cap) { dead = true; cv.notify_all(); return; }  // 慢客户端, 断开
        q.push_back(b); bytes += b->size(); cv.notify_all();
    }
    Buf pop() {
        std::unique_lock<std::mutex> lk(m);
        cv.wait(lk, [&] { return dead || !q.empty() || g_stop.load(); });
        if (dead || g_stop.load()) return nullptr;
        Buf b = q.front(); q.pop_front(); bytes -= b->size(); return b;
    }
};

// ---------------- 频道(一路按需会话 + 扇出) ----------------
class Channel {
public:
    Channel(std::string label, ChannelCfg cfg, const Config* g)
        : label_(std::move(label)), cfg_(std::move(cfg)), g_(g) {}
    void start_manager() { mgr_ = std::thread(&Channel::manager_loop, this); }
    void stop_manager() { cv_.notify_all(); if (mgr_.joinable()) mgr_.join(); }

    void add_sub(Subscriber* s) {
        std::lock_guard<std::mutex> lk(mu_);
        for (auto& b : hist_) s->push(b);        // precache: 立即灌最近数据, 快速起播
        subs_.insert(s); cv_.notify_all();
        logmsg("[" + label_ + "] +客户端 (在线=" + std::to_string(subs_.size()) + ")");
    }
    void remove_sub(Subscriber* s) {
        size_t n; { std::lock_guard<std::mutex> lk(mu_); subs_.erase(s); n = subs_.size(); }
        logmsg("[" + label_ + "] -客户端 (在线=" + std::to_string(n) + ")");
    }

private:
    bool transcode() const { return cfg_.height > 0; }

    std::vector<std::string> ffmpeg_args() const {
        bool http = starts_with(cfg_.source, "http");
        std::vector<std::string> a = { g_->ffmpeg, "-hide_banner", "-loglevel", "warning", "-nostdin",
            "-fflags", "nobuffer", "-flags", "low_delay" };
        bool hw = g_->hw == 1;
        if (http) a.insert(a.end(), {"-reconnect","1","-reconnect_streamed","1","-reconnect_delay_max","2"});
        else      a.insert(a.end(), {"-fifo_size","1000000","-overrun_nonfatal","1"});
        if (transcode() && hw) a.insert(a.end(), {"-hwaccel","drm"});  // 硬解 (须在 -i 前)
        a.insert(a.end(), {"-i", cfg_.source});
        if (transcode()) {
            int fps = cfg_.fps > 0 ? cfg_.fps : 25, gop = fps * 2;
            long ncpu = sysconf(_SC_NPROCESSORS_ONLN); if (ncpu < 1) ncpu = 4;
            std::string vf = "scale=-2:" + std::to_string(cfg_.height) + ":flags=fast_bilinear,format=yuv420p";
            a.insert(a.end(), {"-filter_threads", std::to_string(ncpu), "-vf", vf});
            if (hw) a.insert(a.end(), {"-c:v","h264_v4l2m2m","-b:v",cfg_.video_bitrate,"-g",std::to_string(gop),"-bf","0"});
            else    a.insert(a.end(), {"-c:v","libx264","-preset","veryfast","-pix_fmt","yuv420p","-b:v",cfg_.video_bitrate,"-g",std::to_string(gop),"-bf","0"}); // x86/无硬件回退
            if (cfg_.audio.empty() || cfg_.audio == "copy") a.insert(a.end(), {"-c:a","copy"});
            else a.insert(a.end(), {"-c:a","aac","-b:a",cfg_.audio,"-ac","2"});
        } else {
            a.insert(a.end(), {"-c", "copy"});                        // 透传重封装(rtp/udp 源)
        }
        a.insert(a.end(), {"-f", "mpegts", "pipe:1"});
        return a;
    }
    int spawn_ffmpeg(pid_t& pid_out) {
        int fds[2]; if (pipe(fds) != 0) return -1;
        auto args = ffmpeg_args();
        std::vector<char*> argv; for (auto& s : args) argv.push_back(const_cast<char*>(s.c_str())); argv.push_back(nullptr);
        pid_t pid = fork();
        if (pid == 0) { dup2(fds[1], STDOUT_FILENO); close(fds[0]); close(fds[1]); setpgid(0,0);
                        execvp(argv[0], argv.data()); _exit(127); }
        close(fds[1]); if (pid < 0) { close(fds[0]); return -1; }
        pid_out = pid;
        std::string mode = transcode() ? (std::to_string(cfg_.height) + "p@" + cfg_.video_bitrate + (g_->hw == 1 ? " 硬转码" : " 软转码")) : "copy透传";
        logmsg("[" + label_ + "] ffmpeg[" + mode + "] pid=" + std::to_string(pid) + "  源=" + cfg_.source);
        return fds[0];
    }
    void kill_ffmpeg(pid_t pid) {
        if (pid <= 0) return;
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {   // ffmpeg 已自己退出: 打印原因
            if (WIFEXITED(st)) logmsg("[" + label_ + "] ffmpeg 退出码=" + std::to_string(WEXITSTATUS(st)) +
                                      (WEXITSTATUS(st) == 127 ? " (=exec失败/PATH里找不到ffmpeg)" : ""));
            else if (WIFSIGNALED(st)) logmsg("[" + label_ + "] ffmpeg 被信号杀 sig=" + std::to_string(WTERMSIG(st)));
            return;
        }
        killpg(pid, SIGTERM);
        for (int i = 0; i < 20; ++i) { if (waitpid(pid, nullptr, WNOHANG) == pid) return; usleep(50*1000); }
        killpg(pid, SIGKILL); waitpid(pid, nullptr, 0);
    }
    // http 源透传 = 原生中继(纯字节, 全程不碰 ffmpeg; chunked 也原生解); 否则 ffmpeg(转码 或 rtp/udp copy)
    int open_source(pid_t& pid, std::string& leftover, bool& dechunk) {
        pid = -1; leftover.clear(); dechunk = false;
        bool http = starts_with(cfg_.source, "http://") || starts_with(cfg_.source, "https://");
        if (!transcode() && http) {
            bool chunked = false;
            int fd = http_connect(cfg_.source, leftover, chunked);
            if (fd >= 0) { dechunk = chunked;
                logmsg("[" + label_ + "] HTTP真透传(无 ffmpeg" + std::string(chunked ? ", 原生解chunk" : "") + ") <- " + cfg_.source); }
            return fd;
        }
        return spawn_ffmpeg(pid);
    }
    void fanout(const char* b, size_t n) {
        auto buf = std::make_shared<const std::string>(b, n);
        std::lock_guard<std::mutex> lk(mu_);
        hist_.push_back(buf); hist_bytes_ += n;                       // 维护 precache 滚动窗口
        while (hist_bytes_ > g_->precache && hist_.size() > 1) { hist_bytes_ -= hist_.front()->size(); hist_.pop_front(); }
        for (auto* s : subs_) s->push(buf);
    }
    size_t sub_count() { std::lock_guard<std::mutex> lk(mu_); return subs_.size(); }

    void manager_loop() {
        std::vector<char> buf(g_->read_block);
        while (!g_stop.load()) {
            { std::unique_lock<std::mutex> lk(mu_); cv_.wait(lk, [&]{ return g_stop.load() || !subs_.empty(); });
              if (g_stop.load()) return; }
            pid_t pid = -1; std::string leftover; bool dechunk = false;
            int fd = open_source(pid, leftover, dechunk);
            if (fd < 0) { logmsg("[" + label_ + "] 上游打开失败, 2s 后重试"); sleep(2); continue; }
            { std::lock_guard<std::mutex> lk(mu_); hist_.clear(); hist_bytes_ = 0; }   // 新会话清 precache
            Dechunker dc;
            auto emit = [&](const char* p, size_t n) {
                if (!dechunk) { fanout(p, n); return; }
                std::string out; dc.feed(p, n, out);
                if (!out.empty()) fanout(out.data(), out.size());
            };
            if (!leftover.empty()) emit(leftover.data(), leftover.size());
            time_t idle_since = 0; bool died = false;
            while (!g_stop.load()) {
                struct pollfd pfd{fd, POLLIN, 0}; int pr = poll(&pfd, 1, 1000);
                if (pr > 0 && (pfd.revents & (POLLIN | POLLHUP))) {
                    ssize_t n = read(fd, buf.data(), buf.size());
                    if (n > 0) emit(buf.data(), (size_t)n); else { died = true; break; }
                }
                if (sub_count() == 0) {
                    time_t now = time(nullptr);
                    if (idle_since == 0) idle_since = now;
                    else if (now - idle_since >= g_->idle_timeout) { logmsg("[" + label_ + "] 空闲超时, 停"); break; }
                } else idle_since = 0;
            }
            close(fd); kill_ffmpeg(pid);
            if (g_stop.load()) return;
            if (died && sub_count() > 0) { logmsg("[" + label_ + "] 上游断开, 1s 后重连"); sleep(1); }
        }
    }

    std::string label_; ChannelCfg cfg_; const Config* g_;
    std::mutex mu_; std::condition_variable cv_; std::set<Subscriber*> subs_;
    std::deque<Buf> hist_; size_t hist_bytes_ = 0;
    std::thread mgr_;
};

// ---------------- App: 动态会话注册表 ----------------
struct App {
    Config cfg;
    std::mutex reg_mu;
    std::map<std::string, std::unique_ptr<Channel>> chans;
    Channel* get_or_create(const ChannelCfg& c) {
        std::string key = c.key();
        std::lock_guard<std::mutex> lk(reg_mu);
        auto it = chans.find(key);
        if (it != chans.end()) return it->second.get();
        std::string label = c.source; size_t sl = label.rfind('/');
        if (sl != std::string::npos) label = label.substr(sl + 1);
        label += (c.height > 0 ? "@" + std::to_string(c.height) + "p" : std::string("@orig"));
        auto ch = std::make_unique<Channel>(label, c, &cfg);
        ch->start_manager();
        Channel* p = ch.get(); chans[key] = std::move(ch);
        return p;
    }
    void stop_all() { std::lock_guard<std::mutex> lk(reg_mu); for (auto& kv : chans) kv.second->stop_manager(); }
};

static bool send_all(int fd, const char* p, size_t n) {
    while (n > 0) { ssize_t w = send(fd, p, n, MSG_NOSIGNAL); if (w <= 0) return false; p += w; n -= (size_t)w; }
    return true;
}
static bool send_str(int fd, const std::string& s) { return send_all(fd, s.data(), s.size()); }

// 原生 HTTP 中继: 连上游, 跳过响应头, 返回 fd(body 起点); 剩余已读 body 放 leftover; chunked 置位
static int http_connect(const std::string& url, std::string& leftover, bool& chunked) {
    chunked = false;
    std::string u = url; size_t sp = u.find("://"); if (sp != std::string::npos) u = u.substr(sp + 3);
    std::string hostport, path; size_t sl = u.find('/');
    if (sl == std::string::npos) { hostport = u; path = "/"; } else { hostport = u.substr(0, sl); path = u.substr(sl); }
    std::string host = hostport, port = "80"; size_t c = hostport.find(':');
    if (c != std::string::npos) { host = hostport.substr(0, c); port = hostport.substr(c + 1); }
    struct addrinfo hints{}, *res = nullptr; hints.ai_family = AF_UNSPEC; hints.ai_socktype = SOCK_STREAM;
    int gai = getaddrinfo(host.c_str(), port.c_str(), &hints, &res);
    if (gai != 0) { logmsg(std::string("http_connect getaddrinfo(") + host + ":" + port + ") 失败: " + gai_strerror(gai)); return -1; }
    int fd = -1;
    for (auto* ai = res; ai; ai = ai->ai_next) {
        fd = socket(ai->ai_family, ai->ai_socktype, ai->ai_protocol); if (fd < 0) continue;
        if (connect(fd, ai->ai_addr, ai->ai_addrlen) == 0) break;
        close(fd); fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0) { logmsg(std::string("http_connect connect(") + host + ":" + port + ") 失败: " + strerror(errno)); return -1; }
    // Host 必须带端口, 否则 msd_lite 等会 403
    std::string req = "GET " + path + " HTTP/1.0\r\nHost: " + hostport +
                      "\r\nUser-Agent: pi4_msd_proxy\r\nConnection: close\r\n\r\n";
    if (!send_all(fd, req.data(), req.size())) { logmsg("http_connect send 失败"); close(fd); return -1; }
    std::string h; char b[2048];
    while (h.find("\r\n\r\n") == std::string::npos && h.size() < 16384) {
        ssize_t n = recv(fd, b, sizeof(b), 0); if (n <= 0) { logmsg(std::string("http_connect recv 头失败: ") + strerror(errno)); close(fd); return -1; } h.append(b, n);
    }
    if (starts_with(h, "HTTP")) { size_t s2 = h.find(' '); int code = s2 != std::string::npos ? to_int(h.substr(s2 + 1, 3), 0) : 0;
                                  if (code < 200 || code >= 300) { logmsg("http_connect 上游拒绝 码=" + std::to_string(code) + " [" + h.substr(0, h.find('\r')) + "]"); close(fd); return -1; } }
    // 分块编码: 不碰 ffmpeg, 由调用方用原生 Dechunker 解
    std::string hl = h; for (auto& ch : hl) ch = (char)tolower((unsigned char)ch);
    if (hl.find("transfer-encoding: chunked") != std::string::npos) chunked = true;
    size_t hp = h.find("\r\n\r\n");
    leftover = h.substr(hp + 4);
    return fd;
}

static bool read_request(int fd, std::string& path) {
    std::string req; char b[2048];
    while (req.find("\r\n\r\n") == std::string::npos && req.size() < 8192) {
        ssize_t n = recv(fd, b, sizeof(b), 0); if (n <= 0) return false; req.append(b, n);
    }
    std::istringstream is(req); std::string method, ver;
    if (!(is >> method >> path >> ver)) return false;
    return method == "GET";
}
static void http_text(int fd, int code, const std::string& reason, const std::string& body) {
    std::string hdr = "HTTP/1.1 " + std::to_string(code) + " " + reason +
        "\r\nContent-Type: text/plain; charset=utf-8\r\nContent-Length: " +
        std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n";
    send_str(fd, hdr); send_str(fd, body);
}
static void handle_client(App* app, int fd) {
    int one = 1; setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sb = (int)app->cfg.snd_buf; setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sb, sizeof(sb));
    std::string path;
    if (!read_request(fd, path)) { close(fd); return; }
    if (path == "/" || path.empty()) {
        http_text(fd, 200, "OK",
            std::string(PROG) + " 运行中 (Pi4 IPTV 代理 + 按需硬件转码)\n\n"
            "用法: 把现有 m3u 里的  http://HOST:PORT/...  改成\n"
            "      http://<本机>:" + std::to_string(app->cfg.listen_port) + "/HOST:PORT/...\n\n"
            "例:   http://192.168.1.2:8888/rtp/239.0.0.1:1234\n"
            "  ->  http://<本机>:" + std::to_string(app->cfg.listen_port) + "/192.168.1.2:8888/rtp/239.0.0.1:1234\n\n"
            "默认= 纯透传(原始质量)。需降码转码时末尾加 ?target-resolution=720p  (480p/540p/720p/1080p)\n");
        close(fd); return;
    }
    if (path == "/favicon.ico") { http_text(fd, 404, "Not Found", "no\n"); close(fd); return; }
    ChannelCfg cfg;
    if (!parse_target(path, app->cfg, cfg)) { http_text(fd, 400, "Bad Request", "bad path\n"); close(fd); return; }
    std::string hdr = "HTTP/1.1 200 OK\r\nContent-Type: video/mp2t\r\nCache-Control: no-cache\r\nConnection: close\r\n\r\n";
    if (!send_str(fd, hdr)) { close(fd); return; }
    Channel* ch = app->get_or_create(cfg);
    Subscriber sub(app->cfg.ring_buf_size);
    ch->add_sub(&sub);
    for (;;) { Buf b = sub.pop(); if (!b) break; if (!send_all(fd, b->data(), b->size())) break; }
    ch->remove_sub(&sub);
    close(fd);
}

static int g_listen_fd = -1;
static void on_signal(int) { g_stop.store(true); if (g_listen_fd >= 0) { shutdown(g_listen_fd, SHUT_RDWR); close(g_listen_fd); g_listen_fd = -1; } }

static void daemonize() {
    pid_t pid = fork(); if (pid < 0) _exit(1); if (pid > 0) _exit(0);
    setsid();
    pid = fork(); if (pid < 0) _exit(1); if (pid > 0) _exit(0);
    if (chdir("/") != 0) {}
    int fd = open("/dev/null", O_RDWR);
    if (fd >= 0) { dup2(fd, 0); dup2(fd, 1); dup2(fd, 2); if (fd > 2) close(fd); }
}
static void write_pidfile(const std::string& path) { std::ofstream f(path); if (f) f << getpid() << "\n"; }
static void usage() {
    printf("%s — Pi4 IPTV 代理 + 按需硬件转码 (msd_lite 风格)\n", PROG);
    printf("用法: %s [-c 配置文件] [-d] [-p pid文件] [-v] [-h]\n", PROG);
    printf("  -c FILE  配置文件 (默认 %s)\n  -d  后台\n  -p FILE  pid文件\n  -v  详细\n  -h  帮助\n", DEFAULT_CONF);
}

int main(int argc, char** argv) {
    std::string cfg_path = DEFAULT_CONF, pidfile; bool daemon = false; int opt;
    while ((opt = getopt(argc, argv, "c:dp:vh")) != -1) {
        switch (opt) {
            case 'c': cfg_path = optarg; break;
            case 'd': daemon = true; break;
            case 'p': pidfile = optarg; break;
            case 'v': break;
            case 'h': usage(); return 0;
            default:  usage(); return 1;
        }
    }
    App app; std::string err;
    if (!load_config(cfg_path, app.cfg, err)) { fprintf(stderr, "%s: 配置错误: %s\n", PROG, err.c_str()); return 1; }
    if (app.cfg.hw < 0) app.cfg.hw = (access("/dev/video11", F_OK) == 0) ? 1 : 0;  // 自动: 有 bcm2835 编码器=硬, 否则软(x86)
    // 防御性下限: 坏/空配置不能把缓冲设成 0 (否则单客户端 cap=0 会被秒踢)
    if (app.cfg.ring_buf_size < 1u * 1024 * 1024) app.cfg.ring_buf_size = 8u * 1024 * 1024;
    if (app.cfg.read_block < 4096) app.cfg.read_block = 16384;
    if (app.cfg.precache >= app.cfg.ring_buf_size) app.cfg.precache = app.cfg.ring_buf_size / 2;

    int fd = socket(AF_INET, SOCK_STREAM, 0); int one = 1;
    setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    sockaddr_in addr{}; addr.sin_family = AF_INET; addr.sin_addr.s_addr = INADDR_ANY; addr.sin_port = htons(app.cfg.listen_port);
    if (bind(fd, (sockaddr*)&addr, sizeof(addr)) != 0) { perror("bind"); return 1; }
    if (listen(fd, 128) != 0) { perror("listen"); return 1; }

    if (daemon) daemonize();
    if (!pidfile.empty()) write_pidfile(pidfile);
    signal(SIGPIPE, SIG_IGN); signal(SIGINT, on_signal); signal(SIGTERM, on_signal);
    g_listen_fd = fd;

    std::string dh = app.cfg.def.height > 0 ? std::to_string(app.cfg.def.height) + "p转码" : "纯透传(原始)";
    logmsg(std::string(PROG) + " 监听 :" + std::to_string(app.cfg.listen_port) + "  默认=" + dh +
           "  转码=" + std::string(app.cfg.hw == 1 ? "硬件(Pi)" : "软件(libx264)") +
           "  ringBuf=" + std::to_string(app.cfg.ring_buf_size / 1024 / 1024) + "M precache=" +
           std::to_string(app.cfg.precache / 1024 / 1024) + "M  空闲" + std::to_string(app.cfg.idle_timeout) + "s");
    logmsg("用法: http://<pi-ip>:" + std::to_string(app.cfg.listen_port) + "/<上游host:port>/rtp/<组播>:<端口>  [?target-resolution=720p]");

    while (!g_stop.load()) {
        int cfd = accept(fd, nullptr, nullptr);
        if (cfd < 0) { if (g_stop.load()) break; continue; }
        std::thread(handle_client, &app, cfd).detach();
    }
    logmsg("退出中..."); app.stop_all();
    if (!pidfile.empty()) unlink(pidfile.c_str());
    return 0;
}
