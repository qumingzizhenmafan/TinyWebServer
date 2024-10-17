#ifndef HTTPCONNECTION_H
#define HTTPCONNECTION_H
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
#include <map>

#include "../lock/locker.h"
#include "../CGImysql/sql_connection_pool.h"
#include "../timer/lst_timer.h"
#include "../log/log.h"

class http_conn
{
public:
    static const int FILENAME_LEN = 200;
    static const int READ_BUFFER_SIZE = 2048;
    static const int WRITE_BUFFER_SIZE = 1024;
    enum METHOD
    {
        GET = 0,
        POST,
        HEAD,
        PUT,
        DELETE,
        TRACE,
        OPTIONS,
        CONNECT,
        PATH
    };
    enum CHECK_STATE
    {
        CHECK_STATE_REQUESTLINE = 0,
        CHECK_STATE_HEADER,
        CHECK_STATE_CONTENT
    };
    enum HTTP_CODE
    {
        NO_REQUEST, //请求未完成
        GET_REQUEST,
        BAD_REQUEST,
        NO_RESOURCE,
        FORBIDDEN_REQUEST,
        FILE_REQUEST,
        INTERNAL_ERROR,
        CLOSED_CONNECTION
    };
    
    enum LINE_STATUS 
    {
        LINE_OK = 0,    // LINE_OK：成功解析出一行。
        LINE_BAD,       // LINE_BAD：解析行有错误，例如请求行错误。
        LINE_OPEN       // LINE_OPEN：数据还不完整，需要继续接收。
    };

public:
    http_conn() {}
    ~http_conn() {}

public:
    void init(int sockfd, const sockaddr_in &addr, char *, int, int, string user, string passwd, string sqlname);
    
    //关闭连接，关闭一个连接，客户总量减一
    //如果real_close为true，直接关闭连接，
    //将连接假如计时器，等待超时关闭
    void close_conn(bool real_close = true);
    //http连接处理总调用函数
    //利用返回信息分别处理读和写的部分
    void process();
    //从连接中读取数据到读缓冲区
    //循环读取客户数据，直到无数据可读或对方关闭连接
    //根据连接类中的m_TRIGMode字段来决定是LT还是ET
    //非阻塞ET工作模式下，需要一次性将数据读完
    //ET模式下的文件描述符应设为非阻塞
    bool read_once();
    //用于处理数据发送的一个部分，主要用于管理发送缓冲区的状态
    //负责处理写操作完成后索引工作以及文件传输完成后的文件的解映射
    bool write();
    sockaddr_in *get_address()
    {
        return &m_address;
    }
    //数据库初始化，将用户名和密码存入map中，并获取一个数据库连接
    void initmysql_result(connection_pool *connPool);
    int timer_flag;
    int improv;

//用于处理回传报文的设计，将报文信息存入写缓冲区
private:
    void init();
    //它会逐步解析请求行、请求头和请求体，判断 HTTP 请求的完整性并作出相应处理。
    //如果请求是完整的且有效，它将调用实际处理函数 do_request，并返回相应的状态码。
    //待验证：因为不支持上传文件，所以只能用于处理 GET 请求，且只能处理静态资源。
    HTTP_CODE process_read();
    //用于处理 HTTP 连接的写操作，根据不同的 HTTP 状态码，
    //将相应的错误页面或文件内容添加到 HTTP 响应中
    bool process_write(HTTP_CODE ret);
    //解析http请求行，获得请求方法，目标url及http版本号
    //只能解析处理GET和POST请求 ， 只能处理HTTP1.1请求
    //以及简单的网页传输，不支持上传文件
    //例：GET http://index.html HTTP/1.1
    HTTP_CODE parse_request_line(char *text);
    //解析http请求的一个头部信息
    HTTP_CODE parse_headers(char *text);
    //解析http请求体，只能支持用户名和密码的post请求，不支持上传文件
    //只用于判断http请求体是否被完整读入
    HTTP_CODE parse_content(char *text);
    //根据请求行中的url，判断请求的文件类型，并返回相应的状态码
    //如果请求的是静态资源，则将文件路径存入m_real_file
    //并将文件映射到内存上
    //待验证：在parse_headers处理之后就已经调用了一边do_request
    //->但在parse_content又调用了一边，
    HTTP_CODE do_request();
    char *get_line() { return m_read_buf + m_start_line; };
    //从状态机，用于分析出一行内容
    //返回值为行的读取状态，有LINE_OK,LINE_BAD,LINE_OPEN
    LINE_STATUS parse_line();
    void unmap();
    bool add_response(const char *format, ...);
    bool add_content(const char *content);
    bool add_status_line(int status, const char *title);
    bool add_headers(int content_length);
    bool add_content_type();
    bool add_content_length(int content_length);
    bool add_linger();
    bool add_blank_line();

public:
    static int m_epollfd;
    static int m_user_count;    //当前连接数
    MYSQL *mysql;
    int m_state;  //读为0, 写为1

private:
    int m_sockfd;                       //套接字监听端口文件描述符
    sockaddr_in m_address;
    char m_read_buf[READ_BUFFER_SIZE];
    long m_read_idx;                    //当前读取到的字节位置
    long m_checked_idx;                 //当前处理完的字节位置
    int m_start_line;                   
    char m_write_buf[WRITE_BUFFER_SIZE];//写缓冲区
    int m_write_idx;                    //当前写入缓冲区的字节位置
    CHECK_STATE m_check_state;
    METHOD m_method;
    char m_real_file[FILENAME_LEN];     //实际需要传输的文件路径
    char *m_url;        //请求的url
    char *m_version;    //http版本
    char *m_host;   //请求头中的host
    long m_content_length;  //数据内容长度
    bool m_linger;  //是否保持连接
    char *m_file_address;   //映射到内存中的文件地址
    struct stat m_file_stat;//映射到内存中的文件状态
    struct iovec m_iv[2];   //使用多个写缓冲区来处理不同部分的写入，加快写入速度
    int m_iv_count;         //缓冲区数量
    int cgi;        //是否启用的POST
    char *m_string; //存储请求头数据
    int bytes_to_send;  //要发送的字节数
    int bytes_have_send;//已经发送的字节数
    char *doc_root;     //服务器资源文件的根目录

    map<string, string> m_users;    //存储用户名和密码
    int m_TRIGMode; //监听事件的监听类型
    int m_close_log;    

    char sql_user[100]; //服务器初始化用户名
    char sql_passwd[100];   //服务器初始化密码
    char sql_name[100];     //服务器初始化数据库名
};

#endif
