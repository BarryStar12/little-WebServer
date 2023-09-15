#ifndef HTTP_CONNECT_H
#define HTTP_CONNECT_H
#include <arpa/inet.h>
#include <sys/epoll.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <stdarg.h>
#include <errno.h>
#include <sys/uio.h>
#include <string.h>
#include "../threadPool/locker.h"
#include "../threadPool/threadpool.h"
#include "skiplist.h"

extern SkipList<int, string> skipList;

class HttpConn
{
public:
    static int m_epfd;  //所有的socket上的事件都被注册到同一个epoll实例上
    static int m_user_count;  //统计连接进来的用户数量
    static const int READ_BUFFER_SIZE=2048;  //读缓冲区大小
    static const int WRITE_BUFFER_SIZE=2048;  //写缓冲区大小
    static const int FILENAME_LEN=200;  //文件名的最大长度

    //HTTP请求方法（目前只支持GET）
    enum METHOD{GET=0, POST, HEAD, PUT, DELETE, TRACE, OPTIONS, CONNECT};

    /** 解析客户端请求时，主状态机的状态
     * CHECK_STATE_REQUESTLINE:当前正在分析请求行
     * CHECK_STATE_HEADER:当前正在分析头部字段
     * CHECK_STATE_CONTENT:当前正在解析请求体
    */
    enum CHECK_STATE{CHECK_STATE_REQUESTLINE=0, CHECK_STATE_HEADER, CHECK_STATE_CONTENT};
    
    /** 解析客户端请求时，从状态机的状态
     * LINE_OK：读取到一个完整的行
     * LINE_BAD：行出错
     * LINE_OPEN：正在检测行中的数据，还没有检测完成
    */
    enum LINE_STATUS{LINE_OK, LINE_BAD, LINE_OPEN};

    /** 服务器处理HTTP请求的可能结果，报文解析的结果
     * NO_REQUEST           ：请求不完整，需要继续读取客户端数据
     * GET_REQUEST          : 表示获得了一个完整的客户请求
     * BAD_REQUEST          : 表示客户端请求语法错误
     * NO_RESOURCE          ：表示服务器没有资源
     * FORBIDDEN_REQUEST    ：表示客户对资源没有足够的访问权限
     * FILE_REQUEST         ：文件请求，获取文件成功
     * INTERNAL_ERROR       ：表示服务器内部错误
     * CLOSED_CONNECTION    ：表示客户端已经关闭连接
    */
    enum HTTP_CODE{NO_REQUEST, GET_REQUEST, BAD_REQUEST, NO_RESOURCE, FORBIDDEN_REQUEST, 
    FILE_REQUEST, INTERNAL_ERROR, CLOSED_CONNECTION};

    HttpConn();
    ~HttpConn();
    //初始化客户端的sockfd和socket地址、设置端口复用、将sockfd添加到epoll实例中、客户端数目+1
    void init(int sockfd, sockaddr_in & clientAddr);
    //将某个客户端连接关闭（将客户端的传输问及那描述符从epoll实例中删除、关闭该文件描述符、将该文件描述符变量置-1、客户端数目-1）
    void close_conn();
    //一次性读完文件描述符中所有到来的数据(非阻塞)，成功则返回true，失败返回false
    bool read();
    //一次性将要发送给客户端的所有数据全部发送(非阻塞)，成功则返回true，失败则返回false
    bool write();
    void process();  //处理客户端的请求(由线程池中的工作线程调用，处理HTTP请求的入口函数)
    
private:
    /* data */
    int m_sockfd;  //该HTTP连接的socket（其实就是accept返回的那个用于通信的文件描述符）
    sockaddr_in m_address;  //通信的socket地址（其实就是clientAddr，保存连接进来的客户端地址(IP地址，端口号)）
    char m_read_buf[READ_BUFFER_SIZE];  //读缓冲区
    int m_read_index;  //标识读缓冲区中已经读入的客户端数据的最后一个字节的下一个位置

    char m_write_buf[WRITE_BUFFER_SIZE];  //写缓冲区
    int m_write_index;  //写缓冲区中待发送的字节数
    char *m_file_address;  //客户请求的目标文件被mmap到内存中的起始位置
    struct stat m_file_stat;  //目标文件的状态（用于判断文件是否存在，是否为目录，是否可读，并获取文件大小等信息）
    struct iovec m_iv[2];  //我们将采用writev来执行写操作，其中m_iv_count表示被写内存块的数量
    int m_iv_count;  //被写内存块的数量

    int m_checked_index;  //当前正在分析的字符在读缓冲区中的位置
    int m_start_line;  //当前正在解析的行的起始位置
    CHECK_STATE m_check_state;  //主状态机当前所处的状态
    char *m_url;  //请求目标文件的文件名
    char *m_version;  //协议版本，目前只支持HTTP1.1
    METHOD m_method;  //请求方法
    char *m_host;  //主机名
    bool m_linker;  //HTTP请求是否要保持连接
    int m_content_length;
    char m_real_file[200];  //客户请求的目标文件的完整路径，其内容等于doc_root+m_url，doc_root是网站根
    void init();  //初始化连接其余的信息

    int cgi;  //是否启用POST
    char *m_string;  //存储请求头数据
    int bytes_to_send;
    int bytes_have_send;
    //char *doc_root;

    
//下面一组函数被process_read调用来解析并处理HTTP请求
    LINE_STATUS parse_line();  //解析每一行（可以认为是获取一行）
    HTTP_CODE parse_request_line(char *text);  //解析请求首行
    HTTP_CODE parse_header(char *text);  //解析请求头
    HTTP_CODE parse_content(char *text);  //解析请求体
    HTTP_CODE process_read();  //解析HTTP请求

    //获取一行数据（内联函数）
    char *get_line(){return m_read_buf+m_start_line;};
    /*具体处理请求（当得到一个完整、正确的HTTP请求时，我们就分析目标文件的属性）
    如果目标文件存在、对所有用户可读、且不是目录，则使用mmap将其映射到内存地址m_file)address处
    并告诉调用者获取问及那成功
    */
    HTTP_CODE do_request();

//下面一组函数被process_write调用以填充HTTP应答
    void unmap();
    bool add_response( const char* format, ... );
    bool add_content( const char* content );
    bool add_content_type();
    bool add_status_line( int status, const char* title );
    bool add_headers( int content_length );
    bool add_content_length( int content_length );
    bool add_linger();
    bool add_blank_line();
    bool process_write(HTTP_CODE ret);  //填充HTTP应答
};




#endif