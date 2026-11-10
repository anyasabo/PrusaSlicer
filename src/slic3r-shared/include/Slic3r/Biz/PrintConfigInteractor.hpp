#pragma once

#include <functional>
#include <memory>
#include <string>

namespace Slic3r {
class DynamicPrintConfig;
}

namespace Slic3r::Biz {

class PrintConfigInteractor {
public:
    struct InputPort
    {
        virtual ~InputPort() = default;
        virtual void present_modal(const std::string& title, const std::string& message, std::function<void()> yes_callback) = 0;
    };

    PrintConfigInteractor(DynamicPrintConfig* print_config, InputPort* input_port) : m_input_port(input_port) {}

private:
    std::unique_ptr<InputPort> m_input_port;
};

}