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

#include "cocaine/framework/manager.hpp"

#include <boost/optional.hpp>
#include <boost/thread/thread.hpp>

#include "cocaine/framework/scheduler.hpp"

#include "cocaine/framework/detail/loop.hpp"
#include "cocaine/framework/detail/runnable.hpp"

using namespace cocaine::framework;
using namespace cocaine::framework::detail;

class cocaine::framework::execution_unit_t {
public:
    loop_t io;

    boost::optional<loop_t::work> work;
    event_loop_t event_loop;
    scheduler_t scheduler;
    boost::thread thread;

    execution_unit_t() :
        work(boost::optional<loop_t::work>(loop_t::work(io))),
        event_loop(io),
        scheduler(event_loop),
        thread(named_runnable<loop_t>("[CF::M]", io))
    {}

    ~execution_unit_t() {
        work.reset();
        thread.join();
    }
};

static const std::vector<session_t::endpoint_type> DEFAULT_LOCATIONS = {
    { boost::asio::ip::tcp::v6(), 10053 }
};

class cocaine::framework::service_manager_data {
public:
    loop_t io;
    boost::optional<loop_t::work> work;
    event_loop_t event_loop;
    scheduler_t scheduler;

    std::vector<session_t::endpoint_type> locations;

    std::vector<boost::thread> threads;

    service_manager_data() :
        work(boost::optional<loop_t::work>(loop_t::work(io))),
        event_loop(io),
        scheduler(event_loop),
        locations(DEFAULT_LOCATIONS)
    {}
};

service_manager_t::service_manager_t() :
    d(new service_manager_data)
{
    auto threads = boost::thread::hardware_concurrency();
    start(threads != 0 ? threads : 1);
}

service_manager_t::service_manager_t(unsigned int threads) :
    d(new service_manager_data)
{
    if (threads == 0) {
        throw std::invalid_argument("thread count must be a positive number");
    }

    start(threads);
}

service_manager_t::~service_manager_t() {
    d->work.reset();
    for (auto& thread : d->threads) {
        thread.join();
    }
}

std::vector<session_t::endpoint_type> service_manager_t::endpoints() const {
    return d->locations;
}

void service_manager_t::endpoints(std::vector<session_t::endpoint_type> endpoints) {
    d->locations = std::move(endpoints);
}

void service_manager_t::start(unsigned int threads) {
    for (unsigned int i = 0; i < threads; ++i) {
        d->threads.emplace_back(named_runnable<loop_t>("[CF::M]", d->io));
    }
}

scheduler_t& service_manager_t::next() {
    return d->scheduler;
}
