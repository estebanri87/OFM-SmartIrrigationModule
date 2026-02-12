#pragma once
#include "OpenKNX.h"

class SmartIrrigationModule : public OpenKNX::Module
{
  public:
    const std::string name() override;
    const std::string version() override;

    void init() override;
    void setup(bool configured) override;
    void loop(bool configured) override;
};

extern SmartIrrigationModule openknxSmartIrrigationModule;
