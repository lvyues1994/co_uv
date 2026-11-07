#include <cerrno>
#include <csignal>
#include <chrono>
#include <iostream>
#include <thread>
#include <sys/wait.h>
#include <unistd.h>
namespace {volatile sig_atomic_t stopped=0;void stop(int){stopped=1;}}
int main() {
    pid_t const child=fork();if(child<0)return 2;
    if(child==0){for(;;)pause();}
    struct sigaction action{};action.sa_handler=stop;sigemptyset(&action.sa_mask);
    if(sigaction(SIGTERM,&action,nullptr)!=0){kill(child,SIGKILL);waitpid(child,nullptr,0);return 3;}
    std::cout<<"ready\n"<<std::flush;
    while(!stopped)std::this_thread::sleep_for(std::chrono::milliseconds{1});
    int status=0;pid_t result;
    do{result=waitpid(child,&status,0);}while(result<0 && errno==EINTR);
    if(result!=child || !WIFSIGNALED(status) || WTERMSIG(status)!=SIGTERM)return 4;
    std::cout<<"reaped\n"<<std::flush;return 0;
}
