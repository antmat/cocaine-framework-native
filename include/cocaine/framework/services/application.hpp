#ifndef COCAINE_FRAMEWORK_SERVICES_APPLICATION_HPP
#define COCAINE_FRAMEWORK_SERVICES_APPLICATION_HPP

#include <cocaine/framework/service.hpp>

#include <cocaine/traits/literal.hpp>

namespace cocaine { namespace framework {

struct application_client_t :
    public service_t
{
    static const unsigned int version = cocaine::io::protocol<cocaine::io::app_tag>::version::value;

    application_client_t(std::shared_ptr<service_connection_t> connection) :
        service_t(connection)
    {
        // pass
    }

    service_traits<cocaine::io::app::enqueue>::future_type
    enqueue(const std::string& event,
            const std::string& chunk)
    {
        return call<cocaine::io::app::enqueue>(event, chunk);
    }
};

}} // namespace cocaine::framework

#endif // COCAINE_FRAMEWORK_SERVICES_APPLICATION_HPP
