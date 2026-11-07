#include <chrono>
#include <cstdlib>
#include <iostream>
#include <string>
#include <thread>
#include <uv.h>
int main(int argc,char** argv) {
 if(argc<2)return 2;
 std::string mode{argv[1]};
 if(mode=="exit")return argc>2 ? std::atoi(argv[2]) : 0;
 if(mode=="sleep") {
  double seconds=argc>2 ? std::atof(argv[2]) : 30;
  std::this_thread::sleep_for(std::chrono::duration<double>{seconds});return 0;
 }
 if(mode=="echo"){std::cout<<(argc>2 ? argv[2] : "")<<'\n';return 0;}
 if(mode=="duplex"){std::cout<<std::string(131072,'o')<<std::flush;std::cerr<<std::string(131072,'e')<<std::flush;std::cout<<std::cin.rdbuf();return 0;}
 if(mode=="cat"){std::cout<<std::cin.rdbuf();return 0;}
 if(mode=="envcwd") {
  char cwd[4096];std::size_t n=sizeof cwd;
  if(uv_cwd(cwd,&n)<0)return 3;
  char const* env=std::getenv("UVCO_TEST");std::cout<<(env?env:"")<<':'<<cwd;return 0;
 }
 return 2;
}
