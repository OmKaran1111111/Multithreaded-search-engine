#include "ultrasearch.hpp"
#include <iostream>
#include <iomanip>
#include <vector>
#include <string>
#include <pthread.h>
#include <dirent.h>
#include <sys/types.h>
#include <cstring>
#include <unistd.h>
#include <time.h>

using namespace std;


static bool name_matches(const string& fn, const string& pat, bool cs) {
    return cs ? fn == pat : to_lower(fn) == to_lower(pat);
}


static void fast_crawl(const string& path, vector<string>& file_list) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return;

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0)
            continue;

        string full_path = path + "/" + entry->d_name;

        if (entry->d_type == DT_DIR) {
            stats_add_dirs(1);
            fast_crawl(full_path, file_list);
        } else if (entry->d_type == DT_REG) {
            file_list.push_back(full_path);
        }
    }
    closedir(dir);
}


struct FileMatchArg {
    const string* paths;
    size_t start;
    size_t end;
    string pattern;
    bool case_sensitive;
};


static void* file_match_thread(void* raw) {
    FileMatchArg* a = (FileMatchArg*)raw;
    size_t local_hits = 0;

    for (size_t i = a->start; i < a->end; ++i) {
        size_t slash_pos = a->paths[i].find_last_of('/');
        string filename = (slash_pos == string::npos)
                            ? a->paths[i]
                            : a->paths[i].substr(slash_pos + 1);

        if (name_matches(filename, a->pattern, a->case_sensitive)) {
            ++local_hits;
            out_file_hit(a->paths[i]);
        }
    }

    if (local_hits > 0)
        stats_add_matches(local_hits);

    return NULL;
}


void file_search(const Config& cfg, size_t hw_threads) {
    vector<string> all_paths;
    all_paths.reserve(20000);

    fast_crawl(cfg.root.string(), all_paths);
    stats_add_files(all_paths.size());

    size_t n = all_paths.size();
    if (n == 0) return;

    size_t stride = (n / hw_threads) == 0 ? 1 : (n / hw_threads);
    vector<pthread_t> tids;
    vector<FileMatchArg> args;

    for (size_t i = 0; i < hw_threads; ++i) {
        size_t start = i * stride;
        size_t end = (i + 1 == hw_threads) ? n : start + stride;
        if (start >= n) break;
        args.push_back({all_paths.data(), start, end, cfg.target, cfg.case_sensitive});
    }

    tids.resize(args.size());
    for (size_t i = 0; i < args.size(); ++i)
        pthread_create(&tids[i], NULL, file_match_thread, &args[i]);

    for (auto& tid : tids)
        pthread_join(tid, NULL);
}


static Config parse_args(int argc, char** argv) {
    if (argc < 3) {
        cerr << "Usage: ./ultrasearch <mode: text/file> <target> [root_dir] [-c]\n";
        exit(EXIT_FAILURE);
    }

    Config cfg;
    string mode_str = to_lower(argv[1]);

    if (mode_str == "text")       cfg.mode = SearchMode::TEXT;
    else if (mode_str == "file")  cfg.mode = SearchMode::FILE_SEARCH;
    else {
        cerr << "Error: Mode must be 'text' or 'file'\n";
        exit(EXIT_FAILURE);
    }

    cfg.target = argv[2];
    cfg.root = (argc >= 4 && string(argv[3]) != "null") ? fs::path(argv[3]) : fs::current_path();

    for (int i = 4; i < argc; ++i)
        if (string(argv[i]) == "-c") cfg.case_sensitive = true;

    return cfg;
}


int main(int argc, char** argv) {
    if (!isatty(STDOUT_FILENO)) g_use_color = 0;

    stats_init();
    out_init();

    Config cfg = parse_args(argc, argv);

    long hw = sysconf(_SC_NPROCESSORS_ONLN);
    size_t hw_threads = (hw > 0) ? (size_t)hw : 1;

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    if (cfg.mode == SearchMode::TEXT)
        text_search(cfg, hw_threads);
    else
        file_search(cfg, hw_threads);

    clock_gettime(CLOCK_MONOTONIC, &ts1);
    double ms = (ts1.tv_sec - ts0.tv_sec) * 1000.0 + (ts1.tv_nsec - ts0.tv_nsec) / 1e6;

    cout << "\n--- Summary ---\n"
         << "Scanned : " << g_files_scanned << " files\n"
         << "Matches : " << g_matches_found << "\n"
         << "Time    : " << ms << " ms\n";

    stats_destroy();
    out_destroy();

    return (g_matches_found > 0) ? EXIT_SUCCESS : 2;
}
