#pragma once

#include <chrono>
#include <memory>
#include <string>
#include <utility>
#include <co2/co2.hpp>
#include "test.hpp"
#include "uvco/uvco.hpp"

namespace watcher_test {
using namespace std::chrono_literals;

struct Files {
    std::string path;
    Files() {
        uv_fs_t req{};
        auto pattern = std::string{UVCO_TEST_WATCH_DIR} + "/watch-XXXXXX";
        int const s = uv_fs_mkdtemp(nullptr, &req, pattern.c_str(), nullptr);
        if (s == 0) path = req.path;
        uv_fs_req_cleanup(&req);
        CHECK(s == 0);
    }
    std::string file(std::string const& name) const { return path + "/" + name; }
    void write(std::string const& name, std::string const& data, bool append = false) {
        uv_fs_t req{};
        int const flags = UV_FS_O_CREAT | UV_FS_O_WRONLY |
                          (append ? UV_FS_O_APPEND : UV_FS_O_TRUNC);
        int const fd = uv_fs_open(nullptr, &req, file(name).c_str(), flags, 0600, nullptr);
        uv_fs_req_cleanup(&req);
        CHECK(fd >= 0);
        uv_buf_t buf = uv_buf_init(const_cast<char*>(data.data()),
                                   static_cast<unsigned int>(data.size()));
        auto const count = uv_fs_write(nullptr, &req, fd, &buf, 1, -1, nullptr);
        uv_fs_req_cleanup(&req);
        int const closed = uv_fs_close(nullptr, &req, fd, nullptr);
        uv_fs_req_cleanup(&req);
        CHECK(count >= 0 && static_cast<std::size_t>(count) == data.size());
        CHECK(closed == 0);
    }
    void rename(std::string const& oldName, std::string const& newName) {
        uv_fs_t req{};
        int const s = uv_fs_rename(nullptr, &req, file(oldName).c_str(),
                                  file(newName).c_str(), nullptr);
        uv_fs_req_cleanup(&req);
        CHECK(s == 0);
    }
    void remove(std::string const& name) {
        uv_fs_t req{};
        int const s = uv_fs_unlink(nullptr, &req, file(name).c_str(), nullptr);
        uv_fs_req_cleanup(&req);
        CHECK(s == 0);
    }
    ~Files() {
        uv_fs_t req{};
        if (uv_fs_scandir(nullptr, &req, path.c_str(), 0, nullptr) >= 0) {
            uv_dirent_t entry{};
            while (uv_fs_scandir_next(&req, &entry) == 0) {
                uv_fs_t del{};
                static_cast<void>(uv_fs_unlink(nullptr, &del, file(entry.name).c_str(), nullptr));
                uv_fs_req_cleanup(&del);
            }
        }
        uv_fs_req_cleanup(&req);
        static_cast<void>(uv_fs_rmdir(nullptr, &req, path.c_str(), nullptr));
        uv_fs_req_cleanup(&req);
    }
};

template<class Watcher>
auto next(uvco::Loop& loop, Watcher& watcher)
    CO2_BEG(co2::Task<uvco::Result<typename Watcher::Info>>, (loop, watcher),
            uvco::Result<typename Watcher::Info> result;) {
    CO2_AWAIT_SET(result, uvco::timeout(loop, 5s, watcher.next()));
    CO2_RETURN(std::move(result));
} CO2_END

template<class Watcher>
auto rawNext(Watcher& watcher)
    CO2_BEG(co2::Task<uvco::Result<typename Watcher::Info>>, (watcher),
            uvco::Result<typename Watcher::Info> result;) {
    CO2_AWAIT_SET(result, watcher.next());
    CO2_RETURN(std::move(result));
} CO2_END

template<class Awaiter, class Info>
auto held(Awaiter awaiter)
    CO2_BEG(co2::Task<uvco::Result<Info>>, (awaiter), uvco::Result<Info> result;) {
    CO2_AWAIT_SET(result, awaiter);
    CO2_RETURN(std::move(result));
} CO2_END

inline void pump(uvco::Loop& loop) { static_cast<void>(uv_run(loop.raw(), UV_RUN_NOWAIT)); }
#if defined(__linux__)
inline int fdCount() {
    uv_fs_t req{};
    int const count = uv_fs_scandir(nullptr, &req, "/proc/self/fd", 0, nullptr);
    uv_fs_req_cleanup(&req);
    CHECK(count >= 0);
    return count;
}
#endif
} // namespace watcher_test
