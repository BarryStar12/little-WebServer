#include "http_connect.h"

int HttpConn::m_epfd=-1;  
int HttpConn::m_user_count=0;

// 定义HTTP响应的一些状态信息
const char* ok_200_title = "OK";
const char* error_400_title = "Bad Request";
const char* error_400_form = "Your request has bad syntax or is inherently impossible to satisfy.\n";
const char* error_403_title = "Forbidden";
const char* error_403_form = "You do not have permission to get file from this server.\n";
const char* error_404_title = "Not Found";
const char* error_404_form = "The requested file was not found on this server.\n";
const char* error_500_title = "Internal Error";
const char* error_500_form = "There was an unusual problem serving the requested file.\n";

//网站的根目录（项目的根目录（不是服务器项目，而是resources中的网站项目））
const char *doc_root="/home/lipengju/Linux/WebServer/resources";

HttpConn::HttpConn()
{
}

HttpConn::~HttpConn()
{
}
//设置文件描述符非阻塞
void setNonBlock(int fd){
    int flag=fcntl(fd, F_GETFL);
    flag |=O_NONBLOCK;
    int ret=fcntl(fd, F_SETFL, flag);
    if(ret==-1){
        perror("setNonBlock");
        exit(-1);
    }
}
//将要监听的文件描述符相关信息添加到epoll实例中
void addFd(int epfd, int fd, bool one_shot, bool enable_ET){
    struct epoll_event epev;
    //EPOLLRDHUP设置之后，客户端断开时，服务器端就不需要通过read、write等函数判断是否断开了，而可以直接通过EPOLLRDHUP这个事件判断是否断开连接了
    epev.events=EPOLLIN | EPOLLRDHUP;
    epev.data.fd=fd; //data数据结构中别的数据不用管，只设置fd就行
    if(one_shot){
        epev.events |= EPOLLONESHOT;
    }
    if(enable_ET){
        epev.events |= EPOLLET;
    }
    epoll_ctl(epfd, EPOLL_CTL_ADD, fd, &epev);

    //因为要设置ET模式，一次性将文件描述符指向的数据流中的所有数据都读出来，那么需要设置文件描述符非阻塞，即使没有数据也立刻返回
    setNonBlock(fd);
}
//将要监听的文件描述符相关信息从epoll实例中删除
void removeFd(int epfd, int fd){
    epoll_ctl(epfd, EPOLL_CTL_DEL, fd, 0);
    close(fd);
}

//在epoll实例中修改要监听的文件描述符(重置socket上的EPOLLONESHOT事件，以确保下一次可读时，EPOLLIN事件能被触发)
void modFd(int epfd, int fd, int ev){
    struct epoll_event epev;
    epev.data.fd=fd;
    epev.events=ev | EPOLLONESHOT | EPOLLRDHUP;
    epoll_ctl(epfd, EPOLL_CTL_MOD, fd, &epev);
}

void HttpConn::init(int sockfd, sockaddr_in & clientAddr){
    this->m_sockfd=sockfd;
    this->m_address=clientAddr;
    this->m_read_index=0;  //将与该客户端通信的读缓冲区m_read_buf位置置为0
    //设置端口复用
    int reuse=1;
    setsockopt(this->m_sockfd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    //将传输的文件描述符有关信息添加到epoll实例中
    addFd(m_epfd, this->m_sockfd, true, true);
    m_user_count++;  //总用户数+1

    init();
}

void HttpConn::init(){
    memset(m_read_buf, 0, READ_BUFFER_SIZE);
    memset(m_write_buf, 0, WRITE_BUFFER_SIZE);
    memset(this->m_real_file, 0, FILENAME_LEN);
    this->m_check_state=CHECK_STATE_REQUESTLINE;  //初始化状态为解析请求首行
    this->m_checked_index=0;
    this->m_start_line=0;
    this->m_read_index=0;
    this->m_write_index=0;
    this->m_url=0;
    this->m_method=GET;
    this->m_version=0;
    this->m_host=0;
    this->m_linker=false;
    this->m_content_length=0;
    this->cgi=0;
}

void HttpConn::close_conn(){
    if(this->m_sockfd!=-1){
        removeFd(m_epfd, this->m_sockfd);
        this->m_sockfd=-1;
        m_user_count--;  //关闭一个连接，客户总数量-1
    }
}

HttpConn::HTTP_CODE HttpConn::do_request(){
    // “home/lipengju/Linux/WebServer/resources”
    strcpy(m_real_file, doc_root);
    int len=strlen(doc_root);
    printf("m_url:%s\n", m_url);
    const char *p=strrchr(m_url, '/');

    //处理cgi
    if(cgi==1&&(*(p+1)=='2'||*(p+1)=='3')){
        //根据标志判断是登录检测还是注册检测
        char flag=m_url[1];
        char *m_url_real=(char *)malloc(sizeof(char)*200);
        strcpy(m_url_real, "/");
        strcat(m_url_real, m_url+2);
        strncpy(m_real_file+len, m_url_real, FILENAME_LEN-len-1);
        free(m_url_real);

        //将用户名和密码提取出来
        //user=123&passwd=123
        char name[100], password[100];
        int i;
        for (i = 5; m_string[i] != '&'; ++i)
            name[i - 5] = m_string[i];
        name[i - 5] = '\0';

        int j = 0;
        for (i = i + 10; m_string[i] != '\0'; ++i, ++j)
            password[j] = m_string[i];
        password[j] = '\0';

        int intName=atoi(name);
        string strPass(password);
        if (*(p + 1) == '3')
        {
            //如果是注册，先检测数据库中是否有重名的
            //没有重名的，进行增加数据

            if (skipList.search_element(intName)==false)
            {
                skipList.insert_element(intName, strPass);
                strcpy(m_url, "/log.html");  //注册成功自动跳转登录界面
            }
            else{
                strcpy(m_url, "/registerError.html");
            } 
        }
        //如果是登录，直接判断
        //若浏览器端输入的用户名和密码在表中可以查找到，返回1，否则返回0
        else if (*(p + 1) == '2')
        {
            if (skipList.search_element(intName)==true)
                strcpy(m_url, "/welcome.html");  //登录成功自动跳转欢迎界面
            else
                strcpy(m_url, "/logError.html");
        }
    }

    //如果点了新用户按钮（触发注册界面）
    if (*(p + 1) == '0')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/register.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //如果点了已有账号按钮（触发登录界面）
    else if (*(p + 1) == '1')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/log.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //在欢迎界面点了“图片”按钮
    else if (*(p + 1) == '5')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/picture.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //在欢迎界面点了“视频”按钮
    else if (*(p + 1) == '6')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/video.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    //在欢迎界面点了“关注”按钮
    else if (*(p + 1) == '7')
    {
        char *m_url_real = (char *)malloc(sizeof(char) * 200);
        strcpy(m_url_real, "/fans.html");
        strncpy(m_real_file + len, m_url_real, strlen(m_url_real));

        free(m_url_real);
    }
    else  //都不是说明是一个GET请求，直接返回一个judge.html界面
        strncpy(m_real_file + len, m_url, FILENAME_LEN - len - 1);

    //strncpy(m_real_file+len, m_url, FILENAME_LEN-len-1);
    //获取m_real_file文件的相关状态信息，-1失败，0成功
    if(stat(m_real_file, &m_file_stat)<0){
        return NO_RESOURCE;
    }

    //判断访问权限
    if(!(m_file_stat.st_mode&S_IROTH)){  //判断是否有读的权限
        return FORBIDDEN_REQUEST;
    }
    //判断是否是目录
    if(S_ISDIR(m_file_stat.st_mode)){
        return BAD_REQUEST;
    }
    //以只读方式打开文件
    int fd=open(m_real_file, O_RDONLY);
    //创建内存映射(现在响应体的数据被放到内存映射m_file_address中了)
    m_file_address=(char *)mmap(0, m_file_stat.st_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    return FILE_REQUEST;
}

//循环读取数据，直到无数据可读或客户端关闭连接
bool HttpConn::read(){
    printf("read all data once\n");
    if(this->m_read_index>=READ_BUFFER_SIZE){
        return false;  //数据已经读完了，这次读不到数据了，return false将要关闭此次连接
    }
    int bytes_read=0;  //读取到的字节
    while(true){
        bytes_read=recv(this->m_sockfd, this->m_read_buf+this->m_read_index, READ_BUFFER_SIZE-this->m_read_index, 0);
        if(bytes_read==-1){
            if(errno==EAGAIN||errno==EWOULDBLOCK){
                //已经没有数据可读
                break;  //退出读循环,return true表示读到了数据，以便后面整理已经读到m_read_buf中的数据
            }
            return false;  //其他情况则标识读真的出错了
        }else if(bytes_read==0){  //对方关闭连接
            return false;
        }
        this->m_read_index+=bytes_read;
    }
    printf("read finished, the data has been received is: %s\n", this->m_read_buf);
    return true;
}

//解析一行，判断依据\r\n（相当于enter），解析到一行就立刻返回
HttpConn::LINE_STATUS HttpConn::parse_line(){
    char temp;
    for(; m_checked_index<m_read_index; m_checked_index++){
        temp=m_read_buf[m_checked_index];
        if(temp=='\r'){
            if((m_checked_index+1)==m_read_index){  //才\r就到所读内容结尾了，说明行还没有检测完成
                return LINE_OPEN;  //行还没检测完成会怎样呢？
            }else if(m_read_buf[m_checked_index+1]=='\n'){
                m_read_buf[m_checked_index++]='\0';  //也就是将\r替换成\0，然后index+1
                m_read_buf[m_checked_index++]='\0';  //将\n替换成\0，然后index+1
                return LINE_OK;
            }
            return LINE_BAD;
        }else if(temp=='\n'){
            if((m_checked_index>1)&&(m_read_buf[m_checked_index-1]=='\r')){
                m_read_buf[m_checked_index-1]='\0';
                m_read_buf[m_checked_index++]='\0';
                return LINE_OK;  //真的会跳到这个循环里面来吗？为什么呢？
            }
            return LINE_BAD;
        }
    }
    return LINE_OPEN;
}

//解析HTTP请求行（获取请求方法、目标URL、HTTP版本）
HttpConn::HTTP_CODE HttpConn::parse_request_line(char *text){
    //GET /index.html HTTP/1.1
    m_url=strpbrk(text, " \t");
    if (! m_url) { 
        return BAD_REQUEST;
    }
    *m_url++='\0';

    char *method=text;
    if(strcasecmp(method, "GET")==0){
        printf("the request method is: %s\n", method);
        m_method=GET;
    }else if(strcasecmp(method, "POST")==0){
        printf("the request method is: %s\n", method);
        m_method=POST;
        cgi=1;
    }else{
        return BAD_REQUEST;
    }
    m_version=strpbrk(m_url, " \t");
    if(!m_version){
        return BAD_REQUEST;
    }
    *m_version++ = '\0';
    if(strcasecmp(m_version, "HTTP/1.1")!=0){
        return BAD_REQUEST;
    }
    printf("the version is: %s\n", m_version);
    if(strncasecmp(m_url, "http://", 7)==0){
        m_url+=7;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url=strchr(m_url, '/');
    }
    if(strncasecmp(m_url, "https://", 8)==0){
        m_url+=8;
        // 在参数 str 所指向的字符串中搜索第一次出现字符 c（一个无符号字符）的位置。
        m_url=strchr(m_url, '/');
    }
    printf("the url is: %s\n", m_url);
    if(!m_url||m_url[0]!='/'){
        return BAD_REQUEST;
    }
    //当url为/时，显示判断界面
    if(strlen(m_url)==1){  //也就是发送GET请求时，什么也没做，url什么都没有，就返回一个judge.html静态页面
        strcat(m_url, "judge.html");
    }
    
    printf("the url length is %d\n", strlen(m_url));

    m_check_state=CHECK_STATE_HEADER;  //检查状态变成检查请求头
    return NO_REQUEST;
}
HttpConn::HTTP_CODE HttpConn::parse_header(char *text){
    //(parse_header()函数一次只解析一行，而这一行行的由parse_line()函数得到)
    //遇到空行，表示头部字段解析完毕
    if(text[0]=='\0'){  //请求头和请求体之间会隔一个'\0'，检测到'\0'后，说明请求头解析结束了，看请求体长度是否为0
        //如果HTTP请求有消息体，则还需要读取m_content_length字节的消息体
        //状态机转移到CHECK_STATE_CONTENT状态
        if(m_content_length!=0){  //如果有请求体的话，在请求头中会有content_length这一项
            m_check_state=CHECK_STATE_CONTENT;
            return NO_REQUEST;  //表示目前请求的报文还没有解析完成
        }
        //否则说明我们已经得到了一个完整的HTTP请求（没有请求体）
        return GET_REQUEST;
    }else if(strncasecmp(text, "Connection:", 11)==0){
        //处理Connection头部字段  Connection: keep-alive
        text+=11;
        text+=strspn(text, " \t");  //跳过Connection:和keep-alive之间的空格和制表符
        if(strcasecmp(text, "keep-alive")==0){
            m_linker=true;
        }       
    }else if(strncasecmp(text, "Content-Length:", 15)==0){
        //处理Content-Length头部字段
        text+=15;
        text+=strspn(text, " \t");
        m_content_length=atoi(text);
    }else if(strncasecmp(text, "Host:", 5)==0){
        //处理Host头部字段
        text+=5;
        text+=strspn(text, " \t");
        m_host=text;
    }else{
        printf("oop! unknow header %s\n", text);
    }
    return NO_REQUEST;
}
HttpConn::HTTP_CODE HttpConn::parse_content(char *text){
    if(m_read_index>=(m_content_length+m_checked_index)){
        text[m_content_length]='\0';
        m_string=text;
        return GET_REQUEST;
    }
    return NO_REQUEST;
}
//主状态机，解析请求
HttpConn::HTTP_CODE HttpConn::process_read(){
    LINE_STATUS line_status=LINE_OK;
    HTTP_CODE ret=NO_REQUEST;

    char *text=0;
    while(((m_check_state==CHECK_STATE_CONTENT)&&(line_status==LINE_OK))
    ||((line_status=parse_line())==LINE_OK)){
        //解析到了一行完整的数据，或者解析到了请求体（也是完整的数据）

        //获取一行数据
        text=get_line();  //怎么判断这行数据的结尾呢？（由\0判断好像）
        m_start_line=m_checked_index;
        printf("got 1 http line : %s\n", text);

        switch(m_check_state){
            case CHECK_STATE_REQUESTLINE:
            {
                ret=parse_request_line(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;  //语法错误就直接结束
                }
                break;  //其他情况则继续解析
            }
            case CHECK_STATE_HEADER:
            {
                ret=parse_header(text);
                if(ret==BAD_REQUEST){
                    return BAD_REQUEST;
                }else if(ret==GET_REQUEST){  //解析请求头后可能得到完整的请求（无请求体的请求）
                    printf("the request has no content\n");
                    return do_request();
                }
                break;
            }
            case CHECK_STATE_CONTENT:  //CHECK_STATE_CONTENT状态在parse_header()中获得的
            {
                ret=parse_content(text);
                if(ret==GET_REQUEST){  //解析请求体后可能得到完整的请求（有请求体的请求）
                    printf("the request has content\n");
                    return do_request();
                }
                line_status=LINE_OPEN;
                break;
            }
            default:
            {
                return INTERNAL_ERROR;
            }
        }
    }
    return NO_REQUEST;
}

//将放入m_file_address中的响应体的数据和放入m_write_buf中的响应头的数据组合起来写给客户端
bool HttpConn::write(){
    printf("write all data once\n");
    int temp = 0;
    int bytes_have_send = 0;    // 已经发送的字节
    int bytes_to_send = m_write_index;// 将要发送的字节 （m_write_idx）写缓冲区中待发送的字节数
    
    if ( bytes_to_send == 0 ) {
        // 将要发送的字节为0，这一次响应结束。
        modFd( m_epfd, m_sockfd, EPOLLIN ); 
        init();
        return true;
    }

    while(1) {
        // 分散写
        temp = writev(m_sockfd, m_iv, m_iv_count);
        if ( temp <= -1 ) {
            // 如果TCP写缓冲没有空间，则等待下一轮EPOLLOUT事件，虽然在此期间，
            // 服务器无法立即接收到同一客户的下一个请求，但可以保证连接的完整性。
            if( errno == EAGAIN ) {
                modFd( m_epfd, m_sockfd, EPOLLOUT );
                return true;
            }
            unmap();
            return false;
        }
        bytes_to_send -= temp;
        bytes_have_send += temp;
        if ( bytes_to_send <= bytes_have_send ) {
            // 发送HTTP响应成功，根据HTTP请求中的Connection字段决定是否立即关闭连接
            unmap();
            if(m_linker) {
                init();
                modFd( m_epfd, m_sockfd, EPOLLIN );
                return true;
            } else {
                modFd( m_epfd, m_sockfd, EPOLLIN );
                return false;
            } 
        }
    }
}

void HttpConn::unmap(){
    if(m_file_address){
        munmap(m_file_address, m_file_stat.st_size);
        m_file_address=0;
    }
}

bool HttpConn::add_response( const char* format, ... ){
    if( m_write_index >= WRITE_BUFFER_SIZE ) {
        return false;
    }
    va_list arg_list;
    va_start( arg_list, format );
    int len = vsnprintf( m_write_buf + m_write_index, WRITE_BUFFER_SIZE - 1 - m_write_index, format, arg_list );
    if( len >= ( WRITE_BUFFER_SIZE - 1 - m_write_index ) ) {
        return false;
    }
    m_write_index += len;
    va_end( arg_list );
    return true;
}
bool HttpConn::add_content( const char* content ){
    return add_response( "%s", content );
}
bool HttpConn::add_content_type(){
    return add_response("Content-Type:%s\r\n", "text/html");
}
bool HttpConn::add_status_line( int status, const char* title ){
    return add_response( "%s %d %s\r\n", "HTTP/1.1", status, title );
}
bool HttpConn::add_headers( int content_length ){
    add_content_length(content_length);
    add_content_type();
    add_linger();
    add_blank_line();  //响应头结尾要加一个换行\r\n
}
bool HttpConn::add_content_length( int content_length ){
    return add_response( "Content-Length: %d\r\n", content_length );
}
bool HttpConn::add_linger(){
    return add_response( "Connection: %s\r\n", ( m_linker == true ) ? "keep-alive" : "close" );
}
bool HttpConn::add_blank_line(){
    return add_response( "%s", "\r\n" );
}

bool HttpConn::process_write(HTTP_CODE ret){
    switch (ret)
    {
        case INTERNAL_ERROR:
            add_status_line( 500, error_500_title );
            add_headers( strlen( error_500_form ) );
            if ( ! add_content( error_500_form ) ) {
                return false;
            }
            break;
        case BAD_REQUEST:
            add_status_line( 400, error_400_title );
            add_headers( strlen( error_400_form ) );
            if ( ! add_content( error_400_form ) ) {
                return false;
            }
            break;
        case NO_RESOURCE:
            add_status_line( 404, error_404_title );
            add_headers( strlen( error_404_form ) );
            if ( ! add_content( error_404_form ) ) {
                return false;
            }
            break;
        case FORBIDDEN_REQUEST:
            add_status_line( 403, error_403_title );
            add_headers(strlen( error_403_form));
            if ( ! add_content( error_403_form ) ) {
                return false;
            }
            break;
        case FILE_REQUEST:
            add_status_line(200, ok_200_title );
            if(m_file_stat.st_size!=0){
                add_headers(m_file_stat.st_size);
                m_iv[ 0 ].iov_base = m_write_buf;
                m_iv[ 0 ].iov_len = m_write_index;
                m_iv[ 1 ].iov_base = m_file_address;
                m_iv[ 1 ].iov_len = m_file_stat.st_size;
                m_iv_count = 2;
                bytes_to_send=m_write_index+m_file_stat.st_size;
                return true;
            }else{
                const char *ok_string = "<html><body></body></html>";
                add_headers(strlen(ok_string));
                if (!add_content(ok_string))
                    return false;
            }
            
        default:
            return false;
    }

    m_iv[ 0 ].iov_base = m_write_buf;
    m_iv[ 0 ].iov_len = m_write_index;
    m_iv_count = 1;
    bytes_to_send=m_write_index;
    return true;
}

void HttpConn::process(){
    printf("parse request\n");
    //解析HTTP请求
    HTTP_CODE parse_ret=process_read();
    if(parse_ret==NO_REQUEST){
        modFd(m_epfd, this->m_sockfd, EPOLLIN);  //请求不完整，继续检测该客户端的文件描述符中的数据（通过modFd重置一下，否则就算监听到了有新数据到来也不通知，不过重置后再来新数据的话，处理这些数据的可能就是新线程了，也就是客户端重发请求？）
        return;  //请求不完整的话，线程直接结束，而不是等完整的请求到来，完整的请求又被当作一个新任务，由新的线程处理，相当于需要客户端重发一次HTTP请求。
    }

    printf("create response\n");
    //生成响应(也就是把数据准备好，下次监测到EPOLLOUT时，将准备好的数据写给客户端)
    bool write_ret=process_write(parse_ret);
    if(!write_ret){
        close_conn();
    }
    modFd(m_epfd, this->m_sockfd, EPOLLOUT);
}