const int64_t page_size=4096;


enum{
    BLOCKTYPE_START=0,
    BLOCKTYPE_64=BLOCKTYPE_START,
    BLOCKTYPE_128,
    BLOCKTYPE_256,
    BLOCKTYPE_512,
    BLOCKTYPE_1024,
    BLOCKTYPE_2048,
    BLOCKTYPE_4096,
    BLOCKTYPE_COUNT
};
const int64_t pageconf[][3]={
    {64,0xFFFFFFFFFFFFFFFF,page_size/64},
    {128,0xFFFFFFFF,page_size/128},
    {256,0xFFFF,page_size/256},
    {512,0xFF,page_size/512},
    {1024,0xF,page_size/1024},
    {2048,0x3,page_size/2048},
    {4096,0x1,page_size/4096},
};

const int64_t nb_slot=BLOCKTYPE_COUNT;

struct zv_block_s{
    int64_t sizetype;
    int64_t used;
    void *mem;
    struct zv_block_s *next;
};

struct zv_slab_s{
    int blockcount;          //内存池中有多少的page
    struct zv_block_s *empty;
    struct zv_block_s *end;
    struct zv_block_s *slot[BLOCKTYPE_COUNT];
    void *pages;
    struct zv_block_s blocks[0];
};


struct zv_slab_s* zv_slab_init(void *pool,int64_t size){
    struct zv_slab_s *s=(struct zv_slab_s *)pool;
    s->blockcount=(size-sizeof(struct zv_slab_s))/(page_size + sizeof(struct zv_block_s));
    s->empty=s->blocks;
    s->end=s->blocks+s->blockcount-1;
    s->free=NULL;
    s->pages=(cahr *)pool+sizeof(struct zv_slab_s)+s->blockcount*sizeof(struct zv_block_s);
    for(int i=0;i<s->blockcount;i++){
        s->blocks[i].sizetype=64*2^i;
        s->blocks[i].mem=s->pages+i*page_size;
    }
    for(int i=0;i<nb_slot;i++){
        s->slot[i]=s->end;
    }
    return s;
}


//malloc

struct zv_block_s* zv_slab_add_block(struct zv_slab_s *s,int index){
    struct zv_block_s *b=s->slot[index];
    if(b==NULL){
        return NULL;
    }
    s->slot[index]=b->next;
    return b;
}


void *zv_slab_malloc(struct zv_slab_s *s,size_t size){
    if(size>page_size||s==NULL){
        return NULL;
    }
    int sizetype=BLOCKTYPE_START;
    while(size>pageconf[sizetype][0]){
        sizetype++;
    }
    struct zv_block_s *b=s->slot[sizetype];
    if(b==s->end){
        b=zv_slab_add_block(s,sizetype);
    }
    int64_t n=pageconf[sizetype][2];
    int64_t i = 0;
    while(i<n&&(b->used&(0x1<<i))) i++;
    b->used|=(0x1<<i);
    void *p=(char *)b->mem+i*pageconf[sizetype][0];
    return p;
}

//free
void zv_slab_free(struct zv_slab_s *s,void *p){
//     根据用户传入的指针 p，找到它属于哪个 page
//     再找到它是这个 page 里面第几个小块（idx）
//     然后把 used 里面对应 bit 清掉
    int64_t page=((char *)p-(char *)s->pages)/page_size;
    struct zv_block_s *b=s->blocks[page];
    int64_t idx=(p-s->pages)%(page_size/pageconf[b->sizetype][0]);


    b->used&=~(0x1<<idx);


}   

# if 1   //debug

const size_t size=20*1024*1024;
int main(){
    void *pool=malloc(size);
    if(pool==NULL){
        printf("malloc failed\n");
        return -1;
    }
    struct zv_slab_s *slab=zv_slab_init(pool,size);
    void *p=zv_slab_malloc(slab,100); 
    zv_slab_free(slab,p); 
    free(pool);
    return 0;
}
#endif