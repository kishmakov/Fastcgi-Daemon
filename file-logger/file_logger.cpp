#include "settings.h"

#include "file_logger.h"

#include <cerrno>
#include <cstring>
#include <cstdio>
#include <iostream>

#include <sys/types.h>
#include <sys/stat.h>
#include <fcntl.h>
#include <time.h>

#include <boost/bind.hpp>

#include "fastcgi2/component_factory.h"
#include "fastcgi2/config.h"

#ifdef HAVE_DMALLOC_H
#include <dmalloc.h>
#endif

namespace fastcgi {

const size_t BUF_SIZE = 512;
const size_t IOV_SIZE = 8;

FileLogger::FileLogger(ComponentContext *context) : Component(context),
        openMode_(S_IRUSR | S_IWUSR | S_IRGRP | S_IWGRP | S_IROTH),
        print_level_(true), print_time_(true),
        fd_(-1), stopping_(false),
        lastNotify_(std::chrono::system_clock::now()),
        writingThread_(boost::bind(&FileLogger::writingThread, this))
{
    const Config *config = context->getConfig();
    const std::string componentXPath = context->getComponentXPath();

    filename_ = config->asString(componentXPath + "/file");
    setLevel(stringToLevel(config->asString(componentXPath + "/level")));
    lines_per_shot_ = config->asInt(componentXPath + "/lines_per_shot", IOV_SIZE);
    iov_.resize(lines_per_shot_);

    print_level_ =
        (0 == strcasecmp(config->asString(componentXPath + "/print-level", "yes").c_str(), "yes"));

    print_time_ =
        (0 == strcasecmp(config->asString(componentXPath + "/print-time", "yes").c_str(), "yes"));

    time_format_ = config->asString(componentXPath + "/time-format", "[%Y/%m/%d %T]");

    std::string read = config->asString(componentXPath + "/read", "");
    if (!read.empty()) {
        if (read == "all") {
            openMode_ = S_IRUSR | S_IWUSR | S_IRGRP | S_IROTH;
        }
        else if (read == "group") {
            openMode_ = S_IRUSR | S_IWUSR | S_IRGRP;
        }
        else if (read == "user") {
            openMode_ = S_IRUSR | S_IWUSR;
        }
    }

    std::string::size_type pos = 0;
    while (true) {
        pos = filename_.find('/', pos + 1);
        if (std::string::npos == pos) {
            break;
        }
        std::string name = filename_.substr(0, pos);
        int res = mkdir(name.c_str(), openMode_ | S_IXUSR | S_IXGRP | S_IXOTH);
        if (-1 == res && EEXIST != errno) {
            std::cerr << "failed to create dir: " << name << ". Errno: " << errno << std::endl;
        }
    }

    openFile();
}

FileLogger::~FileLogger() {
    stopping_ = true;
    queueCondition_.notify_one();
    writingThread_.join();

    if (fd_ != -1) {
        close(fd_);
    }
}

void
FileLogger::onLoad() {
}

void
FileLogger::onUnload() {
}

void FileLogger::openFile() {
    if (fd_ != -1) {
        close(fd_);
    }
    fd_ = open(filename_.c_str(), O_WRONLY | O_CREAT | O_APPEND, openMode_);
    if (fd_ == -1) {
        std::cerr << "File logger cannot open file for writing: " << filename_ << std::endl;
    }
}

void
FileLogger::log(const Logger::Level level, const char* format, va_list args) {
    if (level < getLevel()) {
        return;
    }

    // Check without lock!
    if (fd_ == -1) {
        return;
    }

    char fmt[BUF_SIZE];
    prepareFormat(fmt, sizeof(fmt), level, format);

    va_list tmpargs;
    va_copy(tmpargs, args);
    size_t size = vsnprintf(NULL, 0, fmt, tmpargs);
    va_end(tmpargs);

    if (size > 0) {
        std::vector<char> data(size + 1);
        vsnprintf(&data[0], size + 1, fmt, args);
        boost::mutex::scoped_lock lock(queueMutex_);
        queue_.push_back(std::string(data.begin(), data.begin() + size));

        // Trying to decrease csw counter
        // It is not necessary to notify for every row - huge logs cause too many system calls
        // Timers are comfort for human eyes and cheap for system
        using namespace std::chrono;
        auto now = system_clock::now();
        if ( duration_cast<milliseconds>(now - lastNotify_).count() > 500 ) {
            queueCondition_.notify_one(); // system call
            lastNotify_ = now;
        }
    }
}

void
FileLogger::prepareFormat(char * buf, size_t size, const Logger::Level level, const char* format) {
    char timestr[64];
    if (print_time_) {
        struct tm tm;
        time_t t;
        time(&t);
        localtime_r(&t, &tm);
        strftime(timestr, sizeof(timestr) - 1, time_format_.c_str(), &tm);
    }

    std::string level_str;
    if (print_level_) {
        level_str = levelToString(level);
    }


    if (print_time_ && print_level_) {
        snprintf(buf, size - 1, "%s %s: %s\n", timestr, level_str.c_str(), format);
    }
    else if (print_time_) {
        snprintf(buf, size - 1, "%s %s\n", timestr, format);
    }
    else if (print_level_) {
        snprintf(buf, size - 1, "%s: %s\n", level_str.c_str(), format);
    }
    else {
        snprintf(buf, size - 1, "%s\n", format);
    }
    buf[size - 1] = '\0';
}

void
FileLogger::writingThread() {
    while (!stopping_) {
        std::vector<std::string> queueCopy;
        {
            boost::mutex::scoped_lock lock(queueMutex_);
            if (queue_.empty()) {
                // timed_wait - protection against loss notify()
                queueCondition_.timed_wait(lock, boost::posix_time::seconds(1));
            }
            std::swap(queueCopy, queue_);
        }

        if (fd_ != -1) {
            std::vector<std::string>::const_iterator curStr = queueCopy.begin();
            while (curStr != queueCopy.end()) {
                ssize_t toWrite = 0;
                size_t endPos = 0;

                for (; endPos < lines_per_shot_ && curStr != queueCopy.end(); ++endPos, ++curStr) {
                    iov_[endPos].iov_base = const_cast<char*>(curStr->data());
                    iov_[endPos].iov_len  = curStr->size();
                    toWrite += curStr->size();
                }

                size_t beginPos = 0;
                ssize_t writen = 0;
                while( writen < toWrite) {
                    ssize_t res = ::writev(fd_, iov_.data() + beginPos, endPos - beginPos);
                    if (res < 0) {
                       std::cerr << "Failed to write to log " << filename_
                                 << " : " << strerror(errno) << std::endl;
                    }
                    else {
                       writen += res;
                       if (writen >= toWrite) break;

                       while (res >= iov_[beginPos].iov_len) {
                           res -= iov_[beginPos].iov_len;
                           ++beginPos;
                       }

                       if (res) {
                           iov_[beginPos].iov_len -= res;
                           iov_[beginPos].iov_base += res;
                       }
                    }
                }
            }

            queueCopy.clear();
        }
    }
}

FCGIDAEMON_REGISTER_FACTORIES_BEGIN()
FCGIDAEMON_ADD_DEFAULT_FACTORY("logger", fastcgi::FileLogger)
FCGIDAEMON_REGISTER_FACTORIES_END()

} // namespace xscript
