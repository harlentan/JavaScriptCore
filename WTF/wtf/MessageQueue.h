/*
 * Copyright (C) 2008 Apple Inc. All rights reserved.
 * Copyright (C) 2009 Google Inc. All rights reserved.
 *
 * Redistribution and use in source and binary forms, with or without
 * modification, are permitted provided that the following conditions
 * are met:
 *
 * 1.  Redistributions of source code must retain the above copyright
 *     notice, this list of conditions and the following disclaimer.
 * 2.  Redistributions in binary form must reproduce the above copyright
 *     notice, this list of conditions and the following disclaimer in the
 *     documentation and/or other materials provided with the distribution.
 * 3.  Neither the name of Apple Inc. ("Apple") nor the names of
 *     its contributors may be used to endorse or promote products derived
 *     from this software without specific prior written permission.
 *
 * THIS SOFTWARE IS PROVIDED BY APPLE AND ITS CONTRIBUTORS "AS IS" AND ANY
 * EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 * WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 * DISCLAIMED. IN NO EVENT SHALL APPLE OR ITS CONTRIBUTORS BE LIABLE FOR ANY
 * DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 * (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 * LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 * ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 * (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF
 * THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
 */

#ifndef MessageQueue_h
#define MessageQueue_h

#include <limits>
#include <wtf/Assertions.h>
#include <wtf/Deque.h>
#include <wtf/Noncopyable.h>
#include <wtf/Threading.h>

namespace WTF {

    enum MessageQueueWaitResult {
        MessageQueueTerminated,       // Queue was destroyed while waiting for message.
        MessageQueueTimeout,          // Timeout was specified and it expired.
        MessageQueueMessageReceived   // A message was successfully received and returned.
    };

    // The queue takes ownership of messages and transfer it to the new owner
    // when messages are fetched from the queue.
    // Essentially, MessageQueue acts as a queue of std::unique_ptr<DataType>.
    template<typename DataType>
    class MessageQueue {
        WTF_MAKE_NONCOPYABLE(MessageQueue);
    public:
        MessageQueue() : m_killed(false) { }
        ~MessageQueue();

        void append(std::unique_ptr<DataType>);
        void appendAndKill(std::unique_ptr<DataType>);
        bool appendAndCheckEmpty(std::unique_ptr<DataType>);
        void prepend(std::unique_ptr<DataType>);

        std::unique_ptr<DataType> waitForMessage();
        std::unique_ptr<DataType> tryGetMessage();
        std::unique_ptr<DataType> tryGetMessageIgnoringKilled();
        template<typename Predicate>
        std::unique_ptr<DataType> waitForMessageFilteredWithTimeout(MessageQueueWaitResult&, Predicate&&, double absoluteTime);

        template<typename Predicate>
        void removeIf(Predicate&&);

        void kill();
        bool killed() const;

        // The result of isEmpty() is only valid if no other thread is manipulating the queue at the same time.
        bool isEmpty();

        static double infiniteTime() { return std::numeric_limits<double>::max(); }

    private:
        mutable Mutex m_mutex;
        ThreadCondition m_condition;
        Deque<std::unique_ptr<DataType>> m_queue;
        bool m_killed;
    };

    template<typename DataType>
    MessageQueue<DataType>::~MessageQueue()
    {
    }

    template<typename DataType>
    inline void MessageQueue<DataType>::append(std::unique_ptr<DataType> message)
    {
        MutexLocker lock(m_mutex);
        m_queue.append(std::move(message));
        m_condition.signal();
    }

    template<typename DataType>
    inline void MessageQueue<DataType>::appendAndKill(std::unique_ptr<DataType> message)
    {
        MutexLocker lock(m_mutex);
        m_queue.append(std::move(message));
        m_killed = true;
        m_condition.broadcast();
    }

    // Returns true if the queue was empty before the item was added.
    template<typename DataType>
    inline bool MessageQueue<DataType>::appendAndCheckEmpty(std::unique_ptr<DataType> message)
    {
        MutexLocker lock(m_mutex);
        bool wasEmpty = m_queue.isEmpty();
        m_queue.append(std::move(message));
        m_condition.signal();
        return wasEmpty;
    }

    template<typename DataType>
    inline void MessageQueue<DataType>::prepend(std::unique_ptr<DataType> message)
    {
        MutexLocker lock(m_mutex);
        m_queue.prepend(std::move(message));
        m_condition.signal();
    }

    template<typename DataType>
    inline auto MessageQueue<DataType>::waitForMessage() -> std::unique_ptr<DataType>
    {
        MessageQueueWaitResult exitReason; 
        std::unique_ptr<DataType> result = waitForMessageFilteredWithTimeout(exitReason, [](const DataType&) { return true; }, infiniteTime());
        ASSERT(exitReason == MessageQueueTerminated || exitReason == MessageQueueMessageReceived);
        return result;
    }

    template<typename DataType>
    template<typename Predicate>
    inline auto MessageQueue<DataType>::waitForMessageFilteredWithTimeout(MessageQueueWaitResult& result, Predicate&& predicate, double absoluteTime) -> std::unique_ptr<DataType>
    {
        MutexLocker lock(m_mutex);
        bool timedOut = false;

        auto found = m_queue.end();
        while (!m_killed && !timedOut) {
            found = m_queue.findIf([&predicate](const std::unique_ptr<DataType>& ptr) -> bool {
                ASSERT(ptr);
                return predicate(*ptr);
            });
            if (found != m_queue.end())
                break;

            timedOut = !m_condition.timedWait(m_mutex, absoluteTime);
        }

        ASSERT(!timedOut || absoluteTime != infiniteTime());

        if (m_killed) {
            result = MessageQueueTerminated;
            return nullptr;
        }

        if (timedOut) {
            result = MessageQueueTimeout;
            return nullptr;
        }

        ASSERT(found != m_queue.end());
        std::unique_ptr<DataType> message = std::move(*found);
        m_queue.remove(found);
        result = MessageQueueMessageReceived;
        return message;
    }

    template<typename DataType>
    inline auto MessageQueue<DataType>::tryGetMessage() -> std::unique_ptr<DataType>
    {
        MutexLocker lock(m_mutex);
        if (m_killed)
            return nullptr;
        if (m_queue.isEmpty())
            return nullptr;

        return m_queue.takeFirst();
    }

    template<typename DataType>
    inline auto MessageQueue<DataType>::tryGetMessageIgnoringKilled() -> std::unique_ptr<DataType>
    {
        MutexLocker lock(m_mutex);
        if (m_queue.isEmpty())
            return nullptr;

        return m_queue.takeFirst();
    }

    template<typename DataType>
    template<typename Predicate>
    inline void MessageQueue<DataType>::removeIf(Predicate&& predicate)
    {
        MutexLocker lock(m_mutex);
        while (true) {
            auto found = m_queue.findIf([&predicate](const std::unique_ptr<DataType>& ptr) -> bool {
                ASSERT(ptr);
                return predicate(*ptr);
            });

            if (found == m_queue.end())
                break;

            m_queue.remove(found);
        }
    }

    template<typename DataType>
    inline bool MessageQueue<DataType>::isEmpty()
    {
        MutexLocker lock(m_mutex);
        if (m_killed)
            return true;
        return m_queue.isEmpty();
    }

    template<typename DataType>
    inline void MessageQueue<DataType>::kill()
    {
        MutexLocker lock(m_mutex);
        m_killed = true;
        m_condition.broadcast();
    }

    template<typename DataType>
    inline bool MessageQueue<DataType>::killed() const
    {
        MutexLocker lock(m_mutex);
        return m_killed;
    }
} // namespace WTF

using WTF::MessageQueue;
// MessageQueueWaitResult enum and all its values.
using WTF::MessageQueueWaitResult;
using WTF::MessageQueueTerminated;
using WTF::MessageQueueTimeout;
using WTF::MessageQueueMessageReceived;

#endif // MessageQueue_h
