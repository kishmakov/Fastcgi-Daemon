#pragma once

#include "fastcgi2/component.h"
#include "fastcgi2/logger.h"

#include <condition_variable>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fastcgi {

class FileLogger : virtual public Logger, virtual public Component {
public:
    FileLogger(ComponentContext *context);
    ~FileLogger();

    virtual void onLoad();
    virtual void onUnload();

    virtual void log(const Logger::Level level, const char *format, va_list args);
private:
    virtual void rollOver();

private:
    std::string filename_;

    mode_t openMode_;

    // Time format specification
    std::string time_format_;

    bool print_level_;
    bool print_time_;

    // File descriptor
    int fd_;

    // Lock of file descriptor to avoid logrotate race-condition
    std::mutex fdMutex_;

    // Writing queue.
    // All writes happens in separate thread. All someInternal methods just
    // push string into queue and signal conditional variable.

    // Logger is stopping.
    volatile bool stopping_;

    // Writing queue.
    std::vector<std::string> queue_;

    // Condition and mutex for signalling.
    std::condition_variable queueCondition_;
    std::mutex queueMutex_;

    std::thread writingThread_;

    void openFile();
    void prepareFormat(char * buf, size_t size, const Logger::Level level, const char* format);

    void writingThread();
};

} // namespace fastcgi
