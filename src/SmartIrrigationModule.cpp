#include "ModuleVersionCheck.h"
#include "SmartIrrigationModule.h"
#include "versions.h"

const std::string SmartIrrigationModule::name()
{
    return "SmartIrrigation";
}

const std::string SmartIrrigationModule::version()
{
    return MODULE_SmartIrrigation_Version;
}

void SmartIrrigationModule::init()
{
}

void SmartIrrigationModule::setup(bool configured)
{
    (void)configured;
}

void SmartIrrigationModule::loop(bool configured)
{
    (void)configured;
}

SmartIrrigationModule openknxSmartIrrigationModule;
