#include "ultrasearch.hpp"
#include <iostream>

using namespace std;

constexpr uintmax_t FILE_SMALL_LIMIT = 1024ULL * 1024;
constexpr size_t DIR_ENTRY_LIMIT = 500;
constexpr uintmax_t CHUNK_MIN = 512ULL * 1024;

int enable_color = 1;

string apply_color(const char* code, const string& text) {
    if (!enable_color) return text;
    return string(code) + text + "\033[0m";
}

struct GlobalStats {
    size_t files = 0;
    size_t matches = 0;
    size_t dirs = 0;
    size_t errors = 0;
} stats;

pthread_mutex_t stats_lock;
pthread_mutex_t output_lock;

void init_stats() {
    pthread_mutex_init(&stats_lock, nullptr);
}

void destroy_stats() {
    pthread_mutex_destroy(&stats_lock);
}

void add_to_counter(size_t& counter, size_t value) {
    pthread_mutex_lock(&stats_lock);
    counter += value;
    pthread_mutex_unlock(&stats_lock);
}

void init_output() {
    pthread_mutex_init(&output_lock, nullptr);
}

void destroy_output() {
    pthread_mutex_destroy(&output_lock);
}

void safe_print(const string& msg) {
    pthread_mutex_lock(&output_lock);
    cout << msg << '\n';
    pthread_mutex_unlock(&output_lock);
}

void print_match(const string& file, size_t line, string_view content) {
    string text(content);

    if (text.length() > 150) {
        text = text.substr(0, 150) + "...";
    }

    pthread_mutex_lock(&output_lock);
    cout << apply_color("\033[1;36m", file) << ":"
         << apply_color("\033[1;33m", to_string(line))
         << ": " << text << '\n';
    pthread_mutex_unlock(&output_lock);
}

void print_file_found(const string& file) {
    pthread_mutex_lock(&output_lock);
    cout << apply_color("\033[1;32m", "FOUND ")
         << apply_color("\033[1m", file) << '\n';
    pthread_mutex_unlock(&output_lock);
}

void print_warning(const string& msg) {
    pthread_mutex_lock(&output_lock);
    cerr << apply_color("\033[1;31m", "WARN: ") << msg << '\n';
    pthread_mutex_unlock(&output_lock);
}

void* worker_routine(void* arg) {
    ThreadPool* p = static_cast<ThreadPool*>(arg);

    while (true) {
        pthread_mutex_lock(&p->pool_mtx);

        while (p->queue.empty() && !p->stop_flag) {
            pthread_cond_wait(&p->queue_cond, &p->pool_mtx);
        }

        if (p->stop_flag && p->queue.empty()) {
            pthread_mutex_unlock(&p->pool_mtx);
            return nullptr;
        }

        Task task = p->queue.front();
        p->queue.pop();
        ++p->active_threads;

        pthread_mutex_unlock(&p->pool_mtx);

        task.fn(task.arg);

        pthread_mutex_lock(&p->pool_mtx);
        --p->active_threads;

        if (p->queue.empty() && p->active_threads == 0) {
            pthread_cond_broadcast(&p->completion_cond);
        }

        pthread_mutex_unlock(&p->pool_mtx);
    }
}

void threadpool_init(ThreadPool& p, size_t thread_count) {
    p.active_threads = 0;
    p.stop_flag = false;

    pthread_mutex_init(&p.pool_mtx, nullptr);
    pthread_cond_init(&p.queue_cond, nullptr);
    pthread_cond_init(&p.completion_cond, nullptr);

    p.threads.resize(thread_count);

    for (size_t i = 0; i < thread_count; ++i) {
        pthread_create(&p.threads[i], nullptr, worker_routine, &p);
    }
}

void threadpool_submit(ThreadPool& p, void (*func)(void*), void* arg) {
    pthread_mutex_lock(&p.pool_mtx);
    p.queue.push({func, arg});
    pthread_mutex_unlock(&p.pool_mtx);
    pthread_cond_signal(&p.queue_cond);
}

void threadpool_wait(ThreadPool& p) {
    pthread_mutex_lock(&p.pool_mtx);

    while (!p.queue.empty() || p.active_threads > 0) {
        pthread_cond_wait(&p.completion_cond, &p.pool_mtx);
    }

    pthread_mutex_unlock(&p.pool_mtx);
}

void threadpool_destroy(ThreadPool& p) {
    pthread_mutex_lock(&p.pool_mtx);
    p.stop_flag = true;
    pthread_mutex_unlock(&p.pool_mtx);

    pthread_cond_broadcast(&p.queue_cond);

    for (auto& t : p.threads) {
        pthread_join(t, nullptr);
    }

    pthread_mutex_destroy(&p.pool_mtx);
    pthread_cond_destroy(&p.queue_cond);
    pthread_cond_destroy(&p.completion_cond);
}

string make_lowercase(string str) {
    for (auto& ch : str) {
        ch = static_cast<char>(tolower(static_cast<unsigned char>(ch)));
    }
    return str;
}
