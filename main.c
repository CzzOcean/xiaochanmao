#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>

#define DEFAULT_TIME        10   // 10s检测一次
#define MIN_WAIT_TASK_NUM   10   // 如果 queue_size > MIN_WAIT_TASK_NUM 添加新的线程到线程池
#define DEFAULT_THREAD_VARY 10   // 每次创建和销毁线程的个数
#define true  1
#define false 0


typedef struct {
    void *(*function)(void *);  // 指针函数，回调函数
    void *arg;                  // 上面函数的参数
} threadpool_task_t; 

typedef struct threadpool_t {               
    pthread_mutex_t lock;            // 用于锁住本结构体
    pthread_mutex_t thread_counter;  // 记录忙状态线程个数的锁 busy_thr_num
 
    pthread_cond_t queue_not_full;   // 当任务队列满时，添加任务的线程阻塞，等待此条件变量
    pthread_cond_t queue_not_empty;  // 当任务队列里不为空时，通知等待任务的线程
 
    pthread_t *threads;              // 存放线程池中每个线程的 tid 数组
    pthread_t adjust_tid;            // 管理线程 tid
    threadpool_task_t *task_queue;   // 任务队列(数组首地址)
    
    int min_thr_num;        // 线程池最小线程数 
    int max_thr_num;        // 线程池最大线程数
    int live_thr_num;       // 当前存活线程个数
    int busy_thr_num;       // 忙状态线程个数
    int wait_exit_thr_num;  // 要销毁的线程个数
 
    int queue_front;        // task_queue 队头下标
    int queue_rear;         // task_queue 队尾下标
    int queue_size;         // task_queue 队中实际任务数
    int queue_max_size;     // task_queue 队列可容纳任务数上限
 
    int shutdown;           // 标志位，线程池使用状态，true 或 false
} threadpool_t;

void *threadpool_thread(void *threadpool)
{
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;
    while(true)
    {       
        pthread_mutex_lock(&(pool->lock));
        while((pool->queue_size == 0) && (!pool->shutdown))
        {
            printf("thread 0x%x is waiting\n", (unsigned int)pthread_self());
            pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));
            // 清除指定数目的空闲线程，如果要结束的线程个数大于 0 ,结束线程
            if(pool->wait_exit_thr_num>0)
            {
                pool->wait_exit_thr_num--;
                // 如果线程池里线程个数大于最小值时可以结束当前线程
                if(pool->live_thr_num > pool->min_thr_num)
                {
                    printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
                    pool->live_thr_num--; 
                    pthread_mutex_unlock(&(pool->lock));
                    pthread_exit(NULL);
                }
            }
        }
        // 如果指定了 true,要关闭线程池里的每个线程,自行退出处理---销毁线程池
        if(pool->shutdown)
        {       
            pthread_mutex_unlock(&(pool->lock));
            printf("thread 0x%x is exiting\n", (unsigned int)pthread_self());
            pthread_detach(pthread_self());
            pthread_exit(NULL);  // 线程自行结束
        }
         // 从任务队列里获取任务,是一个出队操作
        task.function = pool->task_queue[pool->queue_front].function;
        task.arg = pool->task_queue[pool->queue_front].arg;     
        pool->queue_front = (pool->queue_front + 1) % pool->queue_max_size;  // 出队,模拟环形队列
        pool->queue_size--;  
        // 通知可以有新的任务添加进来
        pthread_cond_broadcast(&(pool->queue_not_full));
        // 任务取出后,立即将线程池琐释放
        pthread_mutex_unlock(&(pool->lock));
        // 执行任务
        printf("thread 0x%x start working\n", (unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));  // 忙状态线程数变量琐
        pool->busy_thr_num++;                         // 忙状态线程数+1
        pthread_mutex_unlock(&(pool->thread_counter)); 
        (*(task.function))(task.arg);  // 执行回调函数任务
        // task.function(task.arg)；   // 执行回调函数任务
        // 任务结束处理
        printf("thread 0x%x end working\n", (unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thr_num--;   // 处理掉一个任务,忙状态数线程数 -1
        pthread_mutex_unlock(&(pool->thread_counter));
    }
     pthread_exit(NULL);
}

void *adjust_thread(void *threadpool)
{
    int i;
    threadpool_t *pool = (threadpool_t *)threadpool;
    while(!pool->shutdown)
    {
    sleep(DEFAULT_TIME);    //管理者不用时时刻刻盯着，过一会来转一下就行
    pthread_mutex_lock(&(pool->lock));
    int queue_size=pool->queue_size;
    int live_thr_num=pool->live_thr_num;
    pthread_mutex_unlock(&(pool->lock));

    pthread_mutex_lock(&(pool->thread_counter));
    int busy_thr_num=pool->busy_thr_num;
    pthread_mutex_unlock(&(pool->thread_counter));

    if(queue_size>=MIN_WAIT_TASK_NUM&&live_thr_num<=pool->max_thr_num)
    {
        pthread_mutex_lock(&(pool->lock));
        int add = 0;
        // 一次增加 DEFAULT_THREAD 个线程
        for(i=0;i<pool->max_thr_num&&add < DEFAULT_THREAD_VARY&&pool->live_thr_num < pool->max_thr_num; i++)
        {
            if (pool->threads[i] == 0 || !is_thread_alive(pool->threads[i]))//如果线程为空或者不活跃，
              {
                    pthread_create(&(pool->threads[i]), NULL, threadpool_thread, (void *)pool);//简单说就是让线程活跃
              }                                                              //可能需要采取一些操作，比如创建一个新线程来替代这个线程
                                                                            //，以确保线程池中的线程一直处于可用状态，
                                                                            //能够执行任务。这是线程池管理的一部分，
                                                                            //用于维护线程的健康状态和可用性。
        }
        pthread_mutex_unlock(&(pool->lock));
    }
            // 销毁多余的空闲线程 算法: 忙线程 X2 小于 存活的线程数 且 存活的线程数 大于 最小线程数时
            if((busy_thr_num * 2) < live_thr_num && live_thr_num > pool->min_thr_num) {
            // 一次销毁 DEFAULT_THREAD 个线程，随即 10 个即可
            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;  // 要销毁的线程数设置为 10
            pthread_mutex_unlock(&(pool->lock));
 
            for (i = 0; i < DEFAULT_THREAD_VARY; i++) {
                // 通知处在空闲状态的线程，他们会自行停止

                pthread_cond_signal(&(pool->queue_not_empty));  //对应到threadpool_thread的pthread_cond_wait(&(pool->queue_not_empty), &(pool->lock));来进行他下面的结束线程操作
            }
        }
    }

        return NULL;
}


threadpool_t *threadpool_creat(int min_thr_num, int max_thr_num, int queue_max_size)
{
    int i;
    threadpool_t *pool=NULL;
    do{
        pool=(threadpool_t*)malloc(sizeof(threadpool_t));

        pool->min_thr_num = min_thr_num;
        pool->max_thr_num = max_thr_num;
        pool->busy_thr_num = 0;
        pool->live_thr_num = min_thr_num;  // 活着的线程数初值=最小线程数
        pool->wait_exit_thr_num = 0;
        pool->queue_size = 0;              // 有0个产品
        pool->queue_max_size = queue_max_size;  // 最大任务队列数
        pool->queue_front = 0;
        pool->queue_rear = 0;
        pool->shutdown = false;            // 不关闭线程池
    
    // 根据最大线程上限数，给工作线程数组开辟空间，并清零
     pool->threads=(pthread_t*)malloc(sizeof(pthread_t)*max_thr_num);
     memset(pool->threads, 0, sizeof(pthread_t) * max_thr_num);  // 数组清零
      
    // 给任务队列开辟空间
        pool->task_queue=(threadpool_task_t *)malloc(sizeof(threadpool_task_t)*queue_max_size);

    // 初始化互斥琐、条件变量
    pthread_mutex_init(&(pool->lock),NULL);
    pthread_mutex_init(&(pool->thread_counter),NULL);
    pthread_cond_init(&(pool->queue_not_empty), NULL); //当任务队列满时，添加任务的线程阻塞，等待此条件变量
    pthread_cond_init(&(pool->queue_not_full),NULL);    //当任务队列里不为空时，通知等待任务的线程

    // 启动 min_thr_num 个 work thread
    for(i=0;i<pool->min_thr_num;i++)
    {
        pthread_creat(&pool->threads[i],NULL, threadpool_thread, (void *)pool);
        printf("start thread 0x%x...\n", (unsigned int)pool->threads[i]);
    }
    // 创建管理者线程
    pthread_create(&(pool->adjust_tid), NULL, adjust_thread, (void *)pool);  

    return pool;

    }while(0);

      //  threadpool_free(pool); // 前面代码调用失败时，释放poll存储空间

      // return NULL;

}
void *process(void *arg)  //模拟完成任务的函数
{
    printf("thread 0x%x working on task %d\n",(unsigned int)pthread_self,(int)arg);
    sleep(1);
    printf("task %d is end\n",(int)arg);
    return NULL;
}

int threadpool_add(threadpool_t *pool, void*(*function)(void *arg), void *arg)
{
    pthread_mutex_lock(&(pool->lock));

    while((pool->queue_size==pool->queue_max_size)&&(!pool->shutdown))
    {
        pthread_cond_wait(&(pool->queue_not_full),&(pool->lock));
    }

    /*if(pool->shutdown)这段代码用于在向线程池添加任务之前检查线程池是否已经关闭。
    如果线程池已关闭，则不会添加新任务，而是通知可能在等待任务的线程，并解锁互斥锁，
    最后返回0以表示任务未被添加。这有助于确保线程池在关闭后不再接受新的任务，并且能够正确地通知等待线程以便它们能够完成工作并退出*/
    if (pool->shutdown) {
        pthread_cond_broadcast(&(pool->queue_not_empty));
        pthread_mutex_unlock(&(pool->lock));
        return 0;
    }

    if (pool->task_queue[pool->queue_rear].arg != NULL) {
        pool->task_queue[pool->queue_rear].arg = NULL;
    }

    pool->task_queue[pool->queue_rear].function = function;
    pool->task_queue[pool->queue_rear].arg = arg;
    pool->queue_rear = (pool->queue_rear + 1) % pool->queue_max_size;  // 队尾指针移动，模拟环形
    pool->queue_size++;

    // 添加完任务后,队列不为空,唤醒线程池中等待处理任务的线程
    pthread_cond_signal(&(pool->queue_not_empty));    //从这里唤醒后会唤醒threadpool_thread()函数中被wait的那一步并且向下执行
    pthread_mutex_unlock(&(pool->lock));
 
    return 0;

}
int threadpool_destroy(threadpool_t *pool)
{
    int i;
    if (pool == NULL) {
        return -1;
    }
    pool->shutdown = true;
    
    // 先销毁管理线程
    pthread_join(pool->adjust_tid, NULL);
    for (i = 0; i < pool->live_thr_num; i++) {
        // 通知所有的空闲线程
        pthread_cond_broadcast(&(pool->queue_not_empty));
    }
    for (i = 0; i < pool->live_thr_num; i++) {
        pthread_join(pool->threads[i], NULL);
    }
    threadpool_free(pool);
 
    return 0; 
}

int threadpool_free(threadpool_t *pool)
{
    if(pool == NULL) {
        return -1;
    }
    if(pool->task_queue) {
        free(pool->task_queue);
    }
    if (pool->threads) {
        free(pool->threads);
        pthread_mutex_lock(&(pool->lock));
        pthread_mutex_destroy(&(pool->lock)); 
        pthread_mutex_lock(&(pool->thread_counter));
        pthread_mutex_destroy(&(pool->thread_counter));
        pthread_cond_destroy(&(pool->queue_not_empty));
        pthread_cond_destroy(&(pool->queue_not_full));
    }
    free(pool);
    pool = NULL;
    return 0;
}


int main()
{

    threadpool_t *thp = threadpool_create(3, 100, 100);
    printf("pool inited");

    int num[20],i;
    for(i=0;i<20;i++)
    {
        num[i]=i;
        printf("add task %d\n",i);      
        threadpool_add(thp,process,(void*)&num[i]); //第三个参数看实际情况，比如那个小写转大写的程序，
                                                    //第三个参数就是穿客户端的文件描述符，对应process函数里面应该实现转大写的功能
    }
   
    return 0;
}

