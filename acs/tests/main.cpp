#include "test/Test.h"
#include "foundation/Log.h"

int main() {
    acs::LogConfig cfg{};
    cfg.console = true;
    cfg.debug_output = false;
    cfg.min_severity = acs::LogSeverity::Info;
    acs::Logger::Init(cfg);

    int rc = acs::test::RunAll();

    acs::Logger::Flush();
    acs::Logger::Shutdown();
    return rc;
}
