#ifndef _FASTCGI_FILE_LOGGER_H_
#define _FASTCGI_FILE_LOGGER_H_

#include <chrono>
#include <vector>
#include <string>
#include <boost/thread.hpp>
#include <boost/thread/condition.hpp>
#include <sys/uio.h>

#include "fastcgi2/component.h"
#include "fastcgi2/logger.h"

namespace fastcgi
{

class FileLogger : virtual public Logger, virtual public Component {
public:
    FileLogger(ComponentContext *context);
    ~FileLogger();

    virtual void onLoad();
    virtual void onUnload();

    virtual void log(const Logger::Level level, const char *format, va_list args);

private:
    // File name
    std::string filename_;

    // Open mode
    mode_t openMode_;

    // Time format specification
    std::string time_format_;

    bool print_level_;
    bool print_time_;

    // File descriptor
    int fd_;


    // Writing queue.
    // All writes happens in separate thread. All someInternal methods just
    // push string into queue and signal conditional variable.

    // Logger is stopping.
    volatile bool stopping_;

    // Writing queue.
    std::vector<std::string> queue_;

    // Condition and mutex for signalling.
    boost::condition queueCondition_;
    boost::mutex queueMutex_;

    std::chrono::system_clock::time_point lastNotify_;

    // Writing thread.
    boost::thread writingThread_;
    size_t lines_per_shot_;
    std::vector<iovec> iov_;

    void openFile();
    void prepareFormat(char * buf, size_t size, const Logger::Level level, const char* format);

    void writingThread();
};

}

#endif // _FASTCGI_FILE_LOGGER_H_
