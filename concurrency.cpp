#include "ultrasearch.hpp"
#include <iostream>

const std::uintmax_t SMALL_FILE_BYTES = 1ULL * 1024 * 1024; // 1 MB
const std::size_t SMALL_DIR_ENTRIES = 500;
const std::uintmax_t MIN_CHUNK_BYTES = 512ULL * 1024;       // 512 KB

int g_use_color = 1;

std::string col(const char* c, std::string_view s) {
    if (!g_use_color) return std::string(s);
    return std::string(c) + std::string(s) + "\033[0m";
}

std::size_t g_files_scanned = 0;
std::size_t g_matches_found = 0;
std::size_t g_dirs_visited  = 0;
std::size_t g_errors        = 0;

static pthread_mutex_t g_stats_mtx;
static pthread_mutex_t g_out_mtx;

void stats_init()    { pthread_mutex_init(&g_stats_mtx, NULL); }
void stats_destroy() { pthread_mutex_destroy(&g_stats_mtx); }

// Using batching: threads will calculate hits locally and call this ONCE.
void stats_add_matches(std::size_t n) {
    pthread_mutex_lock(&g_stats_mtx);
    g_matches_found += n;
    pthread_mutex_unlock(&g_stats_mtx);
}
void stats_add_files(std::size_t n) {
    pthread_mutex_lock(&g_stats_mtx);
    g_files_scanned += n;
    pthread_mutex_unlock(&g_stats_mtx);
}
void stats_add_dirs(std::size_t n) {
    pthread_mutex_lock(&g_stats_mtx);
    g_dirs_visited += n;
    pthread_mutex_unlock(&g_stats_mtx);
}
void stats_add_errors(std::size_t n) {
    pthread_mutex_lock(&g_stats_mtx);
    g_errors += n;
    pthread_mutex_unlock(&g_stats_mtx);
}

void out_init()    { pthread_mutex_init(&g_out_mtx, NULL); }
void out_destroy() { pthread_mutex_destroy(&g_out_mtx); }

void out_print(const std::string& line) {
    pthread_mutex_lock(&g_out_mtx);
    std::cout << line << '\n';
    pthread_mutex_unlock(&g_out_mtx);
}

void out_match(const std::string& path, std::size_t line_no, std::string_view line_text) {
    // Convert string_view to a standard string first
    std::string ltext(line_text);
    
    // Truncate and add "..." if it's too long
    if (ltext.size() > 150) {
        ltext.resize(150);
        ltext += "...";
    }

    pthread_mutex_lock(&g_out_mtx);
    std::cout << col("\033[1;36m", path) << ":" 
              << col("\033[1;33m", std::to_string(line_no)) 
              << ": " << ltext << '\n';
    pthread_mutex_unlock(&g_out_mtx);
}

void out_file_hit(const std::string& path) {
    pthread_mutex_lock(&g_out_mtx);
    std::cout << col("\033[1;32m", "FOUND ") << col("\033[1m", path) << '\n';
    pthread_mutex_unlock(&g_out_mtx);
}

void out_warn(const std::string& msg) {
    pthread_mutex_lock(&g_out_mtx);
    std::cerr << col("\033[1;31m", "WARN: ") << msg << '\n';
    pthread_mutex_unlock(&g_out_mtx);
}

// Lab 8: POSIX Thread Pool Worker
static void* pool_worker(void* raw) {
    ThreadPool* pool = (ThreadPool*)raw;
    for (;;) {
        pthread_mutex_lock(&pool->pool_mtx);
        while (pool->queue.empty() && !pool->stop)
            pthread_cond_wait(&pool->queue_cond, &pool->pool_mtx);
        if (pool->stop && pool->queue.empty()) {
            pthread_mutex_unlock(&pool->pool_mtx);
            return NULL;
        }
        Task t = pool->queue.front();
        pool->queue.pop();
        ++(pool->active);
        pthread_mutex_unlock(&pool->pool_mtx);

        t.fn(t.arg);

        pthread_mutex_lock(&pool->pool_mtx);
        --(pool->active);
        if (pool->queue.empty() && pool->active == 0)
            pthread_cond_broadcast(&pool->done_cond);
        pthread_mutex_unlock(&pool->pool_mtx);
    }
}

void pool_init(ThreadPool& pool, std::size_t n) {
    pool.active = 0;
    pool.stop   = 0;
    pthread_mutex_init(&pool.pool_mtx,  NULL);
    pthread_cond_init(&pool.queue_cond, NULL);
    pthread_cond_init(&pool.done_cond,  NULL);
    pool.threads.resize(n);
    for (std::size_t i = 0; i < n; ++i)
        pthread_create(&pool.threads[i], NULL, pool_worker, &pool);
}

void pool_submit(ThreadPool& pool, void (*fn)(void*), void* arg) {
    pthread_mutex_lock(&pool.pool_mtx);
    pool.queue.push({fn, arg});
    pthread_mutex_unlock(&pool.pool_mtx);
    pthread_cond_signal(&pool.queue_cond);
}

void pool_wait_all(ThreadPool& pool) {
    pthread_mutex_lock(&pool.pool_mtx);
    while (!pool.queue.empty() || pool.active > 0)
        pthread_cond_wait(&pool.done_cond, &pool.pool_mtx);
    pthread_mutex_unlock(&pool.pool_mtx);
}

void pool_destroy(ThreadPool& pool) {
    pthread_mutex_lock(&pool.pool_mtx);
    pool.stop = 1;
    pthread_mutex_unlock(&pool.pool_mtx);
    pthread_cond_broadcast(&pool.queue_cond);
    for (auto& tid : pool.threads)
        pthread_join(tid, NULL);
    pthread_mutex_destroy(&pool.pool_mtx);
    pthread_cond_destroy(&pool.queue_cond);
    pthread_cond_destroy(&pool.done_cond);
}

std::string to_lower(std::string s) {
    for (char& c : s) c = (char)tolower((unsigned char)c);
    return s;
}
