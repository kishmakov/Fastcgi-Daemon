#pragma once

#include "details/response_time_statistics.h"

#include "fastcgi2/component.h"
#include "fastcgi2/handler.h"

#include <boost/cstdint.hpp>

#include <mutex>
#include <string>

namespace fastcgi {

class CounterData {
public:
	CounterData();
	void add(boost::uint64_t time);
	boost::uint64_t min() const;
	boost::uint64_t max() const;
	boost::uint64_t avg() const;
	boost::uint64_t hits() const;

private:
	boost::uint64_t min_;
	boost::uint64_t max_;
	boost::uint64_t total_;
	boost::uint64_t hits_;
};

class ResponseTimeHandler : virtual public Handler, virtual public Component,
	virtual public ResponseTimeStatistics {
public:
	ResponseTimeHandler(ComponentContext *context);
    virtual ~ResponseTimeHandler();

    virtual void onLoad();
    virtual void onUnload();

    virtual void handleRequest(Request *req, HandlerContext *handlerContext);
	virtual void add(const std::string &handler, unsigned short status, boost::uint64_t time);

private:
	std::mutex mutex_;
	typedef std::map<unsigned short, boost::shared_ptr<CounterData> > CounterMapType;
	std::map<std::string, CounterMapType> data_;
};

} // namespace fastcgi
