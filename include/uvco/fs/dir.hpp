#pragma once
#include <array>
#include <memory>
#include "uvco/fs/fs.hpp"
namespace uvco { namespace fs {
namespace detail {
inline int closeDirSync(uv_dir_t* dir) noexcept {
    uv_fs_t req{};int status=uv_fs_closedir(nullptr,&req,dir,nullptr);uv_fs_req_cleanup(&req);return status;
}
struct DirState {
    Loop* loop;uv_dir_t* dir;bool busy=false;std::array<uv_dirent_t,64> entries{};
    DirState(Loop& l,uv_dir_t* d):loop{&l},dir{d}{}
    ~DirState(){if(dir)static_cast<void>(closeDirSync(dir));}
};
struct ReadDirSpec {
    using value_type=std::vector<DirEntry>;
    std::shared_ptr<DirState> state;std::size_t batch=64;bool active=false;
    int submit(uv_loop_t* l,uv_fs_t* req,uv_fs_cb cb) {
        if(!state || !state->dir)return UV_EBADF;
        UVCO_CONTRACT_CHECK(!state->busy);
        if(batch==0 || batch>state->entries.size())return UV_EINVAL;
        state->busy=true;active=true;state->dir->dirents=state->entries.data();state->dir->nentries=batch;
        return uv_fs_readdir(l,req,state->dir,cb);
    }
    static std::vector<DirEntry> project(uv_fs_t& req,Loop&) {
        auto* dir=static_cast<uv_dir_t*>(req.ptr);std::vector<DirEntry> out;
        out.reserve(static_cast<std::size_t>(req.result));
        for(std::size_t i=0;i<static_cast<std::size_t>(req.result);++i)out.push_back(DirEntry{dir->dirents[i].name,dir->dirents[i].type});
        return out;
    }
    void completed(uv_fs_t&) noexcept {if(active){state->busy=false;active=false;}}
};
} // namespace detail
using ReadDirOp=FsOp<detail::ReadDirSpec>;
class Dir {
  public:
    Dir()=default;
    Dir(Loop& loop,uv_dir_t* native):state_{std::make_shared<detail::DirState>(loop,native)}{}
    Dir(Dir&&)=default;Dir& operator=(Dir&&)=default;
    Dir(Dir const&)=delete;Dir& operator=(Dir const&)=delete;
    bool isOpen() const noexcept{return state_ && state_->dir;}
    // Last owner closes synchronously. Unstarted/read-in-flight operations hold
    // a state lease. Explicit close requires no read in flight and invalidates
    // even previously constructed operations (which then report EBADF).
    Result<void> close() noexcept {
        if(!isOpen())return {};
        UVCO_CONTRACT_CHECK(!state_->busy);
        return fromStatus(detail::closeDirSync(std::exchange(state_->dir,nullptr)));
    }
    ReadDirOp readDir(std::size_t batch=64) {
        UVCO_CONTRACT_CHECK(state_!=nullptr);
        return ReadDirOp{*state_->loop,state_,batch};
    }
  private:
    std::shared_ptr<detail::DirState> state_;
};
namespace detail {
struct OpenDirSpec {
    using value_type=Dir;std::string path;
    int submit(uv_loop_t* l,uv_fs_t* r,uv_fs_cb cb)const{return uv_fs_opendir(l,r,path.c_str(),cb);}
    static Dir project(uv_fs_t& r,Loop& loop){return Dir{loop,static_cast<uv_dir_t*>(r.ptr)};}
    static void release(uv_fs_t& r) noexcept {static_cast<void>(closeDirSync(static_cast<uv_dir_t*>(r.ptr)));r.ptr=nullptr;}
};
} // namespace detail
using OpenDirOp=FsOp<detail::OpenDirSpec>;
inline OpenDirOp openDir(Loop& loop,std::string path){return OpenDirOp{loop,std::move(path)};}
inline ReadDirOp readDir(Dir& dir,std::size_t batch=64){return dir.readDir(batch);}
} } // namespace uvco::fs
