/*
    Copyright (c) 2015 Evgeny Safronov <division494@gmail.com>
    Copyright (c) 2011-2015 Other contributors as noted in the AUTHORS file.
    This file is part of Cocaine.
    Cocaine is free software; you can redistribute it and/or modify
    it under the terms of the GNU Lesser General Public License as published by
    the Free Software Foundation; either version 3 of the License, or
    (at your option) any later version.
    Cocaine is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
    GNU Lesser General Public License for more details.
    You should have received a copy of the GNU Lesser General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

#include "cocaine/framework/detail/basic_session.hpp"

#include <memory>

#include <asio/connect.hpp>

#include "cocaine/framework/sender.hpp"
#include "cocaine/framework/scheduler.hpp"

#include "cocaine/framework/detail/log.hpp"
#include "cocaine/framework/detail/loop.hpp"
#include "cocaine/framework/detail/net.hpp"
#include "cocaine/framework/detail/shared_state.hpp"

namespace ph = std::placeholders;

using namespace cocaine;
using namespace cocaine::framework;
using namespace cocaine::framework::detail;

//! \note single shot.
class basic_session_t::push_t : public std::enable_shared_from_this<push_t> {
    io::encoder_t::message_type message;
    std::shared_ptr<basic_session_t> connection;
    task<void>::promise_type promise;

public:
    push_t(io::encoder_t::message_type&& message, std::shared_ptr<basic_session_t> connection, task<void>::promise_type&& promise) :
        message(std::move(message)),
        connection(connection),
        promise(std::move(promise))
    {}

    /*!
     * \warning guaranteed to be called from the event loop thread, otherwise the behavior is
     * undefined.
     */
    void operator()() {
        if (connection->channel) {
            CF_DBG("writing %lu bytes ...", message.size());
            connection->channel->writer->write(message, wrap(std::bind(&push_t::on_write, shared_from_this(), ph::_1)));
        } else {
            CF_DBG("<< write aborted: not connected");
            promise.set_exception(std::system_error(asio::error::not_connected));
        }
    }

private:
    void on_write(const std::error_code& ec) {
        CF_DBG("<< write: %s", CF_EC(ec));

        if (ec) {
            connection->on_error(ec);
            promise.set_exception(std::system_error(ec));
        } else {
            promise.set_value();
        }
    }
};

basic_session_t::basic_session_t(scheduler_t& scheduler) noexcept :
    scheduler(scheduler),
    state(0),
    counter(1),
    message(boost::none)
{}

basic_session_t::~basic_session_t() {}

bool basic_session_t::connected() const noexcept {
    return state == static_cast<std::uint8_t>(state_t::connected);
}

auto basic_session_t::connect(const endpoint_type& endpoint) -> task<std::error_code>::future_type {
    return connect(std::vector<endpoint_type> {{ endpoint }});
}

auto basic_session_t::connect(const std::vector<endpoint_type>& endpoints) -> task<std::error_code>::future_type {
    CF_CTX("bC");
    CF_DBG(">> connecting ...");

    task<std::error_code>::promise_type promise;
    auto future = promise.get_future();

    std::unique_lock<std::mutex> lock(state_mutex);
    switch (static_cast<state_t>(state.load())) {
    case state_t::disconnected: {
        std::unique_ptr<socket_type> socket(new socket_type(scheduler.loop().loop));

        // The code above can throw std::bad_alloc, so here it is the right place to change
        // current object's state.
        state = static_cast<std::uint8_t>(state_t::connecting);
        lock.unlock();

        auto converted = endpoints_cast<asio::ip::tcp::endpoint>(endpoints);
        socket_type* socket_ = socket.get();
        asio::async_connect(
            *socket_,
            converted.begin(), converted.end(),
            wrap(std::bind(&basic_session_t::on_connect, shared_from_this(), ph::_1, std::move(promise), std::move(socket)))
        );

        break;
    }
    case state_t::connecting: {
        lock.unlock();
        CF_DBG("<< already in progress");
        promise.set_value(asio::error::already_started);
        break;
    }
    case state_t::connected: {
        lock.unlock();
        CF_DBG("<< already connected");
        promise.set_value(asio::error::already_connected);
        break;
    }
    default:
        BOOST_ASSERT(false);
    }

    return future;
}

auto basic_session_t::endpoint() const -> boost::optional<endpoint_type> {
    // TODO: Implement `basic_session_t::endpoint()`.
    return boost::none;
}

void basic_session_t::disconnect() {
    CF_DBG(">> disconnecting ...");

    state = static_cast<std::uint8_t>(state_t::dying);
    if (channels->empty()) {
        CF_DBG("<< stop listening");
        std::lock_guard<std::mutex> channel_lock(channel_mutex);
        channel.reset();
    }

    CF_DBG("<< disconnected");
}

auto basic_session_t::invoke(std::function<io::encoder_t::message_type(std::uint64_t)> encoder)
    -> task<invoke_result>::future_type
{
    std::lock_guard<std::mutex> invoke_lock(invoke_mutex);
    const auto span = counter++;

    CF_CTX("bI" + std::to_string(span));
    CF_DBG("invoking span %llu event ...", CF_US(span));

    auto tx = std::make_shared<basic_sender_t<basic_session_t>>(span, shared_from_this());
    auto state = std::make_shared<shared_state_t>();
    auto rx = std::make_shared<basic_receiver_t<basic_session_t>>(span, shared_from_this(), state);

    channels->insert(std::make_pair(span, std::move(state)));
    return push(encoder(span))
        .then(scheduler, wrap([tx, rx](task<void>::future_move_type future) -> invoke_result {
            future.get();
            return std::make_tuple(tx, rx);
        }));
}

auto basic_session_t::push(io::encoder_t::message_type&& message) -> task<void>::future_type {
    CF_CTX("bP");
    CF_DBG(">> writing message ...");

    task<void>::promise_type promise;
    auto future = promise.get_future();
    auto action = std::make_shared<push_t>(std::move(message), shared_from_this(), std::move(promise));

    std::lock_guard<std::mutex> channel_lock(channel_mutex);
    (*action)();
    return future;
}

void basic_session_t::revoke(std::uint64_t span) {
    CF_DBG(">> revoking span %llu channel", CF_US(span));

    auto channels = this->channels.synchronize();
    channels->erase(span);
    if (channels->empty() && state == static_cast<std::uint8_t>(state_t::dying)) {
        // At this moment there are no references left to this session and also nobody is intrested
        // for data reading.
        // TODO: But there can be pending writing events.
        CF_DBG("<< stop listening");
        std::lock_guard<std::mutex> channel_lock(channel_mutex);
        channel.reset();
    }
    CF_DBG("<< revoke span %llu channel", CF_US(span));
}

void basic_session_t::on_connect(const std::error_code& ec, task<std::error_code>::promise_move_type promise, std::unique_ptr<socket_type>& s) {
    CF_DBG("<< connect: %s", CF_EC(ec));

    std::unique_lock<std::mutex> channel_lock(channel_mutex);
    if (ec) {
        channel.reset();
        state = static_cast<std::uint8_t>(state_t::disconnected);
    } else {
        CF_CTX_POP();
        CF_CTX("bR");
        CF_DBG(">> listening for read events ...");

        channel.reset(new channel_type(std::move(s)));
        channel->reader->read(message, wrap(std::bind(&basic_session_t::on_read, shared_from_this(), ph::_1)));
        state = static_cast<std::uint8_t>(state_t::connected);
    }
    channel_lock.unlock();

    promise.set_value(ec);
}

void basic_session_t::on_read(const std::error_code& ec) {
    CF_DBG("<< read: %s", CF_EC(ec));

    if (ec) {
        on_error(ec);
        return;
    }

    std::unique_lock<std::mutex> channel_lock(channel_mutex);
    if (!channel) {
        CF_DBG("received message from disconnected channel");
        on_error(asio::error::operation_aborted);
        return;
    }
    channel_lock.unlock();

    CF_DBG("received message [%llu, %llu, %s]", CF_US(message.span()), CF_US(message.type()), CF_MSG(message.args()).c_str());
    std::shared_ptr<shared_state_t> state;
    {
        auto channels = this->channels.synchronize();
        auto it = channels->find(message.span());
        if (it == channels->end()) {
            CF_DBG("dropping an orphan span %llu message", CF_US(message.span()));
        } else {
            state = it->second;
        }
    }

    if (state) {
        state->put(std::move(message));
    }

    channel_lock.lock();
    if (channel) {
        CF_DBG(">> listening for read events ...");
        channel->reader->read(message, wrap(std::bind(&basic_session_t::on_read, shared_from_this(), ph::_1)));
    }
}

void basic_session_t::on_error(const std::error_code& ec) {
    BOOST_ASSERT(ec);

    state = static_cast<std::uint8_t>(state_t::disconnected);

    auto channels = this->channels.synchronize();
    for (auto channel : *channels) {
        channel.second->put(ec);
    }
    channels->clear();
}

#include "sender.cpp"
template class cocaine::framework::basic_sender_t<basic_session_t>;

#include "receiver.cpp"
template class cocaine::framework::basic_receiver_t<basic_session_t>;
