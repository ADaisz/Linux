#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <signal.h>
#include <errno.h>
#include "threadpool.h"

#define DEFAULT_TIME 10                 /*10s检测一次*/
#define MIN_WAIT_TASK_NUM 10            /*如果queue_size > MIN_WAIT_TASK_NUM 添加新的线程到线程池*/ 
#define DEFAULT_THREAD_VARY 10          /*每次创建和销毁线程的个数*/
#define true 1
#define false 0

typedef struct{
    void *(*function)(void *);          //函数指针
    void *arg;
}threadpool_task_t;                     //各子线程任务结构体

struct threadpool_t{
    pthread_mutex_t lock;       //
    pthread_mutex_t thread_counter;     //忙线程个数

    pthread_cond_t queue_not_full;      //任务队列满时，添加任务的线程阻塞，等待此条件变量
    pthread_cond_t queue_not_empty;     //任务队列部位空时，通知等待任务的线程

    pthread_t *threads;                 //存放线程池中每个线程的tid
    pthread_t adjust_tid;               //管理线程tid
    threadpool_task_t *task_queue;      //任务队列

    int min_thr_num;                    //线程池最小线程数
    int max_thr_num;                    //线程池最大线程数
    int live_thr_num;                   //当前存活线程个数
    int busy_thr_num;                   //忙状态线程个数
    int wait_exit_thr_num;              //要销毁的线程个数

    int queue_front;                    //task_queue队头
    int queue_rear;                     //队尾
    int queue_size;                     //队列实际任务数
    int queue_max_size;                 //队列可容纳任务数上限

    int shutdown;                       //线程池使用状态
};


void *threadpool_thread(void *threadpool);

void *adjust_thread(void *threadpool);

int is_thread_alive(pthread_t tid);
int threadpool_free(threadpool_t *pool);


threadpool_t *threadpool_create(int min_thr_num,int max_thr_num,int queue_max_size){
    int i;
    threadpool_t *pool = NULL;


    //do while 为了防止使用goto语句，当遇到break时，直接跳过下面代码
    do{
        if((pool = (threadpool_t*)malloc(sizeof(threadpool_t))) == NULL){
            printf("malloc threadpool fail!\n");
            break;
        }
        pool->min_thr_num = min_thr_num;
        pool->max_thr_num = max_thr_num;
        pool->busy_thr_num = 0;
        pool->live_thr_num = min_thr_num;       //活着的线程数
        pool->wait_exit_thr_num = 0;
        pool->queue_size = 0;
        pool->queue_max_size = queue_max_size;  //最大任务队列数
        pool->queue_front = 0;
        pool->queue_rear = 0;
        pool->shutdown = false;             //不关闭线程

        //根据最大线程上限，给工作空间数组开辟空间，并清零
        pool->threads = (pthread_t *)malloc(sizeof(pthread_t)*max_thr_num);
        if(pool->threads == NULL){
            printf("malloc threads fail\n");
            break;
        }

        memset(pool->threads,0,sizeof(pthread_t)*max_thr_num);
        
        // 给任务队列开辟空间
        pool->task_queue = (threadpool_task_t *)malloc(sizeof(threadpool_task_t)*queue_max_size);
        if(pool->task_queue == NULL){
            printf("malloc task_queue fail.\n");
            break;
            
        }

        //初始化互斥锁，条件变量
        if(pthread_mutex_init(&(pool->lock),NULL) !=0
            || pthread_mutex_init(&(pool->thread_counter),NULL) != 0
            || pthread_cond_init(&(pool->queue_not_empty),NULL) != 0
            || pthread_cond_init(&(pool->queue_not_full),NULL) != 0)
        {
            printf("init the lock or cond fail.\n");
            break;
        }

        //启动min_thr_num 个work thread
        for(i =0;i<min_thr_num;i++){
            pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void *)pool);
            printf("start thread 0x%x...\n",(unsigned int)pool->threads[i]);
        }
        //创建管理者线程
        pthread_create(&(pool->adjust_tid),NULL,adjust_thread,(void *)pool);
        return pool;


    }while(0);

    threadpool_free(pool);
    return NULL;
}

//向线程池中添加一个任务
int threadpool_add(threadpool_t *pool,void*(*function)(void *arg),void *arg){
    
    pthread_mutex_lock(&(pool->lock));

    //队列已经满，调wait阻塞
    while((pool->queue_size == pool->queue_max_size) && (!pool->shutdown)){
        pthread_cond_wait(&(pool->queue_not_full),&(pool->lock));
    }

    if(pool->shutdown == true){
        pthread_cond_broadcast(&(pool->queue_not_empty));
        pthread_mutex_unlock(&(pool->lock));
        return 0;
    }

    //清空 工作线程 调用的回调函数 的参数 arg
    if(pool->task_queue[pool->queue_rear].arg != NULL){
        pool->task_queue[pool->queue_rear].arg = NULL;
    }
    
    //添加任务到任务队列
    pool->task_queue[pool->queue_rear].function = function;
    pool->task_queue[pool->queue_rear].arg = arg;

    pool->queue_rear = (pool->queue_rear + 1) % pool->queue_max_size;
    pool->queue_size++;             //向任务队列中添加一个任务


    //队列不为空时，唤醒线程池中 等待处理任务的线程
    pthread_cond_signal(&(pool->queue_not_empty));
    pthread_mutex_unlock(&(pool->lock));        //与144行lock对应
    return 0;
}

//线程池各个工作线程
void * threadpool_thread(void *threadpool){
    threadpool_t *pool = (threadpool_t *)threadpool;
    threadpool_task_t task;

    while(true){
        //刚创建出线程，等待任务队列里有任务，否则阻塞等待任务队列里有任务后再唤醒接收任务
        pthread_mutex_lock(&(pool->lock));

        //queue_size=0,说明没有任务，
        while((pool->queue_size == 0) && (!pool->shutdown)){
            printf("thread 0x%x is waiting\n",(unsigned int)pthread_self());
            //阻塞，解锁，等待，加锁
            pthread_cond_wait(&(pool->queue_not_empty),&(pool->lock));

            //清除空闲的线程，如果要结束的线程个数>0，结束线程
            if(pool->wait_exit_thr_num >0){
                pool->wait_exit_thr_num--;
                //如果线程池里线程个数大于最小值时，可以结束当前线程
                if(pool->live_thr_num > pool->min_thr_num){
                    printf("thread 0x%x is existing\n",(unsigned int)pthread_self());
                    pool->live_thr_num--;
                    pthread_mutex_unlock(&(pool->lock));

                    pthread_exit(NULL);
                }
            }
        }
        //销毁线程池
        if(pool->shutdown == true){
            pthread_mutex_unlock(&(pool->lock));
            printf("thread 0x%x is existing\n",(unsigned int)pthread_self());
            pthread_detach(pthread_self());
            pthread_exit(NULL);     //线程自行结束
        }
        //从任务队列里获取任务，出队操作
        task.function = pool->task_queue[pool->queue_front].function;
        task.arg = pool->task_queue[pool->queue_front].arg;

        pool->queue_front = (pool->queue_front +1)%pool->queue_max_size;
        pool->queue_size--;

        //通知可以有新任务添加
        pthread_cond_broadcast(&(pool->queue_not_full));
        //任务取出后，释放线程
        pthread_mutex_unlock(&(pool->lock));

        //执行任务
        printf("thread 0x%x is start working\n",(unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thr_num++;
        pthread_mutex_unlock(&(pool->thread_counter));
        
        //执行回调函数
        (*(task.function))(task.arg);

        //任务处理结束
        printf("thread 0x%x is end working\n",(unsigned int)pthread_self());
        pthread_mutex_lock(&(pool->thread_counter));
        pool->busy_thr_num--;
        pthread_mutex_unlock(&(pool->thread_counter));
    }
    pthread_exit(NULL);
}

void *adjust_thread(void *threadpool){
    int i;
    threadpool_t *pool = (threadpool_t *)threadpool;

    while(!pool->shutdown){
        sleep(DEFAULT_TIME);        //定时对线程池管理

        //管理者将所需的变量从公共区取出
        pthread_mutex_lock(&(pool->lock));
        int queue_size = pool->queue_size;
        int live_thr_num = pool->live_thr_num;
        pthread_mutex_unlock(&(pool->lock));

        pthread_mutex_lock(&(pool->thread_counter));
        int busy_thr_num = pool->busy_thr_num;
        pthread_mutex_unlock(&(pool->thread_counter));

        //创建新线程算法:
        //任务数>=最小线程池个数 且 存活线程个数 < 最大线程个数
        if(queue_size >= MIN_WAIT_TASK_NUM && live_thr_num < pool->max_thr_num){
            pthread_mutex_lock(&(pool->lock));
            int add = 0;

            //一次增加 DEFAULT_THREAD_VARY个线程
            for(i = 0;i < pool->max_thr_num && add < DEFAULT_THREAD_VARY
                    && pool->live_thr_num < pool->max_thr_num;i++){
                if(pool->threads[i]==0 || !is_thread_alive(pool->threads[i])){
                    pthread_create(&(pool->threads[i]),NULL,threadpool_thread,(void*)pool);
                    add++;
                    pool->live_thr_num++;
                }
            }

            pthread_mutex_unlock(&(pool->lock));
        }

        //销毁线程算法:
        //忙线程数*2 <存活线程数 且 存活线程数 > 最小线程数
        if((busy_thr_num * 2) < live_thr_num &&live_thr_num > pool->min_thr_num){
            pthread_mutex_lock(&(pool->lock));
            pool->wait_exit_thr_num = DEFAULT_THREAD_VARY;
            pthread_mutex_unlock(&(pool->lock));
            
            for(i=0;i<DEFAULT_THREAD_VARY;i++){
                //通知处在空闲状态的线程，它们会自行终止
                pthread_cond_signal(&(pool->queue_not_empty));
            }
        }
    }
    return NULL;
}

int threadpool_destory(threadpool_t *pool){
    int i;
    if(pool == NULL){
        return -1;
    }
    pool->shutdown = true;

    pthread_join(pool->adjust_tid,NULL);

    for(i = 0;i< pool->live_thr_num;i++){
        pthread_cond_broadcast(&(pool->queue_not_empty));
    }
    for(i = 0;i< pool->live_thr_num;i++){
        pthread_join(pool->threads[i],NULL);
    }
    threadpool_free(pool);
    return 0;
}

int threadpool_free(threadpool_t *pool){
    if(pool == NULL){
        return -1;
    }
    if(pool->task_queue){
        free(pool->task_queue);
    }
    if(pool->threads){
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

int threadpool_all_threadnum(threadpool_t *pool){
    int all_threadnum = -1;
    pthread_mutex_lock(&(pool->lock));
    all_threadnum = pool->live_thr_num;
    pthread_mutex_unlock(&(pool->lock));
    return all_threadnum;
}

int threadpool_busy_threadnum(threadpool_t *pool){
    int busy_threadnum = -1;
    pthread_mutex_lock(&(pool->thread_counter));
    busy_threadnum = pool->busy_thr_num;
    pthread_mutex_unlock(&(pool->thread_counter));
    return busy_threadnum;
}

int is_thread_alive(pthread_t tid){
    int kill_rc = pthread_kill(tid, 0);     //发0号信号，测试线程是否存活
    if (kill_rc == ESRCH) {
        return false;
    }
    return true;
}


/*测试*/ 

#if 1

//线程池中的线程，模拟处理任务
void *process(void *arg){
    printf("thread 0x%x working on task %d\n",(unsigned int)pthread_self(),*(int*)arg);
    sleep(1);       //表示处理任务
    printf("task %d is end\n",*(int*)arg);
    return NULL;
}

int main(){
    // 创建线程池
    threadpool_t *thp = threadpool_create(3,100,100);


    printf("pool inited\n");

    int num[20],i;
    
    //模拟任务产生
    for(i =0;i<20;i++){
        num[i] = i;
        printf("add task %d\n",i);
        //向线程池中添加任务
        threadpool_add(thp,process,(void*)&num[i]);
    }

    // 等待子线程完成
    sleep(5);
    threadpool_destory(thp);

    return 0;
}

#endif
