#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#define MEM_PAGE_SIZE  4096

typedef struct mempool_s{
    int block_size;
    int free_count;

    void *mem;
    void *ptr;
}mempool_t;

int mempool_init(mempool_t *mp ,size_t block_size){
    if(!mp) return -1;
    memset(mp,0,sizeof(mempool_t));
    mp->mem = malloc(MEM_PAGE_SIZE);
    mp->block_size=block_size;
    mp->free_count=MEM_PAGE_SIZE/block_size;
    if(!mp->mem) return -1;
    mp->ptr=mp->mem;
    char *ptr=mp->ptr;
    for(int i=0;i<mp->free_count;i++){
        *(char**) ptr=ptr+block_size;
        ptr+=block_size;
    }
    *(char**) ptr=NULL;
    return 0;
}

void *_malloc(mempool_t *mp, size_t size){   //这里为什么要是*malloc,以及size_t
    if(!mp||mp->block_size<size||mp->free_count<=0) return NULL;
    void *ptr=mp->ptr;
    mp->ptr=*(char **)ptr;
    mp->free_count--;
    printf("malloc\n");
    return ptr;
}

void _free(mempool_t *mp,void *ptr){
    if(!mp)return;
    *(char **)ptr=mp->ptr;
    mp->ptr=ptr;
    mp->free_count++;
    printf("free\n");
}
#define malloc(mp,size) _malloc(mp,size)
#define free(mp,ptr) _free(mp,ptr)

int main(){
    mempool_t mp;
    mempool_init(&mp,32);
    void *p1=malloc(&mp,5);
    printf("p1=%p\n",p1);
    void *p2=malloc(&mp,10);
    printf("p2=%p\n",p2);
    void *p3=malloc(&mp,15);
    printf("p3=%p\n",p3);
    void *p4=malloc(&mp,20);
    printf("p4=%p\n",p4);

    free(&mp,p1);
    printf("mp.ptr=%p\n",mp.ptr);
    free(&mp,p2);
    printf("mp.ptr=%p\n",mp.ptr);
    void *p5=malloc(&mp,25);
    printf("p5=%p\n",p5);
    free(&mp,p3);
    printf("mp.ptr=%p\n",mp.ptr);
    free(&mp,p4);
    printf("mp.ptr=%p\n",mp.ptr);
}