#include "ultrasearch.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <pthread.h>
#include <dirent.h>      // For fast POSIX directory crawling
#include <sys/types.h>
#include <cstring>       // For strcmp
#include <unistd.h>      // For sysconf, isatty
#include <time.h>        // For clock_gettime

// ---------------------------------------------------------
// Helper: String Matching
// ---------------------------------------------------------
static bool name_matches(const std::string& fn, const std::string& pat, bool cs) {
    return cs ? fn == pat : to_lower(fn) == to_lower(pat);
}

// ---------------------------------------------------------
// 1. THE FAST POSIX CRAWLER
// ---------------------------------------------------------
static void fast_crawl(const std::string& path, std::vector<std::string>& file_list) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return; // Skip if permission denied or doesn't exist

    struct dirent* entry;
    // Read directly from the kernel buffer
    while ((entry = readdir(dir)) != nullptr) {
        // Skip current (.) and parent (..) directories
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = path + "/" + entry->d_name;

        // Check the type instantly using the directory entry metadata
        if (entry->d_type == DT_DIR) {
            stats_add_dirs(1); 
            fast_crawl(full_path, file_list); // Recursively crawl folders
        } 
        else if (entry->d_type == DT_REG) {
            file_list.push_back(full_path);   // Save regular files
        }
    }
    closedir(dir);
}

// ---------------------------------------------------------
// 2. THE THREAD WORKER DATA
// ---------------------------------------------------------
struct FileMatchArg {
    const std::string* paths; // Raw string array for speed
    std::size_t start;
    std::size_t end;
    std::string pattern;
    bool case_sensitive;
};

// ---------------------------------------------------------
// 3. THE THREAD WORKER FUNCTION
// ---------------------------------------------------------
static void* file_match_thread(void* raw) {
    FileMatchArg* a = (FileMatchArg*)raw;
    std::size_t local_hits = 0;
    
    for (std::size_t i = a->start; i < a->end; ++i) {
        // Extract just the file name from the full path for matching
        std::size_t slash_pos = a->paths[i].find_last_of('/');
        std::string filename = (slash_pos == std::string::npos) ? 
                                a->paths[i] : a->paths[i].substr(slash_pos + 1);

        // Check if the filename matches our target
        if (name_matches(filename, a->pattern, a->case_sensitive)) {
            ++local_hits;
            out_file_hit(a->paths[i]);
        }
    }
    
    // Update global stats safely
    if (local_hits > 0) {
        stats_add_matches(local_hits);
    }
    return NULL;
}

// ---------------------------------------------------------
// 4. THE MAIN SEARCH ORCHESTRATOR
// ---------------------------------------------------------
void file_search(const Config& cfg, std::size_t hw_threads) {
    std::vector<std::string> all_paths;
    
    // Pre-allocate memory to prevent the vector from resizing during the crawl
    all_paths.reserve(20000); 
    
    // 1. Crawl the file system on the main thread instantly
    fast_crawl(cfg.root.string(), all_paths);
    
    stats_add_files(all_paths.size());
    std::size_t n = all_paths.size();
    if (n == 0) return; // Nothing to search

    // 2. Divide the workload among threads
    std::size_t stride = (n / hw_threads) == 0 ? 1 : (n / hw_threads);
    std::vector<pthread_t> tids;
    std::vector<FileMatchArg> args;

    for (std::size_t i = 0; i < hw_threads; ++i) {
        std::size_t start = i * stride;
        std::size_t end = (i + 1 == hw_threads) ? n : start + stride;
        
        if (start >= n) break; // No more files to assign
        
        args.push_back({all_paths.data(), start, end, cfg.target, cfg.case_sensitive});
    }

    // 3. Launch threads
    tids.resize(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        pthread_create(&tids[i], NULL, file_match_thread, &args[i]);
    }
    
    // 4. Wait for all threads to finish
    for (auto& tid : tids) {
        pthread_join(tid, NULL);
    }
}

// ---------------------------------------------------------
// 5. CLI ARGUMENT PARSER
// ---------------------------------------------------------
static Config parse_args(int argc, char** argv) {
    if (argc < 3) {
        std::cerr << "Usage: ./ultrasearch <mode: text/file> <target> [root_dir] [-c]\n";
        std::exit(EXIT_FAILURE);
    }

    Config cfg;
    std::string mode_str = to_lower(argv[1]);
    if (mode_str == "text") cfg.mode = SearchMode::TEXT;
    else if (mode_str == "file") cfg.mode = SearchMode::FILE_SEARCH;
    else {
        std::cerr << "Error: Mode must be 'text' or 'file'\n";
        std::exit(EXIT_FAILURE);
    }

    cfg.target = argv[2];
    cfg.root = (argc >= 4 && std::string(argv[3]) != "null") ? fs::path(argv[3]) : fs::current_path();

    for (int i = 4; i < argc; ++i) {
        if (std::string(argv[i]) == "-c") cfg.case_sensitive = true;
    }
    return cfg;
}

// ---------------------------------------------------------
// 6. PROGRAM ENTRY POINT
// ---------------------------------------------------------
int main(int argc, char** argv) {
    // Disable colors if output is being piped or redirected (e.g., > /dev/null)
    if (!isatty(STDOUT_FILENO)) g_use_color = 0;

    stats_init();
    out_init();

    Config cfg = parse_args(argc, argv);
    
    // Get number of CPU cores
    long hw = sysconf(_SC_NPROCESSORS_ONLN);
    std::size_t hw_threads = (hw > 0) ? (std::size_t)hw : 1;

    // Start timer
    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    // Route to correct engine
    if (cfg.mode == SearchMode::TEXT) {
        text_search(cfg, hw_threads);
    } else {
        file_search(cfg, hw_threads);
    }

    // Stop timer
    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double ms = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;

    std::cout << "\n--- Summary ---\n"
              << "Scanned : " << g_files_scanned << " files\n"
              << "Matches : " << g_matches_found << "\n"
              << "Time    : " << ms << " ms\n";

    stats_destroy();
    out_destroy();
    
    return (g_matches_found > 0) ? EXIT_SUCCESS : 2;
}

