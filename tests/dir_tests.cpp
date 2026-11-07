#include <co2/co2.hpp>
#include <string>
#include "test.hpp"
#include "uvco/uvco.hpp"
namespace {
struct Fixture {
 std::string path;
 Fixture(){uv_fs_t req{};std::string pattern=std::string{UVCO_TEST_DIR_PARENT}+"/dir-XXXXXX";int s=uv_fs_mkdtemp(nullptr,&req,pattern.c_str(),nullptr);if(s==0)path=req.path;uv_fs_req_cleanup(&req);CHECK(s==0);
 for(int i=0;i<70;++i){std::string file=path+"/f"+std::to_string(i);int fd=uv_fs_open(nullptr,&req,file.c_str(),O_CREAT|O_WRONLY,0600,nullptr);uv_fs_req_cleanup(&req);CHECK(fd>=0);uv_fs_close(nullptr,&req,fd,nullptr);uv_fs_req_cleanup(&req);}}
 ~Fixture(){uv_fs_t req{};for(int i=0;i<70;++i){std::string file=path+"/f"+std::to_string(i);uv_fs_unlink(nullptr,&req,file.c_str(),nullptr);uv_fs_req_cleanup(&req);}uv_fs_rmdir(nullptr,&req,path.c_str(),nullptr);uv_fs_req_cleanup(&req);}
};
auto scan(uvco::Loop& loop,std::string path)
 CO2_BEG(co2::Task<int>,(loop,path),uvco::Result<uvco::fs::Dir> dir;uvco::Result<std::vector<uvco::fs::DirEntry>> entries;int count=0;) {
 CO2_AWAIT_SET(dir,uvco::fs::openDir(loop,std::move(path)));if(!dir)CO2_RETURN(-1);
 for(;;){CO2_AWAIT_SET(entries,dir->readDir(7));if(!entries || entries->size()>7)CO2_RETURN(-2);if(entries->empty())break;count+=static_cast<int>(entries->size());}
 if(!dir->close())CO2_RETURN(-3);
 CO2_RETURN(count);
} CO2_END
auto canceled(uvco::Loop& loop,std::string path) CO2_BEG(co2::Task<uvco::Result<uvco::fs::Dir>>,(loop,path),uvco::Result<uvco::fs::Dir> r;) {
 CO2_AWAIT_SET(r,uvco::fs::openDir(loop,std::move(path)));CO2_RETURN(std::move(r));
} CO2_END
}
TEST_CASE("Dir reads bounded batches until empty and closes with RAII") {uvco::Loop loop;Fixture files;CHECK(uvco::run(loop,scan(loop,files.path))==70);}
TEST_CASE("Dir open pre-cancel is Result canceled") {uvco::Loop loop;Fixture files;co2::stop_source stop;stop.request_stop();auto r=uvco::run(loop,canceled(loop,files.path),stop.get_token());CHECK(!r);CHECK(r.error()==uvco::Errc::canceled);}

namespace {
auto consume(uvco::fs::ReadDirOp op) CO2_BEG(co2::Task<uvco::Result<std::vector<uvco::fs::DirEntry>>>,(op),uvco::Result<std::vector<uvco::fs::DirEntry>> r;) {
 CO2_AWAIT_SET(r,op);CO2_RETURN(std::move(r));
} CO2_END
auto leased(uvco::Loop& loop,std::string path,bool explicitClose)
 CO2_BEG(co2::Task<int>,(loop,path,explicitClose),uvco::Result<uvco::fs::Dir> dir;
 co2::Task<uvco::Result<std::vector<uvco::fs::DirEntry>>> next;uvco::Result<std::vector<uvco::fs::DirEntry>> entries;) {
 CO2_AWAIT_SET(dir,uvco::fs::openDir(loop,std::move(path)));if(!dir)CO2_RETURN(-1);
 next=consume(dir->readDir(7));if(explicitClose){if(!dir->close())CO2_RETURN(-2);}else *dir=uvco::fs::Dir{};
 CO2_AWAIT_SET(entries,std::move(next));
 if(explicitClose)CO2_RETURN(!entries && entries.error()==uvco::Errc::bad_descriptor ? 0 : -3);
 CO2_RETURN(entries?static_cast<int>(entries->size()):-4);
} CO2_END
}
TEST_CASE("Dir operation lease survives owner destruction but explicit close invalidates it") {
 uvco::Loop loop;Fixture files;CHECK(uvco::run(loop,leased(loop,files.path,false))==7);CHECK(uvco::run(loop,leased(loop,files.path,true))==0);
}
UVCO_TEST_MAIN

