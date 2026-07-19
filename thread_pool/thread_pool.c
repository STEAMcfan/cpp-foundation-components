struct thrdpool_s{
    task_queue_t *task_queue;       //任务队列
    atomic_int quit;               //标识 线程池是否退出
    int thrd_count;                //线程池中的线程数
    pthread_t *threads;            //线程数组
};

typedef struct spinlock spinlock_t;

typedef struct task_s{
    void *next;
    handler_pt func;
    void *arg;
}task_t;


typedef struct task_queue_s{
    void *head;
    void **tail;
    int block;
    spinlock_t lock;
    pthread_mutex_t mutex;
    pthread_cond_t cond;
}task_queue_t;




static void __nonblock(task_queue_t *queue){
    pthread_mutex_lock(&queue->mutex);
    queue->block=0;
    pthread_mutex_unlock(&queue->mutex);
    pthread_cond_broadcast(&queue->cond);
}

static inline void __push_task(task_queue_t *queue,void *task){

    //task的内存大小是24字节，而link的内存大小是8字节，所以需要将task的内存大小转换为8字节的内存大小
    void **link=(void **)task;
    //此时*link可以是 next也可以是pnext
    *link=NULL;
    spinlock_lock(&queue->lock);
    *queue->tail=link;
    queue->tail=link;
    spinlock_unlock(&queue->lock);
    pthread_cond_signal(&queue->cond);
}
//不带阻塞
static inline void * __pop_task(task_queue_t *queue){
    spinlock_lock(&queue->lock);
    if(queue->head==NULL){
        spinlock_unlock(&queue->lock);
        return NULL;
    }
    task_t *task=(task_t *)queue->head;
    task =queue->head;
    queue->head=task->next;
    if(queue->head==NULL){
        queue->tail=&queue->head;
    }
    spinlock_unlock(&queue->lock);
    return task;
}

static inline void * __get_task(task_queue_t *queue){
    task_t *task;
    while((task=__pop_task(queue))==NULL){
        pthread_mutex_lock(&queue->mutex);
        if(queue->block==0){
            pthread_mutex_unlock(&queue->mutex);
            return NULL;
        }
        pthread_cond_wait(&queue->cond,&queue->mutex);
        pthread_mutex_unlock(&queue->mutex);
    }
    return task;
}

static void __taskqueue_destroy(task_queue_t *queue){
    task_t *task;
    while((task=__pop_task(queue))){
        free(task);
    }
    spinlock_destroy(&queue->lock);
    pthread_mutex_destroy(&queue->mutex);
    pthread_cond_destroy(&queue->cond);
    free(queue);
}


static void * __thrdpool_worker(void *arg){
    thrdpool_t *pool=(thrdpool_t *)arg;
    task_t *task;
    void *ctx;
    while(atomic_load(&pool->quit)==0){
        task=(task_t *)__get_task(pool->task_queue);
        if(!task){
            break;
        }
        handler_pt func=task->func;
        ctx=task->arg;
        free(task);
        func(ctx);
    }
    return NULL;
}

static task_queue_t *__taskqueue_create(void){
    int ret;
    task_queue_t *queue=malloc(sizeof(task_queue_t *));
    if(queue){
        ret=pthread_mutex_init(&queue->mutex,NULL);
        if(ret==0){
            ret=pthread_cond_init(&queue->cond,NULL);
            if(ret==0){
                spinlock_init(&queue->lock);
                queue->head=NULL;
                queue->tail=&queue->head;
                queue->block=1;
                return queue;
            }
            pthread_mutex_destroy(&queue->mutex);
        }
            free(queue);
    }
    return NULL;
}

static void __threads_terminate(thrdpool_t *pool){
    atomic_store(&pool->quit,1);
    __nonblock(pool->task_queue);
    for(int i=0;i<pool->thrd_count;i++){
        pthread_join(pool->threads[i],NULL);
    }
}


static int __threads_create(thrdpool_t *thrdpool,size_t thrd_count){
    pthread_attr_t attr;
    int ret;
    ret=pthread_attr_init(&attr);
    if(ret==0){
        pool->threads=malloc(thrd_count*sizeof(pthread_t));
        if(pool->threads){
            int i=0;
            for(i=0;i<thrd_count;i++){
                if(pthread_create(&pool->threads[i],&attr,__thrdpool_worker,pool)!=0){
                    break;
                }
            }
            pool->thrd_count=i;
            pthread_attr_destroy(&attr);
            if(i==thrd_count){
                return 0;     //所有线程都创建成功了
            }
            __threads_terminate(pool);     //如果不是所有的线程都创建成功那么就把之前的线程都销毁
            free(pool->threads);
        }
        ret=-1;
    }
    return ret;
}

void thrdpool_terminate(thrdpool_t *pool){
    atomic_store(&pool->quit,1);
    __nonblock(pool->task_queue);
}

//复杂创建使用回滚式编写代码的方式
thrdpool_t *thrdpool_create(int thrd_count){
    thrdpool_t *pool=malloc(sizeof(thrdpool_t));
    if(pool){
        int ret =__threads_create(pool,thrd_count);
        if(ret==0){
            task_queue_t *queue=__taskqueue_create();
            if(queue){
                pool->task_queue=queue;
                atomic_init(&pool->quit,0);
                return pool;
            }
        }
        free(pool);
    }
    return NULL;
}