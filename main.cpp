#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <errno.h>
#include <fcntl.h>
#include <sys/epoll.h>
#include "locker.h"
#include "threadpool.h"
#include <signal.h>
#include "http_conn.h"

#define MAX_FD 65535 //最大文件描述符个数
#define MAX_EVENT_NUMBER 10000 //监听的最大的事件数量


    //处理信号的函数,添加信号捕捉
    //网上查函数指针应该带*，但有些编译器允许不带*，效果相同
    void addsig(int sig, void (handler)(int)){
        //sa为注册信号的参数
        struct sigaction sa;
        //清空sa
        memset(&sa,'\n',sizeof(sa));
        sa.sa_handler = handler;
        //设置临时阻塞的信号集
        sigfillset(&sa.sa_mask);

        sigaction(sig,&sa,NULL);
    }
    //这一块不是很懂


    //添加文件描述符到epoll中(不判断是否成功了)
    extern void addfd(int epollfd, int fd, bool one_shot);

    //从epoll中删除文件描述符
    extern int removefd(int epollfd, int fd);

    //修改文件描述符
    extern void modfd(int epollfd, int fd, int ev);

int main(int argc,char* argv[]){
    if(argc <= 1){
        printf("按照如下格式运行： %s port_number\n",basename(argv[0]));
        exit(-1);
    }

    //获取端口号,0程序名，1端口号
    int port =atoi(argv[1]);

    //对SIGPIE信号进行处理
    //遇到SIGPIPE默认终止进程，SIG_IGN忽略它
    addsig(SIGPIPE,SIG_IGN);

    // 创建线程池，初始化线程池
    //http——conn为连接任务
    threadpool<http_conn> * pool = NULL;
    try{
        pool = new threadpool<http_conn>;

    }catch(...){
        exit(-1);
    }

    //创建数组，保存所有的客户端信息
    http_conn * users = new http_conn[ MAX_FD ];
    
    //编写网络代码

    //创建监听的套接字
    int listenfd = socket(PF_INET,SOCK_STREAM,0);
    //不判断了，默认认为没什么问题

    //设置端口复用(一定要绑定之前设置)
    int reuse = 1;
    setsockopt(listenfd,SOL_SOCKET,SO_REUSEADDR,&reuse,sizeof(reuse));

    //绑定
    struct sockaddr_in address;
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = INADDR_ANY;
    address.sin_port = htons(port);
    bind(listenfd,(struct sockaddr *)&address, sizeof(address));
    //不判断了

    //监听
    listen(listenfd,5);

    //创建epoll对象,事件数组，添加监听文件描述符
    epoll_event events[MAX_EVENT_NUMBER];
    //创建epoll对象，可以传任意但不能为0
    int  epollfd = epoll_create(5);

    //将监听的文件描述符添加到epoll对象中
    addfd(epollfd, listenfd, false);
    http_conn::m_epollfd = epollfd;

    while(true){
        //检测到了几个事件
        int num = epoll_wait(epollfd,events,MAX_EVENT_NUMBER,-1);
        if((num < 0) && (errno != EINTR)){
            printf("epoll failure\n");
            break;
        }

        //循环遍历事件数组
        for(int i = 0; i< num; i++){
            int sockfd = events[i].data.fd;
            if(sockfd == listenfd) {
                //有客户端连接进来
                struct sockaddr_in client_address;
                socklen_t client_addrlen = sizeof(client_address);
                int connfd = accept(listenfd,(struct sockaddr*)&client_address,&client_addrlen);
                //不判断默认连接成功
                if(http_conn::m_user_count >= MAX_FD){
                    //目前连接数满了,给客户端写一个信息，服务器内部正忙
                    close(connfd);
                    continue;
                }
                //将新的客户的数据初始化，放到数组当中
                //拿连接的描述符作为索引(一般为递增的)
                users[connfd].init(connfd,client_address);
            } else if(events[i].events & (EPOLLRDHUP | EPOLLHUP | EPOLLERR)){
                //对方异常断开或者错误的事件发生
                //关闭连接
                users[sockfd].close_conn();
            }else if(events[i].events & EPOLLIN){//读事件发生
                if(users[sockfd].read()){
                    //一次性把所有数据都读完
                    //交给工作线程处理业务逻辑
                    pool->append(users + sockfd);
                }else{
                    users[sockfd].close_conn();
                }
            }else if(events[i].events & EPOLLOUT){
                if( !users[sockfd].write()){ //一次性写完所有数据
                users[sockfd].close_conn();
                }
            }
        }
    }

    close(epollfd);
    close(listenfd);
    delete [] users;
    delete pool;

    return 0;
}