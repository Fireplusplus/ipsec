#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>

#include <iostream>
#include <unordered_map>
#include <event2/event.h>

#include "log.h"
#include "mem.h"
#include "ev.h"

using namespace std;

struct event_action_st {
	int (*create)();
	void (*destroy)(int);
	void (*on_do)(int, short, void *);
	const char *desc;
};

#define ZONE_MAX	8										/* 最大支持区域数 */

static uint32_t easy_ev_rss(int fd, void *arg);

static unordered_map<int, struct event *> s_ev_list;		/* 事件缓存表 */
struct event_base* s_ev_base[ZONE_MAX];						/* event base */
pthread_t s_ev_thread[ZONE_MAX];							/* 每线程处理一个区 */
static int s_zone_size;										/* 实际区域数(线程数/ev_base数) */
ev_rss s_ev_rss = easy_ev_rss;								/* 区域选择 */

#define CUR_EV_BASE(_fd, _arg)		(s_ev_base[s_ev_rss(_fd, _arg) % s_zone_size])

struct timer_st {
	struct event *ev;
	void (*user_fn)(void *arg);		/* 用户回调 */
	void *user_data;				/* 用户回调参数 */
};

static void ev_timer_callback(int fd, short what, void *arg){
	struct timer_st *tm = (struct timer_st *)arg;
	assert(tm != NULL);
	
	event_del(tm->ev);
	event_free(tm->ev);

	if (tm->user_fn)
		tm->user_fn(tm->user_data);

	free(tm);
}

/*
 * @brief 超时事件
 * @param[in] timeout 超时时间(s)
 * @param[in] fn 超时事件回调
 * @param[in] arg fn的参数
 * @return <0: 失败 0: 成功
 */
int ev_timer(int timeout, void (*fn)(void *), void *arg)
{
	struct event_base *eb = s_ev_base[0];
	assert(eb != NULL);

	struct timer_st *tm = (struct timer_st*)alloc_die(sizeof(struct timer_st));

	tm->ev = evtimer_new(eb, ev_timer_callback, tm);
	if (!tm->ev) {
		DEBUG("create event failed");
		return -1;
	}

	tm->user_fn = fn;
	tm->user_data = arg;

	struct timeval tv;
	evutil_timerclear(&tv);
	tv.tv_sec = timeout;

	event_add(tm->ev, &tv);
	return 0;
}

/*
 * @brief 注册事件
 * @param[in] fd 文件描述符
 * @param[in] fn 读事件回调
 * @param[in] arg fn的第三个参数
 * @return <0: 失败 0: 成功
 */
int ev_register(int fd, ev_callback fn, void *arg)
{
	if (fd < 0 || !fn)
		return -1;
	
	if (s_ev_list.find(fd) != s_ev_list.end())
		return 0;
	
	struct event_base *eb = CUR_EV_BASE(fd, arg);

	struct event *ev = event_new(eb, fd, EV_READ | EV_PERSIST, fn, arg);
	if (!ev) {
		DEBUG("create event failed");
		return -1;
	}

	event_add(ev, NULL);
	s_ev_list[fd] = ev;
	return 0;
}

/*
 * @brief 注销事件
 * @param[in] fd 文件描述符
 * @return 无
 */
int ev_unregister(int fd)
{
	if (fd < 0)
		return -1;
	
	unordered_map<int, struct event *>::iterator it = s_ev_list.find(fd);
	if (it == s_ev_list.end()) {
		DEBUG("not find event, destroy failed !");
		return -1;
	}

	event_del(it->second);
	event_free(it->second);

	s_ev_list.erase(it);
	return 0;
}

/*
 * @brief 外部主动事件分发
 * @return 无
 */
void ev_run()
{
	if (!s_ev_thread[0])
		event_base_dispatch(s_ev_base[0]);
	else
		WARN("ev auto dispatch, not need call ev_run()");
}

/* 内置的事件分发函数 */
static uint32_t easy_ev_rss(int fd, void *arg)
{
	static uint32_t rss = 0;
	return rss++;
}

void * do_thread(void *arg)
{
	pthread_detach(pthread_self());

	int id = (int)(uint64_t)arg;
	DEBUG("thread: %u, id: %d", (uint32_t)pthread_self(), id);
	struct event_base *eb = s_ev_base[id];

	while (1) {
		event_base_dispatch(eb);	/* 循环分发, 没有事件的时候会退出, sleep后重新进入 */
		sleep(3);
	}

	return NULL;
}

/*
 * @brief 设置区域分发函数

 * @return无
 */
static void set_ev_rss(ev_rss fn)
{
	if (fn)
		s_ev_rss = fn;
}

/*
 * @brief 初始化event环境
 * @param[in] nzone 划分的区域数: <= 0: 不划分区域，外部分发 >0: 内部划分nzone区域并用nzone个线程分发
 * @param[in] fn 分发函数
 * @return <0: 失败 0: 成功
 */
int ev_init(int nzone, ev_rss fn)
{
	if (nzone > ZONE_MAX)
		nzone = ZONE_MAX;
	
	s_zone_size = nzone > 0 ? nzone : 1;

	for (int i = 0; i < s_zone_size; i++) {
		s_ev_base[i] = event_base_new();
		if (!s_ev_base[i]) {
			ERROR("init event base failed");
			goto failed;
		}

		if (nzone <= 0)		/* 不使用内部线程 */
			break;
		
		if (pthread_create(&s_ev_thread[i], NULL, do_thread, (void*)(uint64_t)i) < 0) {
			ERROR("start thread failed");
			goto failed;
		}
		
		DEBUG("init base(%p), pthread(%u)", s_ev_base[i], (uint32_t)s_ev_thread[i]);
	}

	set_ev_rss(fn);

	return 0;

failed:
	for (int i = 0; i < nzone; i++) {
		if (s_ev_base[i])
			event_base_free(s_ev_base[i]);
		if (s_ev_thread[i])
			pthread_cancel(s_ev_thread[i]);
	}

	s_zone_size = 0;
	return -1;
}
