#include "config.h"

#include <unistd.h>

#include <phosphor-logging/elog.hpp>
#include <phosphor-logging/log.hpp>
#include <sdbusplus/bus.hpp>
#include <sdbusplus/exception.hpp>
#include <xyz/openbmc_project/Logging/Create/server.hpp>
#include <xyz/openbmc_project/Logging/Entry/server.hpp>

#include <cstdlib>
#include <fstream>
#include <string>

constexpr auto HOST_STATE_SVC = "xyz.openbmc_project.State.Host";
constexpr auto HOST_STATE_PATH = "/xyz/openbmc_project/state/host0";
constexpr auto PROPERTY_INTERFACE = "org.freedesktop.DBus.Properties";
constexpr auto BOOT_STATE_INTF = "xyz.openbmc_project.State.Boot.Progress";
constexpr auto BOOT_PROGRESS_PROP = "BootProgress";

constexpr auto LOGGING_SVC = "xyz.openbmc_project.Logging";
constexpr auto LOGGING_PATH = "/xyz/openbmc_project/logging";
constexpr auto LOGGING_CREATE_INTF = "xyz.openbmc_project.Logging.Create";

using namespace phosphor::logging;

bool wasHostBooting(sdbusplus::bus::bus& bus)
{
    try
    {
        auto method = bus.new_method_call(HOST_STATE_SVC, HOST_STATE_PATH,
                                          PROPERTY_INTERFACE, "Get");
        method.append(BOOT_STATE_INTF, BOOT_PROGRESS_PROP);

        auto response = bus.call(method);

        std::variant<std::string> bootProgress;
        response.read(bootProgress);

        if (std::get<std::string>(bootProgress) ==
            "xyz.openbmc_project.State.Boot.Progress.ProgressStages."
            "Unspecified")
        {
            log<level::INFO>("Host was not booting before BMC reboot");
            return false;
        }

        log<level::INFO>("Host was booting before BMC reboot",
                         entry("BOOTPROGRESS=%s",
                               std::get<std::string>(bootProgress).c_str()));
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>("Error reading BootProgress",
                        entry("ERROR=%s", e.what()),
                        entry("SERVICE=%s", HOST_STATE_SVC),
                        entry("PATH=%s", HOST_STATE_PATH));

        throw;
    }

    return true;
}

void createErrorLog(sdbusplus::bus::bus& bus)
{
    try
    {
        // Create interface requires something for additionalData
        std::map<std::string, std::string> additionalData;
        additionalData.emplace("_PID", std::to_string(getpid()));

        static constexpr auto errorMessage =
            "xyz.openbmc_project.State.Error.HostNotRunning";
        auto method = bus.new_method_call(LOGGING_SVC, LOGGING_PATH,
                                          LOGGING_CREATE_INTF, "Create");
        auto level =
            sdbusplus::xyz::openbmc_project::Logging::server::convertForMessage(
                sdbusplus::xyz::openbmc_project::Logging::server::Entry::Level::
                    Error);
        method.append(errorMessage, level, additionalData);
        auto resp = bus.call(method);
    }
    catch (const sdbusplus::exception::exception& e)
    {
        log<level::ERR>("sdbusplus D-Bus call exception",
                        entry("OBJPATH=%s", LOGGING_PATH),
                        entry("INTERFACE=%s", LOGGING_CREATE_INTF),
                        entry("EXCEPTION=%s", e.what()));

        throw std::runtime_error(
            "Error in invoking D-Bus logging create interface");
    }
    catch (std::exception& e)
    {
        log<level::ERR>("D-bus call exception",
                        entry("EXCEPTION=%s", e.what()));
        throw e;
    }
}

// Once CHASSIS_ON_FILE is removed, the obmc-chassis-poweron@.target has
// completed and the phosphor-chassis-state-manager code has processed it.
bool isChassisTargetComplete()
{
    auto size = std::snprintf(nullptr, 0, CHASSIS_ON_FILE, 0);
    size++; // null
    std::unique_ptr<char[]> buf(new char[size]);
    std::snprintf(buf.get(), size, CHASSIS_ON_FILE, 0);

    std::ifstream f(buf.get());
    return !f.good();
}

int main()
{

    auto bus = sdbusplus::bus::new_default();

    // Chassis power is on if this service starts but need to wait for the
    // obmc-chassis-poweron@.target to complete before potentially initiating
    // another systemd target transition (i.e. Quiesce->Reboot)
    while (!isChassisTargetComplete())
    {
        log<level::DEBUG>("Waiting for chassis on target to complete");
        std::this_thread::sleep_for(std::chrono::seconds(1));

        // There is no timeout here, wait until it happens or until system
        // is powered off and this service is stopped
    }

    log<level::INFO>("Chassis power on has completed, checking if host is "
                     "still running after the BMC reboot");

    // Check the last BootProgeress to see if the host was booting before
    // the BMC reboot occurred
    if (!wasHostBooting(bus))
    {
        return 0;
    }

    // Host was booting before the BMC reboot so log an error and go to host
    // quiesce target
    createErrorLog(bus);

    // TODO Move to Host Quiesce

    return 0;
}