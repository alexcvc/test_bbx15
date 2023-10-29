/* SPDX-License-Identifier: MIT */
//
// Copyright (c) 2023 Alexander Sacharov <a.sacharov@gmx.de>
//               All rights reserved.
//
// This work is licensed under the terms of the MIT license.
// For a copy, see <https://opensource.org/licenses/MIT>.
//

/************************************************************************/ /**
* @file
* @brief   main process
* @author A.Sacharov <a.sacharov@gmx.de> 29.10.23
****************************************************************************/

//-----------------------------------------------------------------------------
// includes
//-----------------------------------------------------------------------------
#include <getopt.h>
#include <syslog.h>

#include "submodules/fswatch/include/fswatch.hpp"
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

using namespace std::chrono_literals;

//-----------------------------------------------------------------------------
// local Defines and Macros
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// local Typedefs, Enums, Unions
//-----------------------------------------------------------------------------

/**
 * @brief event plan structure
 */
struct Event {
public:
    std::mutex event_mutex;
    std::condition_variable event_condition;
};

//-----------------------------------------------------------------------------
// local/global Variables Definitions
//-----------------------------------------------------------------------------
/**
 * @brief Events for three asynchronous tasks
 */
Event task_event_stop_fswatcher;
Event task_event_stop_test;

//-----------------------------------------------------------------------------
// local/global Function Prototypes
//-----------------------------------------------------------------------------

//-----------------------------------------------------------------------------
// local Function Definitions
//-----------------------------------------------------------------------------

/************************************************************************/ /**
* @fn      void ShowVersion(const char* prog)
* @brief   show version for winconfig.
* @param prog - name of program
****************************************************************************/
static void ShowVersion(const char *prog) {
    auto verString{"1.0"};
    // info
    std::cout << prog << " v." << verString << std::endl;
}

/************************************************************************/ /**
* @fn      void DisplayHelp(const char* prog)
* @brief   view help
* @param  prog - Name of the program in the display help
****************************************************************************/
static void DisplayHelp(const char *prog) {
    std::cout << "Usage: " << prog << " [OPTION]\n"
              << "  -v, --version            version\n"
              << "  -h, --help               this message\n\n";
}

/************************************************************************/ /**
* @fn      void ProcessOptions(int argc, char* argv[])
* @brief   parse command line parameters
* @details reads parameters from command line
*
* @param argc - number parameters in command line
* @param argv - command line parameters as array
*
* @return void
*
* @exception void
****************************************************************************/

static void ProcessOptions(int argc, char *argv[]) {
    for (;;) {
        int option_index = 0;
        static const char *short_options = "h?v";
        static const struct option long_options[] = {
                {"help", no_argument, 0, 0},
                {"version", no_argument, 0, 'v'},
                {0, 0, 0, 0},
        };

        int var = getopt_long(argc, argv, short_options, long_options, &option_index);

        if (var == EOF) {
            break;
        }
        switch (var) {
            case 0:
                DisplayHelp(argv[0]);
                break;
            case '?':
            case 'h': {
                DisplayHelp(argv[0]);
                exit(EXIT_SUCCESS);
            }
            case 'v': {
                ShowVersion(argv[0]);
                exit(EXIT_SUCCESS);
            }
            default: {
                DisplayHelp(argv[0]);
                exit(-1);
            }
        }
    }
}

/**
 * @brief   get char from cin
 * @return bool
 *          true if received an exit command,
 *          otherwise - false
 **/
static bool HandleGetChar() {
    char var = getchar();
    switch (var) {
        case '\n':
            break;
        case 'q': {
            std::cout << "Received QUIT command\nExiting.." << std::endl;
            return (true);
        }
        default: {
            std::cout << "Console \n"
                      << " Key options are:\n"
                      << "  q - quit from the program" << std::endl;
            break;
        }
    }
    return false;
}

//-----------------------------------------------------------------------------
// global Function Definitions
//-----------------------------------------------------------------------------

/**
 * @brief Waking up all running tasks
 * @param config - configuration the manager
 * @param all_tasks_wakeup - wakeup all tasks but not only file system event driving tasks
 */
void WakeUpRunningTasks(bool all_tasks_wakeup = true) {
    if (all_tasks_wakeup) {
        std::unique_lock lck(task_event_stop_fswatcher.event_mutex);
        task_event_stop_fswatcher.event_condition.notify_all();// Wakes up stop a file system watcher
    }
    {
        std::unique_lock lck(task_event_stop_test.event_mutex);
        task_event_stop_test.event_condition.notify_all();// Wakes up a displacement task
    }
}

/**
 * @brief File system watcher task main function
 * @desc This task is mandatory. fswatch library was used here.
 * @param lptr - log appender
 * @param cfg_converter - configuration
 * @param token - stop task token
 */
void TaskWorkerFsWatcher(std::stop_token token) {
    using namespace std::chrono_literals;
    const auto waitDuration = 2s;
    auto watcher = fswatch("/tmp");

    // add watching events
    watcher.on({fswatch::Event::FILE_CLOSED, fswatch::Event::FILE_DELETED},
               [&]([[maybe_unused]] auto &event) {
                   WakeUpRunningTasks(true);// Wake up sleeping tasks by an event in the file system
               });

    // Register a stop callback for stop task
    std::stop_callback stop_cb(token, [&]() {
        // Wake up thread on stop request
        task_event_stop_fswatcher.event_condition.notify_all();
    });

    // start thread with stop fs-watcher task
    std::thread stop_watching_task([&]() {
        // create observer
        while (true) {
            // Start of locked block
            std::unique_lock lck(task_event_stop_fswatcher.event_mutex);
            task_event_stop_fswatcher.event_condition.wait(lck, [&, token]() {
                return token.stop_requested();
            });

            //Stop if requested to stop
            if (token.stop_requested()) {
                std::printf("Stop requested for a stop watcher task\n");
                break;
            }
        }// End of while loop
        watcher.stop();
        // wakeup watcher via event in file system
        std::ofstream tmpfile;
        tmpfile.open("/tmp/~watcher_wakeup", std::ios_base::trunc);
        tmpfile << "Wakeup\n";
        tmpfile.close();
        std::printf("Stop filesystem watcher task stopped.");
    });

    try {
        watcher.start();
    } catch (std::filesystem::filesystem_error &error) {
        std::printf("Filesystem exception was caught: %s\n", error.what());
    } catch (std::exception &error) {
        std::printf("Exception was caught: %s\n", error.what());
    } catch (...) {
        std::printf("Unknown exception was caught\n");
    }

    stop_watching_task.join();

    std::printf("Filesystem watcher task stopped.\n");
}

/**
 * @brief Displacement connection main function
 * @param lptr - log appender
 * @param cfg_converter - configuration
 * @param token - stop task token
 */
void TaskWorker_Test(std::stop_token token) {
    using namespace std::chrono_literals;
    const auto waitDurationDef = 3000ms;

    // Register a stop callback
    std::stop_callback stop_cb(token, [&]() {
        // Wake thread on stop request
        task_event_stop_test.event_condition.notify_all();
    });

    // create displacement context
    auto sooner = waitDurationDef;

    while (true) {
        // observe serves states
        // Start of locked block
        std::unique_lock lck(task_event_stop_test.event_mutex);
        task_event_stop_test.event_condition.wait_for(lck, std::chrono::milliseconds(sooner));

        //Stop if requested to stop
        if (token.stop_requested()) {
            std::printf("Stop requested for a displacement connection task\n");
            break;
        }
    }// End of while loop

    std::printf("Displacement connection task stopped.\n");
}

/************************************************************************/ /**
* @fn      int main()
* @brief   initializes and run stuff.
* @param   argc will be the number of strings pointed to by argv.
* @param   argv array of command line parameters.
* @return EXIT_SUCCESS if successfully, otherwise - EXIT_FAILURE
****************************************************************************/

int main(int argc, char **argv) {
    std::stop_source stop_src;// Create a stop source
    std::thread task_worker_fswatcher;
    std::thread tas_worker_test;

    //----------------------------------------------------------
    // parse parameters
    //----------------------------------------------------------
    ProcessOptions(argc, argv);

    // show information
    ShowVersion(argv[0]);

    //----------------------------------------------------------
    // go to idle in main
    //----------------------------------------------------------
    // Create all workers and pass stop tokens
    tas_worker_test = std::move(std::thread(TaskWorker_Test, stop_src.get_token()));
    // start task filesystem watcher
    task_worker_fswatcher = std::move(std::thread(TaskWorkerFsWatcher, stop_src.get_token()));

    // main loop or sleep
    while (true) {
            if (HandleGetChar()) {
                break;
            }
    }

    std::printf("Request stop all  tasks\n");
    // set token to stop all worker
    stop_src.request_stop();

    // wakeup all tasks
    WakeUpRunningTasks(true);

    // Join threads
    task_worker_fswatcher.join();
    tas_worker_test.join();

    return EXIT_SUCCESS;
}
