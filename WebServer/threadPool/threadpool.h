#ifndef THREADPOOL_H
#define THREADPOOL_H
#include <pthread.h>
#include <list>
#include <exception>
#include <cstdio>
#include "locker.h"
using namespace std;

// 模板T就是任务类，为了代码复用
template <typename T>
class ThreadPool
{
private:
    // 线程数量
    int m_thread_number;
    // 线程池
    pthread_t *m_threads;

    // 工作队列(放的到来的任务)
    list<T *> m_workQueue;
    // 允许的最多请求数量
    int m_max_requests;

    // 请求队列由所有线程共享，因此需要互斥锁来保护访问
    Locker m_mutex_workQueue;
    // 信号量，用来判断是否有任务需要处理（应该是表示请求队列的空间，那岂不是和m_workQueue.empty()作用重复了）
    Sem m_workQueueStat;  //这个信号量在操作的时候用上锁吗？（不需要，信号量的操作本身是原子操作）

    // 是否结束线程
    bool m_stop;
private:
    static void* worker(void *arg);
    //线程要做的事情（被work调用，感觉可以和worker写成一个函数）
    void run();  

public:
    ThreadPool(int thread_number = 8, int max_request = 10000);
    ~ThreadPool();

    //将到来的请求事件（在这里是HTTP连接）添加到工作队列中
    bool append(T *request);
};

template <typename T>
ThreadPool<T>::ThreadPool(int thread_number, int max_request) : m_thread_number(thread_number), m_max_requests(max_request),
                                                                m_stop(false), m_threads(NULL)
{
    if (thread_number <= 0 || max_request <= 0)
    {
        printf("thread_number should be greater than 0, or request count is greater than max_request\n");
        throw exception();
    }
    m_threads = new pthread_t[m_thread_number];
    if (!m_threads)
    {
        printf("new pthread_t failed\n");
        throw exception();
    }
    //创建m_thread_number个线程放入线程池，并设置线程脱离
    for(int i=0; i<m_thread_number; i++){
        printf("create the %dth thread\n", i+1);
        if(pthread_create(m_threads+i, NULL, worker, this)!=0){
            delete[] m_threads;
            throw exception();
        }
        //设置线程脱离
        if(pthread_detach(m_threads[i])!=0){
            delete[] m_threads;
            throw exception();
        }

    }
}
template<typename T>
ThreadPool<T>::~ThreadPool(){
    delete[] m_threads;
    m_stop=true;
}

template<typename T>
bool ThreadPool<T>::append(T *request){
    m_mutex_workQueue.lock();
    if(m_workQueue.size()>=m_max_requests){
        m_mutex_workQueue.unlock();
        return false;
    }
    m_workQueue.push_back(request);
    m_mutex_workQueue.unlock();
    m_workQueueStat.post();
    return true;
}

template<typename T>
void ThreadPool<T>::run(){
    while(!m_stop){
        m_workQueueStat.wait();
        m_mutex_workQueue.lock();  //任务队列上锁，各线程互斥访问
        //从任务队列中取出任务
        T *request=m_workQueue.front();
        m_workQueue.pop_front();
        m_mutex_workQueue.unlock();
        if(!request){
            continue;
        }
        request->process();
    }
}

template<typename T>
void* ThreadPool<T>::worker(void *arg){
    ThreadPool *mypool=(ThreadPool *)arg;
    mypool->run();
    return mypool;  //为什么要return以下mypool呢？
}

#endif