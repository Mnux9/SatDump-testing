#define SATDUMP_DLL_EXPORT 1
#include "init.h"
#include "logger.h"
#include "core/module.h"
#include "core/pipeline.h"
#include <filesystem>
//#include "tle.h"
#include "core/plugin.h"
#include "satdump_vars.h"
#include "core/config.h"

#include "common/tracking/tle.h"
#include "products/products.h"

#include "core/opencl.h"

namespace satdump
{
    SATDUMP_DLL std::string user_path;
    SATDUMP_DLL std::string tle_file_override = "";

    void initSatdump()
    {
        logger->info("   _____       __  ____                      ");
        logger->info("  / ___/____ _/ /_/ __ \\__  ______ ___  ____ ");
        logger->info("  \\__ \\/ __ `/ __/ / / / / / / __ `__ \\/ __ \\");
        logger->info(" ___/ / /_/ / /_/ /_/ / /_/ / / / / / / /_/ /");
        logger->info("/____/\\__,_/\\__/_____/\\__,_/_/ /_/ /_/ .___/ ");
        logger->info("                                    /_/      ");
        logger->info("Starting SatDump v" + (std::string)SATDUMP_VERSION);
        logger->info("");

#ifdef _WIN32
        user_path = ".";
#elif __ANDROID__
        user_path = ".";
#else
        user_path = std::string(getenv("HOME")) + "/.config/satdump";
#endif

        if (std::filesystem::exists("satdump_cfg.json"))
            config::loadConfig("satdump_cfg.json", user_path);
        else
            config::loadConfig(satdump::RESPATH + "satdump_cfg.json", user_path);

        loadPlugins();

        registerModules();

        // Load pipelines
        if (std::filesystem::exists("pipelines") && std::filesystem::is_directory("pipelines"))
            loadPipelines("pipelines");
        else
            loadPipelines(satdump::RESPATH + "pipelines");

        // List them
        logger->debug("Registered pipelines :");
        for (Pipeline &pipeline : pipelines)
            logger->debug(" - " + pipeline.name);

#ifdef USE_OPENCL
        // OpenCL
        opencl::initOpenCL();
#endif

        // TLEs
        if (tle_file_override == "")
        {
            if (config::main_cfg["satdump_general"]["update_tles_startup"]["value"].get<bool>())
                updateTLEFile(user_path + "/satdump_tles.txt");
            loadTLEFileIntoRegistry(user_path + "/satdump_tles.txt");
            if (general_tle_registry.size() == 0) // NO TLEs? Download now.
            {
                updateTLEFile(user_path + "/satdump_tles.txt");
                loadTLEFileIntoRegistry(user_path + "/satdump_tles.txt");
            }
        }
        else
        {
            if (std::filesystem::exists(tle_file_override))
                loadTLEFileIntoRegistry(tle_file_override);
            else
                logger->error("TLE File doesn't exist : " + tle_file_override);
        }

        // Products
        registerProducts();

        // Let plugins know we started
        eventBus->fire_event<SatDumpStartedEvent>({});
    }
}