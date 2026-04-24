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

/*
You got it. Let's do a literal line-by-line teardown of `main.cpp` so you can see exactly what every single piece of syntax is doing.

### The Includes (Lines 1-11)
```cpp
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
```
* **Line 1:** Includes the custom header containing your shared data structures and function declarations.
* **Lines 2-5:** Includes standard C++ libraries for console input/output (`iostream`), output formatting (`iomanip`), dynamic arrays (`vector`), and text handling (`string`).
* **Line 6:** Includes the POSIX threads library (`pthread.h`) for managing multithreading.
* **Line 7:** Includes POSIX directory entry definitions (`dirent.h`) to read directory contents directly from the OS.
* **Line 8:** Includes basic system data types (`sys/types.h`) used by the POSIX APIs.
* **Line 9:** Includes C-style string manipulation (`cstring`), specifically needed for `strcmp`.
* **Line 10:** Includes standard symbolic constants and types for UNIX systems (`unistd.h`), providing access to OS-level info.
* **Line 11:** Includes time-tracking structures and functions (`time.h`) for benchmarking.

### Helper Function (Lines 13-17)
```cpp
static bool name_matches(const std::string& fn, const std::string& pat, bool cs) {
    return cs ? fn == pat : to_lower(fn) == to_lower(pat);
}
```
* **Line 14:** Defines a helper function restricted to this file (`static`) that takes a filename (`fn`), a pattern (`pat`), and a case-sensitive boolean (`cs`).
* **Line 15:** Uses a ternary operator (`? :`). If `cs` is true, it strictly compares the strings (`fn == pat`). If false, it converts both to lowercase using your custom `to_lower` function before comparing.
* **Line 16:** Closes the function.

### Fast POSIX Crawler (Lines 19-42)
```cpp
static void fast_crawl(const std::string& path, std::vector<std::string>& file_list) {
    DIR* dir = opendir(path.c_str());
    if (!dir) return; // Skip if permission denied or doesn't exist

    struct dirent* entry;
    while ((entry = readdir(dir)) != nullptr) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }

        std::string full_path = path + "/" + entry->d_name;

        if (entry->d_type == DT_DIR) {
            stats_add_dirs(1); 
            fast_crawl(full_path, file_list); 
        } 
        else if (entry->d_type == DT_REG) {
            file_list.push_back(full_path);   
        }
    }
    closedir(dir);
}
```
* **Line 20:** Defines the recursive crawling function. It takes the directory path and a reference to the `file_list` vector to populate.
* **Line 21:** Opens the directory using the POSIX `opendir` function, converting the C++ string to a C-string (`c_str()`).
* **Line 22:** If the directory fails to open (e.g., lack of permissions), it safely returns to prevent a crash.
* **Line 24:** Declares a pointer to a `dirent` struct, which will hold the metadata for each item in the directory.
* **Line 26:** A `while` loop that continuously reads the next item in the directory using `readdir` until there are no items left (`nullptr`).
* **Lines 28-30:** Compares the current item's name (`d_name`). If it is `.` (the current directory) or `..` (the parent directory), it skips to the next iteration (`continue`) to prevent an infinite loop.
* **Line 32:** Constructs the absolute path by combining the current path, a slash, and the item's name.
* **Lines 35-38:** Checks if the directory entry type (`d_type`) is a directory (`DT_DIR`). If so, it increments the global directory stat and recursively calls `fast_crawl` to dive into that folder.
* **Lines 39-41:** Otherwise, if the entry is a regular file (`DT_REG`), it adds the full path to the `file_list` vector.
* **Line 43:** Closes the directory handle to free up OS resources.

### Thread Data Structure (Lines 44-51)
```cpp
struct FileMatchArg {
    const std::string* paths; 
    std::size_t start;
    std::size_t end;
    std::string pattern;
    bool case_sensitive;
};
```
* **Lines 45-51:** Defines a struct used to pass data into the worker threads. It holds a pointer to the raw array of strings (`paths`), the starting and ending indices this specific thread should process (`start`, `end`), the target `pattern`, and whether the search is `case_sensitive`.

### Thread Worker Function (Lines 53-73)
```cpp
static void* file_match_thread(void* raw) {
    FileMatchArg* a = (FileMatchArg*)raw;
    std::size_t local_hits = 0;
    
    for (std::size_t i = a->start; i < a->end; ++i) {
        std::size_t slash_pos = a->paths[i].find_last_of('/');
        std::string filename = (slash_pos == std::string::npos) ? 
                                a->paths[i] : a->paths[i].substr(slash_pos + 1);

        if (name_matches(filename, a->pattern, a->case_sensitive)) {
            ++local_hits;
            out_file_hit(a->paths[i]);
        }
    }
    
    if (local_hits > 0) {
        stats_add_matches(local_hits);
    }
    return NULL;
}
```
* **Line 54:** Defines the thread execution function. It must take a `void*` and return a `void*` to comply with the `pthread_create` signature.
* **Line 55:** Casts the raw `void*` argument back into the `FileMatchArg*` struct so the thread can access its data.
* **Line 56:** Initializes a local counter for matches found by this specific thread.
* **Line 58:** Starts a loop that iterates only from the thread's assigned `start` index to its `end` index.
* **Lines 60-62:** Finds the last forward slash in the path. If no slash exists (`npos`), the filename is the whole string. Otherwise, it extracts the substring *after* the slash to isolate just the file name.
* **Lines 65-68:** Calls the `name_matches` helper. If it returns true, it increments the thread's local hit counter and calls the thread-safe console output function `out_file_hit`.
* **Lines 71-73:** After the loop finishes checking all its files, if the thread found anything, it batch-updates the global stats by calling `stats_add_matches` once, minimizing mutex locking.
* **Line 74:** Returns `NULL` to signify thread completion.

### Search Orchestrator (Lines 75-110)
```cpp
void file_search(const Config& cfg, std::size_t hw_threads) {
    std::vector<std::string> all_paths;
    all_paths.reserve(20000); 
    
    fast_crawl(cfg.root.string(), all_paths);
    
    stats_add_files(all_paths.size());
    std::size_t n = all_paths.size();
    if (n == 0) return; 

    std::size_t stride = (n / hw_threads) == 0 ? 1 : (n / hw_threads);
    std::vector<pthread_t> tids;
    std::vector<FileMatchArg> args;

    for (std::size_t i = 0; i < hw_threads; ++i) {
        std::size_t start = i * stride;
        std::size_t end = (i + 1 == hw_threads) ? n : start + stride;
        
        if (start >= n) break; 
        
        args.push_back({all_paths.data(), start, end, cfg.target, cfg.case_sensitive});
    }

    tids.resize(args.size());
    for (std::size_t i = 0; i < args.size(); ++i) {
        pthread_create(&tids[i], NULL, file_match_thread, &args[i]);
    }
    
    for (auto& tid : tids) {
        pthread_join(tid, NULL);
    }
}
```
* **Line 76:** Function signature, taking the user's config and the number of logical CPU cores.
* **Lines 77-78:** Creates the empty vector for file paths and reserves memory for 20,000 elements upfront to avoid expensive vector reallocations during the fast crawl.
* **Line 81:** Kicks off the single-threaded POSIX crawler, populating the `all_paths` vector.
* **Lines 83-85:** Updates the global file count statistic. If no files were found, the function immediately exits.
* **Line 88:** Calculates the `stride` (how many files each thread will process) by dividing total files by thread count. Ensures stride is at least 1.
* **Lines 89-90:** Creates vectors to hold the thread IDs (`tids`) and the arguments (`args`) that will be passed to each thread.
* **Lines 92-100:** Loops to create the arguments for each thread. It calculates the `start` and `end` indices. The ternary operator on line 94 ensures the very last thread grabs any remainder files. If `start` exceeds the array size (edge case with very few files), it breaks early. It pushes the packaged arguments into the `args` vector.
* **Lines 103-106:** Resizes the thread ID array. A loop then spawns the actual POSIX threads using `pthread_create`, passing them the `file_match_thread` function and their specific argument package.
* **Lines 109-111:** A range-based loop that calls `pthread_join` on every thread. This freezes the main thread and waits until all worker threads have finished their execution before proceeding.

### CLI Argument Parser (Lines 112-132)
```cpp
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
```
* **Line 113:** Defines the parser, taking the system command line argument count (`argc`) and values (`argv`).
* **Lines 114-117:** Checks if the user provided less than 3 arguments (the program name, mode, and target are required). If not, prints usage instructions to standard error and forcefully exits the program.
* **Lines 119-126:** Initializes the `Config` struct. Reads the first argument (index 1), converts it to lowercase, and assigns the correct enum to `cfg.mode`. If the mode is invalid, it prints an error and exits.
* **Line 128:** Sets the search target from the second argument (index 2).
* **Line 129:** Sets the root directory to search. If a 3rd argument is provided and isn't "null", it uses that path. Otherwise, it defaults to the OS's current working directory.
* **Lines 131-133:** Loops through any remaining optional arguments (index 4 onwards). If it finds `-c`, it enables case sensitivity.
* **Line 134:** Returns the populated configuration object.

### Program Entry Point (Lines 134-171)
```cpp
int main(int argc, char** argv) {
    if (!isatty(STDOUT_FILENO)) g_use_color = 0;

    stats_init();
    out_init();

    Config cfg = parse_args(argc, argv);
    
    long hw = sysconf(_SC_NPROCESSORS_ONLN);
    std::size_t hw_threads = (hw > 0) ? (std::size_t)hw : 1;

    struct timespec ts0, ts1;
    clock_gettime(CLOCK_MONOTONIC, &ts0);

    if (cfg.mode == SearchMode::TEXT) {
        text_search(cfg, hw_threads);
    } else {
        file_search(cfg, hw_threads);
    }

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
```
* **Line 135:** Standard C++ entry point.
* **Line 137:** Uses the POSIX `isatty` function to check if standard output is a terminal window. If it isn't (e.g., the user is piping the output to a text file), it disables ANSI color codes.
* **Lines 139-140:** Calls initialization functions for your mutexes (defined in `concurrency.cpp`) to prepare for multithreading.
* **Line 142:** Runs the CLI parser.
* **Lines 145-146:** Asks the operating system for the number of online CPU cores via `sysconf`. Ensures the thread count defaults to 1 if the system call fails.
* **Lines 149-150:** Creates time-tracking variables and takes a timestamp exactly before the search begins using `clock_gettime` with the `CLOCK_MONOTONIC` flag (which ensures the timer isn't affected by system clock adjustments).
* **Lines 153-157:** Evaluates the user's requested mode from the config and routes execution to either `text_search` or `file_search`.
* **Lines 160-161:** Takes a second timestamp right after the search finishes. Calculates the elapsed time in milliseconds by comparing the seconds (`tv_sec`) and nanoseconds (`tv_nsec`) between the two timestamps.
* **Lines 163-166:** Prints the final summary report to the console using the global statistics variables.
* **Lines 168-169:** Safely destroys the POSIX mutexes to free resources.
* **Line 171:** Returns an exit code to the operating system. Returns `0` (Success) if matches were found, and `2` if no matches were found, mimicking the behavior of tools like `grep`.
*/
