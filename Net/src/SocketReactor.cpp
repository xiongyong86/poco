//
// SocketReactor.cpp
//
// Library: Net
// Package: Reactor
// Module:  SocketReactor
//
// Copyright (c) 2005-2006, Applied Informatics Software Engineering GmbH.
// and Contributors.
//
// SPDX-License-Identifier:	BSL-1.0
//


#include "Poco/Net/SocketReactor.h"
#include "Poco/Net/SocketNotification.h"
#include "Poco/Net/SocketNotifier.h"
#include "Poco/ErrorHandler.h"
#include "Poco/Thread.h"
#include "Poco/Exception.h"
#include <memory>
#include <limits>


using Poco::Exception;
using Poco::ErrorHandler;


namespace Poco {
namespace Net {


const Timestamp::TimeDiff SocketReactor::PERMANENT_COMPLETION_HANDLER =
	std::numeric_limits<Timestamp::TimeDiff>::max();


SocketReactor::SocketReactor():
	_stop(false),
	_timeout(DEFAULT_TIMEOUT),
	_pReadableNotification(new ReadableNotification(this)),
	_pWritableNotification(new WritableNotification(this)),
	_pErrorNotification(new ErrorNotification(this)),
	_pTimeoutNotification(new TimeoutNotification(this)),
	_pIdleNotification(new IdleNotification(this)),
	_pShutdownNotification(new ShutdownNotification(this)),
	_pThread(0)
{
}


SocketReactor::SocketReactor(const Poco::Timespan& timeout):
	_stop(false),
	_timeout(timeout),
	_pReadableNotification(new ReadableNotification(this)),
	_pWritableNotification(new WritableNotification(this)),
	_pErrorNotification(new ErrorNotification(this)),
	_pTimeoutNotification(new TimeoutNotification(this)),
	_pIdleNotification(new IdleNotification(this)),
	_pShutdownNotification(new ShutdownNotification(this)),
	_pThread(0)
{
}


SocketReactor::~SocketReactor()
{
}


int SocketReactor::poll(int* pHandled)
{
	int handled = 0;
	if (!hasSocketHandlers()) onIdle();
	else
	{
		bool readable = false;
		PollSet::SocketModeMap sm = _pollSet.poll(_timeout);
		if (sm.size() > 0)
		{
			onBusy();
			PollSet::SocketModeMap::iterator it = sm.begin();
			PollSet::SocketModeMap::iterator end = sm.end();
			for (; it != end; ++it)
			{
				if (it->second & PollSet::POLL_READ)
				{
					dispatch(it->first, _pReadableNotification);
					readable = true;
					++handled;
				}
				if (it->second & PollSet::POLL_WRITE)
				{
					dispatch(it->first, _pWritableNotification);
					++handled;
				}
				if (it->second & PollSet::POLL_ERROR)
				{
					dispatch(it->first, _pErrorNotification);
					++handled;
				}
			}
		}
		if (!readable) onTimeout();
	}
	if (pHandled) *pHandled = handled;
	return onComplete();
}


int SocketReactor::onComplete(bool handleOne)
{
	std::unique_ptr<CompletionHandler> pCH;
	int handled = 0;
	{
		HandlerList::iterator it = _complHandlers.begin();
		while (it != _complHandlers.end())
		{
			std::size_t prevSize = 0;
			// A completion handler may add new
			// completion handler(s), so the mutex must
			// be unlocked before the invocation.
			{
				SpinScopedLock lock(_completionMutex);
				bool alwaysRun = isPermanent(it->second);
				bool isExpired = !alwaysRun && (Timestamp() > it->second);
				if (isExpired)
				{
					pCH.reset(new CompletionHandler(std::move(it->first)));
					it = _complHandlers.erase(it);
				}
				else if (alwaysRun)
				{
					pCH.reset(new CompletionHandler(it->first));
					++it;
				}
				else ++it;
				prevSize = _complHandlers.size();
			}

			if (pCH)
			{
				(*pCH)();
				pCH.reset();
				++handled;
				if (handleOne) break;
			}
			// handler call may add or remove handlers;
			// if so, we must start from the beginning
			{
				SpinScopedLock lock(_completionMutex);
				if (prevSize != _complHandlers.size())
					it = _complHandlers.begin();
			}
		}
	}
	return handled;
}


int SocketReactor::runOne()
{
	try
	{
		while (0 == onComplete(true));
		return 1;
	}
	catch(...) {}
	return 0;
}


void SocketReactor::run()
{
	_pThread = Thread::current();
	while (!_stop)
	{
		try
		{
			if (!poll())
			{
				if (!hasSocketHandlers())
					Thread::trySleep(static_cast<long>(_timeout.totalMilliseconds()));
			}
		}
		catch (Exception& exc)
		{
			ErrorHandler::handle(exc);
		}
		catch (std::exception& exc)
		{
			ErrorHandler::handle(exc);
		}
		catch (...)
		{
			ErrorHandler::handle();
		}
	}
	onShutdown();
}


bool SocketReactor::hasSocketHandlers()
{
	if (!_pollSet.empty())
	{
		FastScopedLock lock(_ioMutex);
		for (auto& p: _handlers)
		{
			if (p.second->accepts(_pReadableNotification) ||
				p.second->accepts(_pWritableNotification) ||
				p.second->accepts(_pErrorNotification)) return true;
		}
	}

	return false;
}


void SocketReactor::stop()
{
	_stop = true;
}


void SocketReactor::wakeUp()
{
	if (_pThread) _pThread->wakeUp();
}


void SocketReactor::setTimeout(const Poco::Timespan& timeout)
{
	_timeout = timeout;
}


const Poco::Timespan& SocketReactor::getTimeout() const
{
	return _timeout;
}


void SocketReactor::addCompletionHandler(const CompletionHandler& ch, Timestamp::TimeDiff ms)
{
	addCompletionHandler(CompletionHandler(ch), ms);
}


void SocketReactor::addCompletionHandler(CompletionHandler&& ch, Timestamp::TimeDiff ms, int pos)
{
	Poco::Timestamp expires = (ms != PERMANENT_COMPLETION_HANDLER) ? Timestamp() + ms*1000 : Timestamp(PERMANENT_COMPLETION_HANDLER);

	if (pos == -1)
	{
		SpinScopedLock lock(_completionMutex);
		_complHandlers.push_back({std::move(ch), expires});
	}
	else
	{
		if (pos < 0) throw Poco::InvalidArgumentException("SocketReactor::addCompletionHandler()");
		SpinScopedLock lock(_completionMutex);
		_complHandlers.insert(_complHandlers.begin() + pos, {std::move(ch), expires});
	}
}


void SocketReactor::removeCompletionHandlers()
{
	SpinScopedLock lock(_completionMutex);
	_complHandlers.clear();
}


int SocketReactor::scheduledCompletionHandlers()
{
	int cnt = 0;
	SpinScopedLock lock(_completionMutex);
	HandlerList::iterator it = _complHandlers.begin();
	for (; it != _complHandlers.end(); ++it)
	{
		if (!isPermanent(it->second)/*it->second != Timestamp(0)*/) ++cnt;
	}
	return cnt;
}


int SocketReactor::removeScheduledCompletionHandlers(int count)
{
	auto isScheduled = [this](const Timestamp& ts) { return !isPermanent(ts); };
	return removeCompletionHandlers(isScheduled, count);
}


int SocketReactor::permanentCompletionHandlers()
{
	int cnt = 0;
	SpinScopedLock lock(_completionMutex);
	HandlerList::iterator it = _complHandlers.begin();
	for (; it != _complHandlers.end(); ++it)
	{
		if (isPermanent(it->second))
			++cnt;
	}
	return cnt;
}


int SocketReactor::removePermanentCompletionHandlers(int count)
{
	auto perm = [this](const Timestamp& ts) { return isPermanent(ts); };
	return removeCompletionHandlers(perm, count);
}


bool SocketReactor::isPermanent(const Timestamp& entry) const
{
	return entry == Timestamp(PERMANENT_COMPLETION_HANDLER);
}


void SocketReactor::addEventHandler(const Socket& socket, const Poco::AbstractObserver& observer)
{
	NotifierPtr pNotifier = getNotifier(socket, true);

	if (!pNotifier->hasObserver(observer))
	{
		pNotifier->addObserver(this, observer);
	}

	int mode = 0;
	if (pNotifier->accepts(_pReadableNotification)) mode |= PollSet::POLL_READ;
	if (pNotifier->accepts(_pWritableNotification)) mode |= PollSet::POLL_WRITE;
	if (pNotifier->accepts(_pErrorNotification))    mode |= PollSet::POLL_ERROR;
	if (mode) _pollSet.add(socket, mode);
}


bool SocketReactor::hasEventHandler(const Socket& socket, const Poco::AbstractObserver& observer)
{
	NotifierPtr pNotifier = getNotifier(socket);
	if (!pNotifier) return false;
	if (pNotifier->hasObserver(observer)) return true;
	return false;
}


SocketReactor::NotifierPtr SocketReactor::getNotifier(const Socket& socket, bool makeNew)
{
	const SocketImpl* pImpl = socket.impl();
	if (pImpl == nullptr) return 0;
	poco_socket_t sockfd = pImpl->sockfd();
	FastScopedLock lock(_ioMutex);

	EventHandlerMap::iterator it = _handlers.find(sockfd);
	if (it != _handlers.end()) return it->second;
	else if (makeNew) return (_handlers[sockfd] = new SocketNotifier(socket));

	return 0;
}


void SocketReactor::removeEventHandler(const Socket& socket, const Poco::AbstractObserver& observer)
{
	const SocketImpl* pImpl = socket.impl();
	if (pImpl == nullptr) { return; }
	NotifierPtr pNotifier = getNotifier(socket);
	if (pNotifier && pNotifier->hasObserver(observer))
	{
		if(pNotifier->countObservers() == 1)
		{
			{
				FastScopedLock lock(_ioMutex);
				_handlers.erase(pImpl->sockfd());
			}
			_pollSet.remove(socket);
		}
		pNotifier->removeObserver(this, observer);
	}
}


bool SocketReactor::has(const Socket& socket) const
{
	return _pollSet.has(socket);
}


void SocketReactor::onTimeout()
{
	dispatch(_pTimeoutNotification);
}


void SocketReactor::onIdle()
{
	dispatch(_pIdleNotification);
}


void SocketReactor::onShutdown()
{
	dispatch(_pShutdownNotification);
}


void SocketReactor::onBusy()
{
}


void SocketReactor::dispatch(const Socket& socket, SocketNotification* pNotification)
{
	NotifierPtr pNotifier = getNotifier(socket);
	if (!pNotifier) return;
	dispatch(pNotifier, pNotification);
}


void SocketReactor::dispatch(SocketNotification* pNotification)
{
	std::vector<NotifierPtr> delegates;
	{
		FastScopedLock lock(_ioMutex);
		delegates.reserve(_handlers.size());
		for (EventHandlerMap::iterator it = _handlers.begin(); it != _handlers.end(); ++it)
			delegates.push_back(it->second);
	}
	for (std::vector<NotifierPtr>::iterator it = delegates.begin(); it != delegates.end(); ++it)
	{
		dispatch(*it, pNotification);
	}
}


void SocketReactor::dispatch(NotifierPtr& pNotifier, SocketNotification* pNotification)
{
	try
	{
		Socket s = pNotification->socket();
		pNotifier->dispatch(pNotification);
	}
	catch (Exception& exc)
	{
		ErrorHandler::handle(exc);
	}
	catch (std::exception& exc)
	{
		ErrorHandler::handle(exc);
	}
	catch (...)
	{
		ErrorHandler::handle();
	}
}


} } // namespace Poco::Net
