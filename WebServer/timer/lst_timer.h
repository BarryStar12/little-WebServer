#ifndef LST_TIMER_H
#define LST_TIMER_H

#include <unistd.h>
#include <signal.h>
#include <sys/types.h>
#include <sys/epoll.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <sys/stat.h>
#include <string.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/wait.h>
#include <sys/uio.h>
//#include "./src/http_connect.h"
extern SkipList<int, string> skipList;
extern void setNonBlock(int fd);
extern void addFd(int epfd, int fd, bool one_shot, bool enable_ET);

extern void addsig(int sig, sighandler_t handler, bool restart);

struct client_data;

class util_timer{
    public:
        util_timer():prev(NULL), next(NULL){}
    public:
        time_t expire;
        void (* cb_func)(client_data *);
        client_data *user_data;
        util_timer *prev;
        util_timer *next;
};

struct client_data{
    sockaddr_in address;
    int sockfd;
    util_timer *timer;
};

class sort_timer_lst{
    public:
        sort_timer_lst():head(NULL), tail(NULL){};
        ~sort_timer_lst();
        void add_timer(util_timer *timer);
        void adjust_timer(util_timer *timer);
        void del_timer(util_timer *timer);
        void tick();
    private:
        void add_timer(util_timer *timer, util_timer *lst_head);
        util_timer *head;
        util_timer *tail;
};

class Utils{
    public:
        Utils();
        ~Utils();
        void init(int timeslot);
        //信号处理函数
        static void sig_handler(int sig);
        //定时处理任务，重新定时以不断触发SIGALRM信号
        void timer_handler();
        void show_error(int connfd, const char *info);
    public:
        static int u_pipefdIN;
        static int u_pipefdOUT;
        sort_timer_lst m_timer_lst;
        static int u_epollfd;
        int m_TIMERSLOT;
};
int Utils::u_pipefdIN=0;
int Utils::u_pipefdOUT=0;
int Utils::u_epollfd=0;

//信号处理函数
void Utils::sig_handler(int sig){
    skipList.dump_file();
    //为了保证函数的可重入性，保留原来的errno
    int save_errno=errno;
    int msg=sig;
    send(u_pipefdOUT, (char *)&msg, 1, 0);
    errno=save_errno;
}

Utils::Utils(){}
Utils::~Utils(){};

sort_timer_lst::~sort_timer_lst(){
    util_timer *tmp=head;
    while(tmp){
        head=tmp->next;
        delete tmp;
        tmp=head;
    }
}

void sort_timer_lst::add_timer(util_timer *timer, util_timer *lst_head){
    util_timer *prev=lst_head;
    util_timer *tmp=prev->next;
    while(tmp){
        if(timer->expire<tmp->expire){
            prev->next=timer;
            timer->next=tmp;
            tmp->prev=timer;
            timer->prev=prev;
            break;
        }
        prev=tmp;
        tmp=tmp->next;
    }
    if(!tmp){
        prev->next=timer;
        timer->prev=prev;
        timer->next=NULL;
        tail=timer;
    }

}

void sort_timer_lst::add_timer(util_timer *timer){
    if(!timer) return;
    if(!head){
        head=tail=timer;
        return;
    }
    if(timer->expire<head->expire){
        timer->next=head;
        head->prev=timer;
        head=timer;
        return;
    }
    add_timer(timer, head);
}

void sort_timer_lst::adjust_timer(util_timer *timer){  //修改节点（删掉一个，在同一位置新增一个）
    if(!timer) return;
    util_timer *tmp=timer->next;
    if(!tmp||(timer->expire<tmp->expire)) return;
    if(timer==head){
        head=head->next;
        head->prev=NULL;
        timer->next=NULL;
        add_timer(timer, head);
    }else{
        timer->prev->next = timer->next;
        timer->next->prev = timer->prev;
        add_timer(timer, timer->next);
    }
}

void sort_timer_lst::del_timer(util_timer *timer)
{
    if (!timer)
    {
        return;
    }
    if ((timer == head) && (timer == tail))
    {
        delete timer;
        head = NULL;
        tail = NULL;
        return;
    }
    if (timer == head)
    {
        head = head->next;
        head->prev = NULL;
        delete timer;
        return;
    }
    if (timer == tail)
    {
        tail = tail->prev;
        tail->next = NULL;
        delete timer;
        return;
    }
    timer->prev->next = timer->next;
    timer->next->prev = timer->prev;
    delete timer;
}

void sort_timer_lst::tick(){
    if(!head) return;
    time_t cur=time(NULL);
    util_timer *tmp=head;
    while(tmp){
        if(cur<tmp->expire) break;
        tmp->cb_func(tmp->user_data);
        head=tmp->next;
        if(head) head->prev=NULL;
        delete tmp;
        tmp=head;
    }
}

void Utils::init(int timeslot){
    m_TIMERSLOT=timeslot;
}



//定时处理任务
void Utils::timer_handler(){
    m_timer_lst.tick();
    alarm(m_TIMERSLOT);
}

void Utils::show_error(int connfd, const char *info){
    send(connfd, info, strlen(info), 0);
    close(connfd);
}

#endif