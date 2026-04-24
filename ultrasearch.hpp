#ifndef ULTRASEARCH_HPP
#define ULTRASEARCH_HPP

#include <string>
#include <string_view>
#include <vector>
#include <queue>
#include <filesystem>
#include <pthread.h>

namespace fs = std::filesystem;

//Constants for mmap and pthread decisions
extern const std::uintmax_t SMALL_FILE_BYTES;
extern const std::size_t SMALL_DIR_ENTRIES;
extern const std::uintmax_t MIN_CHUNK_BYTES;

// Colors for terminal
extern int g_use_color;
std::string col(const char* c, std::string_view s);

// CLI Structures
enum class SearchMode { TEXT, FILE_SEARCH };

struct Config {
    SearchMode  mode           = SearchMode::TEXT;
    std::string target;
    fs::path    root;
    bool        case_sensitive = false;
    bool        target_null    = false;
};

// Global Stats
extern std::size_t g_files_scanned;
extern std::size_t g_matches_found;
extern std::size_t g_dirs_visited;
extern std::size_t g_errors;

void stats_init();
void stats_destroy();
void stats_add_files(std::size_t n);
void stats_add_matches(std::size_t n); // Batched addition for speed
void stats_add_dirs(std::size_t n);
void stats_add_errors(std::size_t n);

void out_init();
void out_destroy();
void out_print(const std::string& line);
void out_match(const std::string& path, std::size_t line_no, std::string_view line_text);
void out_file_hit(const std::string& path);
void out_warn(const std::string& msg);


struct Task {
    void (*fn)(void*);
    void* arg;
};

struct ThreadPool {
    std::vector<pthread_t> threads;
    std::queue<Task>       queue;
    pthread_mutex_t        pool_mtx;
    pthread_cond_t         queue_cond;
    pthread_cond_t         done_cond;
    std::size_t            active;
    int                    stop;
};

void pool_init(ThreadPool& pool, std::size_t n);
void pool_submit(ThreadPool& pool, void (*fn)(void*), void* arg);
void pool_wait_all(ThreadPool& pool);
void pool_destroy(ThreadPool& pool);

std::string to_lower(std::string s);

// Search Orchestrators
void text_search(const Config& cfg, std::size_t hw_threads);
void file_search(const Config& cfg, std::size_t hw_threads);

#endif // ULTRASEARCH_HPP
