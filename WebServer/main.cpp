#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <error.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include <signal.h>
#include "./threadPool/locker.h"
#include "./threadPool/threadpool.h"
#include "./src/http_connect.h"
#include "./src/skiplist.h"
#include "./timer/lst_timer.h"
using namespace std;

//#define TIMESLOT 5
#define MAX_FD 65535  //支持的最大客户端数目
#define MAX_EVENTS_NUM 10000  //监听最大的事件数量
//定义一个信号处理函数，处理产生的信号（如：一个端口已经关闭，若还有数据要写入，就会产生SIGPIPE信号）
//也可以将函数指针直接写进去作为参数:
//void addsig(int sig, void(handler)(int)){}  这里的handler前到底加不加*（一会儿测试一下）
void addsig(int sig, sighandler_t handler, bool restart){
    struct sigaction sa;
    memset(&sa, '\0', sizeof(sa));
    sa.sa_handler=handler;
    if(restart) sa.sa_flags|=SA_RESTART;
    sigfillset(&sa.sa_mask);
    sigaction(sig, &sa, NULL);
}

void cb_func(client_data *user_data){
    epoll_ctl(Utils::u_epollfd, EPOLL_CTL_DEL, user_data->sockfd, 0);
    assert(user_data);
    close(user_data->sockfd);
    HttpConn::m_user_count--;
}

//extern void addFd(int epfd, int fd, bool one_shot, bool enable_ET);
extern void removeFd(int epfd, int fd);
extern void modFd(int epfd, int fd, int ev);

mutex mtx;     // mutex for critical section
string delimiter = ":";
SkipList<int, std::string> skipList(18);


int main(int argc, char* argv[]){  //指针数组，数组元素全为指针(通过命令行传递端口号)
    if(argc<=1){  //直接运行程序不传参数时，argc的值为1
        printf("please input argument as: %s: post_name\n", basename(argv[0]));
        exit(-1);
    }
    skipList.load_file();
    //定时
    Utils utiltime;
    //static int epollfd=0;
    //~定时
    //获取端口号
    int portNum=atoi(argv[1]);

    addsig(SIGPIPE, SIG_IGN, false);  //捕捉到SIGPIPE信号后忽略它，原本的回调函数用SIG_IGN替代
    //创建线程池，初始化线程池
    ThreadPool<HttpConn> * pool=NULL;
    try{
        printf("initing thread pool...\n");
        pool=new ThreadPool<HttpConn>;
    }catch(...){
        printf("init thread pool failed...\n");
        exit(-1);
    }

    //创建一个数组用于保存所有的客户端信息（客户端的任务）
    HttpConn *users=new HttpConn[MAX_FD];

    /*1、创建监听套接字*/
    int listenFd=socket(AF_INET, SOCK_STREAM, 0);
    if(listenFd==-1){
        perror("server socket");
        exit(-1);
    }
    //设置端口复用(在bind之前设置端口复用，防止服务器突然退出导致的端口暂时不可用)
    int reuse=1;
    setsockopt(listenFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

    /*2、绑定*/
    struct sockaddr_in serverAddr;
    serverAddr.sin_addr.s_addr=INADDR_ANY;
    serverAddr.sin_port=htons(portNum);
    serverAddr.sin_family=AF_INET;
    bind(listenFd, (sockaddr *)&serverAddr, sizeof(serverAddr));
    listen(listenFd, 128);

    /*3、在内核中创建一个epoll实例*/
    int epfd=epoll_create(100);  //这个size只要大于0就可以，没有别的意义。
    
    //存放将来要监听的文件描述符相关的监测信息
    struct epoll_event epevs[MAX_EVENTS_NUM];

    /*4、将监听的文件描述符相关的监测信息添加到epoll实例中*/
    addFd(epfd, listenFd, false, false);  //监听的文件描述符不能设置EPOLLONESHOT，否则主线程只能处理一个客户端连接，因为后续的客户端连接请求将不再触发listenFd上的EPOLLIN事件
    HttpConn::m_epfd=epfd;

    //定时
    //创建管道
    int pipefd[2];
    int rettime=socketpair(PF_UNIX, SOCK_STREAM, 0, pipefd);
    utiltime.u_pipefdIN=pipefd[0];
    utiltime.u_pipefdOUT=pipefd[1];
    utiltime.m_TIMERSLOT=5;
    assert(rettime!=-1);
    setNonBlock(utiltime.u_pipefdOUT);
    addFd(epfd, utiltime.u_pipefdIN, false, true);
    //设置信号处理函数
    addsig(SIGALRM, utiltime.sig_handler, true);
    //bool stop_server=false;
    client_data* userstime=new client_data[MAX_FD];
    bool timeout=false;
    alarm(utiltime.m_TIMERSLOT);
    
    //~定时

    while(true){
        //num为变化的文件描述符的个数
        int num=epoll_wait(epfd, epevs, MAX_EVENTS_NUM, -1);
        if(num<0&&errno!=EINTR){  //不是因为被中断导致的失败
            printf("epoll failed...\n");
            break;
        }
        //循环遍历事件（发生变化的文件描述符）数组
        for(int i=0; i<num; i++){
            int curfd=epevs[i].data.fd;
            if(curfd==listenFd){
                //监听的文件描述符发生变化，说明有新客户端连接进来
                struct sockaddr_in clientAddr;
                socklen_t clientAddrLen=sizeof(clientAddr);
                int cfd=accept(listenFd, (struct sockaddr *)&clientAddr, &clientAddrLen);
                if(HttpConn::m_user_count>=MAX_FD){
                    //目前客户端连接数已达到最大
                    //给客户端写一个信息：服务器内部正忙。

                    close(cfd);
                    continue;
                }
                //将新的客户端数据初始化，放入数组中
                users[cfd].init(cfd, clientAddr);
                //定时
                // 创建定时器，设置其回调函数与超时时间，然后绑定定时器与用户数据，最后将定时器添加到链表timer_lst中
                util_timer *timer=new util_timer;
                timer->user_data=&userstime[cfd];
                timer->cb_func=cb_func;
                time_t cur=time(NULL);
                timer->expire=cur+3*utiltime.m_TIMERSLOT;
                userstime[cfd].timer=timer;
                utiltime.m_timer_lst.add_timer(timer);
                //~定时
                
            }else if(( curfd == pipefd[0] ) && ( epevs[i].events & EPOLLIN )){
                //定时
                int sig;
                char signals[1024];
                int ret1=recv(pipefd[0], signals, sizeof(signals), 0);
                if(ret1==-1){
                    continue;
                }else if(ret1==0){
                    continue;
                }else{
                    //接收到数据，肯定是SIGALRM信号
                    timeout=true;
                }
                //~定时
            }else if(epevs[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开或者错误等事件

                users[curfd].close_conn();
            }else if(epevs[i].events & EPOLLIN){  //检测到了读事件
                //客户端有消息发过来
                util_timer *timer=userstime[curfd].timer;
                if(users[curfd].read()){  //一次性把所有数据读出来（read返回读到了数据）
                    //将客户端连接事件添加到线程池的工作队列中
                    pool->append(users+curfd);
                    //定时
                    time_t cur=time(NULL);
                    timer->expire=cur+3*utiltime.m_TIMERSLOT;
                    utiltime.m_timer_lst.adjust_timer(timer);
                    //~定时
                }else{  //没读到数据
                    users[curfd].close_conn();
                    //定时
                    cb_func(&userstime[curfd]);
                    if(timer){
                        utiltime.m_timer_lst.del_timer(timer);
                    }
                    //~定时
                }

            }else if(epevs[i].events & EPOLLOUT){  //检测到了写事件
                //有消息要发送到客户端
                if(!users[curfd].write()){  //一次性写完所有数据
                    users[curfd].close_conn();
                }
            }
        }

        //最后处理定时事件，因为I/O事件具有更高的优先级
        if(timeout){
            utiltime.timer_handler();
            timeout=false;
        }
    }

    close(epfd);
    close(listenFd);
    delete[] users;
    delete pool;

    return 0;
}