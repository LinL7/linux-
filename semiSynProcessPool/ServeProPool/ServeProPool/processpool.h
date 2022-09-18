#pragma once
#ifndef PROCESSPOOL_H
#define PROCESSPOOL_H

#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <assert.h>
#include <stdio.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/epoll.h>
#include <signal.h>
#include <sys/wait.h>
#include <sys/stat.h>

/* 描述子进程的类 */
class process
{
public:
	process(): m_pid(-1){}

public:
	pid_t m_pid;
	int m_pipefd[2];
};

/* 进程池类模板 */
template< typename T >
class processpool
{
private:
	processpool(int listenfd, int process_number = 8);

public:
	/* 单例模式 */
	static processpool< T >* create(int listenfd, int process_number = 8)
	{
		if (!m_instance)
		{
			m_instance = new processpool< T >(listenfd, process_number);
		}
		return m_instance;
	}
	~processpool()
	{
		delete[] m_sub_process;
	}
	/* 启动进程池 */
	void run();
private:
	void setup_sig_pipe();
	void run_parent();
	void run_child();

private:
	/* 最大子进程数 */
	static const int MAX_PROCESS_NUMBER = 16;
	/* 子进程最多能处理的客户数量 */
	static const int USER_PER_PROCESS = 65536;
	/* EPOLL 最多能处理的事件数 */
	static const int MAX_EVENT_NUMBER = 10000;
	/* 进程池中进程总数 */
	int m_process_number;
	/* 子进程在池中序号 */
	int m_idx;
	/* epoll内核事件表 */
	int m_epollfd;
	
	int m_listenfd;

	int m_stop;
	/* 子进程的描述信息 */
	process* m_sub_process;
	/* 进程池静态实例 单例*/
	static processpool< T >* m_instance;
};

template< typename T >
processpool< T >* processpool< T >::m_instance = NULL;

/* 信号管道 实现统一事件源 */
static int sig_pipefd[2];

static int setnonblocking( int fd )
{
	int old_option = fcntl(fd, F_GETFL);
	int new_option = old_option | O_NONBLOCK;
	fcntl(fd, F_SETFL, new_option);
	return old_option;
}

static void addfd(int epollfd, int fd)
{
	epoll_event event;
	event.data.fd = fd;
	event.events = EPOLLIN | EPOLLET;
	epoll_ctl(epollfd, EPOLL_CTL_ADD, fd, &event);	//这个表也是在内核中的
	setnonblocking(fd);
}

static void removefd(int epollfd, int fd)
{
	epoll_ctl(epollfd, EPOLL_CTL_DEL, fd, 0);
	close(fd);
}

static void sig_handler(int sig)
{
	int save_errno = errno;
	int msg = sig;
	send(sig_pipefd[1], (char*)&msg, 1, 0);	//信号处理函数向管道写端写数据
	errno = save_errno;
}

// ？？
static void addsig(int sig, void(handler)(int), bool restart = true)
{
	struct sigaction sa;
	memset(&sa, '\0', sizeof(sa));
	sa.sa_handler = handler;	//新的信号处理函数
	if (restart)
	{
		sa.sa_flags |= SA_RESTART;	//如果信号中断了进程的某个系统调用，则系统自动启动该系统调用
	}
	sigfillset(&sa.sa_mask);
	assert(sigaction(sig, &sa, NULL) != -1);	//sigaction函数的功能是检查或修改与指定信号相关联的处理动作

}

/* 进程池构造函数 */
template< typename T >
processpool< T >::processpool(int listenfd, int process_number)
	:m_listenfd(listenfd), m_process_number(process_number), m_idx(-1),m_stop(false)
{
	assert((process_number > 0) && (m_process_number <= MAX_PROCESS_NUMBER));
	m_sub_process = new process[process_number];
	assert(m_sub_process);

	//创建子进程
	for (int i = 0; i < process_number; ++i)
	{
		int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, m_sub_process[i].m_pipefd);
		assert(ret == 0);

		//保存子进程信息
		m_sub_process[i].m_pid = fork();
		assert(m_sub_process[i].m_pid >= 0);
		if (m_sub_process[i].m_pid > 0)
		{
			//父进程
			close(m_sub_process[i].m_pipefd[1]);
			continue;
		}
		else {
			//子进程 拥有与父进程一样的资源，直接跳出，
			close(m_sub_process[i].m_pipefd[0]);
			m_idx = i;
			break;
		}
	}
}

/* 统一事件源 */
template<typename T>
void processpool<T>::setup_sig_pipe()
{
	m_epollfd = epoll_create(5);
	assert(m_epollfd != -1);

	int ret = socketpair(PF_UNIX, SOCK_STREAM, 0, sig_pipefd);
	assert(ret != -1);
	setnonblocking(sig_pipefd[0]);		//多进程的读端

	addsig(SIGCHLD, sig_handler);
	addsig(SIGTERM, sig_handler);
	addsig(SIGINT, sig_handler);
	addsig(SIGPIPE, SIG_IGN);
}

/* 程序主要逻辑运行，m_idx都在子进程中设定了大于等于0的值，可由其来判断是什么进程 */
template<typename T>
void processpool<T>::run()
{
	if (m_idx != -1)
	{
		run_child();
		return;
	}
	run_parent();
}

template<typename T>
void processpool<T>::run_child()
{
	setup_sig_pipe();

	/* 监听每个客户数据中的管道 用于进程间通信， 接收新连接 */
	int pipefd = m_sub_process[m_idx].m_pipefd[1];
	addfd(m_epollfd, pipefd);

	epoll_event events[MAX_EVENT_NUMBER];
	T* users = new T[USER_PER_PROCESS];
	assert(users);
	int number = 0;
	int ret = -1;

	while (!m_stop)
	{
		number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
		if ((number < 0) && (errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}

		for (int i = 0; i < number; i++)
		{
			int sockfd = events[i].data.fd;
			if ((sockfd == pipefd) && (events[i].events & EPOLLIN))
			{
				/* 管道有消息 父进程发来通知，连接 新客户 */
				int client = 0;
				ret = recv(sockfd, (char*)&client, sizeof(client), 0);
				if (((ret < 0) && (errno != EAGAIN)) || ret == 0)
				{
					continue;
				}
				else
				{
					struct sockaddr_in client_address;
					socklen_t client_addrlength = sizeof(client_address);
					int connfd = accept(m_listenfd, (struct sockaddr*)&client_address, &client_addrlength);
					if (connfd < 0)
					{
						printf("errno is %d\n", errno);
						continue;
					}
					addfd(m_epollfd, connfd);	//将连接的子进程上树
					users[connfd].init(m_epollfd, connfd, client_address);

				}

			}
			else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
			{
				/* 信号通道发来消息 信号通道是全进程通用的 子进程会继承父进程的文件描述符*/
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
				if (ret <= 0)
				{
					continue;
				}
				else
				{
					for (int i = 0; i < ret; ++i)
					{
						switch (signals[i])
						{
						case SIGCHLD:
						{
							/* 子进程关闭 */
							pid_t pid;
							int stat;
							/*非阻塞， 获取关闭的子进程pid，信号处理函数自会执行一次，故只会有一个子线程收到
							*/
							while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
							{
								continue;
							}
							break;
						}
						case SIGTERM:
						case SIGINT:
						{
							m_stop = true;
							break;
						}
						default:
						{
							break;
						}
						}

					}
				}
			}
			else if(events[i].events & EPOLLIN)
			{
				/* 客户请求到来 */
				users[sockfd].process();
			}
			else
			{
				continue;
			}
		}
	}
	delete[] users;
	users = nullptr;
	close(pipefd);
	close(m_epollfd);

}

template<typename T>
void processpool<T>::run_parent()
{
	setup_sig_pipe();
	addfd(m_epollfd, m_listenfd);

	epoll_event events[MAX_EVENT_NUMBER];
	int sub_process_counter = 0;
	int new_conn = 1;
	int number = 0;
	int ret = -1;

	while (!m_stop)
	{
		number = epoll_wait(m_epollfd, events, MAX_EVENT_NUMBER, -1);
		if ((number < 0) && (errno != EINTR))
		{
			printf("epoll failure\n");
			break;
		}
		for (int i = 0; i < number; i++)
		{
			int sockfd = events[i].data.fd;
			if (sockfd == m_listenfd)
			{
				/* 有新连接来 通知子进程 */
				int i = sub_process_counter;
				/*使用 Round Robin 方式为其分配 */
				do {
					if (m_sub_process[i].m_pid != -1)
					{
						/* 找到子进程就跳出 */
						break;
					}
					i = (i + 1) % m_process_number;
				} while (i != sub_process_counter);

				if (m_sub_process[i].m_pid == -1)
				{
					m_stop = true;
					break;
				}
				send(m_sub_process[i].m_pipefd[0], (char*)&new_conn, sizeof(new_conn), 0);
				printf("send request to child %d\n", i);
			}
			/* 处理信号 */
			else if ((sockfd == sig_pipefd[0]) && (events[i].events & EPOLLIN))
			{
				int sig;
				char signals[1024];
				ret = recv(sig_pipefd[0], signals, sizeof(signals), 0);
				if (ret <= 0)
				{
					continue;
				}
				else
				{
					for (int i = 0; i < ret; ++i)
					{
						switch (signals[i])
						{
						case SIGCHLD:
						{
							pid_t pid;
							int stat;
							while ((pid = waitpid(-1, &stat, WNOHANG)) > 0)
							{
								for (int i = 0; i < m_process_number; ++i)
								{
									/* 子进程退出后，主进程需要关闭其通信管道， 并设置其m_pid = -1
										不需要释放资源，这是进程池 */
									if (m_sub_process[i].m_pid == pid)
									{
										printf("child %d join\n", i);
										close(m_sub_process[i].m_pipefd[0]);	//必须关闭
										m_sub_process[i].m_pid = -1;
									}
								}
							}

							m_stop = true;
							for (int i = 0; i < m_process_number; i++)
							{
								if (m_sub_process[i].m_pid != -1)
								{
									m_stop = false;
								}
							}
							break;
						}
						case SIGTERM:
						case SIGINT:
						{
							printf("kill all the child now\n");
							for (int i = 0; i < m_process_number; ++i)
							{
								int pid = m_sub_process[i].m_pid;
								if (pid != -1)
								{
									kill(pid, SIGTERM);
								}
							}
							break;
						}
						default:
						{
							break;
						}
						}
					}
				}
			}
			else
			{
				continue;
			}
			}
		}
		close(m_epollfd);
	}

#endif
