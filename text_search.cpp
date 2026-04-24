#include "ultrasearch.hpp"
#include <iostream>
#include <cstring>
#include <fcntl.h>       // Lab 4: open()
#include <sys/mman.h>    // Lab 6: mmap()
#include <sys/stat.h>
#include <unistd.h>      // Lab 4: close()

// ---------------------------------------------------------
// Fast String Search: Boyer-Moore-Horspool Algorithm
// ---------------------------------------------------------
struct BMSearcher {
    std::string pattern;
    std::vector<size_t> bad_char_table;
    bool case_sensitive;

    BMSearcher(const std::string& pat, bool cs) : pattern(pat), case_sensitive(cs) {
        bad_char_table.assign(256, pattern.length());
        for (size_t i = 0; i < pattern.length() - 1; ++i) {
            unsigned char c = pattern[i];
            bad_char_table[c] = pattern.length() - 1 - i;
            if (!cs) {
                bad_char_table[tolower(c)] = pattern.length() - 1 - i;
                bad_char_table[toupper(c)] = pattern.length() - 1 - i;
            }
        }
        if (!cs) {
            for (char& c : pattern) c = tolower((unsigned char)c);
        }
    }

    bool search_in(std::string_view text) const {
        if (pattern.empty()) return true;
        if (text.length() < pattern.length()) return false;

        size_t m = pattern.length();
        size_t n = text.length();
        size_t i = m - 1;

        while (i < n) {
            size_t k = i;
            size_t j = m - 1;
            while (j != (size_t)-1) {
                char text_char = case_sensitive ? text[k] : tolower((unsigned char)text[k]);
                if (text_char != pattern[j]) break;
                --k;
                --j;
            }
            if (j == (size_t)-1) return true; // Match found
            i += bad_char_table[(unsigned char)text[i]]; // Shift
        }
        return false;
    }
};

struct SpanArg {
    const char* data;
    std::size_t len;
    const BMSearcher* searcher;
    std::string filepath;
    std::size_t base_line;
};

// Lab 8: Thread Execution Function
static void* span_search_thread(void* raw) {
    SpanArg* a = (SpanArg*)raw;
    std::size_t line = a->base_line;
    const char* end = a->data + a->len;
    const char* start = a->data;
    
    std::size_t local_hits = 0; // Optimization: Count locally to avoid mutex locks

    while (start < end) {
        const char* nl = (const char*)memchr(start, '\n', (std::size_t)(end - start));
        const char* le = nl ? nl : end;
        std::string_view sv(start, (std::size_t)(le - start));

        if (a->searcher->search_in(sv)) {
            ++local_hits;
            out_match(a->filepath, line, sv);
        }
        ++line;
        start = nl ? nl + 1 : end;
    }
    
    // Batch update the global stats ONCE per thread
    if (local_hits > 0) stats_add_matches(local_hits);
    return NULL;
}

static void text_search_file(const fs::path& path, const BMSearcher& searcher, std::size_t hw_threads) {
    stats_add_files(1);

    // Lab 4 & 6: System Calls open() and mmap()
    int fd = open(path.c_str(), O_RDONLY);
    if (fd == -1) {
        out_warn("Cannot open: " + path.string() + " - " + strerror(errno));
        stats_add_errors(1);
        return;
    }

    struct stat st;
    if (fstat(fd, &st) == -1) { close(fd); return; }

    std::uintmax_t file_size = (std::uintmax_t)st.st_size;
    if (file_size == 0) { close(fd); return; }

    void* mapping = mmap(NULL, (std::size_t)file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd); // Safe to close fd after mmap

    if (mapping == MAP_FAILED) { stats_add_errors(1); return; }
    madvise(mapping, (std::size_t)file_size, MADV_SEQUENTIAL); // OS caching hint

    const char* data = (const char*)mapping;
    std::size_t sz = (std::size_t)file_size;
    std::string fpath = path.string();

    // Chunking logic for large files
    if (file_size < SMALL_FILE_BYTES) {
        SpanArg arg{data, sz, &searcher, fpath, 1};
        span_search_thread(&arg);
    } else {
        std::size_t num_threads = hw_threads > 0 ? hw_threads : 1;
        std::size_t chunk_size = std::max(sz / num_threads, (std::size_t)MIN_CHUNK_BYTES);
        std::size_t overlap = searcher.pattern.size() > 1 ? searcher.pattern.size() - 1 : 0;

        std::vector<std::size_t> base_lines = {1};
        std::size_t off = 0, line = 1;
        while (off < sz) {
            std::size_t end_off = std::min(off + chunk_size, sz);
            for (std::size_t i = off; i < end_off; ++i) if (data[i] == '\n') ++line;
            off = end_off;
            if (off < sz) base_lines.push_back(line);
        }

        std::size_t n_chunks = base_lines.size();
        std::vector<pthread_t> tids(n_chunks);
        std::vector<SpanArg> args(n_chunks);

        for (std::size_t i = 0; i < n_chunks; ++i) {
            std::size_t start = i * chunk_size;
            std::size_t end_off = std::min(start + chunk_size, sz);
            std::size_t ext_end = std::min(end_off + overlap, sz); // Prevent word cutting

            args[i] = {data + start, ext_end - start, &searcher, fpath, base_lines[i]};
            pthread_create(&tids[i], NULL, span_search_thread, &args[i]);
        }
        for (std::size_t i = 0; i < n_chunks; ++i) pthread_join(tids[i], NULL);
    }
    munmap(mapping, sz);
}

struct TextFileArg {
    fs::path path;
    const BMSearcher* searcher;
    std::size_t hw_threads;
};

static void text_file_task(void* raw) {
    TextFileArg* a = (TextFileArg*)raw;
    text_search_file(a->path, *(a->searcher), a->hw_threads);
}

void text_search(const Config& cfg, std::size_t hw_threads) {
    BMSearcher searcher(cfg.target, cfg.case_sensitive); // Build table once!
    std::vector<fs::path> files;
    std::error_code ec;

    for (auto& entry : fs::recursive_directory_iterator(cfg.root, fs::directory_options::skip_permission_denied, ec)) {
        if (ec) { ec.clear(); continue; }
        if (entry.is_regular_file(ec)) files.push_back(entry.path());
    }

    if (files.empty()) return;

    ThreadPool pool;
    pool_init(pool, hw_threads);
    std::vector<TextFileArg*> task_args;
    
    for (auto& p : files) {
        TextFileArg* a = new TextFileArg{p, &searcher, hw_threads};
        task_args.push_back(a);
        pool_submit(pool, text_file_task, a);
    }
    
    pool_wait_all(pool);
    pool_destroy(pool);
    for (auto* a : task_args) delete a;
}
