#pragma once

#include <list>
#include <unordered_map>
#include <cassert>
#include <utility>
#include <iterator>
#include <type_traits>
#include <cstddef>

template<typename T>
struct DefaultValueDeleter{
    void operator()(T* value){
        if constexpr(std::is_pointer_v<T>){
            delete value;
            value=nullptr;
        }
    }
};

template<typename K,typename V,class ValueDeleter = DefaultValueDeleter<V>>
class LRUCache{
private:
    struct Node{
        K key;
        V value;
        int ref;
        typename std::list<Node>::iterator list_pos;
        Node(const K& k,V&& v):key(k),value(std::move(v)),ref(1){}
        Node(const K& k,const V& v):key(k),value(v),ref(1){}

    };
public:
    using ListNodeIterator = typename std::list<Node>::iterator;
    LRUCache(size_t capacity):capacity(capacity),size(0){
        cache.reserve(capacity);
    }
    ~LRUCache(){
        assert(in_use.empty());
        while(!not_use.empty()){
            auto it = not_use.begin();
            cache.erase(it->key);
            value_deleter(it->value);
            not_use.erase(it);
            size--;
        }
    }
//RAII对象，自动管理引用计数
class HandleGuard{
private:
    friend class LRUCache<K,V,ValueDeleter>;
    Node* node;
    LRUCache *cache;
    HandleGuard():cache(nullptr),node(nullptr){}
    explicit HandleGuard(LRUCache *cache,Node *node):cache(cache),node(node){
        if(cache && node){
            cache->ref_node(node);
        }
    }
public:
    HandleGuard(const HandleGuard&) = delete;
    HandleGuard& operator=(const HandleGuard&) = delete;

    HandleGuard(HandleGuard&& other) noexcept
        : node(other.node), cache(other.cache)
    {
        other.node = nullptr;
        other.cache = nullptr;
    }

    HandleGuard& operator=(HandleGuard&& other) noexcept
    {
        if (this != &other) {
            reset();

            node = other.node;
            cache = other.cache;

            other.node = nullptr;
            other.cache = nullptr;
        }

        return *this;
    }
    ~HandleGuard(){
        reset();
    }
    void reset(){
        if(valid()){
            cache->unref_node(node);
            cache = nullptr;
            node = nullptr;
        }
    }
    V & value() const{
        assert(valid());
        return node->value;
    }
    bool valid() const{return (cache && node);}
    explicit operator bool() const{return valid();}
};
HandleGuard get(const K& key){
    auto it = cache.find(key);
    if(it != cache.end())
        return HandleGuard(this,&(*it->second));
    return HandleGuard();
}
template<typename VAL>
HandleGuard put(const K& key,VAL&& value){
    static_assert(std::is_constructible_v<V,VAL&&>,"value type must be the same as V");
    auto it = cache.find(key);
    if(it != cache.end()){
        it->second->value = std::forward<VAL>(value);  //左值，拷贝构造；右值，移动赋值
        return HandleGuard(this,&(*it->second));
    }
    not_use.emplace_back(key,std::forward<VAL>(value));
    auto listIt = std::prev(not_use.end());
    listIt->list_pos = listIt;

    cache.emplace(key, listIt);
    size++;
    HandleGuard handle(this,&(*listIt));
    evict();
    return handle;
}
private:
    void ref_node(Node* node){
        if(node->ref == 1){
            in_use.splice(in_use.end(),not_use,node->list_pos);
        }
        node->ref++;
    }
    void unref_node(Node* node){
        assert(node->ref > 1);
        node->ref--;
        if(node->ref == 1){
            not_use.splice(not_use.end(),in_use,node->list_pos);
        }
    }
    void evict(){
        if(capacity>0){
            while(size > capacity && !not_use.empty()){
                auto it = not_use.begin();
                cache.erase(it->key);
                value_deleter(it->value);
                not_use.erase(it);
                size--;
            }
        } 
    }
    size_t capacity;
    size_t size;

    std::list<Node> not_use;
    std::list<Node> in_use;
    std::unordered_map<K, ListNodeIterator> cache;
    ValueDeleter value_deleter;
};